# Builds build/libus122l.dylib and build/us122l-test. Needs libusb
# (brew install libusb) and the Xcode command line tools.
# libusb: pkg-config, else Homebrew (Apple Silicon or Intel)
LIBUSB_PREFIX ?= $(shell brew --prefix libusb 2>/dev/null || ls -d /opt/homebrew/opt/libusb /usr/local/opt/libusb 2>/dev/null | head -1)
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null || echo -I$(LIBUSB_PREFIX)/include/libusb-1.0)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo -L$(LIBUSB_PREFIX)/lib -lusb-1.0)
CFLAGS ?= -O2 -Wall -Wextra -std=c11
override CFLAGS += -Iinclude $(LIBUSB_CFLAGS)

all: build/libus122l.dylib build/us122l-test build/us122l-probe build/us122l-midi coreaudio

build:
	mkdir -p build

build/libus122l.dylib: src/us122l.c include/us122l.h | build
	$(CC) $(CFLAGS) -dynamiclib -install_name @rpath/libus122l.dylib src/us122l.c $(LIBUSB_LIBS) -o $@

build/us122l-test: tools/us122l_test.c build/libus122l.dylib
	$(CC) $(CFLAGS) tools/us122l_test.c -Lbuild -lus122l -Wl,-rpath,@executable_path -o $@

build/us122l-midi: tools/us122l_midi.c build/libus122l.dylib
	$(CC) $(CFLAGS) tools/us122l_midi.c -Lbuild -lus122l -Wl,-rpath,@executable_path -o $@

build/us122l-probe: tools/probe.c | build
	$(CC) $(CFLAGS) tools/probe.c $(LIBUSB_LIBS) -o $@

clean:
	rm -rf build

.PHONY: all clean

# The Core Audio driver: the us122ld daemon (libusb linked in, so it doesn't
# depend on Homebrew at run time) and the US122L.driver plug-in bundle.
# Install with: sudo coreaudio/install.sh
coreaudio: build/us122ld build/US122L.driver build/US122L-MIDI.plugin build/us122l-shmtest build/us122l-miditest

build/us122ld: coreaudio/daemon.c coreaudio/shared.h src/us122l.c include/us122l.h | build
	$(CC) $(CFLAGS) coreaudio/daemon.c src/us122l.c $(LIBUSB_PREFIX)/lib/libusb-1.0.a \
		-framework IOKit -framework CoreFoundation -framework Security -lobjc -o $@
	codesign -f -s - $@

build/US122L.driver: coreaudio/plugin.c coreaudio/client.h coreaudio/shared.h coreaudio/Info.plist | build
	rm -rf $@ && mkdir -p $@/Contents/MacOS
	cp coreaudio/Info.plist $@/Contents/
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -bundle coreaudio/plugin.c \
		-framework CoreAudio -framework CoreFoundation -o $@/Contents/MacOS/US122L
	codesign -f -s - $@

build/US122L-MIDI.plugin: coreaudio/midi_plugin.c coreaudio/client.h coreaudio/shared.h coreaudio/MIDI-Info.plist | build
	rm -rf $@ && mkdir -p $@/Contents/MacOS
	cp coreaudio/MIDI-Info.plist $@/Contents/Info.plist
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -bundle coreaudio/midi_plugin.c \
		-framework CoreMIDI -framework CoreFoundation -o $@/Contents/MacOS/US122LMIDI
	codesign -f -s - $@

build/us122l-miditest: coreaudio/miditest.c coreaudio/client.h coreaudio/shared.h | build
	$(CC) $(CFLAGS) coreaudio/miditest.c -framework CoreMIDI -framework CoreFoundation -o $@

build/us122l-shmtest: coreaudio/shmtest.c coreaudio/client.h coreaudio/shared.h | build
	$(CC) $(CFLAGS) coreaudio/shmtest.c -o $@

.PHONY: coreaudio
