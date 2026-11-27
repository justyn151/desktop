CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
WLR_PROTOCOLS_DIR ?= /usr/share/wlr-protocols
WAYLAND_PROTOCOLS_DIR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PACKAGES = wayland-client wayland-cursor libpng libjpeg freetype2 fontconfig xkbcommon
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
CFLAGS ?= -O2
WALL_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -DDESK_IMAGE -I$(DESK)
DESK = ../desk

LAYER_XML = $(WLR_PROTOCOLS_DIR)/unstable/wlr-layer-shell-unstable-v1.xml
XDG_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
FRACTIONAL_XML = $(WAYLAND_PROTOCOLS_DIR)/staging/fractional-scale/fractional-scale-v1.xml
VIEWPORTER_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/viewporter/viewporter.xml
PROTOCOLS = layer-shell-protocol.c xdg-shell-protocol.c fractional-scale-protocol.c viewporter-protocol.c
HEADERS = layer-shell-client-protocol.h xdg-shell-client-protocol.h fractional-scale-client-protocol.h viewporter-client-protocol.h
OBJECTS = wall.o desk.o $(PROTOCOLS:.c=.o)

all: wall

wall: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) -lm $(LDLIBS)

wall.o: wall.c wall.h $(HEADERS) default-config.h $(DESK)/desk.h

desk.o: $(DESK)/desk.c $(DESK)/desk.h
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WALL_CFLAGS) -c -o $@ $(DESK)/desk.c

%.o: %.c
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(WALL_CFLAGS) -c -o $@ $<

layer-shell-client-protocol.h: $(LAYER_XML)
xdg-shell-client-protocol.h: $(XDG_XML)
fractional-scale-client-protocol.h: $(FRACTIONAL_XML)
viewporter-client-protocol.h: $(VIEWPORTER_XML)

$(HEADERS):
	$(WAYLAND_SCANNER) client-header $< $@

layer-shell-protocol.c: $(LAYER_XML)
xdg-shell-protocol.c: $(XDG_XML) # Provides the xdg_popup interface used by layer-shell.
fractional-scale-protocol.c: $(FRACTIONAL_XML)
viewporter-protocol.c: $(VIEWPORTER_XML)

$(PROTOCOLS):
	$(WAYLAND_SCANNER) private-code $< $@

default-config.h: wall.conf Makefile
	LC_ALL=C od -An -v -tu1 "$<" | awk '{ for (i = 1; i <= NF; ++i) printf "%s,", $$i; print "" }' > "$@.tmp"
	mv -f "$@.tmp" "$@"

clean:
	rm -f wall $(OBJECTS) $(PROTOCOLS) $(HEADERS) default-config.h default-config.h.tmp

install: wall
	install -Dm755 wall "$(DESTDIR)$(BINDIR)/wall"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/wall"

.PHONY: all clean install uninstall
