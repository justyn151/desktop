CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CFLAGS ?= -O2
BLUE_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -I$(DESK) $(shell $(PKG_CONFIG) --cflags libsystemd)
BLUE_LIBS = $(shell $(PKG_CONFIG) --libs libsystemd)

# desk's Layer 0 only: config paths, the INI parser, the palette and its watch.
DESK = ../desk
BUILD = .build
OBJECTS = $(BUILD)/blue.o $(BUILD)/desk.o

all: blue

blue: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(BLUE_LIBS) $(LDLIBS)

$(BUILD)/blue.o: blue.c blue.h $(DESK)/desk.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BLUE_CFLAGS) -c -o $@ blue.c

$(BUILD)/desk.o: $(DESK)/desk.c $(DESK)/desk.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BLUE_CFLAGS) -c -o $@ $(DESK)/desk.c

$(BUILD):
	mkdir -p $@

clean:
	rm -f blue
	rm -rf $(BUILD)

install: blue
	install -Dm755 blue "$(DESTDIR)$(BINDIR)/blue"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/blue"

.PHONY: all clean install uninstall
