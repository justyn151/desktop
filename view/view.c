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
#include <dirent.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* 256 megapixels as ARGB8888. */
#define MAX_IMAGE_BYTES ((size_t)1 << 30)
#define MAX_STDIN_BYTES ((size_t)512 << 20)
/* Without a palette (background = palette before wall has run). */
#define FALLBACK_BACKGROUND 0x1e1e1eu
/* The checkerboard behind transparent images, anchored to the image so it
   moves with it, in squares this many logical pixels wide. */
#define CHECKER_CELL 8
/* An initial window never covers more than this share of the output. */
#define INITIAL_SHARE 0.8
#define MIN_INITIAL_WIDTH 240
#define MIN_INITIAL_HEIGHT 160
#define FALLBACK_WIDTH 1280
#define FALLBACK_HEIGHT 800

#define ZOOM_STEP 1.4142135623730951
#define MIN_ZOOM 0.1        /* Unless fitting needs less. */
#define MAX_ZOOM 32.0
#define NEAREST_ZOOM 4.0    /* With filter = auto, pixels are sharp squares from here up. */
#define PAN_STEP 0.1        /* Of the window, per key press. */
#define SMOOTH_SCROLL 200.0 /* Touchpad scroll, in logical pixels, that halves or doubles the zoom. */
#define MAX_REPEATS 8       /* Repeats handled at once after a stall. */

/* Config */

static const char default_config[] = {
#include "default-config.h"
};

struct config_parse {
    struct config *config;
    const char *label;
};

static bool set_string(char **target, const char *value)
{
    char *copy = strdup(value);
    if (!copy)
        return false;
    free(*target);
    *target = copy;
    return true;
}

static void config_entry(void *data, const char *section, const char *key, const char *value, int line)
{
    struct config_parse *parse = data;
    struct config *config = parse->config;
    if (!key) {
        fprintf(stderr, "view: %s:%d: expected [section] or key = value\n", parse->label, line);
        return;
    }
    bool known = true, valid = true;
    if (strcmp(section, "view") == 0 && strcmp(key, "background") == 0) {
        if (strcmp(value, "palette") == 0)
            config->background_palette = true;
        else if ((valid = desk_parse_rgb(value, &config->background_rgb)))
            config->background_palette = false;
    } else if (strcmp(section, "view") == 0 && strcmp(key, "transparency") == 0) {
        valid = strcmp(value, "checker") == 0 || strcmp(value, "background") == 0;
        if (valid)
            config->checker = strcmp(value, "checker") == 0;
    } else if (strcmp(section, "view") == 0 && strcmp(key, "fit_enlarge") == 0) {
        valid = desk_parse_bool(value, &config->fit_enlarge);
    } else if (strcmp(section, "view") == 0 && strcmp(key, "filter") == 0) {
        if (strcmp(value, "auto") == 0)
            config->filter = FILTER_AUTO;
        else if (strcmp(value, "smooth") == 0)
            config->filter = FILTER_SMOOTH;
        else if (strcmp(value, "nearest") == 0)
            config->filter = FILTER_NEAREST;
        else
            valid = false;
    } else if (strcmp(section, "browse") == 0 && strcmp(key, "start") == 0) {
        valid = value[0] != 0 && set_string(&config->start, value);
    } else if (strcmp(section, "browse") == 0 && strcmp(key, "wrap") == 0) {
        valid = desk_parse_bool(value, &config->wrap);
    } else if (strcmp(section, "browse") == 0 && strcmp(key, "sort") == 0) {
        if (strcmp(value, "natural") == 0)
            config->sort = SORT_NATURAL;
        else if (strcmp(value, "name") == 0)
            config->sort = SORT_NAME;
        else if (strcmp(value, "modified") == 0)
            config->sort = SORT_MODIFIED;
        else
            valid = false;
    } else {
        known = false;
    }
    if (!known)
        fprintf(stderr, "view: %s:%d: unknown key %s in [%s]\n", parse->label, line, key, section);
    else if (!valid)
        fprintf(stderr, "view: %s:%d: invalid value for %s, keeping the default\n", parse->label, line, key);
}

/* The built-in defaults, then the user's file, which is created when it's
   missing so there's always one to edit. Problems are reported and the
   defaults stay in effect. */
static void config_load(struct config *config)
{
    *config = (struct config){ .wrap = true };
    struct config_parse parse = { .config = config, .label = "built-in config" };
    desk_ini_parse(default_config, sizeof default_config, config_entry, &parse);
    char *path = desk_config_path("view");
    if (!path) {
        fprintf(stderr, "view: neither XDG_CONFIG_HOME nor HOME is set, using the built-in config\n");
        return;
    }
    int error = desk_write_default(path, default_config, sizeof default_config);
    if (error == 0) {
        fprintf(stderr, "view: created %s\n", path);
    } else if (error != EEXIST) {
        fprintf(stderr, "view: can't create %s: %s\n", path, strerror(error));
    } else {
        parse.label = path;
        error = desk_ini_load(path, config_entry, &parse);
        if (error != 0 && error != ENOENT)
            fprintf(stderr, "view: can't read %s: %s\n", path, strerror(error));
    }
    free(path);
}

/* a moved towards b by amount out of 256, per channel. */
static uint32_t mix(uint32_t a, uint32_t b, unsigned amount)
{
    uint32_t out = 0xff000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        unsigned x = a >> shift & 0xff, y = b >> shift & 0xff;
        out |= ((x * (256 - amount) + y * amount) >> 8) << shift;
    }
    return out;
}

/* Resolve the background, from the palette when configured, and derive the
   checkerboard from it: slightly lighter on a dark background, darker on a
   light one. */
static void update_colors(struct view *view)
{
    uint32_t rgb = view->config.background_rgb;
    if (view->config.background_palette) {
        DeskPalette palette;
        rgb = FALLBACK_BACKGROUND;
        if (view->palette_path && desk_palette_load(view->palette_path, &palette) == 0)
            rgb = palette.rgb[DESK_BACKGROUND];
    }
    view->background = 0xff000000u | rgb;
    unsigned luma = (unsigned)(((rgb >> 16 & 0xff) * 299 + (rgb >> 8 & 0xff) * 587 + (rgb & 0xff) * 114) / 1000);
    uint32_t toward = luma < 128 ? 0xffffffffu : 0xff000000u;
    view->checker_dark = mix(view->background, toward, 16);
    view->checker_light = mix(view->background, toward, 36);
}

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
    /* Unique across pictures, so a kept frame is never mistaken for another image's. */
    static unsigned generations;
    picture->generation = ++generations;
}

/* Orientation. Applied to the pixels, so everything after works on an upright
   image. Rotation and flips are view-only: the file is never written. */

enum orientation {
    ROTATE_CLOCKWISE = 0,
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

/* Turn an image stored as EXIF orientation says into upright. */
static void apply_exif(struct level *image, int orientation)
{
    static const enum orientation steps[9][2] = {
        [2] = {FLIP_HORIZONTAL, -1},
        [3] = {ROTATE_CLOCKWISE, ROTATE_CLOCKWISE},
        [4] = {FLIP_VERTICAL, -1},
        [5] = {ROTATE_CLOCKWISE, FLIP_HORIZONTAL}, /* Transpose. */
        [6] = {ROTATE_CLOCKWISE, -1},
        [7] = {ROTATE_CLOCKWISE, FLIP_VERTICAL},   /* Transverse. */
        [8] = {ROTATE_COUNTER_CLOCKWISE, -1},
    };
    if (orientation < 2 || orientation > 8)
        return;
    for (int i = 0; i < 2 && (int)steps[orientation][i] >= 0; ++i) {
        if (!orient_pixels(image, steps[orientation][i]))
            return; /* Out of memory: shown as stored. */
    }
}

/* Decode path ("-" for stdin). On failure, return false with a message in error. */
static bool load_picture(struct picture *picture, const char *path, char *error, size_t error_size)
{
    *picture = (struct picture){ .level_count = 0 };
    char *data = NULL;
    FILE *file;
    if (strcmp(path, "-") == 0) {
        if (isatty(STDIN_FILENO)) {
            snprintf(error, error_size, "refusing to read an image from a terminal");
            return false;
        }
        file = read_stdin(&data);
    } else {
        file = fopen(path, "rb");
    }
    if (!file) {
        snprintf(error, error_size, "%s", errno ? strerror(errno) : "empty input");
        return false;
    }

    DeskImage image;
    bool ok = desk_image_load(file, &image, MAX_IMAGE_BYTES, DESK_IMAGE_SHRINK, error, error_size);
    fclose(file);
    free(data);
    if (!ok)
        return false;

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
    apply_exif(&picture->levels[0], image.orientation);
    levels_build(picture);
    return true;
}

static void picture_free(struct picture *picture)
{
    levels_free(picture);
    free(picture->levels[0].pixels);
    *picture = (struct picture){ .level_count = 0 };
}

/* Layout */

static bool has_image(const struct view *view)
{
    return view->current.picture.level_count > 0;
}

static double fit_zoom(const struct view *view, int width, int height)
{
    const struct level *image = &view->current.picture.levels[0];
    if (!has_image(view))
        return 1;
    double fit = fmin((double)width / image->width, (double)height / image->height);
    /* Unless configured, never enlarged: a small image shows one image pixel per buffer pixel. */
    return view->config.fit_enlarge ? fit : fmin(1.0, fit);
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
    const struct level *image = &view->current.picture.levels[0];
    if (!has_image(view))
        return;
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
   level level_index; enlarging is bilinear, then nearest from nearest_zoom. */
static bool taps_build(struct taps *taps, int outputs, int origin, double zoom, const struct level *level,
                       int level_index, uint32_t full_size, bool horizontal, double nearest_zoom)
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
                if (zoom >= nearest_zoom) {
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

/* A premultiplied pixel over an opaque backdrop. */
static inline uint32_t over(uint32_t backdrop, float a, float r, float g, float b)
{
    float inverse = 1 - a / 255;
    return 0xff000000u | channel(r + (backdrop >> 16 & 0xff) * inverse) << 16 |
           channel(g + (backdrop >> 8 & 0xff) * inverse) << 8 | channel(b + (backdrop & 0xff) * inverse);
}

static inline uint32_t pixel_over(uint32_t backdrop, uint32_t pixel)
{
    if (pixel >> 24 == 255)
        return pixel;
    return over(backdrop, (float)(pixel >> 24), (float)(pixel >> 16 & 0xff), (float)(pixel >> 8 & 0xff),
                (float)(pixel & 0xff));
}

struct sampler {
    const struct level *level;
    struct taps columns, rows;
    uint32_t background, checker_dark, checker_light;
    bool checker;   /* Draw the checkerboard behind the image. */
    int left, top;  /* The image's corner, which the checkerboard is anchored to. */
    int cell;       /* Checker square size in buffer pixels. */
};

static inline int floor_divide(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* What's behind the image at buffer pixel (x, y). */
static inline uint32_t backdrop(const struct sampler *sampler, int x, int y)
{
    if (!sampler->checker)
        return sampler->background;
    int cell = floor_divide(x - sampler->left, sampler->cell) + floor_divide(y - sampler->top, sampler->cell);
    return cell & 1 ? sampler->checker_light : sampler->checker_dark;
}

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
                target[x] = sampler->background;
            continue;
        }
        const float *row_weight = rows->weight + (size_t)y * rows->span;
        const uint32_t *source_row = level->pixels + (size_t)rows->first[y] * level->width;
        bool row_single = row_count == 1 && row_weight[0] == 1.0f;
        for (int x = x0; x < x1; ++x) {
            int column_count = columns->count[x];
            if (!column_count) {
                target[x] = sampler->background;
                continue;
            }
            const float *column_weight = columns->weight + (size_t)x * columns->span;
            const uint32_t *source = source_row + columns->first[x];
            if (row_single && column_count == 1 && column_weight[0] == 1.0f) {
                target[x] = pixel_over(backdrop(sampler, x, y), *source);
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
            target[x] = over(backdrop(sampler, x, y), a, r, g, b);
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

static void fill_background(uint32_t *pixels, int width, int height, uint32_t color)
{
    for (size_t i = 0; i < (size_t)width * height; ++i)
        pixels[i] = color;
}

/* Draw the current view into pixels. The previous frame is kept: when only
   the position changed, it is shifted and only the uncovered strips are
   rendered, so panning costs what scrolls in, not the whole window. */
static void paint(struct view *view, uint32_t *pixels, int width, int height)
{
    struct picture *picture = &view->current.picture;
    struct frame *frame = &view->frame;
    if (!has_image(view)) {
        frame->valid = false;
        fill_background(pixels, width, height, view->background);
        return;
    }
    layout(view, width, height);
    double zoom = view->zoom;
    const struct level *image = &picture->levels[0];
    int left = (int)lround(width / 2.0 - view->center_x * zoom);
    int top = (int)lround(height / 2.0 - view->center_y * zoom);

    if (!frame->pixels || frame->width != width || frame->height != height) {
        free(frame->pixels);
        *frame = (struct frame){ .pixels = malloc((size_t)width * height * 4), .width = width, .height = height };
        if (!frame->pixels) {
            fill_background(pixels, width, height, view->background);
            return;
        }
    }

    int level_index = 0;
    if (zoom < 1) {
        level_index = (int)floor(log2(1 / zoom) + 1e-9);
        if (level_index >= picture->level_count)
            level_index = picture->level_count - 1;
    }
    struct sampler sampler = {
        .level = &picture->levels[level_index],
        .background = view->background,
        .checker_dark = view->checker_dark,
        .checker_light = view->checker_light,
        .checker = !picture->opaque && view->config.checker,
        .left = left,
        .top = top,
        .cell = (int)lround(CHECKER_CELL * (view->width > 0 ? (double)width / view->width : 1)),
    };
    if (sampler.cell < 1)
        sampler.cell = 1;
    double nearest_zoom = view->config.filter == FILTER_NEAREST  ? 1.0
                        : view->config.filter == FILTER_SMOOTH ? INFINITY
                                                               : NEAREST_ZOOM;
    if (!taps_build(&sampler.columns, width, left, zoom, sampler.level, level_index, image->width, true,
                    nearest_zoom) ||
        !taps_build(&sampler.rows, height, top, zoom, sampler.level, level_index, image->height, false,
                    nearest_zoom)) {
        taps_free(&sampler.columns);
        frame->valid = false;
        fill_background(pixels, width, height, view->background);
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

static const char *base_name(const char *path)
{
    if (strcmp(path, "-") == 0)
        return "stdin";
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

/* "name — 3/42 — W×H — 50%", without the position for a single image and
   with the error in place of the size when it failed. */
static void update_title(struct view *view)
{
    const struct level *image = &view->current.picture.levels[0];
    char title[sizeof view->title], position[32] = "";
    if (view->count > 1)
        snprintf(position, sizeof position, " — %d/%d", view->current.index + 1, view->count);
    const char *name = base_name(view->paths[view->current.index]);
    if (has_image(view))
        snprintf(title, sizeof title, "%s%s — %" PRIu32 "×%" PRIu32 " — %.0f%%", name, position, image->width,
                 image->height, view->zoom * 100);
    else
        snprintf(title, sizeof title, "%s%s — %s", name, position, view->current.error);
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
    if (!has_image(view))
        return;
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
    if (!has_image(view))
        return;
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
    struct picture *picture = &view->current.picture;
    struct level *image = &picture->levels[0];
    if (!has_image(view))
        return;
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
    picture->oriented = true;
    mark_dirty(view);
}

/* Browsing */

static int wrap(const struct view *view, int index)
{
    return (index % view->count + view->count) % view->count;
}

/* Whether b is next to a, going either way round the list. */
static bool is_neighbor(const struct view *view, int a, int b)
{
    return a != b && (wrap(view, a + 1) == b || wrap(view, a - 1) == b);
}

static const char *display_path(const char *path)
{
    return strcmp(path, "-") == 0 ? "stdin" : path;
}

static void slot_free(struct slot *slot)
{
    picture_free(&slot->picture);
    *slot = (struct slot){ .used = false };
}

/* Decode image index. A failure is kept in the slot and shown in the title
   when the image is on screen, not printed, since preloading tries images
   that may never be shown. */
static void slot_load(struct view *view, struct slot *slot, int index)
{
    *slot = (struct slot){ .used = true, .index = index };
    load_picture(&slot->picture, view->paths[index], slot->error, sizeof slot->error);
}

static bool file_signature(const char *path, struct signature *signature)
{
    struct stat st;
    *signature = (struct signature){ .size = 0 };
    if (strcmp(path, "-") == 0 || stat(path, &st) != 0)
        return false;
    *signature = (struct signature){ st.st_dev, st.st_ino, st.st_size, st.st_mtim };
    return true;
}

static bool same_signature(const struct signature *a, const struct signature *b)
{
    return a->device == b->device && a->inode == b->inode && a->size == b->size &&
           a->modified.tv_sec == b->modified.tv_sec && a->modified.tv_nsec == b->modified.tv_nsec;
}

/* Watch the current file, so a replaced file (shot, wall, an editor) reloads. */
static void watch_current(struct view *view)
{
    desk_watch_close(&view->watch);
    const char *path = view->paths[view->current.index];
    file_signature(path, &view->signature);
    if (strcmp(path, "-") == 0)
        return;
    char *absolute = realpath(path, NULL);
    if (absolute)
        desk_watch_open(&view->watch, absolute);
    free(absolute);
}

/* Free cached images that aren't next to the current one, and return a free slot. */
static struct slot *cache_trim(struct view *view)
{
    struct slot *free_slot = NULL;
    for (int i = 0; i < 2; ++i) {
        struct slot *slot = &view->cache[i];
        if (slot->used && !is_neighbor(view, view->current.index, slot->index))
            slot_free(slot);
        if (!slot->used && !free_slot)
            free_slot = slot;
    }
    return free_slot;
}

/* Show image index, from the cache when it was preloaded. Each image starts
   fitted and upright. The one left behind is kept if it's a neighbour. */
static void show(struct view *view, int index)
{
    index = wrap(view, index);
    if (index == view->current.index)
        return;
    struct slot left = view->current;
    view->current = (struct slot){ .used = false };
    for (int i = 0; i < 2; ++i) {
        if (view->cache[i].used && view->cache[i].index == index) {
            view->current = view->cache[i];
            view->cache[i] = (struct slot){ .used = false };
        }
    }
    if (!view->current.used)
        slot_load(view, &view->current, index);

    struct slot *free_slot = cache_trim(view);
    if (free_slot && !left.picture.oriented && is_neighbor(view, index, left.index))
        *free_slot = left;
    else
        slot_free(&left);

    view->fit = true;
    view->frame.valid = false;
    watch_current(view);
    mark_dirty(view);
}

static void step(struct view *view, int direction)
{
    int target = view->current.index + direction;
    if (view->count < 2 || (!view->config.wrap && (target < 0 || target >= view->count)))
        return;
    view->direction = direction;
    show(view, view->current.index + direction);
}

/* The neighbour to decode next, ahead in the direction of travel first.
   Return false when both are decoded (or failed). */
static bool preload_wanted(const struct view *view, int *index)
{
    if (view->count < 2)
        return false;
    int order[2] = { view->direction, -view->direction };
    for (int k = 0; k < 2; ++k) {
        int candidate = wrap(view, view->current.index + order[k]);
        bool cached = candidate == view->current.index;
        for (int i = 0; i < 2; ++i)
            cached |= view->cache[i].used && view->cache[i].index == candidate;
        if (!cached) {
            *index = candidate;
            return true;
        }
    }
    return false;
}

static void preload(struct view *view)
{
    int index;
    if (!preload_wanted(view, &index))
        return;
    struct slot *slot = cache_trim(view);
    if (slot)
        slot_load(view, slot, index);
}

/* The current file changed on disk: decode it again. If the new version
   fails while the old one showed, keep the old one; a later write retries.
   Zoom and position stay when the size is unchanged. */
static void reload_current(struct view *view)
{
    struct signature now;
    const char *path = view->paths[view->current.index];
    if (!file_signature(path, &now) || same_signature(&now, &view->signature))
        return;
    struct slot fresh;
    slot_load(view, &fresh, view->current.index);
    if (fresh.picture.level_count == 0 && has_image(view)) {
        slot_free(&fresh);
        return;
    }
    view->signature = now;
    const struct level *old = &view->current.picture.levels[0], *new = &fresh.picture.levels[0];
    bool same_size = has_image(view) && !view->current.picture.oriented && fresh.picture.level_count &&
                     old->width == new->width && old->height == new->height;
    slot_free(&view->current);
    view->current = fresh;
    if (!same_size)
        view->fit = true;
    view->frame.valid = false;
    mark_dirty(view);
}

/* Whether the image is wider than the window, so Left and Right pan rather
   than change images. */
static bool can_pan_across(struct view *view)
{
    if (!has_image(view))
        return false;
    int width, height, scale;
    physical_size(view, &width, &height, &scale);
    layout(view, width, height);
    return view->current.picture.levels[0].width * view->zoom > width + 0.5;
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
    case XKB_KEY_n:
    case XKB_KEY_space:
    case XKB_KEY_Page_Down:
        step(view, 1);
        return false;
    case XKB_KEY_p:
    case XKB_KEY_BackSpace:
    case XKB_KEY_Page_Up:
        step(view, -1);
        return false;
    case XKB_KEY_g:
    case XKB_KEY_Home:
        view->direction = 1;
        show(view, 0);
        return false;
    case XKB_KEY_G:
    case XKB_KEY_End:
        view->direction = -1;
        show(view, view->count - 1);
        return false;
    case XKB_KEY_Left:
        if (!can_pan_across(view)) {
            step(view, -1);
            return false;
        }
        pan_step(view, -1, 0);
        return true;
    case XKB_KEY_Right:
        if (!can_pan_across(view)) {
            step(view, 1);
            return false;
        }
        pan_step(view, 1, 0);
        return true;
    case XKB_KEY_h:
        pan_step(view, -1, 0);
        return true;
    case XKB_KEY_l:
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
    const struct level *image = &view->current.picture.levels[0];
    double bound_width = view->bounds_width > 0 ? view->bounds_width : FALLBACK_WIDTH / INITIAL_SHARE;
    double bound_height = view->bounds_height > 0 ? view->bounds_height : FALLBACK_HEIGHT / INITIAL_SHARE;
    double scale = view->fraction ? view->fraction / 120.0 : view->buffer_scale;
    double image_width = has_image(view) ? image->width / scale : FALLBACK_WIDTH;
    double image_height = has_image(view) ? image->height / scale : FALLBACK_HEIGHT;
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
        /* Preload a neighbour once the current image is on screen and nothing else is waiting. */
        int next;
        bool idle_work = view->configured && !view->dirty && !view->frame_callback && preload_wanted(view, &next);
        struct pollfd fds[4] = {
            { .fd = wl_display_get_fd(display), .events = POLLIN },
            { .fd = view->repeat.fd, .events = POLLIN },
            { .fd = view->watch.fd, .events = POLLIN },
            { .fd = view->palette_watch.fd, .events = POLLIN },
        };
        int ready = poll(fds, 4, idle_work ? 0 : -1);
        if (ready < 0) {
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
        if (view->watch.fd >= 0 && fds[2].revents && desk_watch_changed(&view->watch, fds[2].revents))
            reload_current(view);
        if (view->palette_watch.fd >= 0 && fds[3].revents &&
            desk_watch_changed(&view->palette_watch, fds[3].revents)) {
            uint32_t old = view->background;
            update_colors(view);
            if (view->background != old) {
                view->frame.valid = false;
                mark_dirty(view);
            }
        }
        if (ready == 0 && idle_work)
            preload(view);
        /* A buffer may have been released since a skipped draw. */
        if (view->dirty && view->configured && !view->frame_callback)
            draw(view);
    }
    return true;
}

/* Main */

/* The list of images to browse */

struct list {
    char **paths;
    int count, capacity;
};

static bool list_add(struct list *list, char *path)
{
    if (!path)
        return false;
    if (list->count == list->capacity) {
        int capacity = list->capacity ? list->capacity * 2 : 64;
        char **grown = realloc(list->paths, (size_t)capacity * sizeof *grown);
        if (!grown) {
            free(path);
            return false;
        }
        list->paths = grown;
        list->capacity = capacity;
    }
    list->paths[list->count++] = path;
    return true;
}

static void list_free(struct list *list)
{
    for (int i = 0; i < list->count; ++i)
        free(list->paths[i]);
    free(list->paths);
    *list = (struct list){ .count = 0 };
}

static bool has_image_extension(const char *name)
{
    static const char *const extensions[] = {".png", ".jpg", ".jpeg", ".jpe", ".gif", ".webp"};
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot && i < sizeof extensions / sizeof *extensions; ++i) {
        if (strcasecmp(dot, extensions[i]) == 0)
            return true;
    }
    return false;
}

/* By file name: natural order puts img2 before img10, name is plain bytes. */
static int compare_names(const void *a, const void *b, void *data)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    enum sort sort = *(const enum sort *)data;
    int order = sort == SORT_NAME ? strcmp(base_name(x), base_name(y)) : strverscmp(base_name(x), base_name(y));
    return order ? order : strcmp(x, y);
}

struct dated {
    char *path;
    struct timespec modified;
};

/* Oldest first, then natural order for files changed at the same moment. */
static int compare_dated(const void *a, const void *b)
{
    const struct dated *x = a, *y = b;
    if (x->modified.tv_sec != y->modified.tv_sec)
        return x->modified.tv_sec < y->modified.tv_sec ? -1 : 1;
    if (x->modified.tv_nsec != y->modified.tv_nsec)
        return x->modified.tv_nsec < y->modified.tv_nsec ? -1 : 1;
    enum sort natural = SORT_NATURAL;
    return compare_names(&x->path, &y->path, &natural);
}

static void sort_paths(char **paths, int count, enum sort sort)
{
    if (sort == SORT_MODIFIED) {
        struct dated *dated = malloc((size_t)count * sizeof *dated);
        if (dated) {
            for (int i = 0; i < count; ++i) {
                struct stat st;
                dated[i] = (struct dated){ .path = paths[i] };
                if (strcmp(paths[i], "-") != 0 && stat(paths[i], &st) == 0)
                    dated[i].modified = st.st_mtim;
            }
            qsort(dated, (size_t)count, sizeof *dated, compare_dated);
            for (int i = 0; i < count; ++i)
                paths[i] = dated[i].path;
            free(dated);
            return;
        }
        sort = SORT_NATURAL; /* Out of memory: by name instead. */
    }
    qsort_r(paths, (size_t)count, sizeof *paths, compare_names, &sort);
}

/* Append the image files in directory, sorted, skipping hidden ones.
   With prefix, each path starts with the directory; otherwise it's the bare
   name, for the working directory. Return false with errno set when the
   directory can't be read. */
static bool list_directory(struct list *list, const char *directory, bool prefix, enum sort sort)
{
    DIR *dir = opendir(directory);
    if (!dir)
        return false;
    int start = list->count;
    size_t length = strlen(directory);
    bool slash = length > 0 && directory[length - 1] == '/';
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.' || !has_image_extension(entry->d_name))
            continue;
        char *path = NULL;
        if (!prefix)
            path = strdup(entry->d_name);
        else if (asprintf(&path, "%s%s%s", directory, slash ? "" : "/", entry->d_name) < 0)
            path = NULL;
        if (!list_add(list, path)) {
            closedir(dir);
            errno = ENOMEM;
            return false;
        }
    }
    closedir(dir);
    sort_paths(list->paths + start, list->count - start, sort);
    return true;
}

static bool is_directory(const char *path)
{
    struct stat st;
    return strcmp(path, "-") != 0 && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* One FILE: the images beside it, starting at it. It's included even when
   its name doesn't look like an image, since content decides. */
static bool list_around(struct list *list, const char *file, int *start, enum sort sort)
{
    const char *slash = strrchr(file, '/');
    char *directory = slash ? strndup(file, slash == file ? 1 : (size_t)(slash - file)) : strdup(".");
    if (!directory)
        return false;
    list_directory(list, directory, slash != NULL, sort); /* Unreadable: just the file. */
    free(directory);

    const char *name = base_name(file);
    for (int i = 0; i < list->count; ++i) {
        if (strcmp(base_name(list->paths[i]), name) == 0) {
            *start = i;
            return true;
        }
    }
    if (!list_add(list, strdup(file)))
        return false;
    sort_paths(list->paths, list->count, sort);
    for (int i = 0; i < list->count; ++i) {
        if (strcmp(list->paths[i], file) == 0)
            *start = i;
    }
    return true;
}

/* Build the list from the arguments. Return 0 or an exit status. */
static int build_list(struct list *list, int argc, char **argv, int *start, enum sort sort)
{
    *start = 0;
    if (argc == 1 && !is_directory(argv[0])) {
        bool ok = strcmp(argv[0], "-") == 0 ? list_add(list, strdup("-")) : list_around(list, argv[0], start, sort);
        if (!ok)
            fprintf(stderr, "view: %s\n", strerror(ENOMEM));
        return ok ? 0 : EXIT_FAILED;
    }
    for (int i = 0; i < argc; ++i) {
        if (!is_directory(argv[i])) {
            if (!list_add(list, strdup(argv[i]))) {
                fprintf(stderr, "view: %s\n", strerror(ENOMEM));
                return EXIT_FAILED;
            }
        } else if (!list_directory(list, argv[i], true, sort)) {
            fprintf(stderr, "view: %s: %s\n", argv[i], strerror(errno));
            return EXIT_FAILED;
        }
    }
    if (list->count == 0) {
        fprintf(stderr, "view: no images in %s\n", argc == 1 ? argv[0] : "the arguments");
        return EXIT_FAILED;
    }
    return 0;
}

static void usage(FILE *stream)
{
    fputs("Usage: view [FILE | DIRECTORY]...\n"
          "Show PNG, JPEG, GIF and WebP images. One FILE browses the images beside it, a\n"
          "DIRECTORY browses its images, and several arguments browse exactly those.\n"
          "Without arguments, [browse] start in ~/.config/view/view.conf is browsed.\n"
          "FILE \"-\" reads standard input.\n"
          "\n"
          "  -h, --help     Show this help\n"
          "  -v, --version  Show the version\n"
          "\n"
          "Keys:\n"
          "  n, Space       Next image (also Right when there's nothing to pan)\n"
          "  p, Backspace   Previous image (also Left)\n"
          "  g, G           First, last image\n"
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
    struct config config;
    config_load(&config);

    /* Without arguments, browse the configured start directory. */
    char *start_directory = NULL;
    if (argc - optind < 1) {
        start_directory = desk_expand_home(config.start ? config.start : "~/Pictures/Screenshots");
        if (!start_directory) {
            fprintf(stderr, "view: can't expand %s (is HOME set?)\n", config.start);
            free(config.start);
            return EXIT_FAILED;
        }
    }
    char **arguments = start_directory ? &start_directory : argv + optind;
    int argument_count = start_directory ? 1 : argc - optind;

    struct list list = { .count = 0 };
    int start;
    int status = build_list(&list, argument_count, arguments, &start, config.sort);
    if (status) {
        list_free(&list);
        free(start_directory);
        free(config.start);
        return status;
    }

    struct view view = {
        .config = config, .paths = list.paths, .count = list.count, .direction = 1,
        .watch = { .fd = -1 }, .palette_watch = { .fd = -1 }, .repeat = { .fd = -1 },
        .buffer_scale = 1, .running = true, .fit = true, .zoom = 1,
    };
    if (config.background_palette) {
        view.palette_path = desk_palette_path();
        if (view.palette_path)
            desk_watch_open(&view.palette_watch, view.palette_path);
    }
    update_colors(&view);
    slot_load(&view, &view.current, start);
    /* A single FILE that can't be shown is an error, as before browsing existed. */
    bool single_file = argument_count == 1 && !is_directory(arguments[0]);
    if (!has_image(&view) && (single_file || view.count == 1)) {
        fprintf(stderr, "view: %s: %s\n", display_path(view.paths[start]), view.current.error);
        status = EXIT_FAILED;
        goto done;
    }
    watch_current(&view);
    int error = desk_repeat_open(&view.repeat);
    if (error)
        fprintf(stderr, "view: no key repeat: %s\n", strerror(error));

    if (!open_window(&view)) {
        status = EXIT_FAILED;
    } else if (!run(&view)) {
        fprintf(stderr, "view: lost the Wayland connection\n");
        status = EXIT_FAILED;
    }
    close_window(&view);
done:
    desk_repeat_close(&view.repeat);
    desk_watch_close(&view.watch);
    desk_watch_close(&view.palette_watch);
    free(view.palette_path);
    free(view.frame.pixels);
    slot_free(&view.current);
    for (int i = 0; i < 2; ++i)
        slot_free(&view.cache[i]);
    list_free(&list);
    free(start_directory);
    free(view.config.start);
    return status;
}
