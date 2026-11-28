#ifndef FILES_H
#define FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <wayland-client.h>

#include "desk.h"

#define FILES_VERSION "0.1.0"

/* Exit statuses. */
enum {
    EXIT_CLOSED = 0,
    EXIT_FAILED = 1, /* Couldn't open the directory or the display. */
    EXIT_USAGE = 2,
};

/* Colors, resolved from the palette or the built-in defaults, 0xffRRGGBB. */
struct colors {
    uint32_t background, foreground, directory, muted;
    uint32_t selection_background, selection_foreground, urgent;
};

enum {
    COUNT_UNKNOWN = -1,   /* Not counted yet. */
    COUNT_UNREADABLE = -2,
};

/* One directory entry. Names are kept as the bytes on disk, valid UTF-8 or
   not, so they can always be opened again. */
struct entry {
    char *name;
    mode_t mode;           /* From lstat: the link itself for symlinks. */
    off_t size;            /* Of the target, for links. */
    struct timespec modified;
    long items;            /* Entries in a directory, or a COUNT_ value. */
    long hidden_items;     /* How many of those are hidden. */
    bool directory;        /* A directory, or a link to one. */
    bool link, broken, hidden, executable;
};

/* The current directory's entries, sorted, and the ones shown. */
struct listing {
    struct entry *entries;
    size_t count, capacity;
    size_t *shown;     /* Indices into entries, in order. */
    size_t shown_count;
    int error;         /* errno when the directory couldn't be read, else 0. */
};

/* The entry last selected in a directory, for coming back to it. */
struct memory {
    char *path, *name;
};

struct files {
    char *home;               /* $HOME without a trailing slash, or NULL. */
    char *path;               /* The current directory, absolute. */
    struct listing listing;
    size_t cursor;            /* Index into listing.shown. */
    size_t top;               /* First row on screen. */
    bool show_hidden;

    struct memory *memories;
    size_t memory_count, memory_capacity;

    /* The current directory, watched for changes. */
    int inotify;              /* -1 when unavailable. */
    int watch;                /* The directory's watch descriptor, or -1. */
    int refresh;              /* timerfd that coalesces bursts of changes. */
    bool refresh_armed;

    struct colors colors;
    char *palette_path;
    DeskWatch palette_watch;

    DeskFont *font, *italic, *mono; /* sans-serif, its italic, monospace digits. */

    struct wl_display *display;
    struct wl_registry *registry;
    DeskGlobals globals;
    struct wl_seat *seat;
    struct wl_keyboard *wl_keyboard;
    DeskWindow window;
    DeskKeyboard keyboard;
    DeskRepeat repeat;
    int page_rows;            /* Rows that fit, from the last frame. */
    bool running;
};

#endif
