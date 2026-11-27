#ifndef VIEW_H
#define VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "desk.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define VIEW_VERSION "0.1.0"

/* Exit statuses. */
enum {
    EXIT_CLOSED = 0,
    EXIT_FAILED = 1,
    EXIT_USAGE = 2,
};

/* The decoded image as premultiplied ARGB8888, rows packed. */
struct picture {
    uint32_t width, height;
    uint32_t *pixels;
    bool opaque;
};

/* The picture resampled to one size, kept until the size changes. */
struct scaled {
    int width, height;
    uint32_t *pixels; /* NULL when the size equals the picture's. */
};

struct view {
    struct picture picture;
    struct scaled scaled;
    const char *name; /* Shown in the title. */

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;

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
};

#endif
