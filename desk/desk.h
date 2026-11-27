#ifndef DESK_H
#define DESK_H

/* Shared code for the desktop applications. Each application compiles
   desk.c into its own binary. Nothing here prints, keeps global state or
   runs an event loop: functions return errors and the caller decides. */

#include <stdbool.h>
#include <stdint.h>

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

#endif
