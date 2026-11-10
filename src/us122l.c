/*
 * us122l-mac: userspace driver for the TASCAM US-122L (see us122l.h).
 *
 * How the unit works (from the Linux snd-usb-us122l / usb_stream driver by
 * Karsten Wiese, GPL-2.0-or-later):
 *   - USB 2.0 high speed, vendor-specific. Interface 1, alt setting 1 carries
 *     audio: iso IN endpoint 0x81 (capture) and iso OUT endpoint 0x02
 *     (playback), one packet per 125 us microframe.
 *   - Samples are 24-bit little-endian, 2 channels: 6 bytes per frame.
 *   - Two vendor requests ('I' with 0x11 then 0x10) wake it up; the sample
 *     rate is a UAC1 SET_CUR on endpoint 0x81.
 *   - The unit's clock is the master. It decides how many frames go in each
 *     capture packet, and the host must send playback packets of the same
 *     sizes (implicit feedback), otherwise playback drifts.
 *
 * Threads:
 *   - event thread (real-time): runs libusb; decodes capture packets into the
 *     capture ring, remembers their sizes, and builds playback packets of the
 *     same sizes from the playback ring.
 *   - audio thread (real-time): waits for a block of capture, calls the user
 *     callback, and puts the result in the playback ring.
 * The rings are single-producer/single-consumer and lock-free.
 */
#include "us122l.h"

#include <libusb.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define VID 0x0644
/* US-122L, and the US-122MKII, which the Linux driver handles the same way */
static const uint16_t PIDS[] = {0x800e, 0x8021};
#define IFACE 1
#define ALT 1
#define EP_IN 0x81
#define EP_OUT 0x02
#define FRAME 6                 /* bytes per frame: 2 ch x 24 bit */
#define MAXPKT 78               /* wMaxPacketSize of both iso endpoints */
#define MAX_FRAMES_PKT (MAXPKT / FRAME)
#define NPKT_MAX 64
#define NXFER_MAX 32
/* Packets per transfer (8 microframes = 1 ms) and transfers in flight.
 * Measured on macOS 26 / M1: 1 ms transfers lose ~40% of the audio (macOS
 * can't keep them scheduled back to back); 2 ms transfers with 3 queued run
 * clean, 2 queued starve. Playback latency includes the queued transfers.
 * US122L_PKTS / US122L_XFERS override these for experiments. */
#define DEF_NPKT 16
#define DEF_NIN 4
#define DEF_NOUT 3
#define STALL_NS 500000000ull   /* no capture for this long: the unit stopped */
#define RING 16384              /* frames per ring (power of 2) */
#define FB_CAP 1024             /* packet-size fifo (power of 2) */
#define FB_MAX 64               /* keep at most this many sizes queued */

/* ---------------------------------------------------------------- rings */

struct ring {
    float *buf;                 /* RING stereo frames */
    _Atomic uint32_t w, r;      /* free-running frame counters */
};

static int ring_init(struct ring *g)
{
    g->buf = calloc((size_t)RING * 2, sizeof(float));
    atomic_init(&g->w, 0);
    atomic_init(&g->r, 0);
    return g->buf ? 0 : -1;
}

static uint32_t ring_avail(struct ring *g)
{
    return atomic_load_explicit(&g->w, memory_order_acquire) -
           atomic_load_explicit(&g->r, memory_order_acquire);
}

static uint32_t ring_space(struct ring *g) { return RING - ring_avail(g); }

/* src NULL writes silence. Caller checks space. */
static void ring_write(struct ring *g, const float *src, uint32_t n)
{
    uint32_t w = atomic_load_explicit(&g->w, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = ((w + i) & (RING - 1)) * 2;
        g->buf[k] = src ? src[2 * i] : 0.f;
        g->buf[k + 1] = src ? src[2 * i + 1] : 0.f;
    }
    atomic_store_explicit(&g->w, w + n, memory_order_release);
}

static void ring_read(struct ring *g, float *dst, uint32_t n)
{
    uint32_t r = atomic_load_explicit(&g->r, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = ((r + i) & (RING - 1)) * 2;
        dst[2 * i] = g->buf[k];
        dst[2 * i + 1] = g->buf[k + 1];
    }
    atomic_store_explicit(&g->r, r + n, memory_order_release);
}

static void ring_skip(struct ring *g, uint32_t n)
{
    atomic_fetch_add_explicit(&g->r, n, memory_order_acq_rel);
}

/* 24-bit little-endian <-> float */
static inline float s24_to_float(const uint8_t *p)
{
    int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) |
                          ((uint32_t)p[2] << 24)) >> 8;
    return (float)v * (1.f / 8388608.f);
}

static inline void float_to_s24(float x, uint8_t *p)
{
    float s = x * 8388608.f;
    int32_t v;
    if (!(s > -8388608.f))      /* also catches NaN */
        v = s != s ? 0 : -8388608;
    else if (s >= 8388607.f)
        v = 8388607;
    else
        v = (int32_t)lrintf(s);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
}

static void ring_write_s24(struct ring *g, const uint8_t *src, uint32_t n)
{
    uint32_t w = atomic_load_explicit(&g->w, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++, src += FRAME) {
        uint32_t k = ((w + i) & (RING - 1)) * 2;
        g->buf[k] = s24_to_float(src);
        g->buf[k + 1] = s24_to_float(src + 3);
    }
    atomic_store_explicit(&g->w, w + n, memory_order_release);
}

static void ring_read_s24(struct ring *g, uint8_t *dst, uint32_t n)
{
    uint32_t r = atomic_load_explicit(&g->r, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++, dst += FRAME) {
        uint32_t k = ((r + i) & (RING - 1)) * 2;
        float_to_s24(g->buf[k], dst);
        float_to_s24(g->buf[k + 1], dst + 3);
    }
    atomic_store_explicit(&g->r, r + n, memory_order_release);
}

/* --------------------------------------------------------------- device */

struct us122l {
    libusb_context *ctx;
    libusb_device_handle *h;
    int claimed;
    unsigned rate, block;
    us122l_callback cb;
    void *user;

    int npkt, nin, nout;
    struct libusb_transfer *in[NXFER_MAX], *out[NXFER_MAX];
    _Atomic int active;         /* transfers in flight */
    _Atomic int stopping, dead, stalled, ev_stop;
    int leak;                   /* close timed out: transfers may still be in flight */

    pthread_t ev_thread, proc_thread;
    int ev_started, proc_started;
    pthread_mutex_t mx;
    pthread_cond_t cv;

    struct ring cap, play;
    float *inbuf, *outbuf;

    /* event thread only */
    uint8_t fb[FB_CAP];
    uint32_t fb_w, fb_r;
    uint32_t phase, in_phase, freqn;  /* nominal frames per microframe, Q16.16 */

    _Atomic int primed;         /* audio thread has produced output */
    _Atomic uint32_t cushion;   /* extra frames of output buffering */
    uint32_t base_cushion;
    _Atomic uint32_t in_overflows, out_underflows, usb_errors;
    _Atomic uint64_t frames_in, frames_out;
    _Atomic uint64_t last_cb_ns, last_data_ns;
};

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static int debug_on = -1;
#define DBG(...) do { if (debug_on < 0) debug_on = getenv("US122L_DEBUG") != NULL; \
                      if (debug_on) fprintf(stderr, "us122l: " __VA_ARGS__); } while (0)

static void seterr(char *err, size_t len, const char *fmt, const char *detail)
{
    if (err && len)
        snprintf(err, len, fmt, detail ? detail : "");
}

/* Make a thread real time, the way Core Audio's I/O threads are. */
static void set_thread_realtime(thread_act_t thread, double period_s, double busy)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double abs_per_ns = (double)tb.denom / tb.numer;
    thread_time_constraint_policy_data_t p;
    p.period = (uint32_t)(period_s * 1e9 * abs_per_ns);
    p.computation = (uint32_t)(period_s * busy * 1e9 * abs_per_ns);
    p.constraint = p.period;
    p.preemptible = 1;
    kern_return_t kr = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
                                         THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS)
        DBG("real-time priority refused (%d)\n", kr);
}

static void set_realtime(double period_s, double busy)
{
    set_thread_realtime(pthread_mach_thread_np(pthread_self()), period_s, busy);
}

/* The threads of this process (caller frees with free_threads). */
static thread_act_array_t list_threads(mach_msg_type_number_t *n)
{
    thread_act_array_t list = NULL;
    if (task_threads(mach_task_self(), &list, n) != KERN_SUCCESS) {
        *n = 0;
        return NULL;
    }
    return list;
}

static void free_threads(thread_act_array_t list, mach_msg_type_number_t n)
{
    for (mach_msg_type_number_t i = 0; i < n; i++)
        mach_port_deallocate(mach_task_self(), list[i]);
    if (list)
        vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(*list));
}

/* Give libusb's macOS event thread real-time priority, found by its name. */
static void promote_libusb_thread(void)
{
    mach_msg_type_number_t n;
    thread_act_array_t list = list_threads(&n);
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        pthread_t pt = pthread_from_mach_thread_np(list[i]);
        char name[64] = "";
        if (pt && pthread_getname_np(pt, name, sizeof name) == 0 &&
            strcmp(name, "org.libusb.device-hotplug") == 0) {
            set_thread_realtime(list[i], 0.002, 0.2);
            DBG("libusb thread made real time\n");
        }
    }
    free_threads(list, n);
}

static uint32_t target_level(struct us122l *d)
{
    return d->block + atomic_load(&d->cushion);
}

/* Build a playback transfer: packet sizes copy the capture packets'. */
static void fill_out(struct us122l *d, struct libusb_transfer *t)
{
    uint8_t *p = t->buffer;
    int primed = atomic_load(&d->primed);
    int underflow = 0;

    while (d->fb_w - d->fb_r > FB_MAX)
        d->fb_r++;
    if (primed) {
        /* After a stall the ring can hold more than it should: drop the
         * excess so latency doesn't creep up. */
        uint32_t avail = ring_avail(&d->play), hi = target_level(d) + d->block + d->rate / 1000;
        if (avail > hi)
            ring_skip(&d->play, avail - target_level(d));
    }
    for (int i = 0; i < d->npkt; i++) {
        unsigned n;
        if (d->fb_w != d->fb_r) {
            n = d->fb[d->fb_r++ & (FB_CAP - 1)];
        } else {
            d->phase += d->freqn;
            n = d->phase >> 16;
            d->phase &= 0xffff;
        }
        if (n > MAX_FRAMES_PKT)
            n = MAX_FRAMES_PKT;
        if (primed) {
            uint32_t a = ring_avail(&d->play), k = a < n ? a : n;
            ring_read_s24(&d->play, p, k);
            if (k < n) {
                memset(p + k * FRAME, 0, (n - k) * FRAME);
                if (!underflow)
                    DBG("underflow: pkt %d wanted %u had %u, cap %u, %.2f ms since callback, %.2f ms since capture\n",
                        i, n, a, ring_avail(&d->cap), (now_ns() - atomic_load(&d->last_cb_ns)) / 1e6,
                        (now_ns() - atomic_load(&d->last_data_ns)) / 1e6);
                underflow = 1;
            }
            atomic_fetch_add(&d->frames_out, n);
        } else {
            memset(p, 0, n * FRAME);
        }
        t->iso_packet_desc[i].length = n * FRAME;
        p += n * FRAME;
    }
    t->length = (int)(p - t->buffer);
    if (underflow)
        atomic_fetch_add(&d->out_underflows, 1);
}

static void wake(struct us122l *d)
{
    pthread_mutex_lock(&d->mx);
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mx);
}

static void resubmit(struct us122l *d, struct libusb_transfer *t)
{
    if (atomic_load(&d->stopping) || atomic_load(&d->dead)) {
        atomic_fetch_sub(&d->active, 1);
        return;
    }
    if (t->endpoint == EP_OUT)
        fill_out(d, t);
    int r = libusb_submit_transfer(t);
    if (r != 0) {
        DBG("resubmit ep 0x%02x failed: %s (len %d, packets:", t->endpoint, libusb_strerror(r), t->length);
        for (int i = 0; i < t->num_iso_packets; i++)
            DBG(" %u", t->iso_packet_desc[i].length);
        DBG(")\n");
        atomic_store(&d->dead, 1);
        atomic_fetch_sub(&d->active, 1);
        wake(d);
    }
}

static void check_status(struct us122l *d, struct libusb_transfer *t)
{
    if (t->status != LIBUSB_TRANSFER_COMPLETED)
        DBG("ep 0x%02x transfer status %d\n", t->endpoint, t->status);
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        atomic_store(&d->dead, 1);
        wake(d);
    } else if (t->status != LIBUSB_TRANSFER_COMPLETED &&
               t->status != LIBUSB_TRANSFER_CANCELLED) {
        atomic_fetch_add(&d->usb_errors, 1);
    }
}

static void LIBUSB_CALL in_done(struct libusb_transfer *t)
{
    struct us122l *d = t->user_data;
    check_status(d, t);
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        int got = 0;
        for (int i = 0; i < t->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *pd = &t->iso_packet_desc[i];
            unsigned n = 0;
            if (pd->status == LIBUSB_TRANSFER_COMPLETED) {
                n = pd->actual_length / FRAME;
            } else {
                atomic_fetch_add(&d->usb_errors, 1);
                /* keep playback moving at the right rate */
                d->in_phase += d->freqn;
                n = d->in_phase >> 16;
                d->in_phase &= 0xffff;
            }
            if (pd->status == LIBUSB_TRANSFER_COMPLETED && n) {
                const uint8_t *src = libusb_get_iso_packet_buffer_simple(t, (unsigned)i);
                if (ring_space(&d->cap) >= n)
                    ring_write_s24(&d->cap, src, n);
                else
                    atomic_fetch_add(&d->in_overflows, 1);
                atomic_fetch_add(&d->frames_in, n);
                got = 1;
            }
            /* empty packets come while the unit syncs up; playback runs
             * at the nominal rate until real ones arrive */
            if (n && d->fb_w - d->fb_r < FB_CAP)
                d->fb[d->fb_w++ & (FB_CAP - 1)] = (uint8_t)n;
        }
        if (got)
            atomic_store(&d->last_data_ns, now_ns());
        if (ring_avail(&d->cap) >= d->block)
            wake(d);
    }
    resubmit(d, t);
}

static void LIBUSB_CALL out_done(struct libusb_transfer *t)
{
    struct us122l *d = t->user_data;
    check_status(d, t);
    resubmit(d, t);
}

static void *event_main(void *arg)
{
    struct us122l *d = arg;
    set_realtime(0.001, 0.25);
    while (!atomic_load(&d->ev_stop)) {
        struct timeval tv = {0, 50000};
        libusb_handle_events_timeout_completed(d->ctx, &tv, NULL);
    }
    return NULL;
}

static void *proc_main(void *arg)
{
    struct us122l *d = arg;
    uint32_t seen_ov = 0, seen_ur = 0;
    uint64_t calm_since = now_ns();     /* last underflow (or start) */
    set_realtime((double)d->block / d->rate, 0.5);

    for (;;) {
        pthread_mutex_lock(&d->mx);
        while (!atomic_load(&d->stopping) && !atomic_load(&d->dead) &&
               ring_avail(&d->cap) < d->block) {
            struct timeval now;
            struct timespec ts;
            gettimeofday(&now, NULL);
            ts.tv_sec = now.tv_sec;
            ts.tv_nsec = now.tv_usec * 1000L + 100000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&d->cv, &d->mx, &ts);
            /* Watchdog: the unit stopped sending audio (Mac went to sleep,
             * unplugged, crashed) even though USB still looks alive. */
            uint64_t last = atomic_load(&d->last_data_ns);
            if (last && now_ns() - last > STALL_NS) {
                DBG("no capture for %.0f ms: stalled\n", (now_ns() - last) / 1e6);
                atomic_store(&d->stalled, 1);
                atomic_store(&d->dead, 1);
            }
        }
        pthread_mutex_unlock(&d->mx);
        if (atomic_load(&d->stopping) || atomic_load(&d->dead))
            break;

        unsigned status = 0;
        uint32_t ov = atomic_load(&d->in_overflows), ur = atomic_load(&d->out_underflows);
        if (ov != seen_ov)
            status |= US122L_INPUT_OVERFLOW;
        if (ur != seen_ur) {
            status |= US122L_OUTPUT_UNDERFLOW;
            /* the unit ran dry: buffer 1 ms more from now on (up to 10 ms more) */
            uint32_t c = atomic_load(&d->cushion);
            if (atomic_load(&d->primed) && c < d->base_cushion + d->rate / 100) {
                uint32_t add = d->rate / 1000;
                atomic_store(&d->cushion, c + add);
                if (ring_space(&d->play) >= add)
                    ring_write(&d->play, NULL, add);
            }
            calm_since = now_ns();
        }
        seen_ov = ov;
        seen_ur = ur;

        /* 10 s without underflows: give 1 ms of the extra cushion back */
        uint32_t drop = 0;
        if (now_ns() - calm_since > 10000000000ull) {
            uint32_t c = atomic_load(&d->cushion), sub = d->rate / 1000;
            if (c >= d->base_cushion + sub) {
                atomic_store(&d->cushion, c - sub);
                drop = sub < d->block ? sub : d->block;
            }
            calm_since = now_ns();
        }

        /* After a long stall, catch up to the newest input instead of
         * working through a backlog of old audio. */
        uint32_t backlog = ring_avail(&d->cap);
        if (backlog > 2 * d->block + d->rate / 50) {
            ring_skip(&d->cap, backlog - d->block);
            status |= US122L_INPUT_OVERFLOW;
        }

        ring_read(&d->cap, d->inbuf, d->block);
        d->cb(d->inbuf, d->outbuf, d->block, status, d->user);
        atomic_store(&d->last_cb_ns, now_ns());
        if (!atomic_load(&d->primed)) {
            /* The first block: put silence ahead of it so that what's
             * buffered (playback + capture left over) is block + cushion. */
            uint32_t left = ring_avail(&d->cap), c = atomic_load(&d->cushion);
            ring_write(&d->play, NULL, c > left ? c - left : 0);
        }
        /* (drop: shorten the buffered output by skipping its first frames) */
        if (ring_space(&d->play) >= d->block - drop)
            ring_write(&d->play, d->outbuf + 2 * drop, d->block - drop);
        atomic_store(&d->primed, 1);
    }
    return NULL;
}

/* ------------------------------------------------------------------ API */

const char *us122l_version(void) { return US122L_VERSION; }

int us122l_present(void)
{
    libusb_context *ctx;
    libusb_device **list;
    int found = 0;
    if (libusb_init(&ctx) != 0)
        return 0;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) == 0 && dd.idVendor == VID)
            for (size_t k = 0; k < sizeof(PIDS) / sizeof(*PIDS); k++)
                found |= dd.idProduct == PIDS[k];
    }
    if (n >= 0)
        libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return found;
}

static void release(struct us122l *d)
{
    if (d->leak) {
        /* Transfers may still complete into these: leave everything allocated
         * rather than risk a use-after-free (only after a close timeout). */
        DBG("close timed out with %d transfers in flight; leaking the device\n",
            atomic_load(&d->active));
        return;
    }
    for (int i = 0; i < d->nin; i++)
        if (d->in[i]) {
            free(d->in[i]->buffer);
            libusb_free_transfer(d->in[i]);
        }
    for (int i = 0; i < d->nout; i++)
        if (d->out[i]) {
            free(d->out[i]->buffer);
            libusb_free_transfer(d->out[i]);
        }
    if (d->h) {
        if (d->claimed) {
            libusb_set_interface_alt_setting(d->h, IFACE, 0);
            libusb_release_interface(d->h, IFACE);
        }
        libusb_close(d->h);
    }
    if (d->ctx)
        libusb_exit(d->ctx);
    pthread_mutex_destroy(&d->mx);
    pthread_cond_destroy(&d->cv);
    free(d->cap.buf);
    free(d->play.buf);
    free(d->inbuf);
    free(d->outbuf);
    free(d);
}

static void *close_later(void *d)
{
    us122l_close(d);
    return NULL;
}

void us122l_close(us122l_t *d)
{
    if (!d)
        return;
    if (d->proc_started && pthread_equal(pthread_self(), d->proc_thread)) {
        /* Called from inside the callback: the audio thread can't join
         * itself. Stop now, and finish closing on another thread once the
         * callback has returned. */
        atomic_store(&d->stopping, 1);
        pthread_t t;
        if (pthread_create(&t, NULL, close_later, d) == 0)
            pthread_detach(t);
        return;
    }
    atomic_store(&d->stopping, 1);
    for (int i = 0; i < d->nin; i++)
        if (d->in[i])
            libusb_cancel_transfer(d->in[i]);
    for (int i = 0; i < d->nout; i++)
        if (d->out[i])
            libusb_cancel_transfer(d->out[i]);
    for (int ms = 0; atomic_load(&d->active) > 0 && ms < 2000; ms++) {
        if (!d->ev_started)
            libusb_handle_events_timeout_completed(d->ctx, &(struct timeval){0, 1000}, NULL);
        else
            usleep(1000);
    }
    if (atomic_load(&d->active) > 0)
        d->leak = 1;
    wake(d);
    if (d->proc_started)
        pthread_join(d->proc_thread, NULL);
    atomic_store(&d->ev_stop, 1);
    if (d->ev_started) {
        libusb_interrupt_event_handler(d->ctx);
        pthread_join(d->ev_thread, NULL);
    }
    release(d);
}

us122l_t *us122l_open(unsigned rate, unsigned block, us122l_callback cb, void *user,
                      char *err, size_t errlen)
{
    if (rate != 44100 && rate != 48000 && rate != 88200 && rate != 96000) {
        seterr(err, errlen, "unsupported sample rate (use 44100, 48000, 88200 or 96000)%s", NULL);
        return NULL;
    }
    if (block < 16 || block > 4096) {
        seterr(err, errlen, "block size must be 16 to 4096 frames%s", NULL);
        return NULL;
    }
    if (!cb) {
        seterr(err, errlen, "no callback given%s", NULL);
        return NULL;
    }
    struct us122l *d = calloc(1, sizeof(*d));
    if (!d) {
        seterr(err, errlen, "out of memory%s", NULL);
        return NULL;
    }
    d->npkt = getenv("US122L_PKTS") ? atoi(getenv("US122L_PKTS")) & ~7 : DEF_NPKT;
    if (d->npkt < 8 || d->npkt > NPKT_MAX)
        d->npkt = DEF_NPKT;
    d->nin = DEF_NIN;
    d->nout = DEF_NOUT;
    if (getenv("US122L_XFERS")) {
        int x = atoi(getenv("US122L_XFERS"));
        if (x >= 2 && x <= NXFER_MAX)
            d->nin = d->nout = x;
    }
    pthread_mutex_init(&d->mx, NULL);
    pthread_cond_init(&d->cv, NULL);
    d->rate = rate;
    d->block = block;
    d->cb = cb;
    d->user = user;
    d->freqn = ((rate << 10) + 62) / 125;    /* frames per microframe, Q16.16 */
    /* Playback buffering beyond one block. A capture transfer can land and
     * the next playback transfer get built before the audio thread has run,
     * so cover two transfers, plus 1 ms for scheduling jitter. */
    d->base_cushion = 2 * (rate * d->npkt / 8000) + rate / 1000;
    atomic_init(&d->cushion, d->base_cushion);
    d->inbuf = calloc((size_t)block * 2, sizeof(float));
    d->outbuf = calloc((size_t)block * 2, sizeof(float));
    if (ring_init(&d->cap) || ring_init(&d->play) || !d->inbuf || !d->outbuf) {
        seterr(err, errlen, "out of memory%s", NULL);
        release(d);
        return NULL;
    }

    /* libusb's macOS backend delivers completions from its own thread
     * (created by the first libusb_init in the process). Threads inherit
     * their creator's QoS, so make that the highest for the duration... */
    qos_class_t old_qos = qos_class_self();
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    int r = libusb_init(&d->ctx);
    pthread_set_qos_class_self_np(old_qos, 0);
    /* ...and make it real time, so a busy machine can't delay completions. */
    promote_libusb_thread();
    if (r != 0) {
        d->ctx = NULL;
        seterr(err, errlen, "libusb init failed: %s", libusb_strerror(r));
        release(d);
        return NULL;
    }
    for (size_t k = 0; k < sizeof(PIDS) / sizeof(*PIDS) && !d->h; k++)
        d->h = libusb_open_device_with_vid_pid(d->ctx, VID, PIDS[k]);
    if (!d->h) {
        seterr(err, errlen, "TASCAM US-122L not found (is it plugged in?)%s", NULL);
        release(d);
        return NULL;
    }
    if (libusb_get_device_speed(libusb_get_device(d->h)) < LIBUSB_SPEED_HIGH) {
        /* Full speed (1 ms frames, 44.1/48 kHz only) isn't supported. */
        seterr(err, errlen, "the US-122L is connected at USB 1.1 speed (through a hub or "
               "adapter?): plug it straight into the Mac%s", NULL);
        release(d);
        return NULL;
    }
    r = libusb_claim_interface(d->h, IFACE);
    if (r != 0) {
        seterr(err, errlen, "can't claim the US-122L (another app using it?): %s",
               libusb_strerror(r));
        release(d);
        return NULL;
    }
    d->claimed = 1;
    r = libusb_set_interface_alt_setting(d->h, IFACE, ALT);
    if (r != 0) {
        seterr(err, errlen, "can't select the US-122L's audio setting: %s", libusb_strerror(r));
        release(d);
        return NULL;
    }
    /* wake-up requests, as the Linux driver sends them */
    libusb_control_transfer(d->h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
                            LIBUSB_ENDPOINT_OUT, 'I', 0x11, 0, NULL, 0, 1000);
    libusb_control_transfer(d->h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
                            LIBUSB_ENDPOINT_OUT, 'I', 0x10, 0, NULL, 0, 1000);
    /* UAC1 SET_CUR sampling frequency on the capture endpoint */
    uint8_t f[3] = {(uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16)};
    r = libusb_control_transfer(d->h, LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT |
                                LIBUSB_ENDPOINT_OUT, 0x01, 0x0100, EP_IN, f, 3, 1000);
    if (r < 0) {
        seterr(err, errlen, "can't set the US-122L's sample rate: %s", libusb_strerror(r));
        release(d);
        return NULL;
    }

    for (int i = 0; i < d->nin; i++) {
        struct libusb_transfer *t = libusb_alloc_transfer(d->npkt);
        uint8_t *buf = calloc(d->npkt, MAXPKT);
        if (!t || !buf) {
            free(buf);
            libusb_free_transfer(t);
            seterr(err, errlen, "out of memory%s", NULL);
            release(d);
            return NULL;
        }
        libusb_fill_iso_transfer(t, d->h, EP_IN, buf, d->npkt * MAXPKT, d->npkt, in_done, d, 0);
        libusb_set_iso_packet_lengths(t, MAXPKT);
        d->in[i] = t;
    }
    for (int i = 0; i < d->nout; i++) {
        struct libusb_transfer *t = libusb_alloc_transfer(d->npkt);
        uint8_t *buf = calloc(d->npkt, MAXPKT);
        if (!t || !buf) {
            free(buf);
            libusb_free_transfer(t);
            seterr(err, errlen, "out of memory%s", NULL);
            release(d);
            return NULL;
        }
        libusb_fill_iso_transfer(t, d->h, EP_OUT, buf, 0, d->npkt, out_done, d, 0);
        fill_out(d, t);
        d->out[i] = t;
    }

    if (pthread_create(&d->ev_thread, NULL, event_main, d) == 0)
        d->ev_started = 1;
    for (int i = 0; i < d->nin || i < d->nout; i++) {
        if (i < d->nin) {
            atomic_fetch_add(&d->active, 1);
            if ((r = libusb_submit_transfer(d->in[i])) != 0) {
                atomic_fetch_sub(&d->active, 1);
                break;
            }
        }
        if (i < d->nout) {
            atomic_fetch_add(&d->active, 1);
            if ((r = libusb_submit_transfer(d->out[i])) != 0) {
                atomic_fetch_sub(&d->active, 1);
                break;
            }
        }
    }
    if (r != 0) {
        seterr(err, errlen, "can't start streaming from the US-122L: %s", libusb_strerror(r));
        us122l_close(d);
        return NULL;
    }
    if (pthread_create(&d->proc_thread, NULL, proc_main, d) == 0)
        d->proc_started = 1;

    /* the unit should start sending audio within a few ms */
    for (int ms = 0; ms < 1000 && atomic_load(&d->frames_in) == 0 && !atomic_load(&d->dead); ms += 5)
        usleep(5000);
    if (atomic_load(&d->frames_in) == 0) {
        seterr(err, errlen, "the US-122L isn't sending audio (try unplugging it and plugging it back in)%s", NULL);
        us122l_close(d);
        return NULL;
    }
    return d;
}

void us122l_get_stats(us122l_t *d, us122l_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    if (!d)
        return;
    s->rate = d->rate;
    s->blocksize = d->block;
    /* capture: one transfer before it reaches us, plus a block */
    s->input_latency = d->rate * d->npkt / 8000 + d->block;
    /* playback: what's buffered plus the transfers queued on the bus */
    s->output_latency = target_level(d) + d->nout * d->rate * d->npkt / 8000;
    s->frames_in = atomic_load(&d->frames_in);
    s->frames_out = atomic_load(&d->frames_out);
    s->input_overflows = atomic_load(&d->in_overflows);
    s->output_underflows = atomic_load(&d->out_underflows);
    s->usb_errors = atomic_load(&d->usb_errors);
    s->running = !atomic_load(&d->dead) && !atomic_load(&d->stopping);
    s->stalled = atomic_load(&d->stalled);
}
