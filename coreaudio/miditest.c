/* Tests the US-122L's Core MIDI device like an app would: listens to its
 * MIDI IN, sends notes to its MIDI OUT, and (with the daemon's loopback
 * switch on) checks that every byte comes back through the whole chain:
 * app -> Core MIDI -> MIDI driver -> daemon -> unit (USB) -> loopback ->
 * daemon -> MIDI driver -> Core MIDI -> app.
 *   us122l-miditest [notes]   (run outside any sandbox) */
#include "client.h"
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static _Atomic unsigned got;
static _Atomic uint64_t last_in;
static uint8_t seen[4096];

static void read_proc(const MIDIPacketList *pl, void *a, void *b)
{
    (void)a, (void)b;
    const MIDIPacket *p = &pl->packet[0];
    for (UInt32 i = 0; i < pl->numPackets; i++) {
        for (UInt16 j = 0; j < p->length; j++) {
            unsigned n = atomic_fetch_add(&got, 1);
            if (n < sizeof seen)
                seen[n] = p->data[j];
        }
        p = MIDIPacketNext(p);
    }
    atomic_store(&last_in, mach_absolute_time());
}

static MIDIEndpointRef find(int source)
{
    ItemCount n = source ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef e = source ? MIDIGetSource(i) : MIDIGetDestination(i);
        MIDIEntityRef ent;
        MIDIDeviceRef dev;
        CFStringRef name = NULL;
        if (MIDIEndpointGetEntity(e, &ent) || MIDIEntityGetDevice(ent, &dev) ||
            MIDIObjectGetStringProperty(dev, kMIDIPropertyName, &name) || !name)
            continue;
        int match = CFEqual(name, CFSTR("TASCAM US-122L"));
        CFRelease(name);
        if (match)
            return e;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int notes = argc > 1 ? atoi(argv[1]) : 20;
    us122l_shm_t *s = us122l_connect();
    if (!s) { fprintf(stderr, "can't reach the daemon\n"); return 1; }
    MIDIClientRef client;
    MIDIPortRef in, out;
    MIDIClientCreate(CFSTR("us122l-miditest"), NULL, NULL, &client);
    MIDIInputPortCreate(client, CFSTR("in"), read_proc, NULL, &in);
    MIDIOutputPortCreate(client, CFSTR("out"), &out);
    MIDIEndpointRef src = find(1), dst = find(0);
    if (!src || !dst) { fprintf(stderr, "no TASCAM US-122L MIDI device in Core MIDI\n"); return 1; }
    SInt32 offline = -1;
    MIDIEntityRef ent; MIDIDeviceRef dev;
    MIDIEndpointGetEntity(src, &ent); MIDIEntityGetDevice(ent, &dev);
    MIDIObjectGetIntegerProperty(dev, kMIDIPropertyOffline, &offline);
    printf("found the device (offline=%d); listening\n", (int)offline);
    MIDIPortConnectSource(in, src, NULL);
    usleep(300000);
    printf("daemon: midi_clients=%u running=%u\n", atomic_load(&s->midi_clients), atomic_load(&s->running));

    atomic_store(&s->midi_loopback, 1);
    usleep(100000);
    uint64_t sent0 = atomic_load(&s->midi_sent);
    uint32_t err0 = atomic_load(&s->midi_errors);
    uint8_t expect[4096];
    unsigned nexp = 0;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double worst = 0;
    for (int i = 0; i < notes; i++) {
        uint8_t msg[3] = {(uint8_t)(i & 1 ? 0x80 : 0x90), (uint8_t)(60 + i / 2 % 12), (uint8_t)(i & 1 ? 0 : 100)};
        Byte buf[256];
        MIDIPacketList *pl = (MIDIPacketList *)buf;
        MIDIPacket *p = MIDIPacketListInit(pl);
        MIDIPacketListAdd(pl, sizeof buf, p, 0, 3, msg);
        unsigned before = atomic_load(&got);
        uint64_t t0 = mach_absolute_time();
        MIDISend(out, dst, pl);
        memcpy(expect + nexp, msg, 3);
        nexp += 3;
        while (atomic_load(&got) < before + 3 && (mach_absolute_time() - t0) * tb.numer / tb.denom < 2000000000ull)
            usleep(200);
        if (atomic_load(&got) >= before + 3) {
            double ms = (atomic_load(&last_in) - t0) * (double)tb.numer / tb.denom / 1e6;
            if (ms > worst) worst = ms;
        }
        usleep(20000);
    }
    usleep(300000);
    atomic_store(&s->midi_loopback, 0);
    unsigned n = atomic_load(&got);
    int same = n == nexp && memcmp(seen, expect, nexp) == 0;
    printf("sent %u bytes; the unit accepted %llu over USB; MIDI errors %u\n", nexp,
           (unsigned long long)(atomic_load(&s->midi_sent) - sent0), atomic_load(&s->midi_errors) - err0);
    printf("received %u bytes back through Core MIDI: %s; worst round trip %.1f ms\n", n,
           same ? "identical" : "MISMATCH", worst);
    MIDIPortDisconnectSource(in, src);
    usleep(300000);
    printf("after disconnecting: midi_clients=%u\n", atomic_load(&s->midi_clients));
    return same ? 0 : 1;
}
