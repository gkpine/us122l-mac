# Builds build/libus122l.dylib and build/us122l-test. Needs libusb
# (brew install libusb) and the Xcode command line tools.
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null || echo -I/opt/homebrew/opt/libusb/include/libusb-1.0)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo -L/opt/homebrew/opt/libusb/lib -lusb-1.0)
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CFLAGS += -Iinclude $(LIBUSB_CFLAGS)

all: build/libus122l.dylib build/us122l-test build/us122l-probe

build:
	mkdir -p build

build/libus122l.dylib: src/us122l.c include/us122l.h | build
	$(CC) $(CFLAGS) -dynamiclib -install_name @rpath/libus122l.dylib src/us122l.c $(LIBUSB_LIBS) -o $@

build/us122l-test: tools/us122l_test.c build/libus122l.dylib
	$(CC) $(CFLAGS) tools/us122l_test.c -Lbuild -lus122l -Wl,-rpath,@executable_path -o $@

build/us122l-probe: tools/probe.c | build
	$(CC) $(CFLAGS) tools/probe.c $(LIBUSB_LIBS) -o $@

clean:
	rm -rf build

.PHONY: all clean
