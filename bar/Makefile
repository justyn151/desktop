CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
WLR_PROTOCOLS_DIR ?= /usr/share/wlr-protocols
WAYLAND_PROTOCOLS_DIR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PACKAGES = wayland-client freetype2 fontconfig libsystemd
CFLAGS ?= -O2
BAR_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
LAYER_XML = $(WLR_PROTOCOLS_DIR)/unstable/wlr-layer-shell-unstable-v1.xml
XDG_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
IPC_XML = protocols/dwl-ipc-unstable-v2.xml
OBJECTS = bar.o layer-shell-protocol.o xdg-shell-protocol.o dwl-ipc-protocol.o

all: bar

bar: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) $(LDLIBS)

bar.o: bar.c bar.h layer-shell-client-protocol.h dwl-ipc-client-protocol.h default-config.h

%.o: %.c
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(BAR_CFLAGS) -c -o $@ $<

layer-shell-client-protocol.h: $(LAYER_XML)
	$(WAYLAND_SCANNER) client-header $< $@

layer-shell-protocol.c: $(LAYER_XML)
	$(WAYLAND_SCANNER) private-code $< $@

# Layer shell references the xdg_popup interface.
xdg-shell-protocol.c: $(XDG_XML)
	$(WAYLAND_SCANNER) private-code $< $@

dwl-ipc-client-protocol.h: $(IPC_XML)
	$(WAYLAND_SCANNER) client-header $< $@

dwl-ipc-protocol.c: $(IPC_XML)
	$(WAYLAND_SCANNER) private-code $< $@

default-config.h: bar.conf Makefile
	LC_ALL=C od -An -v -tu1 "$<" | awk '{ for (i = 1; i <= NF; ++i) printf "%s,", $$i; print "" }' > "$@.tmp"
	mv -f "$@.tmp" "$@"

clean:
	rm -f bar $(OBJECTS) layer-shell-client-protocol.h layer-shell-protocol.c xdg-shell-protocol.c
	rm -f dwl-ipc-client-protocol.h dwl-ipc-protocol.c default-config.h default-config.h.tmp

install: bar
	install -Dm755 bar "$(DESTDIR)$(BINDIR)/bar"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/bar"

.PHONY: all clean install uninstall
