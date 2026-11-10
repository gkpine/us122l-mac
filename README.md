# us122l-mac

A userspace driver for the **TASCAM US-122L** USB audio interface on macOS
(Apple Silicon and Intel), built on [libusb](https://libusb.info).

TASCAM never released a US-122L driver for current macOS, and the unit isn't
USB-audio class compliant, so Core Audio doesn't see it. This library talks to
it directly over USB and hands audio to your callback, PortAudio-style. It's
what lets [VibeAmp](../VibeAmp) use the US-122L on a Mac.

- 2 inputs, 2 outputs, 24-bit, at 44.1, 48, 88.2 or 96 kHz
- Notices when the unit stops (unplugged, Mac asleep), so apps can reconnect
- No kernel extension, no root, nothing installed system-wide
- Real-time threads; tested clean with every CPU core saturated
- C API (`include/us122l.h`) and a Python binding (`import us122l`)

It comes in two parts:

- **The Core Audio and Core MIDI drivers**: make the US-122L an ordinary
  audio device, and its MIDI IN/OUT ordinary MIDI ports, for every app
  (GarageBand, Logic, Music, Zoom, Sound settings, Audio MIDI Setup…). See
  [Core Audio driver](#core-audio-driver).
- **The library** (C and Python): for programs that talk to the unit
  directly, like VibeAmp did before the Core Audio driver.

Not supported (yet):

- **USB 1.1 (full speed)** connections, e.g. through some hubs or KVMs: it
  says so; plug the unit straight into the Mac.
- **More than one unit** at once (the first one found is used).
- The **US-122MKII** is recognised (the Linux driver treats it the same) but
  hasn't been tested. The US-144 is not supported.

## Core Audio driver

```sh
brew install libusb       # only needed to build
make
sudo coreaudio/install.sh       # restarts Core Audio: sound stops for a second
```

Then pick **TASCAM US-122L** in System Settings → Sound, or in any app. It
offers 2 inputs and 2 outputs at 44.1, 48, 88.2 or 96 kHz, and a MIDI device
of the same name with one MIDI input and one output. Remove it all with
`sudo coreaudio/uninstall.sh`.

How it fits together:

- **`us122ld`**, a launchd daemon running as root, owns the unit over USB.
  It's built with this library in raw mode, with libusb linked in. It
  streams only while an app is using the device, plus 5 s, and restarts the
  stream if the unit stops (sleep, replug).
- **`US122L.driver`**, an AudioServerPlugIn, is loaded by macOS's audio
  server. It shows the device while a unit is plugged in.
- **`US122L-MIDI.plugin`**, a Core MIDI driver, is loaded by MIDIServer.
  The MIDI device shows as offline when the unit is unplugged.
- **MIDI on the wire:** 9-byte bulk packets. MIDI IN carries up to 8 bytes
  padded with `0xFD`; MIDI OUT carries one byte, padded the same way (as in
  Linux). The unit only passes MIDI while its audio streams: with audio off
  it took 2 of 18 bytes and then stopped accepting. So the daemon also
  streams while any app listens to the MIDI input (Core MIDI's
  `EnableSource`), and for 10 s after anything is sent to MIDI OUT. Bytes
  wait the ~30 ms it takes to start.
- They share one block of memory, handed over by Mach (`coreaudio/shared.h`).
  It holds a capture ring and a playback ring, both indexed by the unit's
  frame count, and the unit's clock. The daemon fits that clock from packet
  arrival times with a 0.5 Hz delay-locked loop, and the plug-in gives it to
  Core Audio as zero time stamps. Nothing is sent per I/O cycle.
- Latency: Core Audio's usual buffer, plus a safety offset of one USB
  transfer and 2 ms each way, plus 2 ms in and 6 ms out on the bus.

Logs: `/Library/Logs/us122ld.log`. Test tools (run them outside any sandbox):

- `build/us122l-shmtest` acts like the audio plug-in, to check the daemon
  without Core Audio.
- `build/us122l-miditest` acts like a MIDI app. It turns on the daemon's
  loopback switch (MIDI OUT echoed back as MIDI IN, after the unit accepted
  it over USB) and checks every byte comes back. Measured: 120/120 bytes,
  worst round trip 2.5 ms.
- `build/us122l-midi` (with the daemon idle) prints what reaches the unit's
  MIDI IN and sends a note to MIDI OUT every 2 s. Use it with a keyboard or
  synth attached; the MIDI sockets themselves haven't been tested with gear
  yet.

## Install the library

```sh
brew install libusb
pip install /path/to/us122l-mac        # into the Python you'll use it from
```

For VibeAmp, `run-mac.command` installs it on its own when this folder sits
next to the VibeAmp folder.

Plain C (library and test tools in `build/`): `make`

## Check the unit

```sh
python -m us122l              # 5 s at 48 kHz: input levels, dropouts
python -m us122l 10 48000 128 --thru    # inputs to outputs (headphones on, volume down!)
./build/us122l-test 10 48000 128 tone   # C version; tone = 440 Hz at -20 dBFS
```

## Python

```python
import us122l

def callback(indata, outdata, frames, time, status):   # same as sounddevice's
    outdata[:] = indata          # (frames, 2) float32 arrays

with us122l.Stream(samplerate=48000, blocksize=128, callback=callback) as s:
    print(s.latency)             # (input, output) seconds, estimated
    ...
```

- `status` is a `CallbackFlags` (`.input_overflow`, `.output_underflow`)
  and `time` has `inputBufferAdcTime` / `outputBufferDacTime` /
  `currentTime`, as in sounddevice.
- Raise `us122l.CallbackStop` (or sounddevice's) to stop. Any other
  exception stops the stream too and is kept in `stream.error`. Either way,
  `stream.active` turns False.
- `stream.active` is also False when the unit is unplugged or stops sending
  audio (`stream.stalled`, e.g. after the Mac slept). Stop it, and `start()`
  again when `us122l.present()`.
- While a stream runs, `sys.setswitchinterval` is lowered to 1 ms so the
  audio thread gets the GIL in time.

Only one program can use the unit at a time.

## How it works

The protocol comes from the Linux `snd-usb-us122l` driver (Karsten Wiese,
GPL-2.0-or-later):

- USB 2.0 high speed, vendor-specific. Interface 1, alt setting 1: iso IN
  endpoint `0x81` (capture) and iso OUT `0x02` (playback), one packet per
  125 µs microframe, frames of 6 bytes (2 × 24-bit little-endian).
- Vendor request `'I'` with `0x11` then `0x10` wakes it. The rate is a UAC1
  `SET_CUR` (sampling frequency) on endpoint `0x81`.
- The unit's clock is the master. Playback packets must mirror the sizes of
  the capture packets (implicit feedback), otherwise playback drifts.

On macOS:

- **Transfers of 16 packets (2 ms).** 1 ms transfers lose about 40% of the
  audio, because macOS can't keep them scheduled back to back. Four capture
  transfers and three playback transfers stay queued; two playback transfers
  starve.
- **Real-time completions.** libusb's macOS backend delivers completions from
  its own thread. The driver gives that thread (and its own two) Mach
  time-constraint priority, like Core Audio's I/O threads. Without that,
  capture arrives up to ~8 ms late on a busy machine and the stream breaks up.
- **Buffering.** Playback buffers one block plus a cushion of two transfers
  and 1 ms. If the unit ever runs dry, the cushion grows by 1 ms (up to
  10 ms more). After 10 s without trouble, it gives 1 ms back.
- **Watchdog.** If no audio arrives for 0.5 s while USB still looks alive
  (the Mac slept, the unit hung), the stream reports itself stopped.

At 48 kHz the estimated round trip is about 16 ms with 64-frame blocks and
about 18 ms with 128.

Debugging: `US122L_DEBUG=1` logs USB errors and underruns to stderr.
`US122L_PKTS` and `US122L_XFERS` override the transfer size and queue depth.

## License

GPL-2.0-or-later (it follows the Linux driver's design). See `LICENSE`.
