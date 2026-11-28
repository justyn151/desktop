#ifndef FILES_H
#define FILES_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <wayland-client.h>

#include "desk.h"

#define FILES_VERSION "0.2.0"

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
    mode_t target_mode;    /* What a link points to; mode otherwise. 0 when broken. */
    uid_t owner;
    gid_t group;
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

enum preview_kind {
    PREVIEW_DIRECTORY, /* listing */
    PREVIEW_IMAGE,     /* pixels */
    PREVIEW_TEXT,      /* text, possibly empty */
    PREVIEW_BINARY,    /* Not text: the entry's details are shown. */
    PREVIEW_ERROR,     /* message */
};

/* A finished preview of one version of a file or directory, built by a
   worker thread and then owned by the main thread's cache. */
struct preview {
    char *path;
    struct timespec modified;
    off_t size;
    int box_width, box_height; /* Images: the box, in buffer pixels, it was scaled to fit. */
    enum preview_kind kind;
    struct listing listing;
    uint32_t *pixels;          /* Premultiplied ARGB, width × height. */
    int width, height;
    int image_width, image_height; /* As decoded, before fitting. */
    char *text;
    size_t text_length;
    char message[192];
    unsigned long used;        /* For evicting the least recently used. */
    struct preview *next;      /* In the list of finished previews. */
};

/* A request to a worker: what to preview and how big an image may be. */
struct job {
    char *path;
    struct timespec modified;
    off_t size;
    bool directory;
    int box_width, box_height;
};

#define PREVIEW_CACHE 16

/* Worker 0 reads directories and text, worker 1 decodes images, so a large
   image never holds up a directory preview. A new request replaces one the
   worker hasn't started. */
enum { WORKER_GENERAL, WORKER_IMAGE, WORKER_COUNT };

struct worker {
    pthread_t thread;
    pthread_cond_t wake;
    struct job *pending;   /* Guarded by files.lock. */
    bool started;
    struct files *files;
    char *sent;            /* Main thread: the key of the last request, until its result arrives. */
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
    struct listing parent;    /* The parent directory, for the left column. */
    size_t parent_cursor;     /* The current directory in it, or SIZE_MAX. */
    unsigned long long free_space; /* Bytes available here, or ULLONG_MAX when unknown. */

    bool show_preview;
    long preview_scroll;      /* Lines or rows scrolled with J and K. */
    char *preview_path;       /* What the scroll applies to. */
    struct preview *cache[PREVIEW_CACHE];
    unsigned long preview_clock;
    pthread_mutex_t lock;     /* Guards the workers' pending jobs, done and stopping. */
    struct worker workers[WORKER_COUNT];
    struct preview *done;     /* Finished, waiting for the main thread. */
    int done_fd;              /* eventfd, written when done grows. */
    bool stopping;

    struct memory *memories;
    size_t memory_count, memory_capacity;

    /* The current directory, watched for changes. */
    int inotify;              /* -1 when unavailable. */
    int watch;                /* The directory's watch descriptor, or -1. */
    int parent_watch;         /* The parent's, or -1. */
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
