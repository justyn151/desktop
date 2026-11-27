CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS_DIR ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PACKAGES = wayland-client libpng
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
PKG_LIBS = $(shell $(PKG_CONFIG) --libs $(PACKAGES))
CFLAGS ?= -O2
SHOT_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -I.build -I../desk

DESK = ../desk
BUILD = .build
COPY_XML = $(WAYLAND_PROTOCOLS_DIR)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
SOURCE_XML = $(WAYLAND_PROTOCOLS_DIR)/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
# The capture source protocol references ext_foreign_toplevel_handle_v1.
TOPLEVEL_XML = $(WAYLAND_PROTOCOLS_DIR)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
IPC_XML = ../dwl/protocols/dwl-ipc-unstable-v2.xml

NAMES = ext-image-copy-capture-v1 ext-image-capture-source-v1 ext-foreign-toplevel-list-v1 dwl-ipc-unstable-v2
HEADERS = $(NAMES:%=$(BUILD)/%-client-protocol.h)
PROTOCOLS = $(NAMES:%=$(BUILD)/%-protocol.c)
OBJECTS = $(BUILD)/shot.o $(BUILD)/desk.o $(PROTOCOLS:.c=.o)

all: shot

shot: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(PKG_LIBS) $(LDLIBS)

$(BUILD)/shot.o: shot.c shot.h $(HEADERS) $(DESK)/desk.h
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) $(SHOT_CFLAGS) -c -o $@ shot.c

$(BUILD)/desk.o: $(DESK)/desk.c $(DESK)/desk.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SHOT_CFLAGS) -c -o $@ $(DESK)/desk.c

$(BUILD)/%-protocol.o: $(BUILD)/%-protocol.c
	$(CC) $(CPPFLAGS) $(PKG_CFLAGS) $(CFLAGS) -std=c11 -c -o $@ $<

$(BUILD):
	mkdir -p $@

$(BUILD)/ext-image-copy-capture-v1-client-protocol.h $(BUILD)/ext-image-copy-capture-v1-protocol.c: $(COPY_XML)
$(BUILD)/ext-image-capture-source-v1-client-protocol.h $(BUILD)/ext-image-capture-source-v1-protocol.c: $(SOURCE_XML)
$(BUILD)/ext-foreign-toplevel-list-v1-client-protocol.h $(BUILD)/ext-foreign-toplevel-list-v1-protocol.c: $(TOPLEVEL_XML)
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
