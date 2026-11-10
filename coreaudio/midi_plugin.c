/*
 * Core MIDI driver for the TASCAM US-122L's MIDI IN and OUT ports. MIDIServer
 * loads it; it shows a "TASCAM US-122L" device with one MIDI source and one
 * destination (in Audio MIDI Setup, GarageBand, Logic…), and moves bytes
 * to and from the us122ld daemon (which owns the unit) through the shared
 * memory in shared.h.
 *
 * The unit only passes MIDI while its audio streams, so while an app listens
 * to MIDI IN this driver counts as a client and the daemon keeps it streaming.
 * (Sending to MIDI OUT starts the stream by itself.)
 */
#include "client.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreMIDI/MIDIDriver.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define DEVICE_NAME CFSTR("TASCAM US-122L")
#define MANUFACTURER CFSTR("TASCAM")
#define MODEL CFSTR("US-122L")

static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static us122l_shm_t *gShm;
static MIDIDeviceRef gDevice;
static MIDIEndpointRef gSource;
static _Atomic int gRunning;
static _Atomic UInt32 gRefs = 1;
static int gOffline = -1;
static int gListening;      /* an app listens to our source (gLock) */
static int gCounted;        /* we're counted in gShm->midi_clients (gLock) */

/* Count us as a MIDI client exactly while someone listens (gLock held). */
static void reconcile(void)
{
    int want = gShm && gListening && atomic_load(&gRunning);
    if (want && !gCounted)
        atomic_fetch_add(&gShm->midi_clients, 1);
    else if (!want && gCounted && gShm)
        atomic_fetch_sub(&gShm->midi_clients, 1);
    gCounted = want;
}

static HRESULT QueryInterface(void *, REFIID, LPVOID *);
static ULONG AddRef(void *);
static ULONG Release(void *);
static OSStatus FindDevices(MIDIDriverRef, MIDIDeviceListRef);
static OSStatus Start(MIDIDriverRef, MIDIDeviceListRef);
static OSStatus Stop(MIDIDriverRef);
static OSStatus Configure(MIDIDriverRef, MIDIDeviceRef);
static OSStatus Send(MIDIDriverRef, const MIDIPacketList *, void *, void *);
static OSStatus EnableSource(MIDIDriverRef, MIDIEndpointRef, Boolean);
static OSStatus Flush(MIDIDriverRef, MIDIEndpointRef, void *, void *);
static OSStatus Monitor(MIDIDriverRef, MIDIEndpointRef, const MIDIPacketList *);

static MIDIDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, FindDevices, Start, Stop, Configure, Send,
    EnableSource, Flush, Monitor, NULL, NULL,
};
static MIDIDriverInterface *gInterfacePtr = &gInterface;
static MIDIDriverRef gDriver = &gInterfacePtr;

/* the factory named in Info.plist */
void *US122LMIDI_Create(CFAllocatorRef alloc, CFUUIDRef type)
{
    (void)alloc;
    return CFEqual(type, kMIDIDriverTypeID) ? gDriver : NULL;
}

static HRESULT QueryInterface(void *self, REFIID iid, LPVOID *out)
{
    CFUUIDRef want = CFUUIDCreateFromUUIDBytes(NULL, iid);
    /* version 1/2 interface: classic MIDIPacketList Send */
    int ok = CFEqual(want, IUnknownUUID) || CFEqual(want, kMIDIDriverInterfaceID) ||
             CFEqual(want, kMIDIDriverInterface2ID);
    CFRelease(want);
    if (!ok) {
        *out = NULL;
        return E_NOINTERFACE;
    }
    atomic_fetch_add(&gRefs, 1);
    *out = self;
    return S_OK;
}

static ULONG AddRef(void *self)
{
    (void)self;
    return atomic_fetch_add(&gRefs, 1) + 1;
}

static ULONG Release(void *self)
{
    (void)self;
    UInt32 r = atomic_load(&gRefs);
    if (r > 1)
        r = atomic_fetch_sub(&gRefs, 1) - 1;
    return r;
}

static OSStatus FindDevices(MIDIDriverRef self, MIDIDeviceListRef list)
{
    (void)self, (void)list;
    return noErr;
}

/* Our device from an earlier run (MIDI setup remembers it), or a new one. */
static MIDIDeviceRef find_or_create_device(MIDIDriverRef self)
{
    MIDIDeviceListRef list = MIDIGetDriverDeviceList(self);
    MIDIDeviceRef found = 0;
    for (ItemCount i = 0; i < MIDIDeviceListGetNumberOfDevices(list); i++) {
        MIDIDeviceRef d = MIDIDeviceListGetDevice(list, i);
        CFStringRef name = NULL;
        if (MIDIObjectGetStringProperty(d, kMIDIPropertyName, &name) == noErr && name) {
            if (CFEqual(name, DEVICE_NAME) && !found)
                found = d;
            CFRelease(name);
        }
    }
    MIDIDeviceListDispose(list);
    if (found)
        return found;
    MIDIDeviceRef d = 0;
    MIDIEntityRef e = 0;
    if (MIDIDeviceCreate(self, DEVICE_NAME, MANUFACTURER, MODEL, &d) != noErr)
        return 0;
    MIDIDeviceAddEntity(d, CFSTR("MIDI"), false, 1, 1, &e);
    MIDISetupAddDevice(d);
    return d;
}

/* Watches the daemon and the unit; the device is offline without them. */
static void *watch(void *arg)
{
    (void)arg;
    while (atomic_load(&gRunning)) {
        pthread_mutex_lock(&gLock);
        if (gShm && !us122l_daemon_alive(gShm)) {
            gShm = NULL;            /* stale; left mapped (other threads may read it) */
            gCounted = 0;
        }
        if (!gShm) {
            gShm = us122l_connect();
            if (gShm)               /* start reading at what arrives from now on */
                atomic_store(&gShm->midi_in_r, atomic_load(&gShm->midi_in_w));
        }
        reconcile();
        int offline = !(gShm && atomic_load(&gShm->present));
        pthread_mutex_unlock(&gLock);
        if (offline != gOffline && gDevice) {
            MIDIObjectSetIntegerProperty(gDevice, kMIDIPropertyOffline, offline);
            gOffline = offline;
        }
        usleep(500000);
    }
    return NULL;
}

/* Hands MIDI IN events to Core MIDI (polls every 1 ms). */
static void *receive(void *arg)
{
    (void)arg;
    while (atomic_load(&gRunning)) {
        us122l_shm_t *s = gShm;
        if (s && gSource) {
            uint32_t r = atomic_load_explicit(&s->midi_in_r, memory_order_relaxed);
            uint32_t w = atomic_load_explicit(&s->midi_in_w, memory_order_acquire);
            if (w - r > US122L_MIDI_EVENTS)          /* way behind (or stale): skip */
                r = w;
            if (r != w) {
                Byte buf[1024];
                MIDIPacketList *pl = (MIDIPacketList *)buf;
                MIDIPacket *p = MIDIPacketListInit(pl);
                for (; r != w; r++) {
                    us122l_midi_event_t *e = &s->midi_in[r & (US122L_MIDI_EVENTS - 1)];
                    MIDIPacket *next = MIDIPacketListAdd(pl, sizeof buf, p, e->host, e->len, e->data);
                    if (!next) {                     /* full: send this batch first */
                        MIDIReceived(gSource, pl);
                        p = MIDIPacketListInit(pl);
                        next = MIDIPacketListAdd(pl, sizeof buf, p, e->host, e->len, e->data);
                    }
                    p = next;
                }
                MIDIReceived(gSource, pl);
                atomic_store_explicit(&s->midi_in_r, r, memory_order_release);
            }
        }
        usleep(1000);
    }
    return NULL;
}

static OSStatus Start(MIDIDriverRef self, MIDIDeviceListRef devList)
{
    (void)devList;
    gDevice = find_or_create_device(self);
    if (!gDevice)
        return kMIDIUnknownError;
    MIDIEntityRef e = MIDIDeviceGetEntity(gDevice, 0);
    gSource = MIDIEntityGetSource(e, 0);
    MIDIObjectSetIntegerProperty(gDevice, kMIDIPropertyOffline, 1);
    gOffline = 1;
    atomic_store(&gRunning, 1);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, watch, NULL);
    pthread_detach(t1);
    pthread_create(&t2, NULL, receive, NULL);
    pthread_detach(t2);
    return noErr;
}

static OSStatus Stop(MIDIDriverRef self)
{
    (void)self;
    atomic_store(&gRunning, 0);
    pthread_mutex_lock(&gLock);
    reconcile();
    gShm = NULL;
    pthread_mutex_unlock(&gLock);
    return noErr;
}

static OSStatus Configure(MIDIDriverRef self, MIDIDeviceRef device)
{
    (void)self, (void)device;
    return noErr;
}

/* Bytes for MIDI OUT: into the daemon's ring (dropped if it's full). */
static OSStatus Send(MIDIDriverRef self, const MIDIPacketList *pl, void *ref1, void *ref2)
{
    (void)self, (void)ref1, (void)ref2;
    us122l_shm_t *s = gShm;
    if (!s)
        return noErr;
    uint32_t w = atomic_load_explicit(&s->midi_out_w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&s->midi_out_r, memory_order_acquire);
    const MIDIPacket *p = &pl->packet[0];
    for (UInt32 i = 0; i < pl->numPackets; i++) {
        for (UInt16 j = 0; j < p->length && w - r < US122L_MIDI_OUT; j++)
            s->midi_out[w++ & (US122L_MIDI_OUT - 1)] = p->data[j];
        p = MIDIPacketNext(p);
    }
    atomic_store_explicit(&s->midi_out_w, w, memory_order_release);
    return noErr;
}

/* Core MIDI tells us when apps start or stop listening to MIDI IN. */
static OSStatus EnableSource(MIDIDriverRef self, MIDIEndpointRef src, Boolean enabled)
{
    (void)self, (void)src;
    pthread_mutex_lock(&gLock);
    gListening = enabled;
    reconcile();
    pthread_mutex_unlock(&gLock);
    return noErr;
}

static OSStatus Flush(MIDIDriverRef self, MIDIEndpointRef dest, void *ref1, void *ref2)
{
    (void)self, (void)dest, (void)ref1, (void)ref2;
    return noErr;
}

static OSStatus Monitor(MIDIDriverRef self, MIDIEndpointRef dest, const MIDIPacketList *pl)
{
    (void)self, (void)dest, (void)pl;
    return noErr;
}
