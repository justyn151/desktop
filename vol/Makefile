CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CFLAGS ?= -O2
VOL_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -I$(DESK) $(shell $(PKG_CONFIG) --cflags libpulse)
VOL_LIBS = $(shell $(PKG_CONFIG) --libs libpulse) -lm

# desk's Layer 0 only: config paths and the INI parser.
DESK = ../desk
BUILD = .build
OBJECTS = $(BUILD)/vol.o $(BUILD)/desk.o

all: vol

vol: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(VOL_LIBS) $(LDLIBS)

$(BUILD)/vol.o: vol.c vol.h $(DESK)/desk.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VOL_CFLAGS) -c -o $@ vol.c

$(BUILD)/desk.o: $(DESK)/desk.c $(DESK)/desk.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VOL_CFLAGS) -c -o $@ $(DESK)/desk.c

$(BUILD):
	mkdir -p $@

clean:
	rm -f vol
	rm -rf $(BUILD)

install: vol
	install -Dm755 vol "$(DESTDIR)$(BINDIR)/vol"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/vol"

.PHONY: all clean install uninstall
