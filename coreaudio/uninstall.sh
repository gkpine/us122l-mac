#!/bin/sh
# Removes what install.sh put in place:  sudo coreaudio/uninstall.sh
if [ "$(id -u)" != 0 ]; then
    echo "Run it with sudo: sudo $0"
    exit 1
fi
launchctl bootout system/org.us122l-mac.daemon 2>/dev/null || true
rm -f /Library/LaunchDaemons/org.us122l-mac.daemon.plist
rm -rf "/Library/Application Support/us122l-mac" /Library/Audio/Plug-Ins/HAL/US122L.driver \
    "/Library/Audio/MIDI Drivers/US122L-MIDI.plugin"
killall coreaudiod 2>/dev/null || true
killall MIDIServer 2>/dev/null || true
echo "Removed the US-122L Core Audio and MIDI drivers."
