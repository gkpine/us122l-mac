/*
 * The Core Audio side of the US-122L: an AudioServerPlugIn that macOS loads
 * into its audio server. It shows a "TASCAM US-122L" device to every app
 * (GarageBand, Logic, Music, Zoom…) and moves audio to and from the us122ld
 * daemon, which owns the unit over USB, through shared memory (shared.h).
 *
 * Objects: the plug-in, one device (only while a unit is plugged in and the
 * daemon is running), and an input and an output stream, each 2 channels of
 * float32. Sample rates 44.1, 48, 88.2 and 96 kHz.
 *
 * Timing: the daemon publishes the unit's clock (device frame -> host time);
 * GetZeroTimeStamp reads it. Capture is in a ring indexed by device frame,
 * and so is playback.
 */
#include "client.h"

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define kPlugIn kAudioObjectPlugInObject
#define kDevice 2
#define kStreamIn 3
#define kStreamOut 4

#define DEVICE_NAME "TASCAM US-122L"
#define MANUFACTURER "TASCAM"
#define DEVICE_UID "US122L-mac:TASCAM-US-122L"
#define MODEL_UID "US122L-mac:US-122L"
#define CHANNELS 2
#define ZTS_PERIOD 4096                 /* frames between zero time stamps */

static const Float64 kRates[] = {44100, 48000, 88200, 96000};
#define NRATES (sizeof kRates / sizeof *kRates)

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static us122l_shm_t *gShm;              /* NULL until the daemon answers */
static _Atomic int gPresent;            /* device published */
static Float64 gRate = US122L_DEFAULT_RATE;
static UInt32 gIOCount;                 /* StartIO without StopIO */
static _Atomic UInt32 gRefs = 1;

/* ------------------------------------------------------------ the daemon */

static UInt32 transfer_frames(Float64 rate) { return (UInt32)(rate * 16 / 8000); }

/* Capture data trails the clock by up to one USB transfer; playback is read
 * one transfer at a time: keep Core Audio that far clear, plus 2 ms. */
static UInt32 safety_offset(Float64 rate) { return transfer_frames(rate) + (UInt32)(rate / 500); }

static void notify(AudioObjectID obj, AudioObjectPropertySelector sel)
{
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    if (gHost)
        gHost->PropertiesChanged(gHost, obj, 1, &a);
}

/* Watches the daemon and the unit; publishes or removes the device. */
static void *watch(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&gLock);
        if (gShm && !us122l_daemon_alive(gShm)) {
            /* The daemon restarted: its old memory is stale. Leave it mapped
             * (the I/O thread may be reading it right now) and connect anew. */
            gShm = NULL;
        }
        if (!gShm) {
            gShm = us122l_connect();
            if (gShm && gIOCount > 0)   /* keep a running device running */
                atomic_fetch_add(&gShm->io_clients, 1);
            if (gShm)
                atomic_store(&gShm->want_rate, (uint32_t)gRate);
        }
        int present = gShm && atomic_load(&gShm->present);
        pthread_mutex_unlock(&gLock);
        if (present != atomic_load(&gPresent)) {
            atomic_store(&gPresent, present);
            notify(kPlugIn, kAudioPlugInPropertyDeviceList);
            notify(kPlugIn, kAudioObjectPropertyOwnedObjects);
        }
        usleep(500000);
    }
    return NULL;
}

/* -------------------------------------------------------------- helpers */

static AudioStreamBasicDescription format(Float64 rate)
{
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian |
                     kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = CHANNELS * sizeof(Float32);
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = CHANNELS;
    f.mBitsPerChannel = 32;
    return f;
}

static int is_rate(Float64 r)
{
    for (size_t i = 0; i < NRATES; i++)
        if (r == kRates[i])
            return 1;
    return 0;
}

static int valid_object(AudioObjectID o)
{
    if (o == kPlugIn)
        return 1;
    return atomic_load(&gPresent) && (o == kDevice || o == kStreamIn || o == kStreamOut);
}

/* Write a value of type T into outData (checking the size). */
#define PUT(T, v)                                                   \
    do {                                                            \
        if (inDataSize < sizeof(T))                                 \
            return kAudioHardwareBadPropertySizeError;              \
        *(T *)outData = (v);                                        \
        *outDataSize = sizeof(T);                                   \
        return 0;                                                   \
    } while (0)

/* ---------------------------------------------------------- COM plumbing */

static HRESULT QueryInterface(void *, REFIID, LPVOID *);
static ULONG AddRef(void *);
static ULONG Release(void *);
static OSStatus Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
static OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef,
                             const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID,
                                const AudioServerPlugInClientInfo *);
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID,
                                   const AudioServerPlugInClientInfo *);
static OSStatus PerformConfigChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus AbortConfigChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t,
                           const AudioObjectPropertyAddress *);
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t,
                                   const AudioObjectPropertyAddress *, Boolean *);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t,
                                    const AudioObjectPropertyAddress *, UInt32, const void *,
                                    UInt32 *);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t,
                                const AudioObjectPropertyAddress *, UInt32, const void *,
                                UInt32, UInt32 *, void *);
static OSStatus SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t,
                                const AudioObjectPropertyAddress *, UInt32, const void *,
                                UInt32, const void *);
static OSStatus StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *,
                                 UInt64 *, UInt64 *);
static OSStatus WillDoIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *,
                         Boolean *);
static OSStatus BeginIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
                        const AudioServerPlugInIOCycleInfo *);
static OSStatus DoIO(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32,
                     UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus EndIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
                      const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice,
    AddDeviceClient, RemoveDeviceClient, PerformConfigChange, AbortConfigChange,
    HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData,
    StartIO, StopIO, GetZeroTimeStamp, WillDoIO, BeginIO, DoIO, EndIO,
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriver = &gInterfacePtr;

/* the factory named in Info.plist */
void *US122L_Create(CFAllocatorRef alloc, CFUUIDRef type)
{
    (void)alloc;
    return CFEqual(type, kAudioServerPlugInTypeUUID) ? gDriver : NULL;
}

static HRESULT QueryInterface(void *self, REFIID iid, LPVOID *out)
{
    CFUUIDRef want = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(want, IUnknownUUID) || CFEqual(want, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&gRefs, 1);
        *out = self;
        r = S_OK;
    } else {
        *out = NULL;
    }
    CFRelease(want);
    return r;
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

static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host)
{
    (void)d;
    gHost = host;
    pthread_t t;
    pthread_create(&t, NULL, watch, NULL);
    pthread_detach(t);
    return 0;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                             const AudioServerPlugInClientInfo *ci, AudioObjectID *out)
{
    (void)d, (void)desc, (void)ci, (void)out;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o)
{
    (void)d, (void)o;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o,
                                const AudioServerPlugInClientInfo *ci)
{
    (void)d, (void)o, (void)ci;
    return 0;
}

static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o,
                                   const AudioServerPlugInClientInfo *ci)
{
    (void)d, (void)o, (void)ci;
    return 0;
}

/* Wait (up to 3 s) for the daemon to stream at the current rate. */
static int wait_running(void)
{
    for (int ms = 0; ms < 3000; ms += 5) {
        us122l_shm_t *s = gShm;
        if (s && atomic_load(&s->running) && atomic_load(&s->rate) == (uint32_t)gRate)
            return 1;
        usleep(5000);
    }
    return 0;
}

static OSStatus PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 action,
                                    void *info)
{
    (void)d, (void)info;
    if (o != kDevice || !is_rate((Float64)action))
        return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    gRate = (Float64)action;
    if (gShm)
        atomic_store(&gShm->want_rate, (uint32_t)gRate);
    int running = gIOCount > 0;
    pthread_mutex_unlock(&gLock);
    if (running)
        wait_running();
    return 0;
}

static OSStatus AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a,
                                  void *info)
{
    (void)d, (void)o, (void)a, (void)info;
    return 0;
}

/* ----------------------------------------------------------- properties */

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                           const AudioObjectPropertyAddress *a)
{
    (void)d;
    UInt32 size;
    /* everything we can report has a size */
    return valid_object(o) && GetPropertyDataSize(d, o, pid, a, 0, NULL, &size) == 0;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                   const AudioObjectPropertyAddress *a, Boolean *out)
{
    (void)d, (void)pid;
    if (!valid_object(o))
        return kAudioHardwareBadObjectError;
    switch (a->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
        *out = o == kDevice;
        return 0;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
    case kAudioStreamPropertyIsActive:
        *out = o == kStreamIn || o == kStreamOut;
        return 0;
    default:
        *out = false;
        return 0;
    }
}

/* Device objects owned in a scope: streams. */
static UInt32 device_streams(AudioObjectPropertyScope scope, AudioObjectID *ids)
{
    UInt32 n = 0;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput)
        ids[n++] = kStreamIn;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput)
        ids[n++] = kStreamOut;
    return n;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                    const AudioObjectPropertyAddress *a, UInt32 qsize,
                                    const void *q, UInt32 *out)
{
    /* Simplest correct answer: get the data into a buffer big enough for any
     * of our properties. (Our CFStrings are constants, so nothing to free.) */
    char buf[1024] __attribute__((aligned(16)));
    return GetPropertyData(d, o, pid, a, qsize, q, sizeof buf, out, buf);
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qsize,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize,
                                void *outData)
{
    (void)d, (void)pid;
    if (!valid_object(o))
        return kAudioHardwareBadObjectError;
    AudioObjectPropertySelector sel = a->mSelector;

    if (o == kPlugIn) {
        switch (sel) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR("us122l-mac"));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            UInt32 n = atomic_load(&gPresent) ? 1 : 0;
            if (inDataSize < n * sizeof(AudioObjectID))
                n = inDataSize / sizeof(AudioObjectID);
            if (n)
                *(AudioObjectID *)outData = kDevice;
            *outDataSize = n * sizeof(AudioObjectID);
            return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID id = kAudioObjectUnknown;
            if (q && qsize == sizeof(CFStringRef) && atomic_load(&gPresent) &&
                CFEqual(*(CFStringRef *)q, CFSTR(DEVICE_UID)))
                id = kDevice;
            PUT(AudioObjectID, id);
        }
        case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
        default: return kAudioHardwareUnknownPropertyError;
        }
    }

    if (o == kDevice) {
        AudioObjectPropertyScope sc = a->mScope;
        switch (sel) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: PUT(CFStringRef, CFSTR(DEVICE_NAME));
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR(MANUFACTURER));
        case kAudioDevicePropertyDeviceUID: PUT(CFStringRef, CFSTR(DEVICE_UID));
        case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR(MODEL_UID));
        case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeUSB);
        case kAudioDevicePropertyClockDomain: PUT(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: {
            pthread_mutex_lock(&gLock);
            UInt32 v = gIOCount > 0;
            pthread_mutex_unlock(&gLock);
            PUT(UInt32, v);
        }
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 1);
        case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
        case kAudioDevicePropertyLatency: {
            /* the USB path: capture is one transfer old; playback waits
             * behind three queued transfers */
            UInt32 t = transfer_frames(gRate);
            PUT(UInt32, sc == kAudioObjectPropertyScopeInput ? t : 3 * t);
        }
        case kAudioDevicePropertySafetyOffset: PUT(UInt32, safety_offset(gRate));
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, ZTS_PERIOD);
        case kAudioDevicePropertyNominalSampleRate: PUT(Float64, gRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = inDataSize / sizeof(AudioValueRange);
            if (n > NRATES)
                n = NRATES;
            AudioValueRange *r = outData;
            for (UInt32 i = 0; i < n; i++)
                r[i].mMinimum = r[i].mMaximum = kRates[i];
            *outDataSize = n * sizeof(AudioValueRange);
            return 0;
        }
        case kAudioDevicePropertyRelatedDevices: PUT(AudioObjectID, kDevice);
        case kAudioObjectPropertyControlList:
            *outDataSize = 0;
            return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            AudioObjectID ids[2];
            UInt32 n = device_streams(sc, ids);
            if (inDataSize < n * sizeof(AudioObjectID))
                n = inDataSize / sizeof(AudioObjectID);
            memcpy(outData, ids, n * sizeof(AudioObjectID));
            *outDataSize = n * sizeof(AudioObjectID);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32))
                return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1;
            ((UInt32 *)outData)[1] = 2;
            *outDataSize = 2 * sizeof(UInt32);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 size = offsetof(AudioChannelLayout, mChannelDescriptions) +
                          CHANNELS * sizeof(AudioChannelDescription);
            if (inDataSize < size)
                return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, size);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = CHANNELS;
            l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outDataSize = size;
            return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }

    /* streams */
    int input = o == kStreamIn;
    switch (sel) {
    case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
    case kAudioObjectPropertyOwner: PUT(AudioObjectID, kDevice);
    case kAudioObjectPropertyName:
        PUT(CFStringRef, input ? CFSTR("US-122L Inputs") : CFSTR("US-122L Outputs"));
    case kAudioObjectPropertyOwnedObjects:
        *outDataSize = 0;
        return 0;
    case kAudioStreamPropertyIsActive: PUT(UInt32, 1);
    case kAudioStreamPropertyDirection: PUT(UInt32, input ? 1 : 0);
    case kAudioStreamPropertyTerminalType:
        PUT(UInt32, input ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeSpeaker);
    case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
    case kAudioStreamPropertyLatency: PUT(UInt32, 0);
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: PUT(AudioStreamBasicDescription, format(gRate));
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        UInt32 n = inDataSize / sizeof(AudioStreamRangedDescription);
        if (n > NRATES)
            n = NRATES;
        AudioStreamRangedDescription *r = outData;
        for (UInt32 i = 0; i < n; i++) {
            r[i].mFormat = format(kRates[i]);
            r[i].mSampleRateRange.mMinimum = r[i].mSampleRateRange.mMaximum = kRates[i];
        }
        *outDataSize = n * sizeof(AudioStreamRangedDescription);
        return 0;
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus request_rate(Float64 rate)
{
    if (!is_rate(rate))
        return kAudioDeviceUnsupportedFormatError;
    if (rate == gRate)
        return 0;
    return gHost->RequestDeviceConfigurationChange(gHost, kDevice, (UInt64)rate, NULL);
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qsize,
                                const void *q, UInt32 size, const void *data)
{
    (void)d, (void)pid, (void)qsize, (void)q;
    if (!valid_object(o))
        return kAudioHardwareBadObjectError;
    switch (a->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
        if (o != kDevice)
            break;
        if (size < sizeof(Float64))
            return kAudioHardwareBadPropertySizeError;
        return request_rate(*(const Float64 *)data);
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        if (o != kStreamIn && o != kStreamOut)
            break;
        if (size < sizeof(AudioStreamBasicDescription))
            return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = data;
        AudioStreamBasicDescription want = format(f->mSampleRate);
        if (f->mFormatID != want.mFormatID || f->mChannelsPerFrame != CHANNELS ||
            f->mBitsPerChannel != 32 || !(f->mFormatFlags & kAudioFormatFlagIsFloat))
            return kAudioDeviceUnsupportedFormatError;
        return request_rate(f->mSampleRate);
    }
    case kAudioStreamPropertyIsActive:
        if (o == kStreamIn || o == kStreamOut)
            return 0;
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ------------------------------------------------------------------- IO */

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client)
{
    (void)d, (void)client;
    if (o != kDevice || !atomic_load(&gPresent))
        return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (!gShm) {
        pthread_mutex_unlock(&gLock);
        return kAudioHardwareNotRunningError;
    }
    if (gIOCount++ == 0) {
        atomic_store(&gShm->want_rate, (uint32_t)gRate);
        atomic_fetch_add(&gShm->io_clients, 1);
    }
    pthread_mutex_unlock(&gLock);
    if (!wait_running()) {
        StopIO(d, o, client);
        return kAudioHardwareNotRunningError;
    }
    return 0;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client)
{
    (void)d, (void)client;
    if (o != kDevice)
        return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (gIOCount > 0 && --gIOCount == 0 && gShm)
        atomic_fetch_sub(&gShm->io_clients, 1);
    pthread_mutex_unlock(&gLock);
    return 0;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client,
                                 Float64 *sample, UInt64 *host, UInt64 *seed)
{
    (void)d, (void)o, (void)client;
    us122l_shm_t *s = gShm;
    uint64_t f, h;
    double tpf;
    if (!s || !us122l_read_clock(s, &f, &h, &tpf) || tpf <= 0) {
        *sample = 0;
        *host = mach_absolute_time();
        *seed = 1;
        return 0;
    }
    /* the latest period boundary on the unit's clock, and when it was */
    double now = (double)f + ((double)mach_absolute_time() - (double)h) / tpf;
    double k = floor(now / ZTS_PERIOD) * ZTS_PERIOD;
    *sample = k;
    *host = (UInt64)((double)h + (k - (double)f) * tpf);
    *seed = atomic_load(&s->seed);
    return 0;
}

static OSStatus WillDoIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op,
                         Boolean *will, Boolean *inPlace)
{
    (void)d, (void)o, (void)client;
    *will = op == kAudioServerPlugInIOOperationReadInput ||
            op == kAudioServerPlugInIOOperationWriteMix;
    *inPlace = true;
    return 0;
}

static OSStatus BeginIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op,
                        UInt32 frames, const AudioServerPlugInIOCycleInfo *info)
{
    (void)d, (void)o, (void)client, (void)op, (void)frames, (void)info;
    return 0;
}

static OSStatus DoIO(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream,
                     UInt32 client, UInt32 op, UInt32 frames,
                     const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary)
{
    (void)d, (void)o, (void)client, (void)secondary;
    us122l_shm_t *s = gShm;
    Float32 *buf = main;
    if (op == kAudioServerPlugInIOOperationReadInput && stream == kStreamIn) {
        int64_t start = (int64_t)info->mInputTime.mSampleTime;
        uint64_t written = s ? atomic_load_explicit(&s->in_written, memory_order_acquire) : 0;
        for (UInt32 i = 0; i < frames; i++) {
            int64_t f = start + i;
            /* only frames already captured and still in the ring */
            if (s && f >= 0 && (uint64_t)f < written && written - (uint64_t)f <= US122L_RING) {
                uint64_t k = ((uint64_t)f & (US122L_RING - 1)) * 2;
                buf[2 * i] = s->in_ring[k];
                buf[2 * i + 1] = s->in_ring[k + 1];
            } else {
                buf[2 * i] = buf[2 * i + 1] = 0.f;
            }
        }
    } else if (op == kAudioServerPlugInIOOperationWriteMix && stream == kStreamOut && s) {
        int64_t start = (int64_t)info->mOutputTime.mSampleTime;
        for (UInt32 i = 0; i < frames; i++) {
            int64_t f = start + i;
            if (f < 0)
                continue;
            uint64_t k = ((uint64_t)f & (US122L_RING - 1)) * 2;
            s->out_ring[k] = buf[2 * i];
            s->out_ring[k + 1] = buf[2 * i + 1];
        }
    }
    return 0;
}

static OSStatus EndIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op,
                      UInt32 frames, const AudioServerPlugInIOCycleInfo *info)
{
    (void)d, (void)o, (void)client, (void)op, (void)frames, (void)info;
    return 0;
}
