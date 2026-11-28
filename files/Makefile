CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS_DIR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PACKAGES = wayland-client xkbcommon freetype2 fontconfig libpng libjpeg libwebp libsystemd
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
CFLAGS ?= -O2
FILES_CFLAGS = -std=c11 -pthread -Wall -Wextra -Wpedantic -DDESK_WAYLAND -DDESK_WINDOW -DDESK_KEYBOARD -DDESK_FONT -DDESK_APPS -DDESK_IMAGE -DDESK_IMAGE_GIF -DDESK_IMAGE_WEBP \
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
OBJECTS = $(BUILD)/files.o $(BUILD)/portal.o $(BUILD)/desk.o $(PROTOCOLS:.c=.o)

all: files

files: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) -lm -pthread $(LDLIBS)

$(BUILD)/files.o: files.c files.h $(HEADERS) $(DESK)/desk.h $(BUILD)/default-config.h
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(FILES_CFLAGS) -c -o $@ files.c

$(BUILD)/portal.o: portal.c files.h $(HEADERS) $(DESK)/desk.h
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(FILES_CFLAGS) -c -o $@ portal.c

$(BUILD)/desk.o: $(DESK)/desk.c $(DESK)/desk.h $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(FILES_CFLAGS) -c -o $@ $(DESK)/desk.c

$(BUILD)/%-protocol.o: $(BUILD)/%-protocol.c
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) -std=c11 -c -o $@ $<

$(BUILD):
	mkdir -p $@

# The sample config, embedded so files can write it when the user has none.
$(BUILD)/default-config.h: files.conf | $(BUILD)
	LC_ALL=C od -An -v -tu1 files.conf | awk '{ for (i = 1; i <= NF; ++i) printf "%s,", $$i; print "" }' > "$@.tmp"
	mv -f "$@.tmp" "$@"

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
	rm -f files
	rm -rf $(BUILD)

DATADIR ?= $(PREFIX)/share

install: files
	install -Dm755 files "$(DESTDIR)$(BINDIR)/files"
	install -Dm644 files.desktop "$(DESTDIR)$(DATADIR)/applications/files.desktop"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/files" "$(DESTDIR)$(DATADIR)/applications/files.desktop"

# Make files the default for folders, for the user running make, so other
# applications' "show in folder" opens it.
default:
	@if command -v xdg-mime >/dev/null; then \
		xdg-mime default files.desktop inode/directory && echo "files is the default for inode/directory"; \
	else \
		echo "xdg-mime isn't installed. Add this line under [Default Applications] in ~/.config/mimeapps.list:"; \
		echo "inode/directory=files.desktop"; \
	fi

# The file dialog for other applications, through xdg-desktop-portal: the
# D-Bus service (activated on demand), the portal description, and
# dwl-portals.conf sending file dialogs to files. Portal descriptions are
# only read from the system's data directory, so this needs root for that
# part: make portal PREFIX=/usr/local PORTALDIR=/usr/share/xdg-desktop-portal/portals
DBUSDIR ?= $(DATADIR)/dbus-1/services
PORTALDIR ?= $(DATADIR)/xdg-desktop-portal/portals
PORTALCONF ?= $(HOME)/.config/xdg-desktop-portal

portal: install
	mkdir -p "$(DESTDIR)$(DBUSDIR)"
	sed 's|@BINDIR@|$(BINDIR)|' org.freedesktop.impl.portal.desktop.files.service.in \
		> "$(DESTDIR)$(DBUSDIR)/org.freedesktop.impl.portal.desktop.files.service"
	install -Dm644 files.portal "$(DESTDIR)$(PORTALDIR)/files.portal"
	install -Dm644 dwl-portals.conf "$(PORTALCONF)/dwl-portals.conf"

.PHONY: all clean install uninstall default portal
