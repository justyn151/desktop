#ifndef RUN_H
#define RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

enum { INPUT_SIZE = 16384, MAX_SCALE = 16 };

struct config {
    uint32_t background, foreground, highlight, muted;
    uint32_t selected_background, selected_foreground;
    double opacity, scale;
    int height, padding, font_size, input_width, result_spacing;
    bool desktop_entries, path_commands, prefer_desktop_entries;
    bool case_sensitive, fuzzy, wallpaper_theme;
    char font_file[4096];
    char prompt[256];
    char terminal[4096];
};

struct command {
    char *name, *path;
    size_t directory;
};

struct application {
    char *name, *path, *directory;
    char **argv;
    size_t argc;
    wchar_t *search;
    bool terminal;
};

enum match_kind { MATCH_EXACT, MATCH_PREFIX, MATCH_SUBSTRING, MATCH_FUZZY, MATCH_NONE };

struct match_quality {
    enum match_kind kind;
    int64_t score;
    bool metadata;
};

struct result {
    const char *name;
    /* Search fields use the configured case and end with an empty field. */
    wchar_t *search;
    const struct command *command;
    const struct application *application;
    size_t order;
    unsigned preference;
    struct match_quality quality;
};

struct terminal {
    char *path, *options;
    char **argv;
    size_t argc;
};

struct buffer {
    struct wl_buffer *proxy;
    uint32_t *pixels;
    size_t size;
    int width, height;
    bool busy;
};

struct launcher {
    struct config config;
    struct command *commands;
    size_t command_count, command_capacity;
    struct application *applications;
    size_t application_count, application_capacity;
    struct terminal terminal;
    struct result *results, **matches;
    size_t result_count;
    size_t match_count, selected, page_start;
    char input[INPUT_SIZE];
    size_t input_length, cursor;
    char message[160];

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *shell;
    struct wl_surface *surface;
    struct wl_callback *startup;
    struct zwlr_layer_surface_v1 *layer;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    uint32_t seat_name;
    struct buffer buffers[2];
    int width, height, buffer_scale, font_scale;
    bool configured, dirty, commit_pending, closed, focused;
    const char *error;

    FT_Library freetype;
    FT_Face font;
    struct xkb_context *xkb;
    struct xkb_state *keys;
    struct xkb_compose_state *compose;
    int timer;
    int repeat_rate, repeat_delay;
    uint32_t repeat_key;
    bool repeating;
};

#endif
