/* Stream from the US-122L for a few seconds and print what's happening.
 *   us122l-test [seconds] [rate] [blocksize] [silence|tone|thru]
 * tone: 440 Hz at -20 dBFS on both outputs. thru: inputs to outputs. */
#include "us122l.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int mode;            /* 0 silence, 1 tone, 2 thru */
static double phase, step;
static float peak[2];
static double sq[2];
static unsigned long nsq, calls, flagged;

static void cb(const float *in, float *out, unsigned n, unsigned status, void *u)
{
    (void)u;
    calls++;
    if (status) flagged++;
    for (unsigned i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) {
            float x = in[2 * i + c];
            if (fabsf(x) > peak[c]) peak[c] = fabsf(x);
            sq[c] += (double)x * x;
        }
    nsq += n;
    for (unsigned i = 0; i < n; i++) {
        float y = 0;
        if (mode == 1) { y = 0.1f * (float)sin(phase); phase += step; }
        out[2 * i] = mode == 2 ? in[2 * i] : y;
        out[2 * i + 1] = mode == 2 ? in[2 * i + 1] : y;
    }
}

static double db(double x) { return x > 0 ? 20 * log10(x) : -999; }

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    unsigned rate = argc > 2 ? (unsigned)atoi(argv[2]) : 48000;
    unsigned block = argc > 3 ? (unsigned)atoi(argv[3]) : 256;
    if (argc > 4) mode = !strcmp(argv[4], "tone") ? 1 : !strcmp(argv[4], "thru") ? 2 : 0;
    step = 2 * M_PI * 440 / rate;
    printf("us122l %s, present=%d\n", us122l_version(), us122l_present());
    char err[256];
    us122l_t *d = us122l_open(rate, block, cb, NULL, err, sizeof err);
    if (!d) { fprintf(stderr, "open failed: %s\n", err); return 1; }
    us122l_stats_t s0, s;
    us122l_get_stats(d, &s0);
    for (int t = 1; t <= secs; t++) {
        sleep(1);
        us122l_get_stats(d, &s);
        double rms0 = nsq ? sqrt(sq[0] / nsq) : 0, rms1 = nsq ? sqrt(sq[1] / nsq) : 0;
        printf("%2ds in %6llu fr/s out %6llu fr/s | L peak %6.1f rms %6.1f dB  R peak %6.1f rms %6.1f dB"
               " | cb %lu ovf %u unf %u usberr %u lat %u+%u%s\n",
               t, s.frames_in - s0.frames_in, s.frames_out - s0.frames_out,
               db(peak[0]), db(rms0), db(peak[1]), db(rms1), calls, s.input_overflows,
               s.output_underflows, s.usb_errors, s.input_latency, s.output_latency,
               s.running ? "" : " DEAD");
        s0 = s; peak[0] = peak[1] = 0; sq[0] = sq[1] = 0; nsq = 0; calls = 0;
        if (!s.running) break;
    }
    us122l_close(d);
    printf("closed (flagged callbacks: %lu)\n", flagged);
    return 0;
}
