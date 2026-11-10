/*
 * us122l-mac: a userspace driver for the TASCAM US-122L USB audio interface
 * on macOS (Apple Silicon and Intel), built on libusb.
 *
 * TASCAM never shipped a driver for the US-122L on current macOS, and the
 * unit is not USB-audio class compliant, so Core Audio never sees it. This
 * library talks to it directly over USB (the protocol follows the Linux
 * snd-usb-us122l driver) and hands audio to a callback, PortAudio-style.
 *
 * Audio is always 2 channels in and 2 out, float32, interleaved (L R L R…).
 *
 * Copyright (C) 2026. Licensed GPL-2.0-or-later (see LICENSE).
 */
#ifndef US122L_H
#define US122L_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define US122L_VERSION "0.2.0"

typedef struct us122l us122l_t;

/* Status bits passed to the callback (something went wrong since the last
 * callback). */
enum {
    US122L_INPUT_OVERFLOW = 1,   /* captured audio was dropped (callback too slow) */
    US122L_OUTPUT_UNDERFLOW = 2, /* the unit played silence (callback too slow) */
};

/* Called from the driver's real-time audio thread with `frames` frames of
 * input; fill `out` with `frames` frames of output. Both are interleaved
 * stereo float32 in [-1, 1]. */
typedef void (*us122l_callback)(const float *in, float *out, unsigned frames,
                                unsigned status, void *user);

typedef struct {
    unsigned rate;            /* sample rate in Hz */
    unsigned blocksize;       /* frames per callback */
    unsigned input_latency;   /* estimated, in frames */
    unsigned output_latency;  /* estimated, in frames (grows if underruns happen) */
    uint64_t frames_in;       /* frames received from the unit */
    uint64_t frames_out;      /* frames sent to the unit */
    unsigned input_overflows;
    unsigned output_underflows;
    unsigned usb_errors;      /* iso packets that failed */
    int running;              /* 0 once the unit was unplugged or failed */
    int stalled;              /* 1 if it stopped because no audio came for 0.5 s
                                 (e.g. the Mac slept): close and reopen */
} us122l_stats_t;

/* 1 if a US-122L is plugged in, else 0. */
int us122l_present(void);

/* Open the unit and start streaming. rate: 44100, 48000, 88200 or 96000.
 * blocksize: frames per callback (16 to 4096). On failure returns NULL and
 * writes a message to err (if not NULL). */
us122l_t *us122l_open(unsigned rate, unsigned blocksize, us122l_callback cb,
                      void *user, char *err, size_t errlen);

/* Stop streaming and release the unit. Safe to call from inside the
 * callback (the rest of the close then happens on another thread). Don't
 * call it at the same time as us122l_get_stats on another thread. */
void us122l_close(us122l_t *dev);

void us122l_get_stats(us122l_t *dev, us122l_stats_t *stats);

const char *us122l_version(void);

#ifdef __cplusplus
}
#endif
#endif
