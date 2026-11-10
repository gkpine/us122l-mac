# us122l-mac

A userspace driver for the **TASCAM US-122L** USB audio interface on macOS
(Apple Silicon and Intel), built on [libusb](https://libusb.info).

TASCAM never released a US-122L driver for current macOS, and the unit isn't
USB-audio class compliant, so Core Audio doesn't see it. This library talks to
it directly over USB and hands audio to your callback, PortAudio-style. It's
what lets [VibeAmp](../VibeAmp) use the US-122L on a Mac.

- 2 inputs, 2 outputs, 24-bit, at 44.1, 48, 88.2 or 96 kHz
- No kernel extension, no root, nothing installed system-wide
- Real-time threads; tested clean with every CPU core saturated
- C API (`include/us122l.h`) and a Python binding (`import us122l`)

It does **not** make the US-122L show up in other Mac apps (GarageBand,
Logic…). That would need a Core Audio plug-in on top of this. The unit's MIDI
ports aren't supported yet either.

## Install

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
  10 ms more). Excess beyond that is trimmed, so latency can't creep up.

At 48 kHz the estimated round trip is about 16 ms with 64-frame blocks and
about 18 ms with 128.

Debugging: `US122L_DEBUG=1` logs USB errors and underruns to stderr.
`US122L_PKTS` and `US122L_XFERS` override the transfer size and queue depth.

## License

GPL-2.0-or-later (it follows the Linux driver's design). See `LICENSE`.
