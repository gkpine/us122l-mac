"""Python binding for us122l-mac, the userspace TASCAM US-122L driver.

    import us122l
    if us122l.present():
        def callback(indata, outdata, frames, time, status):
            outdata[:] = indata                  # (frames, 2) float32 each
        s = us122l.Stream(samplerate=48000, blocksize=128, callback=callback)
        s.start()

The callback has sounddevice's signature, so code written for a
sounddevice.Stream with 2 in / 2 out float32 channels works unchanged.
It runs on the driver's real-time thread: keep it quick.
"""
import ctypes
import glob
import os
import sys
import traceback

import numpy as np

__all__ = ["present", "version", "Stream", "Us122lError", "RATES"]

RATES = (44100, 48000, 88200, 96000)
INPUT_OVERFLOW = 1
OUTPUT_UNDERFLOW = 2


class Us122lError(RuntimeError):
    pass


def _load():
    here = os.path.dirname(os.path.abspath(__file__))
    found = (glob.glob(os.path.join(here, "_driver*.so")) +
             glob.glob(os.path.join(here, "libus122l.dylib")))
    if not found:
        raise ImportError("us122l: the driver library isn't built (pip install the us122l-mac project)")
    lib = ctypes.CDLL(found[0])
    lib.us122l_present.restype = ctypes.c_int
    lib.us122l_version.restype = ctypes.c_char_p
    lib.us122l_open.restype = ctypes.c_void_p
    lib.us122l_open.argtypes = [ctypes.c_uint, ctypes.c_uint, _CALLBACK, ctypes.c_void_p,
                                ctypes.c_char_p, ctypes.c_size_t]
    lib.us122l_close.argtypes = [ctypes.c_void_p]
    lib.us122l_get_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Stats)]
    return lib


_CALLBACK = ctypes.CFUNCTYPE(None, ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                             ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p)


class _Stats(ctypes.Structure):
    _fields_ = [("rate", ctypes.c_uint), ("blocksize", ctypes.c_uint),
                ("input_latency", ctypes.c_uint), ("output_latency", ctypes.c_uint),
                ("frames_in", ctypes.c_uint64), ("frames_out", ctypes.c_uint64),
                ("input_overflows", ctypes.c_uint), ("output_underflows", ctypes.c_uint),
                ("usb_errors", ctypes.c_uint), ("running", ctypes.c_int)]


_lib = _load()


def present():
    """True if a US-122L is plugged in."""
    return bool(_lib.us122l_present())


def version():
    return _lib.us122l_version().decode()


class Stream:
    """A duplex stream on the US-122L: 2 channels in, 2 out, float32."""

    channels = (2, 2)
    dtype = "float32"

    def __init__(self, samplerate=48000, blocksize=256, callback=None):
        if samplerate not in RATES:
            raise Us122lError(f"the US-122L runs at {', '.join(map(str, RATES))} Hz, not {samplerate}")
        self.samplerate = int(samplerate)
        self.blocksize = int(blocksize)
        self.callback = callback
        self.error = None            # the first exception the callback raised
        self._dev = None
        self._cb = _CALLBACK(self._run)     # kept alive as long as the stream

    def _run(self, inp, out, frames, status, _user):
        outdata = np.ctypeslib.as_array(out, shape=(frames, 2))
        try:
            indata = np.ctypeslib.as_array(inp, shape=(frames, 2))
            if self.callback is None:
                outdata.fill(0)
            else:
                self.callback(indata, outdata, frames, None, status)
        except Exception as err:      # an exception must not escape into C
            outdata.fill(0)
            if self.error is None:
                self.error = err
                traceback.print_exc(file=sys.stderr)

    def start(self):
        if self._dev:
            return
        err = ctypes.create_string_buffer(256)
        dev = _lib.us122l_open(self.samplerate, self.blocksize, self._cb, None, err, len(err))
        if not dev:
            raise Us122lError(err.value.decode() or "can't open the US-122L")
        self._dev = dev

    def stop(self):
        if self._dev:
            dev, self._dev = self._dev, None
            _lib.us122l_close(dev)

    close = stop

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    def __del__(self):
        try:
            self.stop()
        except Exception:
            pass

    def stats(self):
        """Counters from the driver, as a dict (empty when stopped)."""
        if not self._dev:
            return {}
        s = _Stats()
        _lib.us122l_get_stats(self._dev, ctypes.byref(s))
        return {name: getattr(s, name) for name, _ in _Stats._fields_}

    @property
    def active(self):
        """True while streaming (False once stopped, unplugged or failed)."""
        return bool(self._dev) and bool(self.stats().get("running"))

    @property
    def latency(self):
        """(input, output) latency in seconds, estimated."""
        s = self.stats()
        if not s:
            return (0.0, 0.0)
        return (s["input_latency"] / self.samplerate, s["output_latency"] / self.samplerate)
