#ifndef SHOT_H
#define SHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>

#include "desk.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dwl-ipc-unstable-v2-client-protocol.h"
#include "ext-data-control-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define SHOT_VERSION "0.3.0"

/* Exit statuses. Cancelling a region selection counts as failed. */
enum {
    EXIT_SAVED = 0,
    EXIT_FAILED = 1,
    EXIT_USAGE = 2,
};

struct config {
    char *directory; /* Expanded, for captures without a FILE argument. */
    char *name;      /* strftime format. */
    bool clipboard;  /* Also copy every saved capture. */
    bool open;       /* Open every saved capture in view. */
    bool cursor;
    int compression; /* zlib level 0–9. */
    double dim;      /* 0 (none) to 1 (black) outside the selection. */
    int border;      /* Selection outline width in logical pixels. */
    bool border_accent; /* Use the palette accent instead of border_rgb. */
    uint32_t border_rgb;
};

/* An encoded PNG. */
struct png_data {
    char *data;
    size_t size;
};

/* 8-bit RGB, upright, rows packed. */
struct image {
    uint32_t width, height;
    uint8_t *rgb;
};

enum capture_state {
    CAPTURE_IDLE,
    CAPTURE_WAITING,
    CAPTURE_READY,
    CAPTURE_FAILED,
};

struct shot;

struct capture {
    struct shot *shot;
    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct wl_buffer *buffer;
    void *pixels;
    size_t size;
    uint32_t buffer_width, buffer_height; /* Size of the allocated buffer. */
    uint32_t width, height;               /* Latest buffer_size. */
    uint32_t format;
    bool have_format;           /* A usable shm format was offered. */
    unsigned constraints;       /* Count of done events received. */
    unsigned frame_constraints; /* constraints when the current frame started. */
    bool want_frame;            /* Start a frame on the next done. */
    unsigned retries;
    uint32_t transform;
    enum capture_state state;
    const char *error;
};

/* The region selection surface on one output. */
struct overlay {
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wp_viewport *viewport;
    struct wl_callback *frame_callback;
    DeskBufferPool buffers;
    uint32_t *dimmed, *bright; /* The frozen frame as XRGB8888, dimmed and not. */
    int width, height;         /* Logical size from configure. */
    bool configured, dirty;
};

struct output {
    struct wl_list link;
    struct shot *shot;
    struct wl_output *wl_output;
    struct zdwl_ipc_output_v2 *ipc;
    uint32_t global;
    char *name; /* wl_output v4 name, such as eDP-1. NULL before v4. */
    char *description;
    int32_t width, height; /* Current mode in physical pixels. */
    bool active;           /* dwl IPC: the focused monitor. */
    struct capture capture;
    struct image frame; /* The captured, upright image. */
    struct overlay overlay;
};

enum selection_state {
    SELECTION_WAITING,
    SELECTION_DONE,
    SELECTION_CANCELLED,
};

struct selection {
    enum selection_state state;
    struct output *pointer_output; /* Output whose overlay has the pointer. */
    double x, y;                   /* Pointer, logical, on pointer_output. */
    struct output *output;         /* Output where the drag started. NULL before a press. */
    double start_x, start_y, end_x, end_y;
    bool dragging;
};

struct shot {
    struct config config;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;
    struct ext_output_image_capture_source_manager_v1 *source_manager;
    struct ext_image_copy_capture_manager_v1 *copy_manager;
    struct zdwl_ipc_manager_v2 *ipc_manager;
    struct wl_list outputs;
    unsigned output_count;
    struct selection selection;
    uint32_t border_rgb; /* Resolved outline color. */
};

#endif
