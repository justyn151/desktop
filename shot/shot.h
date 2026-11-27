#ifndef SHOT_H
#define SHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>

#include "dwl-ipc-unstable-v2-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

#define SHOT_VERSION "0.1.0"

/* Exit statuses. */
enum {
    EXIT_SAVED = 0,
    EXIT_FAILED = 1,
    EXIT_USAGE = 2,
};

struct shot;

struct output {
    struct wl_list link;
    struct shot *shot;
    struct wl_output *wl_output;
    struct zdwl_ipc_output_v2 *ipc;
    uint32_t global;
    char *name;        /* wl_output v4 name, such as eDP-1. NULL before v4. */
    char *description;
    int32_t width, height; /* Current mode in physical pixels. */
    bool active;           /* dwl IPC: the focused monitor. */
};

/* 8-bit RGB, upright, rows packed. */
struct image {
    uint32_t width, height;
    uint8_t *rgb;
};

enum capture_state {
    CAPTURE_WAITING,
    CAPTURE_READY,
    CAPTURE_FAILED,
};

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
    bool have_format;      /* A usable shm format was offered. */
    unsigned constraints;  /* Count of done events received. */
    unsigned frame_constraints; /* constraints when the current frame started. */
    bool want_frame;       /* Start a frame on the next done. */
    unsigned retries;
    uint32_t transform;
    enum capture_state state;
    const char *error;
};

struct shot {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct ext_output_image_capture_source_manager_v1 *source_manager;
    struct ext_image_copy_capture_manager_v1 *copy_manager;
    struct zdwl_ipc_manager_v2 *ipc_manager;
    struct wl_list outputs;
    unsigned output_count;
};

#endif
