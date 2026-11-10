/*
 * us122ld: owns the TASCAM US-122L over USB (through the us122l library) and
 * shares its audio and clock with the Core Audio plug-in (see shared.h).
 *
 * Runs as a launchd daemon with the Mach service US122L_SERVICE. It streams
 * only while the plug-in has I/O clients (plus a few seconds), so the unit is
 * free otherwise.
 *
 * The clock: capture packets arrive every ~2 ms with jittery timestamps. A
 * delay-locked loop (0.5 Hz) turns (frames received, arrival time) into a
 * smooth line, frame -> host time, which the plug-in hands to Core Audio as
 * its zero time stamps.
 */
#include "../include/us122l.h"
#include "shared.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <math.h>
#include <pthread.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define LINGER_S 5.0          /* keep streaming this long after the last client */
#define STALL_S 0.5           /* no capture this long: restart */
#define RETRY_S 2.0           /* after a failed start */

static us122l_shm_t *S;
static mach_port_t mem_entry;
static mach_vm_size_t shm_size;
static double ticks_per_sec;
static volatile sig_atomic_t quit;

/* touched by the USB thread while a stream runs, reset between streams */
static struct {
    int init;
    uint64_t base_frame;
    double base_host, tpf, nominal_tpf;
    int out_init;
    uint64_t out_pos;
    unsigned rate;
    _Atomic uint64_t last_capture;
} st;

static void logf_(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(stderr, "%s us122ld: ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static double now_s(void) { return mach_absolute_time() / ticks_per_sec; }

/* ------------------------------------------------------------ USB thread */

static void on_capture(const float *in, unsigned n, uint64_t first, uint64_t host, void *u)
{
    (void)u;
    for (unsigned i = 0; i < n; i++) {
        uint64_t k = ((first + i) & (US122L_RING - 1)) * 2;
        S->in_ring[k] = in[2 * i];
        S->in_ring[k + 1] = in[2 * i + 1];
    }
    atomic_store_explicit(&S->in_written, first + n, memory_order_release);
    atomic_store(&st.last_capture, host);

    /* delay-locked loop: frame -> host time */
    uint64_t f = first + n;
    if (!st.init) {
        st.base_frame = f;
        st.base_host = (double)host;
        st.tpf = st.nominal_tpf;
        st.init = 1;
    } else {
        double pred = st.base_host + (double)(f - st.base_frame) * st.tpf;
        double e = (double)host - pred;
        if (fabs(e) > 0.05 * ticks_per_sec) {      /* 50 ms off: start over */
            st.base_host = (double)host;
            st.tpf = st.nominal_tpf;
            atomic_fetch_add(&S->glitches, 1);
        } else {
            const double w = 2 * M_PI * 0.5 * 0.002;   /* 0.5 Hz, ~2 ms updates */
            st.base_host = pred + M_SQRT2 * w * e;
            st.tpf += w * w * e / n;
            /* a real clock is within a fraction of a percent */
            if (st.tpf > st.nominal_tpf * 1.01) st.tpf = st.nominal_tpf * 1.01;
            if (st.tpf < st.nominal_tpf * 0.99) st.tpf = st.nominal_tpf * 0.99;
        }
        st.base_frame = f;
    }
    us122l_write_clock(S, st.base_frame, (uint64_t)st.base_host, st.tpf);
}

static void on_playback(float *out, unsigned n, uint64_t first, void *u)
{
    (void)u;
    (void)first;
    if (!st.init) {
        memset(out, 0, n * 2 * sizeof(float));
        return;
    }
    /* Play the device frame "now" on the clock. Core Audio writes ahead of
     * that by its output safety offset. Our position advances with the
     * unit's own packet sizes; resync if it ever wanders off by 5 ms. */
    double now_frame = (double)st.base_frame +
                       ((double)mach_absolute_time() - st.base_host) / st.tpf;
    if (!st.out_init || fabs((double)st.out_pos - now_frame) > st.rate * 0.005) {
        if (st.out_init)
            atomic_fetch_add(&S->glitches, 1);
        st.out_pos = now_frame > 0 ? (uint64_t)now_frame : 0;
        st.out_init = 1;
    }
    for (unsigned i = 0; i < n; i++) {
        uint64_t k = ((st.out_pos + i) & (US122L_RING - 1)) * 2;
        out[2 * i] = S->out_ring[k];
        out[2 * i + 1] = S->out_ring[k + 1];
        S->out_ring[k] = S->out_ring[k + 1] = 0.f;   /* never play it twice */
    }
    st.out_pos += n;
    atomic_store_explicit(&S->out_read, st.out_pos, memory_order_release);
}

/* ---------------------------------------------------------- Mach server */

static void *serve(void *arg)
{
    mach_port_t port = *(mach_port_t *)arg;
    for (;;) {
        struct {
            us122l_request_t req;
            mach_msg_trailer_t trailer;
        } msg;
        memset(&msg, 0, sizeof msg);
        kern_return_t kr = mach_msg(&msg.req.header, MACH_RCV_MSG, 0, sizeof msg, port,
                                    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            if (kr != MACH_RCV_TOO_LARGE)
                usleep(10000);
            continue;
        }
        if (msg.req.header.msgh_size >= sizeof(us122l_request_t) &&
            msg.req.op == US122L_MSG_GET_SHM && msg.req.header.msgh_remote_port) {
            us122l_reply_t r;
            memset(&r, 0, sizeof r);
            r.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) |
                                 MACH_MSGH_BITS_COMPLEX;
            r.header.msgh_size = sizeof r;
            r.header.msgh_remote_port = msg.req.header.msgh_remote_port;
            r.header.msgh_id = msg.req.header.msgh_id + 100;
            r.body.msgh_descriptor_count = 1;
            r.memory.name = mem_entry;
            r.memory.disposition = MACH_MSG_TYPE_COPY_SEND;
            r.memory.type = MACH_MSG_PORT_DESCRIPTOR;
            r.size = shm_size;
            if (mach_msg(&r.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof r, 0,
                         MACH_PORT_NULL, 1000, MACH_PORT_NULL) != KERN_SUCCESS)
                mach_msg_destroy(&r.header);
        } else {
            mach_msg_destroy(&msg.req.header);
        }
    }
    return NULL;
}

/* --------------------------------------------------------------- device */

static int unit_present(void)
{
    static const int pids[] = {0x800e, 0x8021};
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    if (!m)
        return 0;
    int vid = 0x0644;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(props, CFSTR("idVendor"), v);
    CFDictionarySetValue(m, CFSTR(kIOPropertyMatchKey), props);
    CFRelease(props);
    CFRelease(v);
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) != KERN_SUCCESS)
        return 0;
    int found = 0;
    io_object_t o;
    while ((o = IOIteratorNext(it))) {
        CFTypeRef p = IORegistryEntryCreateCFProperty(o, CFSTR("idProduct"), NULL, 0);
        int pid = 0;
        if (p && CFGetTypeID(p) == CFNumberGetTypeID())
            CFNumberGetValue(p, kCFNumberIntType, &pid);
        if (p)
            CFRelease(p);
        for (size_t i = 0; i < sizeof pids / sizeof *pids; i++)
            found |= pid == pids[i];
        IOObjectRelease(o);
    }
    IOObjectRelease(it);
    return found;
}

static us122l_t *start_stream(unsigned rate)
{
    memset(S->in_ring, 0, sizeof S->in_ring);
    memset(S->out_ring, 0, sizeof S->out_ring);
    atomic_store(&S->in_written, 0);
    atomic_store(&S->out_read, 0);
    memset(&st, 0, sizeof st);
    st.rate = rate;
    st.nominal_tpf = ticks_per_sec / rate;
    atomic_fetch_add(&S->seed, 1);

    static const us122l_raw_callbacks cbs = {on_capture, on_playback};
    char err[256] = "";
    us122l_t *dev = us122l_open_raw(rate, &cbs, NULL, err, sizeof err);
    if (!dev) {
        logf_("can't start the US-122L at %u Hz: %s", rate, err);
        return NULL;
    }
    unsigned li, lo;
    us122l_raw_latency(dev, &li, &lo);
    atomic_store(&S->in_latency, li);
    atomic_store(&S->out_latency, lo);
    atomic_store(&S->rate, rate);
    atomic_store(&S->running, 1);
    logf_("streaming at %u Hz", rate);
    return dev;
}

static void stop_stream(us122l_t **dev, const char *why)
{
    atomic_store(&S->running, 0);
    us122l_close(*dev);
    *dev = NULL;
    logf_("stopped (%s)", why);
}

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    ticks_per_sec = 1e9 * tb.denom / tb.numer;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    /* the shared block, and a memory entry to hand it out */
    mach_vm_address_t addr = 0;
    shm_size = round_page(sizeof(us122l_shm_t));
    if (mach_vm_allocate(mach_task_self(), &addr, shm_size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        logf_("can't allocate shared memory");
        return 1;
    }
    memory_object_size_t size = shm_size;
    if (mach_make_memory_entry_64(mach_task_self(), &size, addr, VM_PROT_READ | VM_PROT_WRITE,
                                  &mem_entry, MACH_PORT_NULL) != KERN_SUCCESS) {
        logf_("can't share memory");
        return 1;
    }
    S = (us122l_shm_t *)(uintptr_t)addr;
    S->magic = US122L_SHM_MAGIC;
    S->version = US122L_SHM_VERSION;
    atomic_store(&S->want_rate, US122L_DEFAULT_RATE);
    atomic_store(&S->daemon_pid, (uint32_t)getpid());

    mach_port_t port;
    kern_return_t kr = bootstrap_check_in(bootstrap_port, US122L_SERVICE, &port);
    if (kr != KERN_SUCCESS) {
        logf_("can't check in %s with launchd (%s): run me from launchd",
              US122L_SERVICE, bootstrap_strerror(kr));
        return 1;
    }
    pthread_t server;
    pthread_create(&server, NULL, serve, &port);
    logf_("ready (driver %s)", us122l_version());

    us122l_t *dev = NULL;
    double last_client = -1e9, retry_at = 0, last_presence = -1e9;
    while (!quit) {
        double now = now_s();
        int want = atomic_load(&S->io_clients) > 0;
        if (want)
            last_client = now;
        int keep = want || now - last_client < LINGER_S;

        if (!dev) {
            if (now - last_presence >= (keep ? 0.25 : 1.0)) {
                atomic_store(&S->present, unit_present());
                last_presence = now;
            }
            if (keep && atomic_load(&S->present) && now >= retry_at) {
                dev = start_stream(atomic_load(&S->want_rate));
                if (!dev)
                    retry_at = now + RETRY_S;
            }
        } else {
            us122l_stats_t s;
            us122l_get_stats(dev, &s);
            double since = (mach_absolute_time() - atomic_load(&st.last_capture)) / ticks_per_sec;
            if (!keep) {
                stop_stream(&dev, "no more clients");
            } else if (atomic_load(&S->want_rate) != st.rate) {
                stop_stream(&dev, "sample rate change");
            } else if (!s.running || since > STALL_S) {
                stop_stream(&dev, s.running ? "no audio from the unit" : "unit gone");
                atomic_store(&S->present, unit_present());
                last_presence = now;
                retry_at = now + 0.5;
            }
        }
        usleep(20000);
    }
    if (dev)
        stop_stream(&dev, "daemon exiting");
    atomic_store(&S->present, 0);
    atomic_store(&S->daemon_pid, 0);
    return 0;
}
