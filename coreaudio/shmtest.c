/* Acts like the Core Audio plug-in: connects to us122ld, asks it to stream,
 * and checks the clock and capture data the way Core Audio would use them.
 *   us122l-shmtest [seconds] [rate] */
#include "client.h"
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    unsigned rate = argc > 2 ? (unsigned)atoi(argv[2]) : 48000;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double tps = 1e9 * tb.denom / tb.numer;

    us122l_shm_t *s = us122l_connect();
    if (!s) { fprintf(stderr, "can't reach the daemon\n"); return 1; }
    printf("connected: present %u, daemon pid %u\n", atomic_load(&s->present), atomic_load(&s->daemon_pid));
    atomic_store(&s->want_rate, rate);
    atomic_fetch_add(&s->io_clients, 1);
    double t0 = mach_absolute_time() / tps;
    while (!(atomic_load(&s->running) && atomic_load(&s->rate) == rate) && mach_absolute_time() / tps - t0 < 5)
        usleep(5000);
    if (!atomic_load(&s->running)) { fprintf(stderr, "daemon didn't start streaming\n"); return 1; }
    printf("running at %u Hz after %.0f ms, seed %u, latency in %u out %u frames\n", atomic_load(&s->rate),
           1000 * (mach_absolute_time() / tps - t0), atomic_load(&s->seed), atomic_load(&s->in_latency),
           atomic_load(&s->out_latency));

    uint64_t prev_w = atomic_load(&s->in_written);
    for (int t = 0; t < secs; t++) {
        double lo = 1e9, hi = -1e9, peak[2] = {0, 0};
        uint64_t last_read = atomic_load(&s->in_written);
        for (int k = 0; k < 1000; k++) {          /* sample the clock every 1 ms */
            uint64_t f, h; double tpf;
            us122l_read_clock(s, &f, &h, &tpf);
            double now_frame = f + ((double)mach_absolute_time() - h) / tpf;
            double lead = (double)atomic_load(&s->in_written) - now_frame;   /* data ahead of clock */
            if (lead < lo) lo = lead;
            if (lead > hi) hi = lead;
            uint64_t w = atomic_load(&s->in_written);
            for (uint64_t i = last_read; i < w; i++) {
                uint64_t j = (i & (US122L_RING - 1)) * 2;
                for (int c = 0; c < 2; c++) if (fabsf(s->in_ring[j + c]) > peak[c]) peak[c] = fabsf(s->in_ring[j + c]);
            }
            last_read = w;
            usleep(1000);
        }
        uint64_t f, h; double tpf;
        us122l_read_clock(s, &f, &h, &tpf);
        uint64_t w = atomic_load(&s->in_written);
        printf("%2ds frames %6llu  clock %.2f Hz  data vs clock %+5.0f..%+4.0f frames  peak L %.1f R %.1f dB  glitches %u\n",
               t + 1, w - prev_w, tps / tpf, lo, hi, 20 * log10(peak[0] + 1e-9), 20 * log10(peak[1] + 1e-9),
               atomic_load(&s->glitches));
        prev_w = w;
    }
    atomic_fetch_sub(&s->io_clients, 1);
    return 0;
}
