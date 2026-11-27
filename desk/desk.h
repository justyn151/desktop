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

#endif
