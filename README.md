# TASCAM US-122L driver for Mac

TASCAM stopped making Mac drivers for the US-122L years ago, so on a modern
Mac, including Apple Silicon (M1 and later), the interface isn't recognized.

This free driver brings it back. Once it's installed, your US-122L shows up
as a normal audio interface and MIDI interface in every app on your Mac, just
like it did when it was new.

## What works

- **Recording** from both inputs (mic, line or guitar), as a stereo pair or
  as two separate tracks
- **Playback** through the line outputs and the headphone jack
- **Any app** that uses your Mac's normal audio settings: GarageBand, Logic
  Pro, Ableton Live, Reaper, Audacity, Zoom, Music, System Settings → Sound
  and so on
- **Sample rates** of 44.1, 48, 88.2 and 96 kHz, at 24-bit, chosen by your
  app as usual
- **MIDI In and MIDI Out**, so you can play a keyboard into your music app or
  control a synth from it*
- **Plug and unplug** any time. It picks up again after your Mac wakes from
  sleep.

\* MIDI has been tested from the Mac to the unit and back over USB. It
hasn't yet been tried with a keyboard or synth plugged into the MIDI sockets.
If you try it, please share how it goes.

## What doesn't work (yet)

- **Your Mac's volume slider and volume keys** don't control the US-122L. Use
  the knobs on the unit instead.
- **More than one US-122L** connected at the same time.
- **Some USB hubs and adapters.** A standard USB-C to USB adapter works fine
  (that's how it was tested). If the unit isn't found through a hub, plug it
  straight into your Mac, or through a simple adapter like that.
- **Other TASCAM models.** The US-122MKII will probably work but hasn't been
  tested. The original US-122 (without the "L") and the US-144 aren't
  supported.
- **Intel Macs** should work but haven't been tested.

It has been tested on a MacBook Air (M1) running macOS 26 Tahoe.

## What you need

- A Mac with **macOS 11 Big Sur or newer**
- Your Mac's **admin password**
- An **internet connection**: the first install downloads two free tools,
  Apple's Command Line Tools and Homebrew
- About **15 minutes** the first time

## How to install

1. **Download the driver.** At the top of this page, click the green
   **Code** button, then **Download ZIP**.
2. **Unzip it.** Open your **Downloads** folder and double-click the ZIP
   file. You get a folder called **us122l-mac-main**.
3. **Plug in your US-122L** with its USB cable.
4. **Open Terminal.** Press **⌘ Space**, type **Terminal** and press
   **Return**.
5. **Go to the driver's folder.** Copy this line into Terminal and press
   **Return**:

   ```
   cd ~/Downloads/us122l-mac-main
   ```

6. **Run the installer.** Copy this line into Terminal and press **Return**:

   ```
   sh install.sh
   ```

   The installer checks what your Mac needs and walks you through it:

   - **If a window asks to install the Command Line Tools**, click
     **Install** and wait for it to finish. Then run `sh install.sh` again.
   - **If it says Homebrew is needed**, go to [brew.sh](https://brew.sh),
     copy the install line shown there into Terminal and follow the steps.
     Homebrew prints a few "Next steps" lines at the end: run those as well.
     Then run `sh install.sh` again.
   - **When it asks for your password**, type your Mac password and press
     **Return**. Nothing shows on screen while you type; that's normal.

7. **You're done** when Terminal says **Installed.** Your Mac's sound cuts
   out for a second while the driver loads.

## How to use it

- **For the whole Mac:** open **System Settings → Sound** and choose
  **TASCAM US-122L** under Output and Input.
- **In GarageBand:** go to **GarageBand → Settings → Audio/MIDI** and choose
  **TASCAM US-122L** as the Output Device and Input Device.
- **In Logic Pro:** go to **Logic Pro → Settings → Audio → Devices**.
- **In other apps:** look for the audio device setting in the app's settings.
- **For MIDI:** your keyboard or synth appears as **TASCAM US-122L** in your
  music app's MIDI settings, and in **Audio MIDI Setup** (in Applications →
  Utilities).

## If something goes wrong

- **TASCAM US-122L isn't in the list:**
  - Check the unit is plugged straight into your Mac and its USB light is on.
  - Unplug it, plug it back in and wait a few seconds.
  - If it's still missing, restart your Mac.
- **Crackles or dropouts:** raise the buffer size (sometimes called I/O
  buffer) in your app's audio settings.
- **It stopped working after a macOS update:** run `sh install.sh` again from
  the driver's folder.
- **Reporting a problem:** please include the file `/Library/Logs/us122ld.log`.
  To open it, press **⌘ Shift G** in Finder and paste that path.

## Updating or removing it

- **To update:** download the latest ZIP as in steps 1–2, then run
  `sh install.sh` from the new folder.
- **To remove:** go to the driver's folder in Terminal (step 5) and run
  `sh uninstall.sh`.

## Is it safe?

- The driver is built on your own Mac from the source code in this
  repository, which anyone can read.
- It doesn't change your Mac's security settings, and it doesn't need a
  kernel extension.
- It installs three small pieces:
  - a background helper that talks to the US-122L over USB
  - an audio plug-in that shows the unit to your apps
  - a MIDI plug-in that does the same for MIDI
- `sh uninstall.sh` removes all three.

## For developers

How it works, how to build the parts by hand, and a C and Python library for
talking to the unit directly: see [docs/TECHNICAL.md](docs/TECHNICAL.md).

Free software under the GPL-2.0-or-later licence (see [LICENSE](LICENSE)).
The USB protocol comes from the Linux driver by Karsten Wiese. This project is
not affiliated with or supported by TASCAM.
