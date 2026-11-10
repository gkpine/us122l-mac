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
import threading
import time
import traceback

import numpy as np

__all__ = ["present", "version", "Stream", "Us122lError", "RATES", "CallbackFlags",
           "CallbackStop", "CallbackAbort"]

RATES = (44100, 48000, 88200, 96000)
INPUT_OVERFLOW = 1
OUTPUT_UNDERFLOW = 2


class Us122lError(RuntimeError):
    pass


class CallbackStop(Exception):
    """Raise in the callback to stop the stream (like sounddevice's)."""


class CallbackAbort(Exception):
    """Raise in the callback to stop the stream at once (like sounddevice's)."""


# sounddevice's own CallbackStop / CallbackAbort work too (matched by name, so
# sounddevice needn't be installed)
_STOP_NAMES = {"CallbackStop", "CallbackAbort"}


class CallbackFlags:
    """The status argument of the callback, like sounddevice.CallbackFlags:
    true if anything went wrong since the last callback."""

    __slots__ = ("_flags",)

    def __init__(self, flags=0):
        self._flags = flags

    input_overflow = property(lambda self: bool(self._flags & INPUT_OVERFLOW))
    output_underflow = property(lambda self: bool(self._flags & OUTPUT_UNDERFLOW))
    input_underflow = property(lambda self: False)
    output_overflow = property(lambda self: False)
    priming_output = property(lambda self: False)

    def __bool__(self):
        return bool(self._flags)

    def __int__(self):
        return self._flags

    def __repr__(self):
        names = [n for n in ("input_overflow", "output_underflow") if getattr(self, n)]
        return f"<CallbackFlags: {', '.join(names) or 'none'}>"


class _Time:
    """The time argument of the callback (seconds, time.monotonic's clock)."""

    __slots__ = ("currentTime", "inputBufferAdcTime", "outputBufferDacTime")

    def __init__(self, now, latency_in, latency_out):
        self.currentTime = now
        self.inputBufferAdcTime = now - latency_in
        self.outputBufferDacTime = now + latency_out


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
                ("usb_errors", ctypes.c_uint), ("running", ctypes.c_int),
                ("stalled", ctypes.c_int)]


_lib = _load()


def present():
    """True if a US-122L is plugged in."""
    return bool(_lib.us122l_present())


def version():
    return _lib.us122l_version().decode()


class Stream:
    """A duplex stream on the US-122L: 2 channels in, 2 out, float32.

    If the callback raises, the stream goes silent and inactive and the
    exception is kept in .error (CallbackStop / CallbackAbort just stop it).
    While it runs, Python's thread switch interval is lowered to 1 ms so the
    audio thread gets the GIL in time (restored on stop)."""

    channels = (2, 2)
    dtype = "float32"

    def __init__(self, samplerate=48000, blocksize=256, callback=None):
        if samplerate not in RATES:
            raise Us122lError(f"the US-122L runs at {', '.join(map(str, RATES))} Hz, not {samplerate}")
        self.samplerate = int(samplerate)
        self.blocksize = int(blocksize)
        self.callback = callback
        self.error = None            # the exception that stopped the callback, if any
        self._dev = None
        self._done = False           # the callback stopped (raised)
        self._lock = threading.Lock()
        self._latency = (0.0, 0.0)
        self._old_switch = None
        self._cb = _CALLBACK(self._run)     # kept alive as long as the stream

    def _run(self, inp, out, frames, status, _user):
        outdata = np.ctypeslib.as_array(out, shape=(frames, 2))
        if self._done or self.callback is None:
            outdata.fill(0)
            return
        try:
            indata = np.ctypeslib.as_array(inp, shape=(frames, 2))
            t = _Time(time.monotonic(), *self._latency)
            self.callback(indata, outdata, frames, t, CallbackFlags(status))
        except BaseException as err:    # nothing may escape into C
            outdata.fill(0)
            self._done = True
            if type(err).__name__ not in _STOP_NAMES:
                self.error = err
                traceback.print_exc(file=sys.stderr)

    def start(self):
        with self._lock:
            if self._dev:
                return
            self.error = None
            self._done = False
            if self._old_switch is None:
                self._old_switch = sys.getswitchinterval()
                sys.setswitchinterval(min(self._old_switch, 0.001))
            err = ctypes.create_string_buffer(256)
            dev = _lib.us122l_open(self.samplerate, self.blocksize, self._cb, None, err, len(err))
            if not dev:
                self._restore_switch()
                raise Us122lError(err.value.decode() or "can't open the US-122L")
            self._dev = dev
            self._latency = self._read_latency()

    def stop(self):
        """Stop and release the unit (also fine from inside the callback)."""
        # take the handle under the lock (so stats() can't use it while it's
        # freed), but close outside it: the callback may be calling stats()
        with self._lock:
            dev, self._dev = self._dev, None
        if dev:
            _lib.us122l_close(dev)
            self._restore_switch()

    def _restore_switch(self):
        if self._old_switch is not None:
            sys.setswitchinterval(self._old_switch)
            self._old_switch = None

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
        with self._lock:
            if not self._dev:
                return {}
            s = _Stats()
            _lib.us122l_get_stats(self._dev, ctypes.byref(s))
        return {name: getattr(s, name) for name, _ in _Stats._fields_}

    def _read_latency(self):
        s = _Stats()
        _lib.us122l_get_stats(self._dev, ctypes.byref(s))
        return (s.input_latency / self.samplerate, s.output_latency / self.samplerate)

    @property
    def active(self):
        """True while streaming: False once stopped, unplugged, stalled (the
        Mac slept), or after the callback raised."""
        return not self._done and bool(self.stats().get("running"))

    @property
    def stalled(self):
        """True if the unit stopped sending audio (close and start again)."""
        return bool(self.stats().get("stalled"))

    @property
    def latency(self):
        """(input, output) latency in seconds, estimated (output grows a little
        after dropouts and shrinks back)."""
        s = self.stats()
        if not s:
            return (0.0, 0.0)
        self._latency = (s["input_latency"] / self.samplerate,
                         s["output_latency"] / self.samplerate)
        return self._latency
