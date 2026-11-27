CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS_DIR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WLR_PROTOCOLS_DIR ?= /usr/share/wlr-protocols
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PACKAGES = wayland-client libpng
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
CFLAGS ?= -O2
SHOT_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -DDESK_WAYLAND -I$(BUILD) -I$(DESK)

DESK = ../desk
BUILD = .build
P = $(WAYLAND_PROTOCOLS_DIR)
COPY_XML = $(P)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
SOURCE_XML = $(P)/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
# The capture source protocol references ext_foreign_toplevel_handle_v1.
TOPLEVEL_XML = $(P)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
VIEWPORTER_XML = $(P)/stable/viewporter/viewporter.xml
CURSOR_SHAPE_XML = $(P)/staging/cursor-shape/cursor-shape-v1.xml
# cursor-shape references zwp_tablet_tool_v2, and layer-shell references xdg_popup.
TABLET_XML = $(P)/stable/tablet/tablet-v2.xml
XDG_SHELL_XML = $(P)/stable/xdg-shell/xdg-shell.xml
LAYER_SHELL_XML = $(WLR_PROTOCOLS_DIR)/unstable/wlr-layer-shell-unstable-v1.xml
IPC_XML = ../dwl/protocols/dwl-ipc-unstable-v2.xml

NAMES = ext-image-copy-capture-v1 ext-image-capture-source-v1 ext-foreign-toplevel-list-v1 viewporter \
        cursor-shape-v1 tablet-v2 xdg-shell wlr-layer-shell-unstable-v1 dwl-ipc-unstable-v2
HEADERS = $(NAMES:%=$(BUILD)/%-client-protocol.h)
PROTOCOLS = $(NAMES:%=$(BUILD)/%-protocol.c)
OBJECTS = $(BUILD)/shot.o $(BUILD)/desk.o $(PROTOCOLS:.c=.o)

all: shot

shot: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) -lm $(LDLIBS)

$(BUILD)/shot.o: shot.c shot.h $(HEADERS) $(BUILD)/default-config.h $(DESK)/desk.h
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(SHOT_CFLAGS) -c -o $@ shot.c

$(BUILD)/desk.o: $(DESK)/desk.c $(DESK)/desk.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(SHOT_CFLAGS) -c -o $@ $(DESK)/desk.c

$(BUILD)/%-protocol.o: $(BUILD)/%-protocol.c
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) -std=c11 -c -o $@ $<

$(BUILD):
	mkdir -p $@

# The sample config, embedded so shot can write it when the user has none.
$(BUILD)/default-config.h: shot.conf | $(BUILD)
	LC_ALL=C od -An -v -tu1 shot.conf | awk '{ for (i = 1; i <= NF; ++i) printf "%s,", $$i; print "" }' > "$@.tmp"
	mv -f "$@.tmp" "$@"

$(BUILD)/ext-image-copy-capture-v1-client-protocol.h $(BUILD)/ext-image-copy-capture-v1-protocol.c: $(COPY_XML)
$(BUILD)/ext-image-capture-source-v1-client-protocol.h $(BUILD)/ext-image-capture-source-v1-protocol.c: $(SOURCE_XML)
$(BUILD)/ext-foreign-toplevel-list-v1-client-protocol.h $(BUILD)/ext-foreign-toplevel-list-v1-protocol.c: $(TOPLEVEL_XML)
$(BUILD)/viewporter-client-protocol.h $(BUILD)/viewporter-protocol.c: $(VIEWPORTER_XML)
$(BUILD)/cursor-shape-v1-client-protocol.h $(BUILD)/cursor-shape-v1-protocol.c: $(CURSOR_SHAPE_XML)
$(BUILD)/tablet-v2-client-protocol.h $(BUILD)/tablet-v2-protocol.c: $(TABLET_XML)
$(BUILD)/xdg-shell-client-protocol.h $(BUILD)/xdg-shell-protocol.c: $(XDG_SHELL_XML)
$(BUILD)/wlr-layer-shell-unstable-v1-client-protocol.h $(BUILD)/wlr-layer-shell-unstable-v1-protocol.c: $(LAYER_SHELL_XML)
$(BUILD)/dwl-ipc-unstable-v2-client-protocol.h $(BUILD)/dwl-ipc-unstable-v2-protocol.c: $(IPC_XML)

$(HEADERS): | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTOCOLS): | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@

clean:
	rm -f shot
	rm -rf $(BUILD)

install: shot
	install -Dm755 shot "$(DESTDIR)$(BINDIR)/shot"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/shot"

.PHONY: all clean install uninstall
