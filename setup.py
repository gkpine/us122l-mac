"""Builds the driver into the us122l Python package (pip install .)."""
import os
import shutil
import subprocess

from setuptools import Extension, setup


def libusb_prefix():
    candidates = [os.environ.get("LIBUSB_PREFIX"), "/opt/homebrew/opt/libusb", "/usr/local/opt/libusb"]
    if shutil.which("brew"):
        try:
            candidates.insert(1, subprocess.check_output(["brew", "--prefix", "libusb"], text=True).strip())
        except subprocess.CalledProcessError:
            pass
    for p in candidates:
        if p and os.path.exists(os.path.join(p, "include", "libusb-1.0", "libusb.h")):
            return p
    raise SystemExit("libusb not found: install it with `brew install libusb` "
                     "(or set LIBUSB_PREFIX)")


prefix = libusb_prefix()
setup(ext_modules=[Extension(
    "us122l._driver", ["src/us122l.c"],
    include_dirs=["include", os.path.join(prefix, "include", "libusb-1.0")],
    library_dirs=[os.path.join(prefix, "lib")],
    libraries=["usb-1.0"],
    extra_compile_args=["-O2", "-std=c11"],
)])
