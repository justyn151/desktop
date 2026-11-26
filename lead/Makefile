PREFIX ?= $(HOME)/.local
CFLAGS ?= -O2

.PHONY: all install clean

all: editor

editor: editor.c editor.h
	$(CC) $(CPPFLAGS) -std=c11 $(CFLAGS) $(LDFLAGS) -o $@ editor.c $(LDLIBS)

install: editor
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 editor "$(DESTDIR)$(PREFIX)/bin/editor"

clean:
	rm -f editor
