CC ?= cc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share

CFLAGS ?= -O2
MARK_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -I$(BUILD) -I$(DESK)

# desk's Layer 0 only: the config parser, the palette and the file watch.
DESK = ../desk
BUILD = .build
OBJECTS = $(BUILD)/mark.o $(BUILD)/desk.o

all: mark

mark: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(BUILD)/mark.o: mark.c mark.h $(BUILD)/default-config.h $(DESK)/desk.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MARK_CFLAGS) -c -o $@ mark.c

$(BUILD)/desk.o: $(DESK)/desk.c $(DESK)/desk.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MARK_CFLAGS) -c -o $@ $(DESK)/desk.c

# The sample config, embedded so mark can write it when the user has none.
$(BUILD)/default-config.h: mark.conf | $(BUILD)
	LC_ALL=C od -An -v -tu1 mark.conf | awk '{ for (i = 1; i <= NF; ++i) printf "%s,", $$i; print "" }' > "$@.tmp"
	mv -f "$@.tmp" "$@"

# The test build adds --html, so the parser can be compared with the
# CommonMark spec's examples.
$(BUILD)/mark-test: mark.c mark.h $(BUILD)/default-config.h $(BUILD)/desk.o | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MARK_CFLAGS) -DMARK_TEST -o $@ mark.c $(BUILD)/desk.o $(LDFLAGS) $(LDLIBS)

$(BUILD):
	mkdir -p $@

check: mark $(BUILD)/mark-test
	tests/run.sh ./mark $(BUILD)/mark-test

clean:
	rm -f mark
	rm -rf $(BUILD)

MIME_TYPES = text/markdown text/x-markdown

install: mark
	install -Dm755 mark "$(DESTDIR)$(BINDIR)/mark"
	install -Dm644 mark.desktop "$(DESTDIR)$(DATADIR)/applications/mark.desktop"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/mark" "$(DESTDIR)$(DATADIR)/applications/mark.desktop"

# Make mark the default for markdown files, for the user running make.
default:
	@if command -v xdg-mime >/dev/null; then \
		xdg-mime default mark.desktop $(MIME_TYPES) && echo "mark is the default for $(MIME_TYPES)"; \
	else \
		echo "xdg-mime isn't installed. Add these lines under [Default Applications] in ~/.config/mimeapps.list:"; \
		for type in $(MIME_TYPES); do echo "$$type=mark.desktop"; done; \
	fi

.PHONY: all check clean install uninstall default
