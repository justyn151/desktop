#define _GNU_SOURCE
#include "view.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
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

#define ZOOM_STEP 1.4142135623730951
#define MIN_ZOOM 0.1        /* Unless fitting needs less. */
#define MAX_ZOOM 32.0
#define NEAREST_ZOOM 4.0    /* From here up, pixels are drawn as sharp squares. */
#define PAN_STEP 0.1        /* Of the window, per key press. */
#define SMOOTH_SCROLL 200.0 /* Touchpad scroll, in logical pixels, that halves or doubles the zoom. */
#define MAX_REPEATS 8       /* Repeats handled at once after a stall. */

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

/* Level k from level k−1: each pixel averages the 2×2 (or fewer, at an odd
   edge) pixels it covers. */
static bool halve(const struct level *source, struct level *target)
{
    uint32_t width = (source->width + 1) / 2, height = (source->height + 1) / 2;
    uint32_t *pixels = malloc((size_t)width * height * 4);
    if (!pixels)
        return false;
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t *row0 = source->pixels + (size_t)y * 2 * source->width;
        const uint32_t *row1 = y * 2 + 1 < source->height ? row0 + source->width : row0;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t x0 = x * 2, x1 = x0 + 1 < source->width ? x0 + 1 : x0;
            uint32_t p[4] = { row0[x0], row0[x1], row1[x0], row1[x1] };
            uint32_t out = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                uint32_t sum = 0;
                for (int i = 0; i < 4; ++i)
                    sum += p[i] >> shift & 0xff;
                out |= (sum + 2) / 4 << shift;
            }
            pixels[(size_t)y * width + x] = out;
        }
    }
    *target = (struct level){ width, height, pixels };
    return true;
}

static void levels_free(struct picture *picture)
{
    for (int i = 1; i < picture->level_count; ++i)
        free(picture->levels[i].pixels);
    picture->level_count = picture->levels[0].pixels ? 1 : 0;
}

/* Rebuild the halved levels from levels[0]. Running out of memory only means
   fewer levels, which costs speed when zoomed far out, not correctness. */
static void levels_build(struct picture *picture)
{
    levels_free(picture);
    picture->level_count = 1;
    while (picture->level_count < MAX_LEVELS) {
        const struct level *last = &picture->levels[picture->level_count - 1];
        if (last->width == 1 && last->height == 1)
            break;
        if (!halve(last, &picture->levels[picture->level_count]))
            break;
        ++picture->level_count;
    }
    ++picture->generation;
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
    *picture = (struct picture){ .opaque = opaque };
    picture->levels[0] = (struct level){ image.width, image.height, pixels };
    levels_build(picture);
    return true;
}

static void picture_free(struct picture *picture)
{
    levels_free(picture);
    free(picture->levels[0].pixels);
    *picture = (struct picture){ .level_count = 0 };
}

/* Orientation. Applied to the pixels, so everything after works on an upright
   image. Rotation and flips are view-only: the file is never written. */

enum orientation {
    ROTATE_CLOCKWISE,
    ROTATE_COUNTER_CLOCKWISE,
    FLIP_HORIZONTAL,
    FLIP_VERTICAL,
};

static bool orient_pixels(struct level *image, enum orientation change)
{
    uint32_t width = image->width, height = image->height;
    if (change == FLIP_HORIZONTAL) {
        for (uint32_t y = 0; y < height; ++y) {
            uint32_t *row = image->pixels + (size_t)y * width;
            for (uint32_t a = 0, b = width - 1; a < b; ++a, --b) {
                uint32_t t = row[a];
                row[a] = row[b];
                row[b] = t;
            }
        }
        return true;
    }
    if (change == FLIP_VERTICAL) {
        for (uint32_t a = 0, b = height - 1; a < b; ++a, --b) {
            uint32_t *top = image->pixels + (size_t)a * width, *bottom = image->pixels + (size_t)b * width;
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t t = top[x];
                top[x] = bottom[x];
                bottom[x] = t;
            }
        }
        return true;
    }

    uint32_t *rotated = malloc((size_t)width * height * 4);
    if (!rotated)
        return false;
    bool clockwise = change == ROTATE_CLOCKWISE;
    /* In tiles, so neither the reads nor the writes stride across the whole image. */
    enum { TILE = 64 };
    for (uint32_t ty = 0; ty < height; ty += TILE) {
        for (uint32_t tx = 0; tx < width; tx += TILE) {
            uint32_t y_end = ty + TILE < height ? ty + TILE : height;
            uint32_t x_end = tx + TILE < width ? tx + TILE : width;
            for (uint32_t y = ty; y < y_end; ++y) {
                const uint32_t *row = image->pixels + (size_t)y * width;
                for (uint32_t x = tx; x < x_end; ++x) {
                    /* The result is height wide. */
                    size_t target = clockwise ? (size_t)x * height + (height - 1 - y)
                                              : (size_t)(width - 1 - x) * height + y;
                    rotated[target] = row[x];
                }
            }
        }
    }
    free(image->pixels);
    *image = (struct level){ height, width, rotated };
    return true;
}

/* Layout */

static double fit_zoom(const struct view *view, int width, int height)
{
    const struct level *image = &view->picture.levels[0];
    /* Never enlarged: a small image shows one image pixel per buffer pixel. */
    return fmin(1.0, fmin((double)width / image->width, (double)height / image->height));
}

static double clamp_axis(double center, double extent, double window, double zoom)
{
    if (extent * zoom <= window)
        return extent / 2;
    double half = window / (2 * zoom);
    return fmin(fmax(center, half), extent - half);
}

/* Bring the zoom and centre up to date for a buffer of this size: follow it
   while fitting, otherwise keep the image covering the window where it can. */
static void layout(struct view *view, int width, int height)
{
    const struct level *image = &view->picture.levels[0];
    if (view->fit) {
        view->zoom = fit_zoom(view, width, height);
        view->center_x = image->width / 2.0;
        view->center_y = image->height / 2.0;
    }
    view->center_x = clamp_axis(view->center_x, image->width, width, view->zoom);
    view->center_y = clamp_axis(view->center_y, image->height, height, view->zoom);
}

/* The logical size as buffer pixels, and the wl_surface buffer scale. */
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

/* Sampling */

/* How each output column (or row) is made from source pixels along one axis:
   count[i] weights for source pixels first[i], first[i]+1, …. Weights summing
   to less than 1 mean the pixel is partly background, at the image's edge. */
struct taps {
    int *first, *count;
    float *weight; /* span per output. */
    int span;
};

static void taps_free(struct taps *taps)
{
    free(taps->first);
    free(taps->count);
    free(taps->weight);
    *taps = (struct taps){ .span = 0 };
}

/* outputs pixels, with the image's edge at origin and zoom buffer pixels per
   image pixel. Shrinking averages the area each output pixel covers in mip
   level level_index; enlarging is bilinear, then nearest from NEAREST_ZOOM. */
static bool taps_build(struct taps *taps, int outputs, int origin, double zoom, const struct level *level,
                       int level_index, uint32_t full_size, bool horizontal)
{
    double unit = ldexp(1.0, level_index); /* Image pixels per level pixel. */
    double width = 1.0 / (zoom * unit);    /* Level pixels per output pixel. */
    uint32_t level_size = horizontal ? level->width : level->height;
    double extent = full_size / unit;      /* The image's real edge, in level pixels. */
    taps->span = zoom < 1 ? (int)ceil(width) + 1 : 2;
    taps->first = malloc((size_t)outputs * sizeof *taps->first);
    taps->count = malloc((size_t)outputs * sizeof *taps->count);
    taps->weight = malloc((size_t)outputs * (size_t)taps->span * sizeof *taps->weight);
    if (!taps->first || !taps->count || !taps->weight) {
        taps_free(taps);
        return false;
    }

    for (int p = 0; p < outputs; ++p) {
        float *weight = taps->weight + (size_t)p * taps->span;
        int first = 0, count = 0;
        if (zoom < 1) {
            double a = fmax((p - origin) * width, 0), b = fmin((p + 1 - origin) * width, extent);
            if (b > a) {
                first = (int)floor(a);
                for (int j = first; j < b && j < (int)level_size && count < taps->span; ++j)
                    weight[count++] = (float)((fmin(b, j + 1) - fmax(a, j)) / width);
            }
        } else {
            double u = (p + 0.5 - origin) / zoom;
            if (u >= 0 && u < full_size) {
                if (zoom >= NEAREST_ZOOM) {
                    first = (int)u;
                    weight[count++] = 1;
                } else {
                    double s = u - 0.5;
                    int i = (int)floor(s);
                    double f = s - i;
                    if (i < 0) {
                        i = 0;
                        f = 0;
                    } else if (i >= (int)full_size - 1) {
                        i = (int)full_size - 1;
                        f = 0;
                    }
                    first = i;
                    if (f < 1e-6) {
                        weight[count++] = 1;
                    } else {
                        weight[count++] = (float)(1 - f);
                        weight[count++] = (float)f;
                    }
                }
            }
        }
        taps->first[p] = first;
        taps->count[p] = count;
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

/* A premultiplied pixel over the opaque background. */
static inline uint32_t over_background(float a, float r, float g, float b)
{
    float inverse = 1 - a / 255;
    return 0xff000000u | channel(r + (BACKGROUND >> 16 & 0xff) * inverse) << 16 |
           channel(g + (BACKGROUND >> 8 & 0xff) * inverse) << 8 | channel(b + (BACKGROUND & 0xff) * inverse);
}

static inline uint32_t pixel_over_background(uint32_t pixel)
{
    if (pixel >> 24 == 255)
        return pixel;
    return over_background((float)(pixel >> 24), (float)(pixel >> 16 & 0xff), (float)(pixel >> 8 & 0xff),
                           (float)(pixel & 0xff));
}

struct sampler {
    const struct level *level;
    struct taps columns, rows;
};

/* Render the rectangle [x0, x1) × [y0, y1) of the frame. */
static void render(const struct sampler *sampler, struct frame *frame, int x0, int y0, int x1, int y1)
{
    const struct level *level = sampler->level;
    const struct taps *columns = &sampler->columns, *rows = &sampler->rows;
    for (int y = y0; y < y1; ++y) {
        uint32_t *target = frame->pixels + (size_t)y * frame->width;
        int row_count = rows->count[y];
        if (!row_count) {
            for (int x = x0; x < x1; ++x)
                target[x] = BACKGROUND;
            continue;
        }
        const float *row_weight = rows->weight + (size_t)y * rows->span;
        const uint32_t *source_row = level->pixels + (size_t)rows->first[y] * level->width;
        bool row_single = row_count == 1 && row_weight[0] == 1.0f;
        for (int x = x0; x < x1; ++x) {
            int column_count = columns->count[x];
            if (!column_count) {
                target[x] = BACKGROUND;
                continue;
            }
            const float *column_weight = columns->weight + (size_t)x * columns->span;
            const uint32_t *source = source_row + columns->first[x];
            if (row_single && column_count == 1 && column_weight[0] == 1.0f) {
                target[x] = pixel_over_background(*source);
                continue;
            }
            float a = 0, r = 0, g = 0, b = 0;
            for (int i = 0; i < row_count; ++i, source += level->width) {
                for (int k = 0; k < column_count; ++k) {
                    uint32_t pixel = source[k];
                    float w = row_weight[i] * column_weight[k];
                    a += w * (float)(pixel >> 24);
                    r += w * (float)(pixel >> 16 & 0xff);
                    g += w * (float)(pixel >> 8 & 0xff);
                    b += w * (float)(pixel & 0xff);
                }
            }
            target[x] = over_background(a, r, g, b);
        }
    }
}

/* Move the kept frame's pixels by (dx, dy). */
static void shift_frame(struct frame *frame, int dx, int dy)
{
    int width = frame->width, height = frame->height;
    size_t length = (size_t)(width - abs(dx)) * 4;
    int source_x = dx < 0 ? -dx : 0, target_x = dx > 0 ? dx : 0;
    if (dy > 0) {
        for (int y = height - 1 - dy; y >= 0; --y)
            memmove(frame->pixels + (size_t)(y + dy) * width + target_x,
                    frame->pixels + (size_t)y * width + source_x, length);
    } else {
        for (int y = -dy; y < height; ++y)
            memmove(frame->pixels + (size_t)(y + dy) * width + target_x,
                    frame->pixels + (size_t)y * width + source_x, length);
    }
}

static void fill_background(uint32_t *pixels, int width, int height)
{
    for (size_t i = 0; i < (size_t)width * height; ++i)
        pixels[i] = BACKGROUND;
}

/* Draw the current view into pixels. The previous frame is kept: when only
   the position changed, it is shifted and only the uncovered strips are
   rendered, so panning costs what scrolls in, not the whole window. */
static void paint(struct view *view, uint32_t *pixels, int width, int height)
{
    struct picture *picture = &view->picture;
    struct frame *frame = &view->frame;
    layout(view, width, height);
    double zoom = view->zoom;
    const struct level *image = &picture->levels[0];
    int left = (int)lround(width / 2.0 - view->center_x * zoom);
    int top = (int)lround(height / 2.0 - view->center_y * zoom);

    if (!frame->pixels || frame->width != width || frame->height != height) {
        free(frame->pixels);
        *frame = (struct frame){ .pixels = malloc((size_t)width * height * 4), .width = width, .height = height };
        if (!frame->pixels) {
            fill_background(pixels, width, height);
            return;
        }
    }

    int level_index = 0;
    if (zoom < 1) {
        level_index = (int)floor(log2(1 / zoom) + 1e-9);
        if (level_index >= picture->level_count)
            level_index = picture->level_count - 1;
    }
    struct sampler sampler = { .level = &picture->levels[level_index] };
    if (!taps_build(&sampler.columns, width, left, zoom, sampler.level, level_index, image->width, true) ||
        !taps_build(&sampler.rows, height, top, zoom, sampler.level, level_index, image->height, false)) {
        taps_free(&sampler.columns);
        frame->valid = false;
        fill_background(pixels, width, height);
        return;
    }

    int dx = left - frame->left, dy = top - frame->top;
    bool reuse = frame->valid && frame->zoom == zoom && frame->generation == picture->generation &&
                 abs(dx) < width && abs(dy) < height;
    if (!reuse) {
        render(&sampler, frame, 0, 0, width, height);
    } else if (dx || dy) {
        shift_frame(frame, dx, dy);
        if (dy > 0)
            render(&sampler, frame, 0, 0, width, dy);
        else if (dy < 0)
            render(&sampler, frame, 0, height + dy, width, height);
        if (dx > 0)
            render(&sampler, frame, 0, 0, dx, height);
        else if (dx < 0)
            render(&sampler, frame, width + dx, 0, width, height);
    }
    taps_free(&sampler.columns);
    taps_free(&sampler.rows);

    frame->zoom = zoom;
    frame->left = left;
    frame->top = top;
    frame->generation = picture->generation;
    frame->valid = true;
    memcpy(pixels, frame->pixels, (size_t)width * height * 4);
}

/* Drawing */

static void update_title(struct view *view)
{
    const struct level *image = &view->picture.levels[0];
    char title[sizeof view->title];
    snprintf(title, sizeof title, "%s — %" PRIu32 "×%" PRIu32 " — %.0f%%", view->name, image->width,
             image->height, view->zoom * 100);
    if (strcmp(title, view->title) != 0) {
        memcpy(view->title, title, sizeof title);
        xdg_toplevel_set_title(view->toplevel, title);
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
    update_title(view);

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

/* Redraw on the next frame, or now when none is pending. */
static void mark_dirty(struct view *view)
{
    view->dirty = true;
    if (view->configured && !view->frame_callback)
        draw(view);
}

/* Actions */

/* Zoom to `zoom`, keeping the image point under buffer pixel (x, y) still. */
static void zoom_at(struct view *view, double zoom, double x, double y)
{
    int width, height, scale;
    physical_size(view, &width, &height, &scale);
    layout(view, width, height);
    double old = view->zoom;
    double lowest = fmin(MIN_ZOOM, fit_zoom(view, width, height));
    zoom = fmin(fmax(zoom, lowest), MAX_ZOOM);
    /* Stop at 100% on the way through it. */
    if ((old < 1 && zoom > 1) || (old > 1 && zoom < 1))
        zoom = 1;
    if (zoom == old)
        return;
    double image_x = view->center_x + (x - width / 2.0) / old;
    double image_y = view->center_y + (y - height / 2.0) / old;
    view->fit = false;
    view->zoom = zoom;
    view->center_x = image_x - (x - width / 2.0) / zoom;
    view->center_y = image_y - (y - height / 2.0) / zoom;
    layout(view, width, height);
    mark_dirty(view);
}

static void zoom_centered(struct view *view, double zoom)
{
    int width, height, scale;
    physical_size(view, &width, &height, &scale);
    zoom_at(view, zoom, width / 2.0, height / 2.0);
}

/* Move the view by (dx, dy) buffer pixels. Infinite values go to the edge. */
static void pan(struct view *view, double dx, double dy)
{
    int width, height, scale;
    physical_size(view, &width, &height, &scale);
    layout(view, width, height);
    double x = view->center_x, y = view->center_y;
    view->center_x += dx / view->zoom;
    view->center_y += dy / view->zoom;
    layout(view, width, height);
    if (view->center_x != x || view->center_y != y)
        mark_dirty(view);
}

static void pan_step(struct view *view, int columns, int rows)
{
    int width, height, scale;
    physical_size(view, &width, &height, &scale);
    pan(view, columns * PAN_STEP * width, rows * PAN_STEP * height);
}

static void orient(struct view *view, enum orientation change)
{
    struct picture *picture = &view->picture;
    struct level *image = &picture->levels[0];
    double x = view->center_x, y = view->center_y;
    double width = image->width, height = image->height;
    if (!orient_pixels(image, change)) {
        fprintf(stderr, "view: not enough memory to rotate\n");
        return;
    }
    switch (change) {
    case ROTATE_CLOCKWISE:
        view->center_x = height - y;
        view->center_y = x;
        break;
    case ROTATE_COUNTER_CLOCKWISE:
        view->center_x = y;
        view->center_y = width - x;
        break;
    case FLIP_HORIZONTAL:
        view->center_x = width - x;
        break;
    case FLIP_VERTICAL:
        view->center_y = height - y;
        break;
    }
    levels_build(picture);
    mark_dirty(view);
}

/* Run the action for a key. Return whether it repeats while held. */
static bool key_action(struct view *view, xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_q:
    case XKB_KEY_Q:
    case XKB_KEY_Escape:
        view->running = false;
        return false;
    case XKB_KEY_plus:
    case XKB_KEY_equal:
    case XKB_KEY_KP_Add:
        zoom_centered(view, view->zoom * ZOOM_STEP);
        return true;
    case XKB_KEY_minus:
    case XKB_KEY_KP_Subtract:
        zoom_centered(view, view->zoom / ZOOM_STEP);
        return true;
    case XKB_KEY_0:
    case XKB_KEY_KP_0:
        view->fit = true;
        mark_dirty(view);
        return false;
    case XKB_KEY_1:
    case XKB_KEY_KP_1:
        zoom_centered(view, 1);
        return false;
    case XKB_KEY_h:
    case XKB_KEY_Left:
        pan_step(view, -1, 0);
        return true;
    case XKB_KEY_l:
    case XKB_KEY_Right:
        pan_step(view, 1, 0);
        return true;
    case XKB_KEY_k:
    case XKB_KEY_Up:
        pan_step(view, 0, -1);
        return true;
    case XKB_KEY_j:
    case XKB_KEY_Down:
        pan_step(view, 0, 1);
        return true;
    case XKB_KEY_H:
        pan(view, -INFINITY, 0);
        return false;
    case XKB_KEY_L:
        pan(view, INFINITY, 0);
        return false;
    case XKB_KEY_K:
        pan(view, 0, -INFINITY);
        return false;
    case XKB_KEY_J:
        pan(view, 0, INFINITY);
        return false;
    case XKB_KEY_r:
        orient(view, ROTATE_CLOCKWISE);
        return false;
    case XKB_KEY_R:
        orient(view, ROTATE_COUNTER_CLOCKWISE);
        return false;
    case XKB_KEY_f:
        orient(view, FLIP_HORIZONTAL);
        return false;
    case XKB_KEY_F:
        orient(view, FLIP_VERTICAL);
        return false;
    default:
        return false;
    }
}

/* Window */

/* The size to ask for when the compositor leaves it to us: the image at one
   pixel per logical pixel, shrunk to fit a share of the output. */
static void initial_size(const struct view *view, int *width, int *height)
{
    const struct level *image = &view->picture.levels[0];
    double bound_width = view->bounds_width > 0 ? view->bounds_width : FALLBACK_WIDTH / INITIAL_SHARE;
    double bound_height = view->bounds_height > 0 ? view->bounds_height : FALLBACK_HEIGHT / INITIAL_SHARE;
    double scale = view->fraction ? view->fraction / 120.0 : view->buffer_scale;
    double image_width = image->width / scale, image_height = image->height / scale;
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
    (void)keyboard;
    (void)serial;
    (void)surface;
    struct view *view = data;
    desk_repeat_cancel(&view->repeat);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    struct view *view = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        desk_repeat_release(&view->repeat, key);
        return;
    }
    if (!view->keys)
        return;
    xkb_keycode_t code = key + 8;
    bool repeats = key_action(view, xkb_state_key_get_one_sym(view->keys, code));
    if (repeats && xkb_keymap_key_repeats(view->keymap, code))
        desk_repeat_start(&view->repeat, key);
    else
        desk_repeat_cancel(&view->repeat);
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
    (void)keyboard;
    struct view *view = data;
    desk_repeat_info(&view->repeat, rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* Pointer */

static void set_cursor(struct view *view, enum wp_cursor_shape_device_v1_shape shape)
{
    if (view->cursor_shape)
        wp_cursor_shape_device_v1_set_shape(view->cursor_shape, view->pointer_serial, shape);
}

/* Buffer pixels per logical surface unit. */
static double buffer_units(const struct view *view)
{
    int width, height, scale;
    physical_size(view, &width, &height, &scale);
    return view->width > 0 ? (double)width / view->width : 1;
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)surface;
    struct view *view = data;
    view->pointer_serial = serial;
    view->pointer_x = wl_fixed_to_double(x);
    view->pointer_y = wl_fixed_to_double(y);
    set_cursor(view, view->dragging ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING
                                    : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    struct view *view = data;
    view->dragging = false;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)time;
    struct view *view = data;
    double new_x = wl_fixed_to_double(x), new_y = wl_fixed_to_double(y);
    if (view->dragging) {
        double units = buffer_units(view);
        /* The image follows the pointer, so the view moves the other way. */
        pan(view, (view->pointer_x - new_x) * units, (view->pointer_y - new_y) * units);
    }
    view->pointer_x = new_x;
    view->pointer_y = new_y;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)serial;
    (void)time;
    struct view *view = data;
    if (button != BTN_LEFT)
        return;
    view->dragging = state == WL_POINTER_BUTTON_STATE_PRESSED;
    set_cursor(view, view->dragging ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING
                                    : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer;
    (void)time;
    struct view *view = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        view->axis_value += wl_fixed_to_double(value);
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120)
{
    (void)pointer;
    struct view *view = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        view->axis_value120 += value120;
        view->axis_wheel = true;
    }
}

/* Scrolling zooms around the pointer: a wheel by one step per notch, a
   touchpad smoothly. Scrolling down zooms out. */
static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)pointer;
    struct view *view = data;
    double factor = 1;
    if (view->axis_wheel)
        factor = pow(ZOOM_STEP, -view->axis_value120 / 120.0);
    else if (view->axis_value != 0)
        factor = pow(2, -view->axis_value / SMOOTH_SCROLL);
    view->axis_value = 0;
    view->axis_value120 = 0;
    view->axis_wheel = false;
    if (factor != 1) {
        double units = buffer_units(view);
        zoom_at(view, view->zoom * factor, view->pointer_x * units, view->pointer_y * units);
    }
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)pointer;
    struct view *view = data;
    view->axis_source = source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
}

static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis,
                                            uint32_t direction)
{
    (void)data;
    (void)pointer;
    (void)axis;
    (void)direction;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
};

/* Seat */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct view *view = data;
    bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && !view->keyboard) {
        view->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(view->keyboard, &keyboard_listener, view);
    } else if (!has_keyboard && view->keyboard) {
        desk_repeat_cancel(&view->repeat);
        wl_keyboard_release(view->keyboard);
        view->keyboard = NULL;
    }

    bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && !view->pointer) {
        view->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(view->pointer, &pointer_listener, view);
        if (view->cursor_shape_manager)
            view->cursor_shape = wp_cursor_shape_manager_v1_get_pointer(view->cursor_shape_manager, view->pointer);
    } else if (!has_pointer && view->pointer) {
        if (view->cursor_shape)
            wp_cursor_shape_device_v1_destroy(view->cursor_shape);
        view->cursor_shape = NULL;
        wl_pointer_release(view->pointer);
        view->pointer = NULL;
        view->dragging = false;
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
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        view->cursor_shape_manager = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !view->seat) {
        /* Version 8 adds axis_value120 for wheel notches. */
        view->seat = wl_registry_bind(registry, name, &wl_seat_interface, min_version(version, 8));
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
    /* The seat's capabilities may have arrived before the cursor shape manager. */
    if (view->pointer && view->cursor_shape_manager && !view->cursor_shape)
        view->cursor_shape = wp_cursor_shape_manager_v1_get_pointer(view->cursor_shape_manager, view->pointer);

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
    update_title(view);
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
    if (view->cursor_shape)
        wp_cursor_shape_device_v1_destroy(view->cursor_shape);
    if (view->pointer)
        wl_pointer_release(view->pointer);
    if (view->keyboard)
        wl_keyboard_release(view->keyboard);
    xkb_state_unref(view->keys);
    xkb_keymap_unref(view->keymap);
    xkb_context_unref(view->xkb);
    if (view->display)
        wl_display_disconnect(view->display);
}

/* Dispatch Wayland events and key repeats until the window closes. */
static bool run(struct view *view)
{
    struct wl_display *display = view->display;
    while (view->running) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0)
                return false;
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            return false;
        }
        struct pollfd fds[2] = {
            { .fd = wl_display_get_fd(display), .events = POLLIN },
            { .fd = view->repeat.fd, .events = POLLIN },
        };
        if (poll(fds, view->repeat.fd >= 0 ? 2 : 1, -1) < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR)
                continue;
            return false;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(display) < 0)
                return false;
        } else {
            wl_display_cancel_read(display);
            if (fds[0].revents & (POLLERR | POLLHUP))
                return false;
        }
        if (wl_display_dispatch_pending(display) < 0)
            return false;

        if (view->repeat.fd >= 0 && fds[1].revents & POLLIN) {
            unsigned count = desk_repeat_fired(&view->repeat);
            if (count > MAX_REPEATS)
                count = MAX_REPEATS;
            for (unsigned i = 0; i < count && view->repeat.active && view->keys; ++i)
                key_action(view, xkb_state_key_get_one_sym(view->keys, view->repeat.key + 8));
        }
        /* A buffer may have been released since a skipped draw. */
        if (view->dirty && view->configured && !view->frame_callback)
            draw(view);
    }
    return true;
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
          "Keys:\n"
          "  + = -          Zoom in or out (also scrolling, around the pointer)\n"
          "  0, 1           Fit to the window, 100%\n"
          "  h j k l        Pan (also the arrows and dragging); Shift pans to the edge\n"
          "  r, R           Rotate clockwise, counter-clockwise\n"
          "  f, F           Flip horizontally, vertically\n"
          "  q, Escape      Close\n",
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
    struct view view = { .name = base_name(path), .buffer_scale = 1, .running = true, .fit = true, .zoom = 1 };
    if (!load_picture(&view.picture, path, strcmp(path, "-") == 0 ? "stdin" : path))
        return EXIT_FAILED;
    int error = desk_repeat_open(&view.repeat);
    if (error)
        fprintf(stderr, "view: no key repeat: %s\n", strerror(error));

    int status = EXIT_CLOSED;
    if (!open_window(&view)) {
        status = EXIT_FAILED;
    } else if (!run(&view)) {
        fprintf(stderr, "view: lost the Wayland connection\n");
        status = EXIT_FAILED;
    }
    close_window(&view);
    desk_repeat_close(&view.repeat);
    free(view.frame.pixels);
    picture_free(&view.picture);
    return status;
}
