#ifndef FILES_H
#define FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <wayland-client.h>

#include "desk.h"

#define FILES_VERSION "0.3.0"

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

/* A directory's total size while it's unknown or unknowable. */
enum {
    TOTAL_PENDING = -1,     /* Being summed. */
    TOTAL_UNREADABLE = -2,
    TOTAL_NONE = -3,        /* Not a directory, or a link to one: not summed. */
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
    long long total;       /* A directory's contents in bytes, or a TOTAL_ value. */
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

enum view { VIEW_LIST, VIEW_ICONS, VIEW_COUNT };

/* The list view's columns, left to right. */
enum column { COLUMN_NAME, COLUMN_TYPE, COLUMN_SIZE, COLUMN_MODIFIED, COLUMN_COUNT };

/* Where the sidebar and the list were drawn in the last frame, in buffer
   pixels, for the pointer. */
struct panes {
    bool sidebar;           /* Shown in this frame. */
    int sidebar_left, sidebar_right;
    int list_left, list_right;
    int divider;            /* x of the sidebar|list divider, -1 when absent. */
    int top, rows;          /* Where the sidebar and the list's area start, and how many lines fit. */
    int entries_top;        /* The first line of entries: under the column header in the list view. */
    int place_height;       /* A sidebar row. */
    int line_height;        /* A list row, or a row of icons. */
    int columns, tile_width; /* Icons per row and their width; 1 and the list's width in the list view. */
    /* The list view's columns: where each was drawn, and the edges between
       them, edge[c] being column c's left edge (edge[0] the list's left,
       edge[COLUMN_COUNT] its right). squeeze < 1 when they were narrowed to fit. */
    int column_left[COLUMN_COUNT], column_right[COLUMN_COUNT];
    int edge[COLUMN_COUNT + 1];
    double squeeze;
};

/* One piece of the path in the toolbar: a directory to go to. */
struct crumb {
    int left, right;        /* Buffer x, as drawn. */
    size_t start, end;      /* Its name is path[start, end); its path the first end bytes. */
};

enum { BUTTON_SIDEBAR, BUTTON_BACK, BUTTON_FORWARD, BUTTON_UP, BUTTON_LIST, BUTTON_ICONS, BUTTON_COUNT };

/* The tab strip and toolbar as drawn: tabs, then buttons, the path box,
   the search field and the view buttons. */
struct toolbar {
    int tabs_top, tabs_bottom;
    int new_tab_left, new_tab_right;
    int top, bottom;
    int button_left[BUTTON_COUNT], button_right[BUTTON_COUNT];
    int path_left, path_right;
    int search_left, search_right;
    int search_text_x;      /* Where the search text starts, scrolled. */
    int search_clear_left;  /* The × that clears it, or -1. */
    int search_scroll;      /* Pixels of text scrolled off the left. */
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
    TARGET_ROW,             /* index: an entry in the list or the icons */
    TARGET_TAB,             /* index into tabs */
    TARGET_TAB_CLOSE,       /* index into tabs: its × */
    TARGET_NEW_TAB,
    TARGET_SEARCH,
    TARGET_SEARCH_CLEAR,
    TARGET_HEADING,         /* index: COLUMN_, sorts by it */
    TARGET_COLUMN_EDGE,     /* index: the COLUMN_ to its right, resized by dragging */
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

/* A tab. The current one lives in struct files; the others keep where they
   were here until they're switched to. */
struct tab {
    char *path;             /* NULL for the current tab. */
    char *selected;         /* The entry selected when it was left. */
    char *search;           /* Its search text, or NULL. */
    size_t top;
    struct history back, forward;
    int left, right;        /* As drawn, buffer x. */
    int close_left, close_right;
};

#define SEARCH_MAX 256

struct sizer;
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

    struct tab *tabs;
    size_t tab_count, current_tab;

    enum view view;
    int zoom[VIEW_COUNT];     /* Index into each view's zoom levels. */
    enum column sort;         /* The list's order; directories always come first. */
    bool sort_reverse;        /* Descending. */
    double column_width[COLUMN_COUNT]; /* Type, size and modified, logical pixels at zoom 1. */
    int resizing;             /* The COLUMN_ whose left edge is being dragged, or 0. */
    struct sizer *sizer;      /* Sums directories' sizes on a thread. */
    char *sized_path;         /* The directory whose sizes it's summing. */
    char typed[64];           /* Type to select: what's been typed lately... */
    uint64_t typed_at;        /* ...and when, monotonic ms. */
    char notice[256];         /* Shown over the list's foot until the next click or key, or "". */
    char *terminal;           /* Runs Terminal=true applications: run's setting, or "deal -e". */
    char search[SEARCH_MAX];  /* Filters the list as it's typed. */
    size_t search_cursor;     /* Byte offset in search. */
    bool search_focus;        /* Keys go to the search field. */

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
    DeskFont *list_font, *list_italic, *list_mono; /* The same at the list's zoom. */
    DeskFont *icon_font;      /* Icons in the icon view, at its zoom. */

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
    double zoom_pixels;       /* Touchpad zooming (Ctrl held) not yet a whole step. */
    int row_height;           /* A line of the list or icons, buffer pixels, from the last frame. */
    uint32_t shape;           /* The cursor shape set last. */
    DeskWindow window;
    DeskKeyboard keyboard;
    DeskRepeat repeat;
    int page_rows;            /* Lines that fit, from the last frame. */
    bool running;
};

#endif
