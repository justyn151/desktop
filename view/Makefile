CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS_DIR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PACKAGES = wayland-client xkbcommon libpng libjpeg
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
CFLAGS ?= -O2
VIEW_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -DDESK_WAYLAND -DDESK_IMAGE -I$(BUILD) -I$(DESK)

DESK = ../desk
BUILD = .build
P = $(WAYLAND_PROTOCOLS_DIR)
XDG_SHELL_XML = $(P)/stable/xdg-shell/xdg-shell.xml
VIEWPORTER_XML = $(P)/stable/viewporter/viewporter.xml
FRACTIONAL_XML = $(P)/staging/fractional-scale/fractional-scale-v1.xml

NAMES = xdg-shell viewporter fractional-scale-v1
HEADERS = $(NAMES:%=$(BUILD)/%-client-protocol.h)
PROTOCOLS = $(NAMES:%=$(BUILD)/%-protocol.c)
OBJECTS = $(BUILD)/view.o $(BUILD)/desk.o $(PROTOCOLS:.c=.o)

all: view

view: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) -lm $(LDLIBS)

$(BUILD)/view.o: view.c view.h $(HEADERS) $(DESK)/desk.h
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(VIEW_CFLAGS) -c -o $@ view.c

$(BUILD)/desk.o: $(DESK)/desk.c $(DESK)/desk.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(VIEW_CFLAGS) -c -o $@ $(DESK)/desk.c

$(BUILD)/%-protocol.o: $(BUILD)/%-protocol.c
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) -std=c11 -c -o $@ $<

$(BUILD):
	mkdir -p $@

$(BUILD)/xdg-shell-client-protocol.h $(BUILD)/xdg-shell-protocol.c: $(XDG_SHELL_XML)
$(BUILD)/viewporter-client-protocol.h $(BUILD)/viewporter-protocol.c: $(VIEWPORTER_XML)
$(BUILD)/fractional-scale-v1-client-protocol.h $(BUILD)/fractional-scale-v1-protocol.c: $(FRACTIONAL_XML)

$(HEADERS): | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTOCOLS): | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@

clean:
	rm -f view
	rm -rf $(BUILD)

install: view
	install -Dm755 view "$(DESTDIR)$(BINDIR)/view"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/view"

.PHONY: all clean install uninstall
