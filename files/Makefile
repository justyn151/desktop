CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS_DIR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PACKAGES = wayland-client xkbcommon freetype2 fontconfig libpng libjpeg libwebp
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
CFLAGS ?= -O2
FILES_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -DDESK_WAYLAND -DDESK_WINDOW -DDESK_KEYBOARD -DDESK_FONT \
	-DDESK_IMAGE -DDESK_IMAGE_GIF -DDESK_IMAGE_WEBP -pthread \
	-I$(BUILD) -I$(DESK)

DESK = ../desk
BUILD = .build
P = $(WAYLAND_PROTOCOLS_DIR)
XDG_SHELL_XML = $(P)/stable/xdg-shell/xdg-shell.xml
VIEWPORTER_XML = $(P)/stable/viewporter/viewporter.xml
FRACTIONAL_XML = $(P)/staging/fractional-scale/fractional-scale-v1.xml

NAMES = xdg-shell viewporter fractional-scale-v1
HEADERS = $(NAMES:%=$(BUILD)/%-client-protocol.h)
PROTOCOLS = $(NAMES:%=$(BUILD)/%-protocol.c)
OBJECTS = $(BUILD)/files.o $(BUILD)/desk.o $(PROTOCOLS:.c=.o)

all: files

files: $(OBJECTS)
	$(CC) $(LDFLAGS) -pthread -o $@ $(OBJECTS) $(PKG_LIBS) -lm $(LDLIBS)

$(BUILD)/files.o: files.c files.h $(HEADERS) $(DESK)/desk.h
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(FILES_CFLAGS) -c -o $@ files.c

$(BUILD)/desk.o: $(DESK)/desk.c $(DESK)/desk.h $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(FILES_CFLAGS) -c -o $@ $(DESK)/desk.c

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
	rm -f files
	rm -rf $(BUILD)

install: files
	install -Dm755 files "$(DESTDIR)$(BINDIR)/files"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/files"

.PHONY: all clean install uninstall
