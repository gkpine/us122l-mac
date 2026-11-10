#!/bin/sh
# Installs the TASCAM US-122L driver for Mac in one go:
#     sh install.sh
# Checks what's needed, builds the driver, then installs it (your Mac
# password is asked for once). Run it again any time to update.
set -e
cd "$(dirname "$0")"

say() { printf '\n==> %s\n' "$1"; }

# Apple's command line tools (the compiler)
if ! xcode-select -p >/dev/null 2>&1; then
    say "Installing Apple's Command Line Tools"
    echo "A window will open: click Install and wait for it to finish."
    echo "Then run this installer again:  sh install.sh"
    xcode-select --install || true
    exit 1
fi

# Homebrew (to get libusb, the USB library the driver is built with)
BREW=$(command -v brew || true)
[ -z "$BREW" ] && [ -x /opt/homebrew/bin/brew ] && BREW=/opt/homebrew/bin/brew
[ -z "$BREW" ] && [ -x /usr/local/bin/brew ] && BREW=/usr/local/bin/brew
if [ -z "$BREW" ]; then
    say "Homebrew is needed"
    echo "Install it by following the instructions at https://brew.sh"
    echo "Then run this installer again:  sh install.sh"
    exit 1
fi
if ! "$BREW" list libusb >/dev/null 2>&1; then
    say "Installing libusb with Homebrew"
    "$BREW" install libusb
fi

say "Building the driver"
make coreaudio LIBUSB_PREFIX="$("$BREW" --prefix libusb)"

say "Installing (enter your Mac password when asked; it won't show as you type)"
sudo sh coreaudio/install.sh
