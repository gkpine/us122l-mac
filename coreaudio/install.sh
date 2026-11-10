#!/bin/sh
# Installs the TASCAM US-122L Core Audio driver, so the unit shows up in every
# app (GarageBand, Logic, Music, Zoom…). Build first (`make`), then:
#     sudo coreaudio/install.sh
# Puts the us122ld daemon in /Library/Application Support/us122l-mac, its
# launchd job in /Library/LaunchDaemons and the plug-in in
# /Library/Audio/Plug-Ins/HAL and its MIDI driver in /Library/Audio/MIDI Drivers,
# then restarts Core Audio and Core MIDI (sound stops for a second). Undo with coreaudio/uninstall.sh.
set -e
cd "$(dirname "$0")/.."
if [ "$(id -u)" != 0 ]; then
    echo "Run it with sudo: sudo $0"
    exit 1
fi
if [ ! -x build/us122ld ] || [ ! -d build/US122L.driver ] || [ ! -d build/US122L-MIDI.plugin ]; then
    echo "Build it first: make"
    exit 1
fi
DIR="/Library/Application Support/us122l-mac"
PLIST=/Library/LaunchDaemons/org.us122l-mac.daemon.plist
HAL=/Library/Audio/Plug-Ins/HAL
MIDI="/Library/Audio/MIDI Drivers"

launchctl bootout system/org.us122l-mac.daemon 2>/dev/null || true
mkdir -p "$DIR" "$HAL"
install -m 755 -o root -g wheel build/us122ld "$DIR/us122ld"
rm -rf "$HAL/US122L.driver"
cp -R build/US122L.driver "$HAL/"
chown -R root:wheel "$HAL/US122L.driver"
mkdir -p "$MIDI"
rm -rf "$MIDI/US122L-MIDI.plugin"
cp -R build/US122L-MIDI.plugin "$MIDI/"
chown -R root:wheel "$MIDI/US122L-MIDI.plugin"
cat > "$PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key><string>org.us122l-mac.daemon</string>
	<key>ProgramArguments</key><array><string>$DIR/us122ld</string></array>
	<key>MachServices</key><dict><key>org.us122l-mac.daemon</key><true/></dict>
	<key>RunAtLoad</key><true/>
	<key>KeepAlive</key><true/>
	<key>ProcessType</key><string>Interactive</string>
	<key>StandardErrorPath</key><string>/Library/Logs/us122ld.log</string>
</dict>
</plist>
PLIST
chown root:wheel "$PLIST"
chmod 644 "$PLIST"
launchctl bootstrap system "$PLIST"
echo "Restarting Core Audio and Core MIDI…"
killall coreaudiod 2>/dev/null || true
killall MIDIServer 2>/dev/null || true
sleep 2
echo "Installed. The TASCAM US-122L should now be in Sound settings and in your apps."
echo "Log: /Library/Logs/us122ld.log"
