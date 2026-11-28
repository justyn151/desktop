#ifndef FILES_H
#define FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <wayland-client.h>

#include "desk.h"

#define FILES_VERSION "0.2.2"

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

/* An entry in the sidebar: a place to go, or a heading over a group. */
struct place {
    char *name;
    char *path;             /* NULL for a heading. */
    uint32_t icon;
};

/* Where the sidebar and the list were drawn in the last frame, in buffer
   pixels, for the pointer. */
struct panes {
    bool sidebar;           /* Shown in this frame. */
    int sidebar_left, sidebar_right;
    int list_left, list_right;
    int divider;            /* x of the sidebar|list divider, -1 when absent. */
    int top, rows;          /* The first row's y, and how many fit. */
};

/* One piece of the path in the toolbar: a directory to go to. */
struct crumb {
    int left, right;        /* Buffer x, as drawn. */
    size_t start, end;      /* Its name is path[start, end); its path the first end bytes. */
};

enum { BUTTON_SIDEBAR, BUTTON_BACK, BUTTON_FORWARD, BUTTON_UP, BUTTON_COUNT };

/* The toolbar as drawn: buttons, then the path box. */
struct toolbar {
    int top, bottom;
    int button_left[BUTTON_COUNT], button_right[BUTTON_COUNT];
    int path_left, path_right;
    struct crumb *crumbs;
    size_t crumb_count, crumb_capacity;
};

/* What is under the pointer. */
enum target_kind {
    TARGET_NONE,
    TARGET_BUTTON,          /* index: BUTTON_ */
    TARGET_CRUMB,           /* index into toolbar.crumbs */
    TARGET_DIVIDER,
    TARGET_PLACE,           /* index into places */
    TARGET_ROW,             /* index: a row of the list */
};

struct target {
    enum target_kind kind;
    size_t index;
};

/* Directories visited, for back and forward. */
struct history {
    char **paths;           /* Oldest first. */
    size_t count, capacity;
};

#define HISTORY_LIMIT 100

struct wl_pointer;
struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;

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
    bool free_scroll;         /* Scrolled with the wheel: the cursor may be off screen. */
    struct history back, forward;

    struct place *places;     /* The sidebar: home, its folders, /, mounted drives. */
    size_t place_count;
    int mounts;               /* /proc/self/mountinfo, polled for drives coming and going. */
    bool show_sidebar;
    double sidebar_width;     /* Logical pixels. */
    struct panes panes;
    struct toolbar toolbar;
    char *state_path;         /* Remembers the sidebar between runs. */

    /* Changing files is allowed only inside home (home_real, symlinks
       resolved), unless running as root. */
    char *home_real;
    bool root;
    bool writable;            /* The current directory may be changed. */

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
    struct wl_pointer *pointer;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape;
    uint32_t pointer_serial;
    double pointer_x, pointer_y;  /* Logical surface coordinates. */
    struct target hover;      /* What the pointer is over. */
    bool drag;                /* The divider is being dragged. */
    struct target press;      /* The last click, for telling a double-click... */
    uint32_t press_time;      /* ...by its time. */
    double scroll_pixels;     /* Touchpad scrolling not yet a whole row. */
    int scroll_steps;         /* Wheel notches in this pointer frame. */
    int row_height;           /* Buffer pixels, from the last frame. */
    DeskWindow window;
    DeskKeyboard keyboard;
    DeskRepeat repeat;
    int page_rows;            /* Rows that fit, from the last frame. */
    bool running;
};

#endif
