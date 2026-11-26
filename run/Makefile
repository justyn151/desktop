CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
WLR_PROTOCOLS_DIR ?= /usr/share/wlr-protocols
WAYLAND_PROTOCOLS_DIR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PACKAGES = wayland-client xkbcommon freetype2
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
CFLAGS ?= -O2
RUN_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic

LAYER_XML = $(WLR_PROTOCOLS_DIR)/unstable/wlr-layer-shell-unstable-v1.xml
XDG_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
GENERATED = layer-shell-client-protocol.h layer-shell-protocol.c xdg-shell-protocol.c
OBJECTS = run.o layer-shell-protocol.o xdg-shell-protocol.o

all: run

run: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) $(LDLIBS)

run.o: run.c run.h layer-shell-client-protocol.h default-config.h

%.o: %.c
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(RUN_CFLAGS) -c -o $@ $<

layer-shell-client-protocol.h: $(LAYER_XML)
	$(WAYLAND_SCANNER) client-header $< $@

layer-shell-protocol.c: $(LAYER_XML)
	$(WAYLAND_SCANNER) private-code $< $@

# Layer shell refers to the xdg_popup interface.
xdg-shell-protocol.c: $(XDG_XML)
	$(WAYLAND_SCANNER) private-code $< $@

default-config.h: run.conf Makefile
	LC_ALL=C od -An -v -tu1 "$<" | awk '{ for (i = 1; i <= NF; ++i) printf "%s,", $$i; print "" }' > "$@.tmp"
	mv -f "$@.tmp" "$@"

clean:
	rm -f run $(OBJECTS) $(GENERATED) default-config.h default-config.h.tmp

install: run
	install -Dm755 run "$(DESTDIR)$(BINDIR)/run"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/run"

.PHONY: all clean install uninstall
