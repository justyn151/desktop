#ifndef WALL_H
#define WALL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <wayland-client.h>

enum mode {
    MODE_FILL,
    MODE_FIT,
    MODE_STRETCH,
    MODE_CENTER,
    MODE_SOLID,
};

enum transition_effect {
    TRANSITION_NONE,
    TRANSITION_FADE,
    TRANSITION_SLIDE,
    TRANSITION_CIRCLE,
    TRANSITION_WIPE,
    TRANSITION_DIAGONAL,
    TRANSITION_GROW,
    TRANSITION_RANDOM,
};

enum transition_easing { EASE_LINEAR, EASE_IN, EASE_OUT, EASE_IN_OUT };

struct transition_config {
    bool enabled;
    enum transition_effect type;
    enum transition_easing easing;
    unsigned duration_ms, fps, effects;
};

struct animation {
    enum transition_effect effect;
    enum transition_easing easing;
    unsigned direction;
    int64_t start, end, interval;
};

enum color_theme { THEME_DARK, THEME_LIGHT };

struct color_config {
    bool enabled;
    enum color_theme theme;
    unsigned samples;
    double contrast;
};

enum palette_role {
    PALETTE_BACKGROUND,
    PALETTE_FOREGROUND,
    PALETTE_ACCENT,
    PALETTE_HIGHLIGHT,
    PALETTE_MUTED,
    PALETTE_SELECTION_BACKGROUND,
    PALETTE_SELECTION_FOREGROUND,
    PALETTE_BORDER_ACTIVE,
    PALETTE_BORDER_INACTIVE,
    PALETTE_URGENT,
    PALETTE_COUNT,
};

struct palette {
    uint32_t rgb[PALETTE_COUNT];
};

#define WALL_PATH_MAX 4095
#define WALL_PACKET_MAX (WALL_PATH_MAX + 128)
#define WALL_IPC_CLIENTS 16

enum wall_command {
    WALL_RENDER,
    WALL_SET,
    WALL_GET,
    WALL_RELOAD,
    WALL_STOP,
};

struct wall_reply {
    enum mode mode;
    char path[WALL_PATH_MAX + 1];
    char error[192];
};

/* Paths for WALL_SET are absolute. Returns 0 on success, 1 for a server
 * error in reply->error, or -1 with errno set on a transport error. */
int wall_ipc_request(enum wall_command command, const char *path,
                     const char *mode, struct wall_reply *reply);

struct config {
    char *path;
    enum mode mode;
    uint32_t background;
    struct transition_config transition;
    struct color_config colors;
};

struct options {
    const char *config_path;
    const char *image_path;
    const char *mode;
};

struct browser_config {
    char *directories, *font;
    bool recursive, cache, close_on_apply, wallpaper_theme;
    int columns, thumbnail_width, thumbnail_height, font_size;
    uint32_t background, foreground, highlight, muted;
    double opacity; /* Of the background only, from 0 to 1. */
};

struct image {
    uint32_t width, height;
    unsigned char *pixels; /* Premultiplied RGBA8. */
};

struct geometry {
    uint32_t width, height;
    uint32_t scale;
    bool fractional;
};

struct output_state {
    uint32_t width, height;
    int32_t transform;
    int32_t scale;
};

struct app;
struct buffer;

struct output {
    struct wl_list link;
    struct app *app;
    uint32_t name;
    struct wl_output *proxy;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    struct wl_list buffers;
    struct buffer *prepared, *shown, *animation_old, *animation_final;
    struct wl_callback *frame_callback;
    int64_t next_frame;

    struct output_state pending_state, state;
    int32_t preferred_scale;
    uint32_t fractional_units;
    uint32_t width, height, serial;
    bool configured, configure_pending, closed;
    struct geometry attempted, committed;
    uint64_t attempted_revision, committed_revision;
};

struct buffer {
    struct wl_list link;
    struct wl_buffer *proxy;
    struct output *output;
    uint32_t *pixels;
    int width, height, size;
    bool busy, retired;
};

struct ipc_client {
    int fd;
    int64_t deadline;
};

struct pending_change {
    enum wall_command command;
    int fd;
    int64_t deadline;
    const char *mode;
    char path[WALL_PATH_MAX + 1];
};

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *shell;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct wp_viewporter *viewporter;
    struct wl_list outputs;
    bool registry_failed;

    struct options options;
    struct config config;
    FILE *snapshot;
    uint64_t revision;
    struct animation animation;
    struct palette pending_palette;
    bool palette_pending;
    char *state_dir;

    int listener, lock, stop_client;
    char *socket_path;
    dev_t socket_device;
    ino_t socket_inode;
    struct ipc_client clients[WALL_IPC_CLIENTS];
    struct pending_change pending;
};

#endif
