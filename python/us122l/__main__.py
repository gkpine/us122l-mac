"""python -m us122l [seconds] [rate] [blocksize] [--thru]: check the unit.

Streams for a few seconds and prints input levels and driver counters each
second. --thru sends the inputs to the outputs (headphones on, volume down)."""
import sys
import time

import numpy as np

import us122l


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    secs = int(args[0]) if args else 5
    rate = int(args[1]) if len(args) > 1 else 48000
    block = int(args[2]) if len(args) > 2 else 256
    thru = "--thru" in argv
    print(f"us122l {us122l.version()}: unit {'found' if us122l.present() else 'NOT found'}")
    peak = np.zeros(2, np.float32)

    def cb(indata, outdata, frames, _t, status):
        np.maximum(peak, np.abs(indata).max(axis=0), out=peak)
        if thru:
            outdata[:] = indata
        else:
            outdata.fill(0)

    with us122l.Stream(rate, block, cb) as s:
        li, lo = s.latency
        print(f"streaming at {rate} Hz, {block} frames; latency ~{1000 * li:.1f} ms in + "
              f"{1000 * lo:.1f} ms out")
        last = s.stats()
        for t in range(secs):
            time.sleep(1)
            st = s.stats()
            db = 20 * np.log10(np.maximum(peak, 1e-9))
            print(f"{t + 1:3d}s  {st['frames_in'] - last['frames_in']:6d} frames/s  "
                  f"in peak L {db[0]:6.1f} dB  R {db[1]:6.1f} dB  "
                  f"dropouts {st['input_overflows'] + st['output_underflows']}  "
                  f"usb errors {st['usb_errors']}")
            peak[:] = 0
            last = st
            if not st["running"]:
                print("the unit stopped (unplugged?)")
                return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
