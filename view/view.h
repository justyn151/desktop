#ifndef VIEW_H
#define VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "desk.h"
#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define VIEW_VERSION "0.2.0"

/* Exit statuses. */
enum {
    EXIT_CLOSED = 0,
    EXIT_FAILED = 1,
    EXIT_USAGE = 2,
};

#define MAX_LEVELS 24

/* Premultiplied ARGB8888, rows packed. */
struct level {
    uint32_t width, height;
    uint32_t *pixels;
};

/* The decoded image, upright as currently shown, and its mip levels: level
   k is level k−1 halved by 2×2 averaging. levels[0] is the image itself. */
struct picture {
    struct level levels[MAX_LEVELS];
    int level_count;
    bool opaque;
    unsigned generation; /* Changes whenever the pixels do. */
};

/* The last frame drawn, kept so panning only renders what scrolled in. */
struct frame {
    uint32_t *pixels;
    int width, height;
    double zoom;
    int left, top; /* Where the image's top-left corner was, in buffer pixels. */
    unsigned generation;
    bool valid;
};

struct view {
    struct picture picture;
    struct frame frame;
    const char *name; /* Shown in the title. */
    char title[512];

    /* What's shown. zoom is buffer pixels per image pixel; the centre is in
       image pixels. While fit is set, both follow the window size. */
    bool fit;
    double zoom, center_x, center_y;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wp_cursor_shape_device_v1 *cursor_shape;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional;
    struct wl_callback *frame_callback;
    DeskBufferPool buffers;

    int width, height;                 /* Logical size in use. */
    int pending_width, pending_height; /* From the latest toplevel configure. */
    int bounds_width, bounds_height;   /* configure_bounds, 0 when unknown. */
    uint32_t fraction;                 /* Preferred scale × 120, 0 when unknown. */
    int buffer_scale;                  /* Preferred integer scale. */
    bool configured, dirty, running;

    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *keys;
    DeskRepeat repeat;

    /* Pointer, in logical surface coordinates. */
    uint32_t pointer_serial;
    double pointer_x, pointer_y;
    bool dragging;
    uint32_t axis_source;
    double axis_value;     /* Continuous vertical scroll this frame. */
    int32_t axis_value120; /* Wheel scroll this frame, 120 per notch. */
    bool axis_wheel;       /* axis_value120 arrived this frame. */
};

#endif
