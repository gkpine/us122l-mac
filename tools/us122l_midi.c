/* MIDI check for the US-122L.
 *   us122l-midi [seconds]   print what arrives at MIDI IN, and every 2 s send
 *                           a note (C4, then E4…) to MIDI OUT.
 * (The unit only passes MIDI while audio streams, so audio runs too.) */
#include "us122l.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void midi_in(const uint8_t *b, unsigned n, uint64_t t, void *u)
{
    (void)t, (void)u;
    printf("MIDI IN:");
    for (unsigned i = 0; i < n; i++)
        printf(" %02X", b[i]);
    printf("\n");
    fflush(stdout);
}
static void cap(const float *in, unsigned n, uint64_t f, uint64_t h, void *u) { (void)in, (void)n, (void)f, (void)h, (void)u; }
static void play(float *out, unsigned n, uint64_t f, void *u) { (void)f, (void)u; memset(out, 0, n * 8); }

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    us122l_raw_callbacks cbs = {cap, play, midi_in};
    char err[256];
    us122l_t *d = us122l_open_raw(48000, &cbs, NULL, err, sizeof err);
    if (!d) { fprintf(stderr, "open failed: %s\n", err); return 1; }
    printf("listening on MIDI IN for %d s; sending a note to MIDI OUT every 2 s\n", secs);
    fflush(stdout);
    static const uint8_t notes[] = {60, 64, 67, 72};
    for (int t = 0; t < secs; t += 2) {
        uint8_t on[3] = {0x90, notes[(t / 2) % 4], 100}, off[3] = {0x80, notes[(t / 2) % 4], 0};
        us122l_midi_send(d, on, 3);
        usleep(500000);
        us122l_midi_send(d, off, 3);
        usleep(1500000);
    }
    us122l_stats_t st;
    us122l_get_stats(d, &st);
    printf("MIDI OUT: %llu bytes accepted by the unit; MIDI IN: %llu bytes; MIDI errors: %u\n",
           st.midi_out_bytes, st.midi_in_bytes, st.midi_errors);
    us122l_close(d);
    return 0;
}
