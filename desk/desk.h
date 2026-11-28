#ifndef DESK_H
#define DESK_H

/* Shared code for the desktop applications. Each application compiles
   desk.c into its own binary. Nothing here prints, keeps global state or
   runs an event loop: functions return errors and the caller decides. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* Palette published by wall in $XDG_CACHE_HOME/wall/colors.conf. */
enum desk_role {
    DESK_BACKGROUND,
    DESK_FOREGROUND,
    DESK_ACCENT,
    DESK_HIGHLIGHT,
    DESK_MUTED,
    DESK_SELECTION_BACKGROUND,
    DESK_SELECTION_FOREGROUND,
    DESK_BORDER_ACTIVE,
    DESK_BORDER_INACTIVE,
    DESK_URGENT,
    DESK_ROLE_COUNT
};

typedef struct {
    uint32_t rgb[DESK_ROLE_COUNT]; /* 0xRRGGBB */
} DeskPalette;

/* The role's key in colors.conf, such as "selection_background". */
const char *desk_role_name(enum desk_role role);

/* Return the palette path as a new string, or NULL when neither
   XDG_CACHE_HOME (absolute) nor HOME is usable or memory runs out. */
char *desk_palette_path(void);

/* Read the [colors] section. Every role must appear once as #RRGGBB.
   Unknown keys and other sections are ignored so wall can add roles later.
   Return 0, an errno value when the file can't be read, or EINVAL. */
int desk_palette_load(const char *path, DeskPalette *palette);

/* Watch a file that is replaced atomically. When its directory doesn't exist
   yet, the nearest existing ancestor is watched until it appears. */
typedef struct {
    int fd; /* Poll for POLLIN. -1 when closed. */
    char *path;
    bool direct; /* The watched directory is the file's own directory. */
} DeskWatch;

/* Start watching path. Return false with fd -1 when inotify is unavailable. */
bool desk_watch_open(DeskWatch *watch, const char *path);

/* Drain the events for revents from poll and return whether the file may
   have changed. Re-arms itself when the watched directory moves. */
bool desk_watch_changed(DeskWatch *watch, short revents);

void desk_watch_close(DeskWatch *watch);

/* Atomic file replacement: write to a temporary file in the target's
   directory, then rename it over the target, so readers never see a partial
   file. */
typedef struct {
    FILE *file; /* Write here between open and commit. */
    char *path;
    char *temporary;
} DeskAtomic;

/* Create the temporary file with permissions mode & ~umask. Return 0 or an
   errno value. */
int desk_atomic_open(DeskAtomic *atomic, const char *path, mode_t mode);

/* Flush, sync and rename onto the target. With replace false an existing
   target is left alone and EEXIST is returned, and the temporary file is
   kept so the caller can retry with desk_atomic_retarget. On any other
   error the temporary file is removed. Return 0 or an errno value. */
int desk_atomic_commit(DeskAtomic *atomic, bool replace);

/* Change the target path before committing. Return 0 or ENOMEM. */
int desk_atomic_retarget(DeskAtomic *atomic, const char *path);

/* Remove the temporary file and release everything. Safe after commit. */
void desk_atomic_abort(DeskAtomic *atomic);

/* Anonymous shared memory of the given size, for wl_shm pools. Return a
   file descriptor, or -1 with errno set. */
int desk_shm_open(size_t size);

/* Paths */

/* $XDG_CONFIG_HOME/<app>/<app>.conf, or ~/.config/<app>/<app>.conf. Return a
   new string, or NULL when neither variable is usable or memory runs out. */
char *desk_config_path(const char *app);

/* Expand a leading "~/" (or a bare "~") with HOME. Return a new string, or
   NULL when HOME is needed but unset, or memory runs out. */
char *desk_expand_home(const char *path);

/* Create a directory and its missing parents, like mkdir -p. Return 0 or an
   errno value. */
int desk_mkdirs(const char *path, mode_t mode);

/* Write data to path, creating parent directories, unless path already
   exists. Return 0 when written, EEXIST when it existed, or an errno value. */
int desk_write_default(const char *path, const void *data, size_t length);

/* INI */

/* Called for each "key = value" line. Surrounding spaces are trimmed and one
   pair of double quotes around the value is removed. section is "" before the
   first [section]. For a line that is neither a key, a section, a comment
   nor blank, key and value are NULL. line counts from 1. */
typedef void (*DeskIniHandler)(void *data, const char *section, const char *key, const char *value, int line);

/* Parse text of the given length. Comments are whole lines starting with # or ;. */
void desk_ini_parse(const char *text, size_t length, DeskIniHandler handler, void *data);

/* Read and parse a file of at most 1 MiB. Return 0 or an errno value. */
int desk_ini_load(const char *path, DeskIniHandler handler, void *data);

/* Parse "true"/"false" (also yes/no, on/off, 1/0). */
bool desk_parse_bool(const char *text, bool *value);

/* Parse #RRGGBB into 0xRRGGBB. */
bool desk_parse_rgb(const char *text, uint32_t *rgb);

/* Key repeat */

/* A timerfd that fires while a key is held, at the compositor's repeat rate.
   It knows nothing about keymaps: the caller decides which keys repeat. */
typedef struct {
    int fd;         /* Poll for POLLIN. -1 when unavailable. */
    uint32_t key;   /* The held key, valid while active. */
    bool active;
    int32_t rate;   /* Repeats per second; 0 disables repeat. */
    int32_t delay;  /* Milliseconds before the first repeat. */
} DeskRepeat;

/* Create the timer with 25 per second after 600 ms until desk_repeat_info.
   Return 0 or an errno value. */
int desk_repeat_open(DeskRepeat *repeat);

/* Store wl_keyboard.repeat_info. */
void desk_repeat_info(DeskRepeat *repeat, int32_t rate, int32_t delay);

/* Start repeating key after the delay, replacing any held key. */
void desk_repeat_start(DeskRepeat *repeat, uint32_t key);

/* Stop if key is the one repeating. */
void desk_repeat_release(DeskRepeat *repeat, uint32_t key);

/* Stop whatever is repeating, such as on keyboard focus loss. */
void desk_repeat_cancel(DeskRepeat *repeat);

/* After POLLIN: drain the timer and return how many repeats are due. */
unsigned desk_repeat_fired(DeskRepeat *repeat);

void desk_repeat_close(DeskRepeat *repeat);

/* UTF-8 */

/* Decode the code point at *text, which must be before end, and advance
   past it. Each byte of an invalid or truncated sequence, an overlong form,
   a surrogate or anything above U+10FFFF decodes as U+FFFD, so every byte
   string can be shown. */
uint32_t desk_utf8_next(const char **text, const char *end);

/* One line of editable text: UTF-8, a cursor and a selection, the editing
   behind text fields. Drawing is the application's. */
typedef struct {
    char *text;             /* Always NUL-terminated; "" when empty. */
    size_t length, capacity;
    size_t cursor;          /* Byte offset, on a character boundary. */
    size_t anchor;          /* The selection's other end; == cursor when nothing is selected. */
} DeskText;

/* Replace the text, cursor at the end, nothing selected. Return false when
   memory runs out. */
bool desk_text_set(DeskText *text, const char *value);
void desk_text_free(DeskText *text);

/* The selection as [start, end); false when there is none. */
bool desk_text_selection(const DeskText *text, size_t *start, size_t *end);
void desk_text_select_all(DeskText *text);

/* Insert at the cursor, replacing the selection. Return false when memory
   runs out. */
bool desk_text_insert(DeskText *text, const char *insert, size_t length);

/* Delete the selection, or else a character (or with word a word) before
   the cursor (direction < 0) or after it (> 0). Return whether anything
   was deleted. */
bool desk_text_delete(DeskText *text, int direction, bool word);

/* Move by a character, or a word, left (< 0) or right (> 0); extend keeps
   the anchor, selecting. Without extend, a selection collapses to its
   side. */
void desk_text_move(DeskText *text, int direction, bool word, bool extend);

/* Move to a byte offset (clamped to the text, snapped to a character). */
void desk_text_move_to(DeskText *text, size_t offset, bool extend);

/* Canvas */

/* A 32-bit premultiplied ARGB buffer and a clip rectangle, which every
   drawing function stays inside. */
typedef struct {
    uint32_t *pixels;
    int width, height, stride;               /* stride in pixels. */
    int clip_left, clip_top, clip_right, clip_bottom; /* right and bottom exclusive. */
} DeskCanvas;

/* A canvas over pixels with a stride of width, clipped to all of it. */
DeskCanvas desk_canvas(uint32_t *pixels, int width, int height);

/* Clip to the rectangle, intersected with the whole canvas (not with the
   previous clip). */
void desk_canvas_clip(DeskCanvas *canvas, int left, int top, int right, int bottom);

/* Blend a rectangle of color 0xAARRGGBB (not premultiplied) over the canvas.
   Alpha 0xff replaces. */
void desk_canvas_fill(DeskCanvas *canvas, int x, int y, int width, int height, uint32_t argb);

/* Composite premultiplied ARGB pixels (rows packed) over the canvas with
   their top-left corner at x, y. */
void desk_canvas_blit(DeskCanvas *canvas, int x, int y, const uint32_t *pixels, int width, int height);

#ifdef DESK_KEYBOARD
/* Layer 1: needs xkbcommon, but not wayland-client. */
#include <xkbcommon/xkbcommon.h>

/* A keymap from wl_keyboard and the modifier state. */
typedef struct {
    struct xkb_context *context;
    struct xkb_keymap *keymap; /* NULL until the first keymap event. */
    struct xkb_state *state;
} DeskKeyboard;

enum {
    DESK_MOD_SHIFT = 1 << 0,
    DESK_MOD_CTRL = 1 << 1,
    DESK_MOD_ALT = 1 << 2,
    DESK_MOD_LOGO = 1 << 3,
};

/* Return 0 or ENOMEM. */
int desk_keyboard_open(DeskKeyboard *keyboard);

/* Take wl_keyboard.keymap. format must be WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 (1).
   fd is always closed. On failure the previous keymap stays. Return 0 or
   an errno value (EINVAL for another format or a keymap that won't compile). */
int desk_keyboard_keymap(DeskKeyboard *keyboard, uint32_t format, int fd, uint32_t size);

/* Take wl_keyboard.modifiers. */
void desk_keyboard_modifiers(DeskKeyboard *keyboard, uint32_t depressed, uint32_t latched, uint32_t locked,
                             uint32_t group);

/* The keysym for an evdev key code from wl_keyboard.key (shifted by the
   current modifiers, so Shift+g is G), or XKB_KEY_NoSymbol before a keymap. */
xkb_keysym_t desk_keyboard_sym(const DeskKeyboard *keyboard, uint32_t key);

/* The active DESK_MOD_* modifiers. */
unsigned desk_keyboard_mods(const DeskKeyboard *keyboard);

/* Whether the keymap says the key repeats. */
bool desk_keyboard_repeats(const DeskKeyboard *keyboard, uint32_t key);

void desk_keyboard_close(DeskKeyboard *keyboard);

/* What desk_text_key did. */
enum { DESK_TEXT_IGNORED, DESK_TEXT_MOVED, DESK_TEXT_CHANGED };

/* The usual editing keys for a text field: Left and Right (by word with
   Ctrl, selecting with Shift), Home and End, Backspace and Delete (a word
   with Ctrl), Ctrl+A to select all, and typing (utf8, the key's text, may
   be NULL). Clipboard keys, Enter, Escape and Tab are left to the caller. */
int desk_text_key(DeskText *text, uint32_t sym, unsigned mods, const char *utf8);
#endif

#ifdef DESK_FONT
/* Layer 2: needs freetype2 and fontconfig. */

/* A fontconfig match with its fallback faces, at one pixel size, and a
   cache of rendered glyphs. Characters the first face lacks come from the
   next face that has them, so symbols (Nerd Font icons) and other scripts
   work without the caller knowing which font holds them. */
typedef struct DeskFont DeskFont;

typedef struct {
    int ascent, descent; /* Pixels above and below the baseline. */
    int height;          /* Line height in pixels, at least ascent + descent. */
} DeskFontMetrics;

/* Match a fontconfig pattern such as "sans-serif" or "monospace:bold" at a
   size in pixels. Return NULL with a message in error when nothing matches
   or the first face won't load. */
DeskFont *desk_font_open(const char *pattern, double pixel_size, char *error, size_t error_size);

/* Change the size, such as when the window's scale changes. Clears the
   glyph cache. Return 0 or an errno value. */
int desk_font_resize(DeskFont *font, double pixel_size);

double desk_font_size(const DeskFont *font);

DeskFontMetrics desk_font_metrics(const DeskFont *font);

/* The advance of length bytes of UTF-8 text, in pixels. */
int desk_text_width(DeskFont *font, const char *text, size_t length);

/* The width of one character. */
int desk_char_width(DeskFont *font, uint32_t code);

/* How many bytes from the start of text fit in max_width pixels, ending on a
   character boundary. */
size_t desk_text_fit(DeskFont *font, const char *text, size_t length, int max_width);

/* Draw text with its baseline at y, starting at x, in color 0xAARRGGBB.
   Return the x after the last character. */
int desk_text_draw(DeskFont *font, DeskCanvas *canvas, int x, int y, const char *text, size_t length,
                   uint32_t argb);

void desk_font_close(DeskFont *font);
#endif

#ifdef DESK_WAYLAND
/* Layer 1: needs wayland-client. */
#include <wayland-client.h>

typedef struct {
    struct wl_buffer *buffer;
    uint32_t *pixels;
    int width, height; /* Stride is width * 4. */
    size_t size;
    bool busy; /* Attached and not yet released by the compositor. */
} DeskBuffer;

/* Two shm buffers, so one can be drawn while the compositor reads the other. */
typedef struct {
    DeskBuffer slots[2];
} DeskBufferPool;

/* Return a buffer that the compositor isn't using, of the given size and
   32-bit shm format, reallocating when the size changed. It is marked busy:
   attach and commit it. Return NULL when both are busy or allocation fails. */
DeskBuffer *desk_buffer_get(DeskBufferPool *pool, struct wl_shm *shm, int width, int height, uint32_t format);

void desk_buffer_pool_finish(DeskBufferPool *pool);

#ifdef DESK_WINDOW
/* A plain xdg-shell window. Needs the xdg-shell, viewporter and
   fractional-scale-v1 client headers on the include path and their protocol
   code linked, both generated by the application's build. */

struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_popup;
struct wl_seat;
struct wp_viewporter;
struct wp_viewport;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;

/* The globals a window needs. The last two are optional. */
typedef struct {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base; /* Pings are answered. */
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
} DeskGlobals;

/* Call from wl_registry.global. Return true when the global was one of these
   and is now bound. */
bool desk_globals_bind(DeskGlobals *globals, struct wl_registry *registry, uint32_t name, const char *interface,
                       uint32_t version);

/* The interface name of the first required global that's missing, or NULL. */
const char *desk_globals_missing(const DeskGlobals *globals);

void desk_globals_finish(DeskGlobals *globals);

typedef struct DeskWindow DeskWindow;

typedef struct {
    /* Draw a frame: get a buffer with desk_window_buffer, paint it and give
       it to desk_window_commit. Called after configures, scale changes and
       desk_window_redraw, at most once per frame. Returning without
       committing (no free buffer) keeps the window dirty for later. */
    void (*draw)(void *data, DeskWindow *window);
    /* The compositor asked the window to close. */
    void (*close)(void *data, DeskWindow *window);
    /* Optional: the size to start at when the compositor leaves it to the
       window, in logical pixels. 800×600 otherwise. */
    void (*initial_size)(void *data, DeskWindow *window, int *width, int *height);
} DeskWindowListener;

struct DeskWindow {
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct xdg_popup *popup;                 /* Instead of toplevel, for a popup. */
    struct wp_viewport *viewport;            /* NULL without wp_viewporter. */
    struct wp_fractional_scale_v1 *fractional;
    struct wl_callback *frame_callback;
    struct wl_shm *shm;
    DeskBufferPool buffers;
    int width, height;                 /* Logical size in use. */
    int pending_width, pending_height; /* From the latest toplevel configure. */
    int bounds_width, bounds_height;   /* configure_bounds, 0 when unknown. */
    uint32_t fraction;                 /* Preferred scale × 120, 0 when unknown. */
    int buffer_scale;                  /* Preferred integer scale. */
    bool configured, dirty, activated;
    char *title;
    const DeskWindowListener *listener;
    void *data;
};

/* Create the toplevel and commit it unmapped; the first configure maps it.
   Return false when memory runs out. */
bool desk_window_open(DeskWindow *window, const DeskGlobals *globals, const char *app_id,
                      const DeskWindowListener *listener, void *data);

/* A popup such as a menu, placed against parent (a window or another
   popup): its top-left at x, y (logical, in parent's surface) or, where
   that doesn't fit on screen, flipped or slid by the compositor. It takes
   an explicit grab with seat and serial (of the click or key that opened
   it), so a click outside closes it: then listener->close is called and
   the popup should be closed with desk_window_close. Its listener's draw
   paints it like a window's. Return false when memory runs out. */
bool desk_popup_open(DeskWindow *popup, const DeskGlobals *globals, DeskWindow *parent, int x, int y, int width,
                     int height, struct wl_seat *seat, uint32_t serial, const DeskWindowListener *listener,
                     void *data);

/* Set the title, sending it only when it changed. */
void desk_window_set_title(DeskWindow *window, const char *title);

/* Buffer pixels per logical pixel: the fractional scale through the
   viewport, or the integer buffer scale. */
double desk_window_scale(const DeskWindow *window);

/* The window's size in buffer pixels, at least 1×1. */
void desk_window_buffer_size(const DeskWindow *window, int *width, int *height);

/* A free buffer of the window's buffer size, or NULL when both are busy. */
DeskBuffer *desk_window_buffer(DeskWindow *window, uint32_t format);

/* Attach the painted buffer, damage all of it, ask for a frame callback and
   commit. Clears dirty. */
void desk_window_commit(DeskWindow *window, DeskBuffer *buffer);

/* Mark the window dirty and draw now if no frame is pending. */
void desk_window_redraw(DeskWindow *window);

/* Draw if dirty and nothing is in the way: call after dispatching, so a
   draw skipped for lack of a free buffer happens once one is released. */
void desk_window_update(DeskWindow *window);

/* Configured, drawn and no frame pending: a good time for background work. */
bool desk_window_idle(const DeskWindow *window);

void desk_window_close(DeskWindow *window);
#endif
#endif

#ifdef DESK_APPS
/* Applications and file types: desktop entries, mimeapps.list and the
   shared-mime-info database, for opening files the way other desktops do. */

typedef struct {
    char *id;               /* The desktop file's name, as "view.desktop". */
    char *path;             /* Where it was read from. */
    char *name;
    char *exec;             /* Exec, with the value escapes decoded. */
    char **mime_types;      /* MimeType, split. */
    size_t mime_count;
    bool terminal;          /* Terminal=true: run inside the terminal. */
    bool no_display;        /* NoDisplay=true: not offered in lists. */
} DeskApp;

/* One line of mimeapps.list: a type and an application for it. */
typedef struct {
    char *mime, *id;
    int kind;               /* DESK_DEFAULT, DESK_ADDED or DESK_REMOVED. */
} DeskAssociation;

enum { DESK_DEFAULT, DESK_ADDED, DESK_REMOVED };

/* A glob from shared-mime-info's globs2. */
typedef struct {
    char *pattern, *mime;
    int weight;
    bool case_sensitive;
} DeskGlob;

typedef struct {
    DeskApp *apps;          /* User's first, each id once. */
    size_t app_count;
    DeskAssociation *associations; /* Most important file first. */
    size_t association_count;
    DeskGlob *globs;        /* Empty when shared-mime-info isn't installed. */
    size_t glob_count;
} DeskApps;

/* Read the desktop entries in $XDG_DATA_HOME/applications and each of
   $XDG_DATA_DIRS's, the mimeapps.list files and the globs. Missing files are
   fine. Return 0 or ENOMEM. */
int desk_apps_load(DeskApps *apps);
void desk_apps_free(DeskApps *apps);

/* The MIME type of the file at path, whose name is name: inode/directory,
   then by name from the globs (or a built-in table of common types without
   them), then by content: ELF executables, text, or
   application/octet-stream. The string is static or the database's. */
const char *desk_mime_type(const DeskApps *apps, const char *path, const char *name);

/* The applications for a type, most preferred first: the default from
   mimeapps.list, the added associations, then every application declaring
   the type, without the removed ones. text/ types also get text/plain's.
   Fill up to max and return how many there are in all. */
size_t desk_apps_for(const DeskApps *apps, const char *mime, const DeskApp **out, size_t max);

/* An application by id, or NULL. */
const DeskApp *desk_app_find(const DeskApps *apps, const char *id);

/* Start app on paths (count of them, maybe 0) in directory, detached in its
   own session with stdin from /dev/null. Exec's field codes are expanded;
   an app taking one file (%f, %u) starts once per path. Terminal=true apps
   start inside terminal, a command line such as "deal -e". The caller
   should ignore SIGCHLD so they don't linger as zombies. Return 0 or an
   errno value (EINVAL for an Exec that can't be parsed). */
int desk_app_launch(const DeskApp *app, char *const paths[], size_t count, const char *terminal,
                    const char *directory);

/* Run argv (argv[0] looked up in PATH) the same way: detached, in directory,
   inside terminal when it isn't NULL. Return 0 or an errno value. */
int desk_spawn(char *const argv[], const char *terminal, const char *directory);

/* Make id the default for mime in $XDG_CONFIG_HOME/mimeapps.list (written
   atomically, other lines kept), and add it to the added associations.
   Return 0 or an errno value. */
int desk_mimeapps_set_default(const char *mime, const char *id);
#endif

#ifdef DESK_IMAGE
/* Image decoding: needs libpng and libjpeg. -DDESK_IMAGE_GIF adds GIF (first
   frame, built in, no library) and -DDESK_IMAGE_WEBP adds still WebP
   (libwebp). */

typedef struct {
    uint32_t width, height;
    unsigned char *pixels; /* Premultiplied RGBA8, stride width * 4. free() it. */
    int orientation;       /* EXIF orientation 1–8, as stored; 1 when absent. Not applied. */
} DeskImage;

enum {
    /* Decode a JPEG larger than max_bytes at 1/2, 1/4 or 1/8 size instead of
       failing. PNGs over the limit still fail. */
    DESK_IMAGE_SHRINK = 1 << 0,
};

/* Decode an image, detected by content, from a seekable file. The decoded
   image may use at most max_bytes. Return true, or false with image zeroed
   and a message such as "PNG: CRC error" in error. A known format that isn't
   compiled in is named, as in "AVIF images aren't supported". */
bool desk_image_load(FILE *file, DeskImage *image, size_t max_bytes, unsigned flags,
                     char *error, size_t error_size);
#endif

#endif
