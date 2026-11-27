CC ?= cc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CFLAGS ?= -O2
MARK_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic

# desk joins in 0.5, for the config file, the palette and the file watch.
BUILD = .build
OBJECTS = $(BUILD)/mark.o

all: mark

mark: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(BUILD)/mark.o: mark.c mark.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MARK_CFLAGS) -c -o $@ mark.c

# The test build adds --html, so the parser can be compared with the
# CommonMark spec's examples.
$(BUILD)/mark-test: mark.c mark.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MARK_CFLAGS) -DMARK_TEST -o $@ mark.c $(LDFLAGS) $(LDLIBS)

$(BUILD):
	mkdir -p $@

check: mark $(BUILD)/mark-test
	tests/run.sh ./mark $(BUILD)/mark-test

clean:
	rm -f mark
	rm -rf $(BUILD)

install: mark
	install -Dm755 mark "$(DESTDIR)$(BINDIR)/mark"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/mark"

.PHONY: all check clean install uninstall
