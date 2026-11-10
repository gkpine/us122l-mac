/*
 * What the us122ld daemon (which owns the US-122L over USB) and the Core
 * Audio plug-in (which macOS loads into its audio server) share.
 *
 * The plug-in asks the daemon for this block of shared memory over Mach
 * (service US122L_SERVICE) and maps it. Audio and the device clock go through
 * it; nothing is sent per I/O cycle.
 *
 * Frame numbers ("device frames") count capture frames since the daemon last
 * started the stream; `seed` changes whenever that count restarts.
 */
#ifndef US122L_SHARED_H
#define US122L_SHARED_H

#include <mach/mach.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define US122L_SERVICE "org.us122l-mac.daemon"
#define US122L_SHM_MAGIC 0x55533132u   /* "US12" */
#define US122L_SHM_VERSION 1u
#define US122L_RING 32768u              /* frames per ring (power of 2) */
#define US122L_DEFAULT_RATE 48000u

typedef struct {
    uint32_t magic, version;

    /* daemon -> plug-in */
    _Atomic uint32_t present;       /* a unit is plugged in */
    _Atomic uint32_t running;       /* streaming (clock and rings valid) */
    _Atomic uint32_t rate;          /* rate of the running stream */
    _Atomic uint32_t seed;          /* changes when device frames restart */
    _Atomic uint32_t in_latency;    /* frames: capture age on arrival */
    _Atomic uint32_t out_latency;   /* frames: playback queued on the bus */
    _Atomic uint32_t clock_seq;     /* seqlock (odd while writing) for: */
    _Atomic uint64_t clock_frame;   /*   a device frame... */
    _Atomic uint64_t clock_host;    /*   ...its mach_absolute_time... */
    _Atomic uint64_t clock_tpf;     /*   ...and host ticks per frame (double bits) */
    _Atomic uint64_t in_written;    /* capture written up to (device frame) */
    _Atomic uint64_t out_read;      /* playback read up to (device frame) */
    _Atomic uint32_t glitches;      /* resyncs, for diagnostics */
    _Atomic uint32_t daemon_pid;

    /* plug-in -> daemon */
    _Atomic uint32_t want_rate;     /* rate to run at */
    _Atomic uint32_t io_clients;    /* > 0: keep streaming */

    float in_ring[US122L_RING * 2];  /* capture, indexed by device frame */
    float out_ring[US122L_RING * 2]; /* playback, indexed by device frame */
} us122l_shm_t;

/* Mach messages: the plug-in sends a request with a reply port; the daemon
 * answers with a send right to a memory entry for the shared block. */
#define US122L_MSG_GET_SHM 1

typedef struct {
    mach_msg_header_t header;
    uint32_t op;
} us122l_request_t;

typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t memory;
    uint64_t size;
} us122l_reply_t;

/* the device clock, read consistently */
static inline int us122l_read_clock(us122l_shm_t *s, uint64_t *frame, uint64_t *host,
                                    double *tpf)
{
    for (int tries = 0; tries < 1000; tries++) {
        uint32_t a = atomic_load_explicit(&s->clock_seq, memory_order_acquire);
        if (a & 1)
            continue;
        uint64_t f = atomic_load_explicit(&s->clock_frame, memory_order_relaxed);
        uint64_t h = atomic_load_explicit(&s->clock_host, memory_order_relaxed);
        uint64_t t = atomic_load_explicit(&s->clock_tpf, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->clock_seq, memory_order_relaxed) == a) {
            *frame = f;
            *host = h;
            memcpy(tpf, &t, sizeof t);
            return 1;
        }
    }
    return 0;
}

static inline void us122l_write_clock(us122l_shm_t *s, uint64_t frame, uint64_t host,
                                      double tpf)
{
    uint64_t t;
    memcpy(&t, &tpf, sizeof t);
    uint32_t q = atomic_load_explicit(&s->clock_seq, memory_order_relaxed);
    atomic_store_explicit(&s->clock_seq, q + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->clock_frame, frame, memory_order_relaxed);
    atomic_store_explicit(&s->clock_host, host, memory_order_relaxed);
    atomic_store_explicit(&s->clock_tpf, t, memory_order_relaxed);
    atomic_store_explicit(&s->clock_seq, q + 2, memory_order_release);
}

#endif
