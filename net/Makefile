CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
CFLAGS ?= -O2
NET_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all clean install uninstall

all: net

net: net.c net.h Makefile default-config.h
	$(PKG_CONFIG) --exists libsystemd
	$(CC) $(CPPFLAGS) $(CFLAGS) $(NET_CFLAGS) $$( $(PKG_CONFIG) --cflags libsystemd ) $(LDFLAGS) -o $@ net.c $$( $(PKG_CONFIG) --libs libsystemd ) $(LDLIBS)

default-config.h: net.conf Makefile
	LC_ALL=C od -An -v -tu1 "$<" | awk '{ for (i = 1; i <= NF; ++i) printf "%s,", $$i; print "" }' > "$@.tmp"
	mv -f "$@.tmp" "$@"

clean:
	rm -f net default-config.h default-config.h.tmp

install: net
	install -Dm755 net "$(DESTDIR)$(BINDIR)/net"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/net"
