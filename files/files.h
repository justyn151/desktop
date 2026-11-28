#ifndef FILES_H
#define FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <wayland-client.h>

#include "desk.h"

#define FILES_VERSION "0.5.0"

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
    bool selected;         /* In the selection. */
    bool was_selected;     /* Before a Ctrl rubber band started, which adds to it. */
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
    bool bookmark;          /* From GTK's bookmarks: can be removed. */
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
    int search_clear_left;  /* The × that clears it, or -1. */
    /* The suggestions under the path while it's typed, as drawn. */
    int suggest_left, suggest_right, suggest_top, suggest_row;
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
    TARGET_PATH,            /* The path box: its empty part, or the field while typing a path */
    TARGET_SUGGESTION,      /* index into suggestions */
    TARGET_RENAME,          /* The field a name is being edited in */
    TARGET_DIALOG_BUTTON,   /* index: the dialog's button */
    TARGET_DIALOG_CHECK,    /* Its "Do this for all" box */
    TARGET_JOB_CANCEL,      /* The × on the progress bar */
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

/* A question over the window, such as "Delete 3 items permanently?",
   answered with Cancel or the button that does it. Nothing else can be
   used while it's open. */
#define DIALOG_BUTTONS 3
struct dialog {
    bool open;
    int action;             /* What it's for: a DIALOG_ value in files.c. */
    char title[300], message[300];
    char labels[DIALOG_BUTTONS][32]; /* Left to right. */
    int button_count;
    int danger;             /* The button drawn red, or -1. */
    int focus;              /* The button Enter picks. */
    bool has_check, checked; /* "Do this for all conflicts". */
    char **paths;           /* What it acts on. */
    size_t count;
    /* As drawn, for clicks. */
    int button_left[DIALOG_BUTTONS], button_right[DIALOG_BUTTONS], button_top, button_bottom;
    int check_left, check_right, check_top, check_bottom;
};

/* The last change, for Ctrl+Z. */
enum undo_kind { UNDO_NONE, UNDO_RENAME, UNDO_CREATE, UNDO_TRASH, UNDO_COPY, UNDO_MOVE };
struct undo {
    enum undo_kind kind;
    /* Rename: from[0] to to[0]. Create: to[0]. Trash and move: each from[i]
       went to to[i]. Copy: to[i] are the copies. */
    char **from, **to;
    size_t count;
};
struct worker;
struct counter;

/* ~/.config/files/files.conf. */
struct config {
    double font_points;
    enum view view;           /* For folders without their own. */
    bool hidden, directories_first, single_click, thumbnails;
    enum column sort;
    size_t image_limit;       /* Bytes of pixels. */
    char **writable;          /* Real paths outside home that may change. */
    size_t writable_count;
    char *terminal;           /* NULL: run's, or deal -e. */
};

/* A folder's own view, from switching it there. */
struct folder_view {
    char *path;
    enum view view;
};

/* An image's thumbnail, as made for a box of size buffer pixels. Kept in
   a hash by path while the directory is shown. */
enum thumb_state { THUMB_PENDING, THUMB_READY, THUMB_FAILED };
struct thumb {
    struct thumb *next;     /* In its bucket. */
    char *path;
    time_t mtime;           /* The file's, when it was made. */
    int size;
    enum thumb_state state;
    int width, height;
    uint32_t *pixels;       /* Premultiplied ARGB when ready. */
};
#define THUMB_BUCKETS 1024
#define THUMBS_KEPT 4096    /* More than this and they're all dropped. */
struct thumbnailer;

/* A text field as drawn: its text, and how far it's scrolled sideways. */
struct field {
    DeskText text;
    int scroll;             /* Buffer pixels of text scrolled off the left. */
    int text_x;             /* Where the text starts, scrolled, as drawn. */
    int left, right;        /* The text area, as drawn. */
};

#define SUGGESTIONS_SHOWN 10

/* A line in a right-click menu. */
struct menu_item {
    char label[160];
    int action;             /* What choosing it does: a MENU_ value in files.c. */
    int index;              /* Which application, column... for the action. */
    bool enabled;
    bool separator;         /* A line between groups, not an item. */
    bool submenu;           /* Opens another menu to the right. */
    bool check, checked;    /* Shows a check mark when checked. */
    char shortcut[24];      /* Its key, shown on the right, or "". */
};

#define MENU_ITEMS 40
#define MENU_APPS 16

/* A menu in a popup: the right-click menu, or its submenu. */
struct menu {
    DeskWindow window;
    bool open;
    struct menu_item items[MENU_ITEMS];
    size_t count;
    int hover;              /* Item under the pointer or chosen by keys, or -1. */
    int width, height;      /* Logical pixels. */
    int item_height, separator_height, padding;
    int parent_item;        /* For the submenu: the item it opened from. */
};

/* What a menu was opened on. */
enum menu_context { CONTEXT_ENTRY, CONTEXT_BACKGROUND, CONTEXT_PLACE };

struct sizer;
struct wl_pointer;
struct wl_data_device_manager;
struct wl_data_device;
struct wl_data_source;
struct wl_data_offer;
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
    struct thumbnailer *thumbnailer; /* Makes thumbnails on a thread. */
    struct thumb *thumbs[THUMB_BUCKETS];
    size_t thumb_count;
    char *thumbs_dir;         /* ~/.cache/thumbnails, the freedesktop cache. */
    char *trash_dir;          /* ~/.local/share/Trash, the freedesktop trash: files/ and info/. */
    char *trash_files;        /* Its files/, which is where the Trash place goes. */
    char *trash_real;         /* trash_dir with links resolved: always yours to change. */
    bool renaming;            /* An entry's name is being edited, in rename_field. */
    struct field rename_field;
    char *rename_name;        /* The entry's name as it was. */
    bool rename_created;      /* It was just made by New folder or New file. */
    int rename_top, rename_bottom; /* The field as drawn, or -1. */
    struct dialog dialog;
    struct undo undo;
    struct worker *worker;    /* Copies, moves and deletes on a thread, one job at a time. */
    int job_cancel_left, job_cancel_right, job_top; /* The progress bar's ×, as drawn; job_top -1 when none. */
    char **clip_paths;        /* What Copy or Cut put on the clipboard... */
    size_t clip_count;
    bool clip_cut;            /* ...and whether it's to be moved. */
    char *offer_files_type;   /* The files type the clipboard's offer has, or NULL. */
    /* Dragging files out: what's dragged, while the compositor carries it. */
    struct wl_data_source *drag_source;
    struct wl_surface *drag_icon;         /* The badge under the pointer. */
    struct wp_viewport *drag_viewport;
    DeskBufferPool drag_buffers;
    struct counter *counter;  /* Counting for Properties. */
    struct config config;
    struct folder_view *views; /* Oldest first. */
    size_t view_count;
    char *views_path;         /* ~/.local/state/files/views */
    /* The full name of a cut-short name under the pointer, after a rest. */
    int tip_timer;            /* timerfd */
    bool tip_shown, tip_cut;  /* Shown; the hovered name was drawn cut short this frame. */
    const char *tip_name;
    /* The last press on the list, for single-click opening. */
    size_t press_row;
    unsigned press_mods;
    char **drag_paths;
    size_t drag_count;
    /* Something dragged over the window: where it would go. */
    struct wl_data_offer *drop_offer;
    char *drop_type;          /* The files type it has, or NULL: not files. */
    uint32_t drop_serial, drop_action;
    struct target drop_target; /* The folder row or place lit, or TARGET_NONE for the folder shown. */
    char *drop_into;          /* NULL when it can't be dropped here. */
    char *pending_files_type;
    char *sized_path;         /* The directory whose sizes it's summing. */
    char typed[64];           /* Type to select: what's been typed lately... */
    uint64_t typed_at;        /* ...and when, monotonic ms. */
    char notice[256];         /* Shown over the list's foot until the next click or key, or "". */
    char *terminal;           /* Runs Terminal=true applications: run's setting, or "deal -e". */

    struct menu menus[2];     /* The right-click menu and its submenu. */
    enum menu_context menu_context;
    char *menu_path;          /* What it was opened on: an entry, a place or the directory. */
    bool menu_directory;      /* menu_path is a directory. */
    char *menu_apps[MENU_APPS]; /* Open with's applications, by id. */
    size_t menu_app_count;
    struct wl_surface *pointer_surface; /* The window's or a menu's. */
    uint32_t input_serial;    /* The last click or key, for grabs and the clipboard. */

    struct wl_data_device_manager *data_manager;
    struct wl_data_device *data_device;
    struct wl_data_source *clip_source; /* What Copy path put on the clipboard... */
    char *clip_text;          /* ...and its text. */
    struct wl_data_offer *selection_offer; /* Others' clipboard, unused but released. */
    char *bookmarks_path;     /* GTK's bookmarks file. */
    struct field search;      /* Filters the list as it's typed. */
    bool search_focus;        /* Keys go to the search field. */
    bool path_editing;        /* The path box is a field: Ctrl+L. */
    struct field path_field;
    char **suggestions;       /* Directories matching what's typed, in order. */
    size_t suggestion_count;
    int suggestion;           /* The one chosen with Up and Down, or -1. */
    struct field *selecting;  /* The field a mouse drag is selecting in, or NULL. */
    char *offer_type;         /* The text type the clipboard's offer has, or NULL. */
    struct wl_data_offer *pending_offer; /* The latest offer, before it's known what for. */
    char *pending_type;

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
    size_t anchor;            /* Where Shift+click and Shift+arrows select from: index into listing.shown. */
    /* The rubber band: a press where it may start, then the drag. Its corners
       are in list content pixels (y counted from the first line, not the
       view), so it holds still while the list scrolls under it. */
    bool band_pending, banding, band_add;
    bool press_on_name;       /* The press was on a name: a drag will move it (0.4), not draw a band. */
    int band_x0, band_y0, band_x1, band_y1;
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
