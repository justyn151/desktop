CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS_DIR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share

PACKAGES = libpulse wayland-client xkbcommon freetype2 fontconfig
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
CFLAGS ?= -O2
# desk's Layer 0 (config, INI, palette) and the window layers: Wayland, the
# window, the keyboard and fonts.
VOL_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -DDESK_WAYLAND -DDESK_WINDOW -DDESK_KEYBOARD -DDESK_FONT \
	-I$(BUILD) -I$(DESK)

DESK = ../desk
BUILD = .build
P = $(WAYLAND_PROTOCOLS_DIR)
XDG_SHELL_XML = $(P)/stable/xdg-shell/xdg-shell.xml
VIEWPORTER_XML = $(P)/stable/viewporter/viewporter.xml
FRACTIONAL_XML = $(P)/staging/fractional-scale/fractional-scale-v1.xml
CURSOR_SHAPE_XML = $(P)/staging/cursor-shape/cursor-shape-v1.xml
# cursor-shape references zwp_tablet_tool_v2.
TABLET_XML = $(P)/stable/tablet/tablet-v2.xml

NAMES = xdg-shell viewporter fractional-scale-v1 cursor-shape-v1 tablet-v2
HEADERS = $(NAMES:%=$(BUILD)/%-client-protocol.h)
PROTOCOLS = $(NAMES:%=$(BUILD)/%-protocol.c)
OBJECTS = $(BUILD)/vol.o $(BUILD)/desk.o $(PROTOCOLS:.c=.o)

all: vol

vol: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) -lm $(LDLIBS)

$(BUILD)/vol.o: vol.c vol.h $(HEADERS) $(DESK)/desk.h
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(VOL_CFLAGS) -c -o $@ vol.c

$(BUILD)/desk.o: $(DESK)/desk.c $(DESK)/desk.h $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(VOL_CFLAGS) -c -o $@ $(DESK)/desk.c

$(BUILD)/%-protocol.o: $(BUILD)/%-protocol.c
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) -std=c11 -c -o $@ $<

$(BUILD):
	mkdir -p $@

$(BUILD)/xdg-shell-client-protocol.h $(BUILD)/xdg-shell-protocol.c: $(XDG_SHELL_XML)
$(BUILD)/viewporter-client-protocol.h $(BUILD)/viewporter-protocol.c: $(VIEWPORTER_XML)
$(BUILD)/fractional-scale-v1-client-protocol.h $(BUILD)/fractional-scale-v1-protocol.c: $(FRACTIONAL_XML)
$(BUILD)/cursor-shape-v1-client-protocol.h $(BUILD)/cursor-shape-v1-protocol.c: $(CURSOR_SHAPE_XML)
$(BUILD)/tablet-v2-client-protocol.h $(BUILD)/tablet-v2-protocol.c: $(TABLET_XML)

$(HEADERS): | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTOCOLS): | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@

clean:
	rm -f vol
	rm -rf $(BUILD)

install: vol
	install -Dm755 vol "$(DESTDIR)$(BINDIR)/vol"
	install -Dm644 vol.desktop "$(DESTDIR)$(DATADIR)/applications/vol.desktop"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/vol" "$(DESTDIR)$(DATADIR)/applications/vol.desktop"

.PHONY: all clean install uninstall
