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
