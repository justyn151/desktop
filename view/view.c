#define _GNU_SOURCE
#include "view.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* 256 megapixels as ARGB8888. */
#define MAX_IMAGE_BYTES ((size_t)1 << 30)
#define MAX_STDIN_BYTES ((size_t)512 << 20)
#define BACKGROUND 0xff1e1e1eu
/* An initial window never covers more than this share of the output. */
#define INITIAL_SHARE 0.8
#define MIN_INITIAL_WIDTH 240
#define MIN_INITIAL_HEIGHT 160
#define FALLBACK_WIDTH 1280
#define FALLBACK_HEIGHT 800

/* Loading */

/* Read all of stdin into a seekable stream, which the decoders need. */
static FILE *read_stdin(char **data)
{
    size_t size = 0, capacity = 1 << 16;
    char *buffer = malloc(capacity);
    if (!buffer)
        return NULL;
    for (;;) {
        if (size == capacity) {
            if (capacity >= MAX_STDIN_BYTES) {
                free(buffer);
                errno = EFBIG;
                return NULL;
            }
            capacity *= 2;
            char *grown = realloc(buffer, capacity);
            if (!grown) {
                free(buffer);
                return NULL;
            }
            buffer = grown;
        }
        ssize_t got = read(STDIN_FILENO, buffer + size, capacity - size);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            free(buffer);
            return NULL;
        }
        if (got == 0)
            break;
        size += (size_t)got;
    }
    if (size == 0) {
        free(buffer);
        errno = 0; /* Reported as an empty input. */
        return NULL;
    }
    FILE *file = fmemopen(buffer, size, "rb");
    if (!file) {
        free(buffer);
        return NULL;
    }
    *data = buffer;
    return file;
}

static bool load_picture(struct picture *picture, const char *path, const char *name)
{
    char *data = NULL;
    FILE *file;
    if (strcmp(path, "-") == 0) {
        if (isatty(STDIN_FILENO)) {
            fprintf(stderr, "view: refusing to read an image from a terminal\n");
            return false;
        }
        file = read_stdin(&data);
    } else {
        file = fopen(path, "rb");
    }
    if (!file) {
        fprintf(stderr, "view: %s: %s\n", name, errno ? strerror(errno) : "empty input");
        return false;
    }

    DeskImage image;
    char error[256];
    bool ok = desk_image_load(file, &image, MAX_IMAGE_BYTES, DESK_IMAGE_SHRINK, error, sizeof error);
    fclose(file);
    free(data);
    if (!ok) {
        fprintf(stderr, "view: %s: %s\n", name, error);
        return false;
    }

    /* Premultiplied RGBA bytes to ARGB8888 words, in place. */
    size_t count = (size_t)image.width * image.height;
    uint32_t *pixels = (uint32_t *)(void *)image.pixels;
    bool opaque = true;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *p = image.pixels + i * 4;
        uint32_t a = p[3];
        opaque &= a == 255;
        pixels[i] = a << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    *picture = (struct picture){ image.width, image.height, pixels, opaque };
    return true;
}

/* Scaling */

/* Area-average weights for shrinking source → target along one axis. Output i
   covers source [i·s, (i+1)·s) with s = source / target ≥ 1. */
struct weights {
    int *first, *count; /* Per output: first source index and how many. */
    float *weight;      /* count[i] weights for output i, after the previous outputs'. */
    int span;           /* Most weights for one output. */
};

static void weights_free(struct weights *w)
{
    free(w->first);
    free(w->count);
    free(w->weight);
}

static bool weights_build(struct weights *w, int source, int target)
{
    double s = (double)source / target;
    w->span = (int)ceil(s) + 1;
    w->first = malloc((size_t)target * sizeof *w->first);
    w->count = malloc((size_t)target * sizeof *w->count);
    w->weight = malloc((size_t)target * (size_t)w->span * sizeof *w->weight);
    if (!w->first || !w->count || !w->weight) {
        weights_free(w);
        return false;
    }
    for (int i = 0; i < target; ++i) {
        double start = i * s, end = (i + 1) * s;
        int first = (int)floor(start);
        int last = (int)ceil(end) - 1;
        if (last >= source)
            last = source - 1;
        float *weight = w->weight + (size_t)i * w->span;
        int count = 0;
        for (int j = first; j <= last && count < w->span; ++j) {
            double overlap = fmin(end, j + 1) - fmax(start, j);
            if (overlap > 0)
                weight[count++] = (float)(overlap / s);
            else if (count == 0)
                ++first;
        }
        w->first[i] = first;
        w->count[i] = count;
    }
    return true;
}

static inline uint32_t channel(float value)
{
    if (value <= 0)
        return 0;
    if (value >= 255)
        return 255;
    return (uint32_t)(value + 0.5f);
}

/* Shrink the picture to width × height by area averaging. Each source row is
   resampled horizontally and added into the output rows it overlaps, so the
   extra memory is a few rows, not a whole intermediate image. */
static uint32_t *shrink(const struct picture *picture, int width, int height)
{
    struct weights across = {0}, down = {0};
    uint32_t *out = malloc((size_t)width * height * 4);
    float *row = malloc((size_t)width * 4 * sizeof *row);
    float *sum = malloc((size_t)width * 4 * sizeof *sum);
    if (!out || !row || !sum || !weights_build(&across, (int)picture->width, width) ||
        !weights_build(&down, (int)picture->height, height)) {
        free(out);
        out = NULL;
        goto done;
    }

    for (int y = 0; y < height; ++y) {
        memset(sum, 0, (size_t)width * 4 * sizeof *sum);
        const float *vertical = down.weight + (size_t)y * down.span;
        for (int k = 0; k < down.count[y]; ++k) {
            const uint32_t *source = picture->pixels + (size_t)(down.first[y] + k) * picture->width;
            for (int x = 0; x < width; ++x) {
                const float *horizontal = across.weight + (size_t)x * across.span;
                const uint32_t *p = source + across.first[x];
                float a = 0, r = 0, g = 0, b = 0;
                for (int i = 0; i < across.count[x]; ++i) {
                    uint32_t pixel = p[i];
                    float w = horizontal[i];
                    a += w * (float)(pixel >> 24);
                    r += w * (float)(pixel >> 16 & 0xff);
                    g += w * (float)(pixel >> 8 & 0xff);
                    b += w * (float)(pixel & 0xff);
                }
                row[x * 4 + 0] = a;
                row[x * 4 + 1] = r;
                row[x * 4 + 2] = g;
                row[x * 4 + 3] = b;
            }
            float w = vertical[k];
            for (int i = 0; i < width * 4; ++i)
                sum[i] += w * row[i];
        }
        uint32_t *target = out + (size_t)y * width;
        for (int x = 0; x < width; ++x) {
            const float *s = sum + x * 4;
            target[x] = channel(s[0]) << 24 | channel(s[1]) << 16 | channel(s[2]) << 8 | channel(s[3]);
        }
    }

done:
    weights_free(&across);
    weights_free(&down);
    free(row);
    free(sum);
    return out;
}

/* The picture at width × height, from the cache when the size is unchanged. */
static const uint32_t *scaled_pixels(struct view *view, int width, int height)
{
    struct picture *picture = &view->picture;
    if ((uint32_t)width == picture->width && (uint32_t)height == picture->height)
        return picture->pixels;
    struct scaled *scaled = &view->scaled;
    if (scaled->pixels && scaled->width == width && scaled->height == height)
        return scaled->pixels;
    free(scaled->pixels);
    *scaled = (struct scaled){ width, height, shrink(picture, width, height) };
    return scaled->pixels;
}

/* Drawing */

static inline uint32_t over_background(uint32_t pixel)
{
    uint32_t inverse = 255 - (pixel >> 24);
    uint32_t out = 0xff000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t back = BACKGROUND >> shift & 0xff;
        uint32_t value = (pixel >> shift & 0xff) + (back * inverse + 127) / 255;
        out |= (value > 255 ? 255 : value) << shift;
    }
    return out;
}

/* The logical size as physical buffer pixels, and the wl_surface buffer scale. */
static void physical_size(const struct view *view, int *width, int *height, int *buffer_scale)
{
    if (view->viewport) {
        double scale = view->fraction ? view->fraction / 120.0 : view->buffer_scale;
        *width = (int)lround(view->width * scale);
        *height = (int)lround(view->height * scale);
        *buffer_scale = 1;
    } else {
        *width = view->width * view->buffer_scale;
        *height = view->height * view->buffer_scale;
        *buffer_scale = view->buffer_scale;
    }
    if (*width < 1)
        *width = 1;
    if (*height < 1)
        *height = 1;
}

static void paint(struct view *view, uint32_t *pixels, int width, int height)
{
    const struct picture *picture = &view->picture;
    /* Fit, never enlarged beyond one image pixel per physical pixel. */
    double fit = fmin(1.0, fmin((double)width / picture->width, (double)height / picture->height));
    int image_width = (int)lround(picture->width * fit);
    int image_height = (int)lround(picture->height * fit);
    if (image_width < 1)
        image_width = 1;
    if (image_height < 1)
        image_height = 1;
    if (image_width > width)
        image_width = width;
    if (image_height > height)
        image_height = height;

    for (size_t i = 0; i < (size_t)width * height; ++i)
        pixels[i] = BACKGROUND;
    const uint32_t *image = scaled_pixels(view, image_width, image_height);
    if (!image)
        return; /* Out of memory: leave the background. */

    int left = (width - image_width) / 2, top = (height - image_height) / 2;
    for (int y = 0; y < image_height; ++y) {
        const uint32_t *source = image + (size_t)y * image_width;
        uint32_t *target = pixels + (size_t)(top + y) * width + left;
        if (picture->opaque) {
            memcpy(target, source, (size_t)image_width * 4);
        } else {
            for (int x = 0; x < image_width; ++x)
                target[x] = over_background(source[x]);
        }
    }
}

static const struct wl_callback_listener frame_listener;

static void draw(struct view *view)
{
    int width, height, buffer_scale;
    physical_size(view, &width, &height, &buffer_scale);
    DeskBuffer *buffer = desk_buffer_get(&view->buffers, view->shm, width, height, WL_SHM_FORMAT_XRGB8888);
    if (!buffer)
        return; /* Both busy: retried after a release. */
    paint(view, buffer->pixels, width, height);

    if (view->viewport)
        wp_viewport_set_destination(view->viewport, view->width, view->height);
    wl_surface_set_buffer_scale(view->surface, buffer_scale);
    wl_surface_attach(view->surface, buffer->buffer, 0, 0);
    wl_surface_damage_buffer(view->surface, 0, 0, width, height);
    if (view->frame_callback)
        wl_callback_destroy(view->frame_callback);
    view->frame_callback = wl_surface_frame(view->surface);
    wl_callback_add_listener(view->frame_callback, &frame_listener, view);
    wl_surface_commit(view->surface);
    view->dirty = false;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)time;
    struct view *view = data;
    wl_callback_destroy(callback);
    view->frame_callback = NULL;
    if (view->dirty)
        draw(view);
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

/* Window */

/* The size to ask for when the compositor leaves it to us: the image at one
   pixel per logical pixel, shrunk to fit a share of the output. */
static void initial_size(const struct view *view, int *width, int *height)
{
    double bound_width = view->bounds_width > 0 ? view->bounds_width : FALLBACK_WIDTH / INITIAL_SHARE;
    double bound_height = view->bounds_height > 0 ? view->bounds_height : FALLBACK_HEIGHT / INITIAL_SHARE;
    double scale = view->fraction ? view->fraction / 120.0 : view->buffer_scale;
    double image_width = view->picture.width / scale, image_height = view->picture.height / scale;
    double fit = fmin(1.0, fmin(bound_width * INITIAL_SHARE / image_width,
                                bound_height * INITIAL_SHARE / image_height));
    *width = (int)lround(image_width * fit);
    *height = (int)lround(image_height * fit);
    if (*width < MIN_INITIAL_WIDTH)
        *width = MIN_INITIAL_WIDTH;
    if (*height < MIN_INITIAL_HEIGHT)
        *height = MIN_INITIAL_HEIGHT;
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct view *view = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    int width = view->pending_width, height = view->pending_height;
    if (width <= 0 || height <= 0) {
        if (view->configured) {
            width = view->width;
            height = view->height;
        } else {
            initial_size(view, &width, &height);
        }
    }
    bool changed = !view->configured || width != view->width || height != view->height;
    view->width = width;
    view->height = height;
    view->configured = true;
    if (changed || view->dirty)
        draw(view); /* Commit now, without waiting for a frame, to answer the configure. */
}

static const struct xdg_surface_listener xdg_surface_listener = {.configure = xdg_surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)toplevel;
    (void)states;
    struct view *view = data;
    view->pending_width = width;
    view->pending_height = height;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    struct view *view = data;
    view->running = false;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height)
{
    (void)toplevel;
    struct view *view = data;
    view->bounds_width = width;
    view->bounds_height = height;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel, struct wl_array *capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

/* Scale */

static void mark_dirty(struct view *view)
{
    view->dirty = true;
    if (view->configured && !view->frame_callback)
        draw(view);
}

static void fractional_preferred_scale(void *data, struct wp_fractional_scale_v1 *fractional, uint32_t scale)
{
    (void)fractional;
    struct view *view = data;
    if (scale == view->fraction)
        return;
    view->fraction = scale;
    mark_dirty(view);
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = fractional_preferred_scale,
};

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

static void surface_preferred_buffer_scale(void *data, struct wl_surface *surface, int32_t factor)
{
    (void)surface;
    struct view *view = data;
    if (factor < 1 || factor == view->buffer_scale)
        return;
    view->buffer_scale = factor;
    if (!view->fraction)
        mark_dirty(view);
}

static void surface_preferred_buffer_transform(void *data, struct wl_surface *surface, uint32_t transform)
{
    (void)data;
    (void)surface;
    (void)transform;
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_preferred_buffer_scale,
    .preferred_buffer_transform = surface_preferred_buffer_transform,
};

/* Keyboard */

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    (void)keyboard;
    struct view *view = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;
    struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(view->xkb, map, strnlen(map, size),
                                                           XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!keymap)
        return;
    struct xkb_state *keys = xkb_state_new(keymap);
    if (!keys) {
        xkb_keymap_unref(keymap);
        return;
    }
    xkb_state_unref(view->keys);
    xkb_keymap_unref(view->keymap);
    view->keymap = keymap;
    view->keys = keys;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    struct view *view = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !view->keys)
        return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(view->keys, key + 8);
    switch (sym) {
    case XKB_KEY_q:
    case XKB_KEY_Q:
    case XKB_KEY_Escape:
        view->running = false;
        break;
    default:
        break;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)keyboard;
    (void)serial;
    struct view *view = data;
    if (view->keys)
        xkb_state_update_mask(view->keys, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct view *view = data;
    bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && !view->keyboard) {
        view->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(view->keyboard, &keyboard_listener, view);
    } else if (!has_keyboard && view->keyboard) {
        wl_keyboard_release(view->keyboard);
        view->keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* Registry */

static uint32_t min_version(uint32_t offered, uint32_t wanted)
{
    return offered < wanted ? offered : wanted;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                            uint32_t version)
{
    struct view *view = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        /* Version 6 adds preferred_buffer_scale. */
        view->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, min_version(version, 6));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        view->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        /* Version 4 adds configure_bounds and 5 wm_capabilities. */
        view->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, min_version(version, 5));
        xdg_wm_base_add_listener(view->wm_base, &wm_base_listener, view);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        view->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        view->fractional_manager =
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !view->seat) {
        view->seat = wl_registry_bind(registry, name, &wl_seat_interface, min_version(version, 5));
        wl_seat_add_listener(view->seat, &seat_listener, view);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static bool open_window(struct view *view)
{
    view->display = wl_display_connect(NULL);
    if (!view->display) {
        fprintf(stderr, "view: cannot connect to the Wayland display\n");
        return false;
    }
    view->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    view->registry = wl_display_get_registry(view->display);
    wl_registry_add_listener(view->registry, &registry_listener, view);
    if (wl_display_roundtrip(view->display) < 0) {
        fprintf(stderr, "view: lost the Wayland connection\n");
        return false;
    }
    const char *missing = !view->compositor ? "wl_compositor"
                        : !view->shm        ? "wl_shm"
                        : !view->wm_base    ? "xdg_wm_base"
                                            : NULL;
    if (missing) {
        fprintf(stderr, "view: the compositor doesn't offer %s\n", missing);
        return false;
    }

    view->surface = wl_compositor_create_surface(view->compositor);
    wl_surface_add_listener(view->surface, &surface_listener, view);
    if (view->viewporter) {
        view->viewport = wp_viewporter_get_viewport(view->viewporter, view->surface);
        /* Fractional scale is only usable through a viewport. */
        if (view->fractional_manager) {
            view->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(view->fractional_manager,
                                                                                    view->surface);
            wp_fractional_scale_v1_add_listener(view->fractional, &fractional_listener, view);
        }
    }
    view->xdg_surface = xdg_wm_base_get_xdg_surface(view->wm_base, view->surface);
    xdg_surface_add_listener(view->xdg_surface, &xdg_surface_listener, view);
    view->toplevel = xdg_surface_get_toplevel(view->xdg_surface);
    xdg_toplevel_add_listener(view->toplevel, &toplevel_listener, view);
    xdg_toplevel_set_app_id(view->toplevel, "view");

    char title[512];
    snprintf(title, sizeof title, "%s — %" PRIu32 "×%" PRIu32, view->name, view->picture.width,
             view->picture.height);
    xdg_toplevel_set_title(view->toplevel, title);
    wl_surface_commit(view->surface);
    return true;
}

static void close_window(struct view *view)
{
    if (view->frame_callback)
        wl_callback_destroy(view->frame_callback);
    desk_buffer_pool_finish(&view->buffers);
    if (view->toplevel)
        xdg_toplevel_destroy(view->toplevel);
    if (view->xdg_surface)
        xdg_surface_destroy(view->xdg_surface);
    if (view->fractional)
        wp_fractional_scale_v1_destroy(view->fractional);
    if (view->viewport)
        wp_viewport_destroy(view->viewport);
    if (view->surface)
        wl_surface_destroy(view->surface);
    if (view->keyboard)
        wl_keyboard_release(view->keyboard);
    xkb_state_unref(view->keys);
    xkb_keymap_unref(view->keymap);
    xkb_context_unref(view->xkb);
    if (view->display)
        wl_display_disconnect(view->display);
}

/* Main */

static const char *base_name(const char *path)
{
    if (strcmp(path, "-") == 0)
        return "stdin";
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static void usage(FILE *stream)
{
    fputs("Usage: view [FILE]\n"
          "Show a PNG or JPEG image. FILE \"-\" reads standard input.\n"
          "\n"
          "  -h, --help     Show this help\n"
          "  -v, --version  Show the version\n"
          "\n"
          "Keys: q or Escape closes the window.\n",
          stream);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {0},
    };
    int option;
    while ((option = getopt_long(argc, argv, "hv", options, NULL)) != -1) {
        switch (option) {
        case 'h': usage(stdout); return EXIT_CLOSED;
        case 'v': puts("view " VIEW_VERSION); return EXIT_CLOSED;
        default: usage(stderr); return EXIT_USAGE;
        }
    }
    if (argc - optind != 1) {
        usage(stderr);
        return EXIT_USAGE;
    }

    const char *path = argv[optind];
    struct view view = { .name = base_name(path), .buffer_scale = 1, .running = true };
    if (!load_picture(&view.picture, path, strcmp(path, "-") == 0 ? "stdin" : path))
        return EXIT_FAILED;

    int status = EXIT_CLOSED;
    if (!open_window(&view)) {
        status = EXIT_FAILED;
    } else {
        while (view.running) {
            if (wl_display_dispatch(view.display) < 0) {
                fprintf(stderr, "view: lost the Wayland connection\n");
                status = EXIT_FAILED;
                break;
            }
            /* A buffer may have been released since a skipped draw. */
            if (view.dirty && view.configured && !view.frame_callback)
                draw(&view);
        }
    }
    close_window(&view);
    free(view.scaled.pixels);
    free(view.picture.pixels);
    return status;
}
