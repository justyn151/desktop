#define _GNU_SOURCE
#include "wall.h"
#include "desk.h"

#include "fractional-scale-client-protocol.h"
#include "layer-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <getopt.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <png.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define FRACTIONAL_DENOMINATOR 120u
#define MAX_IMAGE_BYTES ((size_t)256 * 1024 * 1024)
#define MAX_SNAPSHOT_BYTES ((size_t)512 * 1024 * 1024)
#define NSEC_PER_MSEC INT64_C(1000000)
#define NSEC_PER_SEC INT64_C(1000000000)
#define ALL_TRANSITION_EFFECTS ((1u << TRANSITION_RANDOM) - 2u)
#define STATIC_BUFFERS 2
#define ANIMATION_BUFFERS 4
#define MAX_COLOR_SAMPLES 32768
#define COLOR_CLUSTERS 8
#define COLOR_ITERATIONS 16
#define PALETTE_TEXT_SIZE 512

static const unsigned char bundled_config[] = {
#include "default-config.h"
    0
};

static const struct config default_config = {
    .mode = MODE_FILL,
    .background = 0x353535,
    .transition = {
        .enabled = true, .type = TRANSITION_RANDOM, .easing = EASE_IN_OUT,
        .duration_ms = 350, .fps = 60, .effects = ALL_TRANSITION_EFFECTS,
    },
    .colors = {.theme = THEME_DARK, .samples = 4096, .contrast = 4.5},
};

static const struct browser_config default_browser = {
    .recursive = true, .cache = true, .close_on_apply = true,
    .thumbnail_width = 312, .thumbnail_height = 176, .font_size = 14, .opacity = 1.0,
    .background = 0x353535, .foreground = 0xc5ad81,
    .highlight = 0xead4aa, .muted = 0x887d69,
};

static const char *const mode_names[] = {
    [MODE_FILL] = "fill",
    [MODE_FIT] = "fit",
    [MODE_STRETCH] = "stretch",
    [MODE_CENTER] = "center",
    [MODE_SOLID] = "solid",
};

static const char *const transition_names[] = {
    [TRANSITION_NONE] = "none", [TRANSITION_FADE] = "fade", [TRANSITION_SLIDE] = "slide",
    [TRANSITION_CIRCLE] = "circle", [TRANSITION_WIPE] = "wipe",
    [TRANSITION_DIAGONAL] = "diagonal", [TRANSITION_GROW] = "grow", [TRANSITION_RANDOM] = "random",
};

static const char *const palette_names[PALETTE_COUNT] = {
    [PALETTE_BACKGROUND] = "background",
    [PALETTE_FOREGROUND] = "foreground",
    [PALETTE_ACCENT] = "accent",
    [PALETTE_HIGHLIGHT] = "highlight",
    [PALETTE_MUTED] = "muted",
    [PALETTE_SELECTION_BACKGROUND] = "selection_background",
    [PALETTE_SELECTION_FOREGROUND] = "selection_foreground",
    [PALETTE_BORDER_ACTIVE] = "border_active",
    [PALETTE_BORDER_INACTIVE] = "border_inactive",
    [PALETTE_URGENT] = "urgent",
};

static bool parse_mode(const char *value, enum mode *mode)
{
    for (size_t i = 0; i < sizeof mode_names / sizeof mode_names[0]; ++i) {
        if (strcmp(value, mode_names[i]) == 0) {
            *mode = (enum mode)i;
            return true;
        }
    }
    return false;
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text))
        ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

static bool parse_background(const char *value, uint32_t *background)
{
    if (value[0] != '#' || strlen(value) != 7)
        return false;
    for (const char *digit = value + 1; *digit; ++digit) {
        if (!isxdigit((unsigned char)*digit))
            return false;
    }
    *background = (uint32_t)strtoul(value + 1, NULL, 16);
    return true;
}

static char *expand_path(const char *path)
{
    if (path[0] != '~' || (path[1] != '/' && path[1] != '\0'))
        return strdup(path);

    const char *home = getenv("HOME");
    if (!home || !*home) {
        errno = ENOENT;
        return NULL;
    }
    char *expanded;
    if (asprintf(&expanded, "%s%s", home, path + 1) < 0)
        return NULL;
    return expanded;
}

static char *absolute_path(const char *path)
{
    char *expanded = expand_path(path);
    if (!expanded)
        return NULL;
    if (expanded[0] != '/') {
        char *cwd = getcwd(NULL, 0);
        char *absolute = NULL;
        if (cwd && asprintf(&absolute, "%s/%s", cwd, expanded) < 0)
            absolute = NULL;
        free(cwd);
        free(expanded);
        expanded = absolute;
    }
    if (expanded && strlen(expanded) > WALL_PATH_MAX) {
        free(expanded);
        errno = ENAMETOOLONG;
        return NULL;
    }
    return expanded;
}

static char *config_path(const struct options *options)
{
    if (options->config_path)
        return expand_path(options->config_path);

    char *path;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0] == '/') {
        if (asprintf(&path, "%s/wall/wall.conf", xdg) < 0)
            return NULL;
    } else if (home && *home) {
        if (asprintf(&path, "%s/.config/wall/wall.conf", home) < 0)
            return NULL;
    } else {
        path = strdup("");
    }
    return path;
}

static int open_regular(const char *path, struct stat *st)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, st) == 0) {
        if (S_ISREG(st->st_mode))
            return fd;
        errno = EINVAL;
    }
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

static bool browser_setting(struct browser_config *config, bool appearance,
                            const char *key, const char *value)
{
    char **string = NULL;
    bool *flag = NULL;
    int *number = NULL, low = 1, high = 1;
    uint32_t *color = NULL;
    if (appearance) {
        if (!strcmp(key, "font")) string = &config->font;
        else if (!strcmp(key, "font_size")) {
            number = &config->font_size;
            low = 6;
            high = 48;
        } else if (!strcmp(key, "opacity")) {
            char *end;
            errno = 0;
            double opacity = strtod(value, &end);
            if (errno || end == value || *end || !(opacity >= 0.0 && opacity <= 1.0))
                return false;
            config->opacity = opacity;
            return true;
        } else if (!strcmp(key, "background")) color = &config->background;
        else if (!strcmp(key, "foreground")) color = &config->foreground;
        else if (!strcmp(key, "highlight")) color = &config->highlight;
        else if (!strcmp(key, "muted")) color = &config->muted;
    } else {
        if (!strcmp(key, "directories")) string = &config->directories;
        else if (!strcmp(key, "recursive")) flag = &config->recursive;
        else if (!strcmp(key, "cache")) flag = &config->cache;
        else if (!strcmp(key, "close_on_apply")) flag = &config->close_on_apply;
        else if (!strcmp(key, "columns")) {
            if (!strcmp(value, "auto")) {
                config->columns = 0;
                return true;
            }
            number = &config->columns;
            high = INT_MAX;
        } else if (!strcmp(key, "thumbnail_width") || !strcmp(key, "thumbnail_height")) {
            number = !strcmp(key, "thumbnail_width") ? &config->thumbnail_width : &config->thumbnail_height;
            low = 32;
            high = 512;
        }
    }
    if (string) {
        char *copy = strdup(value);
        if (!copy)
            return false;
        free(*string);
        *string = copy;
    } else if (flag) {
        if (strcmp(value, "true") && strcmp(value, "false"))
            return false;
        *flag = !strcmp(value, "true");
    } else if (number) {
        char *end;
        errno = 0;
        long parsed = strtol(value, &end, 10);
        if (errno || end == value || *end || parsed < low || parsed > high)
            return false;
        *number = (int)parsed;
    } else {
        return color && parse_background(value, color);
    }
    return true;
}

static bool parse_transition(const char *value, enum transition_effect *effect)
{
    for (size_t i = 0; i < sizeof transition_names / sizeof transition_names[0]; ++i) {
        if (!strcmp(value, transition_names[i])) {
            *effect = (enum transition_effect)i;
            return true;
        }
    }
    return false;
}

static bool transition_setting(struct transition_config *config, const char *key, char *value)
{
    if (!strcmp(key, "enabled")) {
        if (strcmp(value, "true") && strcmp(value, "false"))
            return false;
        config->enabled = !strcmp(value, "true");
    } else if (!strcmp(key, "type")) {
        return parse_transition(value, &config->type);
    } else if (!strcmp(key, "duration") || !strcmp(key, "fps")) {
        bool duration = !strcmp(key, "duration");
        char *end;
        errno = 0;
        long number = strtol(value, &end, 10);
        if (errno || end == value || *end || number < (duration ? 0 : 1) ||
            number > (duration ? 10000 : 240))
            return false;
        if (duration)
            config->duration_ms = (unsigned)number;
        else
            config->fps = (unsigned)number;
    } else if (!strcmp(key, "easing")) {
        static const char *const names[] = {
            [EASE_LINEAR] = "linear", [EASE_IN] = "ease_in",
            [EASE_OUT] = "ease_out", [EASE_IN_OUT] = "ease_in_out",
        };
        for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
            if (!strcmp(value, names[i])) {
                config->easing = (enum transition_easing)i;
                return true;
            }
        }
        return false;
    } else if (!strcmp(key, "effects")) {
        unsigned mask = 0;
        char *item;
        while ((item = strsep(&value, ","))) {
            enum transition_effect effect;
            if (!parse_transition(trim(item), &effect) ||
                effect == TRANSITION_NONE || effect == TRANSITION_RANDOM ||
                (mask & (1u << effect)))
                return false;
            mask |= 1u << effect;
        }
        config->effects = mask;
    } else {
        return false;
    }
    return true;
}

static bool color_setting(struct color_config *config, const char *key, const char *value)
{
    if (!strcmp(key, "enabled")) {
        if (strcmp(value, "true") && strcmp(value, "false"))
            return false;
        config->enabled = !strcmp(value, "true");
    } else if (!strcmp(key, "theme")) {
        if (strcmp(value, "dark") && strcmp(value, "light"))
            return false;
        config->theme = !strcmp(value, "dark") ? THEME_DARK : THEME_LIGHT;
    } else if (!strcmp(key, "method")) {
        return !strcmp(value, "kmeans");
    } else if (!strcmp(key, "samples")) {
        char *end;
        errno = 0;
        long count = strtol(value, &end, 10);
        if (errno || end == value || *end || count < 1 || count > MAX_COLOR_SAMPLES)
            return false;
        config->samples = (unsigned)count;
    } else if (!strcmp(key, "contrast")) {
        char *end;
        errno = 0;
        double contrast = strtod(value, &end);
        if (errno || end == value || *end || !isfinite(contrast) || contrast < 1 || contrast > 21)
            return false;
        config->contrast = contrast;
    } else {
        return false;
    }
    return true;
}

static bool read_config(struct config *config, struct browser_config *browser,
                        FILE *file, const char *path)
{
    char *line = NULL;
    size_t capacity = 0, number = 0;
    ssize_t length;
    enum { NO_SECTION, WALLPAPER, BROWSER, APPEARANCE, TRANSITION, COLORS, THEME } section = NO_SECTION;
    const char *error = NULL;

    while ((length = getline(&line, &capacity, file)) >= 0) {
        ++number;
        if (memchr(line, '\0', (size_t)length)) {
            error = "embedded NUL byte";
            break;
        }
        char *key = trim(line);
        if (!*key || *key == '#' || *key == ';')
            continue;
        if (!strcmp(key, "[wallpaper]")) {
            section = WALLPAPER;
            continue;
        }
        if (!strcmp(key, "[browser]")) {
            section = BROWSER;
            continue;
        }
        if (!strcmp(key, "[appearance]")) {
            section = APPEARANCE;
            continue;
        }
        if (!strcmp(key, "[transition]")) {
            section = TRANSITION;
            continue;
        }
        if (!strcmp(key, "[colors]")) {
            section = COLORS;
            continue;
        }
        if (!strcmp(key, "[theme]")) {
            section = THEME;
            continue;
        }
        char *value = strchr(key, '=');
        if (section == NO_SECTION || !value) {
            error = "expected a section or a key=value setting";
            break;
        }
        *value++ = '\0';
        key = trim(key);
        value = trim(value);

        if (section == THEME) {
            if (strcmp(key, "source") || (strcmp(value, "manual") && strcmp(value, "wallpaper"))) {
                error = "expected [theme] source = manual or wallpaper";
                break;
            }
            if (browser)
                browser->wallpaper_theme = !strcmp(value, "wallpaper");
        } else if (section == COLORS) {
            if (!browser && !color_setting(&config->colors, key, value)) {
                error = "invalid color setting";
                break;
            }
        } else if (section == TRANSITION) {
            if (!browser && !transition_setting(&config->transition, key, value)) {
                error = "invalid transition setting";
                break;
            }
        } else if (section != WALLPAPER) {
            if (browser && !browser_setting(browser, section == APPEARANCE, key, value)) {
                error = "invalid browser or appearance setting";
                break;
            }
        } else if (strcmp(key, "path") == 0) {
            char *copy = strdup(value);
            if (!copy) {
                error = strerror(errno);
                break;
            }
            free(config->path);
            config->path = copy;
        } else if (strcmp(key, "mode") == 0) {
            if (!parse_mode(value, &config->mode)) {
                error = "invalid wallpaper mode";
                break;
            }
        } else if (strcmp(key, "background") == 0) {
            if (!parse_background(value, &config->background)) {
                error = "expected a background color in #RRGGBB format";
                break;
            }
        } else {
            error = "unknown wallpaper setting";
            break;
        }
    }
    if (!error && !feof(file))
        error = strerror(errno);
    if (error)
        fprintf(stderr, "wall: %s:%zu: %s\n", path, number, error);
    free(line);
    return !error;
}

static bool load_config(struct config *config, const struct options *options,
                        struct browser_config *browser)
{
    char *path = config_path(options);
    if (!path) {
        fprintf(stderr, "wall: configuration path: %s\n", strerror(errno));
        return false;
    }

    struct stat st;
    int fd = -1;
    if (*path || options->config_path)
        fd = open_regular(path, &st);
    const char *name = path;
    FILE *file = NULL;
    if (fd >= 0) {
        file = fdopen(fd, "r");
        if (!file)
            close(fd);
    } else if (!options->config_path && (!*path || errno == ENOENT)) {
        name = "bundled wall.conf";
        file = fmemopen((void *)bundled_config, sizeof bundled_config - 1, "r");
    }
    bool ok = false;
    if (file) {
        ok = read_config(config, browser, file, name);
        fclose(file);
    } else {
        fprintf(stderr, "wall: %s: %s\n", name, strerror(errno));
    }
    free(path);
    return ok;
}

static bool apply_options(struct config *config, const struct options *options)
{
    if (options->mode && !parse_mode(options->mode, &config->mode))
        return false;
    const char *image_path = options->image_path ? options->image_path : config->path;
    if (image_path && *image_path) {
        char *expanded = absolute_path(image_path);
        if (!expanded) {
            fprintf(stderr, "wall: %s: %s\n", image_path, strerror(errno));
            return false;
        }
        free(config->path);
        config->path = expanded;
    } else {
        free(config->path);
        config->path = NULL;
    }
    return true;
}

static bool decode_image(FILE *file, struct image *image, const char *path)
{
    DeskImage decoded;
    char error[256];
    if (!desk_image_load(file, &decoded, MAX_IMAGE_BYTES, 0, error, sizeof error)) {
        fprintf(stderr, "wall: %s: %s\n", path, error);
        *image = (struct image){0};
        return false;
    }
    *image = (struct image){ decoded.width, decoded.height, decoded.pixels };
    return true;
}

static FILE *snapshot_image(const char *path)
{
    struct stat st;
    int input_fd = open_regular(path, &st);
    if (input_fd < 0)
        return NULL;

    if (st.st_size > (off_t)MAX_SNAPSHOT_BYTES) {
        close(input_fd);
        errno = EFBIG;
        return NULL;
    }

    int fd = memfd_create("wall-image", MFD_CLOEXEC);
    FILE *snapshot = fd < 0 ? NULL : fdopen(fd, "w+b");
    if (!snapshot) {
        int error = errno;
        if (fd >= 0)
            close(fd);
        close(input_fd);
        errno = error;
        return NULL;
    }

    unsigned char chunk[32768];
    size_t total = 0;
    int error = 0;
    ssize_t count;
    while ((count = read(input_fd, chunk, sizeof chunk)) > 0) {
        if ((size_t)count > MAX_SNAPSHOT_BYTES - total) {
            error = EFBIG;
            break;
        }
        if (fwrite(chunk, 1, (size_t)count, snapshot) != (size_t)count) {
            error = errno;
            break;
        }
        total += (size_t)count;
    }
    if (count < 0)
        error = errno;
    if (!error && fflush(snapshot) == EOF)
        error = errno;
    close(input_fd);

    if (error) {
        fclose(snapshot);
        errno = error;
        return NULL;
    }
    return snapshot;
}

static bool same_snapshot(FILE *a, FILE *b)
{
    unsigned char first[8192], second[sizeof first];
    clearerr(a);
    if (fseek(a, 0, SEEK_SET) != 0 || fseek(b, 0, SEEK_SET) != 0)
        return false;
    for (;;) {
        size_t length = fread(first, 1, sizeof first, a);
        if (fread(second, 1, sizeof second, b) != length ||
            memcmp(first, second, length) != 0)
            return false;
        if (length < sizeof first)
            return feof(a) && feof(b) && !ferror(a) && !ferror(b);
    }
}

struct color_point {
    double l, a, b;
};

struct color_cluster {
    struct color_point center;
    size_t population;
};

static void linear_channels(double linear[256])
{
    for (unsigned i = 0; i < 256; ++i) {
        double srgb = i / 255.0;
        linear[i] = srgb <= 0.04045 ? srgb / 12.92 : pow((srgb + 0.055) / 1.055, 2.4);
    }
}

static struct color_point to_oklab(uint32_t rgb, const double linear[256])
{
    double r = linear[(rgb >> 16) & 255], g = linear[(rgb >> 8) & 255], b = linear[rgb & 255];
    double l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    double m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    double s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return (struct color_point){
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

static bool oklab_linear(struct color_point point, double rgb[3])
{
    double l = point.l + 0.3963377774 * point.a + 0.2158037573 * point.b;
    double m = point.l - 0.1055613458 * point.a - 0.0638541728 * point.b;
    double s = point.l - 0.0894841775 * point.a - 1.2914855480 * point.b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    rgb[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    rgb[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    rgb[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    for (size_t i = 0; i < 3; ++i) {
        if (rgb[i] < -0.0000001 || rgb[i] > 1.0000001)
            return false;
    }
    return true;
}

static uint32_t palette_tone(struct color_point source, double lightness, double chroma)
{
    struct color_point point = {lightness, source.a * chroma, source.b * chroma};
    double rgb[3];
    if (!oklab_linear(point, rgb)) {
        /* Reduce chroma until the color fits sRGB, keeping its hue and lightness. */
        double low = 0, high = 1;
        for (unsigned i = 0; i < 12; ++i) {
            double scale = (low + high) * 0.5;
            struct color_point candidate = {lightness, point.a * scale, point.b * scale};
            if (oklab_linear(candidate, rgb))
                low = scale;
            else
                high = scale;
        }
        point.a *= low;
        point.b *= low;
        oklab_linear(point, rgb);
    }
    uint32_t result = 0;
    for (size_t i = 0; i < 3; ++i) {
        double channel = fmin(1, fmax(0, rgb[i]));
        double srgb = channel <= 0.0031308 ? channel * 12.92 :
                      1.055 * pow(channel, 1.0 / 2.4) - 0.055;
        result = (result << 8) | (uint32_t)(srgb * 255 + 0.5);
    }
    return result;
}

static uint32_t composite_sample(const unsigned char *pixel, uint32_t background)
{
    uint32_t result = 0;
    unsigned uncovered = 255 - pixel[3];
    for (unsigned i = 0; i < 3; ++i) {
        unsigned base = (background >> ((2 - i) * 8)) & 255;
        unsigned value = pixel[i] + (base * uncovered + 127) / 255;
        result = (result << 8) | value;
    }
    return result;
}

static uint32_t sample_offset(uint32_t index, uint32_t span)
{
    index ^= index >> 16;
    index *= UINT32_C(0x7feb352d);
    index ^= index >> 15;
    index *= UINT32_C(0x846ca68b);
    index ^= index >> 16;
    return (uint32_t)(((uint64_t)index * span) >> 32);
}

static size_t collect_color_samples(const struct image *image, uint32_t background,
                                    unsigned budget, const double linear[256], struct color_point *samples)
{
    uint32_t columns = image->width, rows = image->height;
    if ((uint64_t)columns * rows > budget) {
        double aspect_columns = sqrt((double)budget * image->width / image->height);
        columns = (uint32_t)fmax(1, fmin(image->width, fmin(budget, floor(aspect_columns))));
        rows = budget / columns;
        if (rows > image->height)
            rows = image->height;
        columns = budget / rows;
        if (columns > image->width)
            columns = image->width;
    }

    size_t count = 0;
    /* Take one deterministic, jittered sample from each cell of an image-wide grid. */
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t top = (uint32_t)((uint64_t)row * image->height / rows);
        uint32_t bottom = (uint32_t)((uint64_t)(row + 1) * image->height / rows);
        for (uint32_t column = 0; column < columns; ++column) {
            uint32_t cell = row * columns + column;
            uint32_t left = (uint32_t)((uint64_t)column * image->width / columns);
            uint32_t right = (uint32_t)((uint64_t)(column + 1) * image->width / columns);
            uint32_t x = left + sample_offset(cell + 1, right - left);
            uint32_t y = top + sample_offset(cell + UINT32_C(0x9e3779b9), bottom - top);
            const unsigned char *pixel = image->pixels + ((size_t)y * image->width + x) * 4;
            if (pixel[3])
                samples[count++] = to_oklab(composite_sample(pixel, background), linear);
        }
    }
    return count;
}

static double color_distance(struct color_point a, struct color_point b)
{
    double dl = a.l - b.l, da = a.a - b.a, db = a.b - b.b;
    return dl * dl + da * da + db * db;
}

static size_t nearest_cluster(struct color_point sample, const struct color_cluster *clusters,
                              size_t count, double *distance)
{
    size_t result = 0;
    *distance = color_distance(sample, clusters[0].center);
    for (size_t i = 1; i < count; ++i) {
        double candidate = color_distance(sample, clusters[i].center);
        if (candidate < *distance) {
            result = i;
            *distance = candidate;
        }
    }
    return result;
}

static size_t cluster_colors(const struct color_point *samples, size_t count,
                             struct color_cluster clusters[COLOR_CLUSTERS])
{
    struct color_point mean = {0};
    for (size_t i = 0; i < count; ++i) {
        mean.l += samples[i].l;
        mean.a += samples[i].a;
        mean.b += samples[i].b;
    }
    mean = (struct color_point){mean.l / count, mean.a / count, mean.b / count};
    double nearest = INFINITY;
    for (size_t i = 0; i < count; ++i) {
        double distance = color_distance(samples[i], mean);
        if (distance < nearest) {
            clusters[0].center = samples[i];
            nearest = distance;
        }
    }

    /* Deterministic farthest-first seeding stops once only very similar samples remain. */
    size_t used = 1;
    while (used < COLOR_CLUSTERS) {
        double farthest = 0.0004;
        size_t seed = count;
        for (size_t i = 0; i < count; ++i) {
            double distance;
            nearest_cluster(samples[i], clusters, used, &distance);
            if (distance > farthest) {
                farthest = distance;
                seed = i;
            }
        }
        if (seed == count)
            break;
        clusters[used++].center = samples[seed];
    }

    for (unsigned iteration = 0; iteration < COLOR_ITERATIONS; ++iteration) {
        struct color_cluster sums[COLOR_CLUSTERS] = {0};
        for (size_t i = 0; i < count; ++i) {
            double distance;
            size_t index = nearest_cluster(samples[i], clusters, used, &distance);
            struct color_cluster *sum = &sums[index];
            sum->center.l += samples[i].l;
            sum->center.a += samples[i].a;
            sum->center.b += samples[i].b;
            ++sum->population;
        }
        double shift = 0;
        for (size_t i = 0; i < used; ++i) {
            size_t n = sums[i].population;
            if (n) {
                struct color_point center = {sums[i].center.l / n, sums[i].center.a / n,
                                             sums[i].center.b / n};
                double movement = color_distance(clusters[i].center, center);
                if (movement > shift)
                    shift = movement;
                clusters[i].center = center;
            }
            clusters[i].population = n;
        }
        if (shift < 0.00000001)
            break;
    }
    return used;
}

static double relative_luminance(uint32_t rgb, const double linear[256])
{
    if (rgb == 0xffffffu)
        return 1;
    return 0.2126 * linear[(rgb >> 16) & 255] +
           0.7152 * linear[(rgb >> 8) & 255] + 0.0722 * linear[rgb & 255];
}

static double contrast_toward(uint32_t color, double reference, bool brighter,
                               const double linear[256])
{
    double luminance = relative_luminance(color, linear);
    double lighter = brighter ? luminance : reference, darker = brighter ? reference : luminance;
    return (lighter + 0.05) / (darker + 0.05);
}

static uint32_t tint_rgb(uint32_t from, uint32_t to, double fraction)
{
    uint32_t rgb = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        double a = (from >> shift) & 255, b = (to >> shift) & 255;
        rgb |= (uint32_t)(a + (b - a) * fraction + 0.5) << shift;
    }
    return rgb;
}

static uint32_t ensure_contrast(uint32_t color, uint32_t against, double target,
                                 uint32_t pole, const double linear[256])
{
    double reference = relative_luminance(against, linear);
    bool brighter = pole != 0;
    if (contrast_toward(color, reference, brighter, linear) >= target)
        return color;

    uint32_t result = pole;
    double low = 0, high = 1;
    for (unsigned i = 0; i < 16; ++i) {
        double fraction = (low + high) * 0.5;
        uint32_t candidate = tint_rgb(color, pole, fraction);
        if (contrast_toward(candidate, reference, brighter, linear) >= target) {
            result = candidate;
            high = fraction;
        } else {
            low = fraction;
        }
    }
    return result;
}

static void semantic_palette(struct palette *palette, const struct color_cluster *clusters,
                              size_t count, size_t sampled, const struct color_config *config,
                              const double linear[256])
{
    bool dark = config->theme == THEME_DARK;
    struct color_point base = clusters[0].center, accent = base, secondary, urgent;
    double base_score = -1, accent_score = -1;
    for (size_t i = 0; i < count; ++i) {
        struct color_point color = clusters[i].center;
        double share = (double)clusters[i].population / sampled;
        double affinity = dark ? 1 - color.l : color.l;
        double score = share * (0.45 + affinity);
        if (score > base_score) {
            base = color;
            base_score = score;
        }
        if (share > 0) {
            double chroma = hypot(color.a, color.b);
            score = sqrt(share) * (0.035 + chroma) * (1 - 0.35 * fabs(color.l - 0.59));
            if (score > accent_score) {
                accent = color;
                accent_score = score;
            }
        }
    }

    double accent_chroma = hypot(accent.a, accent.b);
    secondary = accent;
    urgent = (struct color_point){accent.l, 0.065 + 0.32 * accent_chroma, 0.035 + 0.20 * accent.b};
    double secondary_score = 0, urgent_score = 0;
    for (size_t i = 0; i < count; ++i) {
        struct color_point color = clusters[i].center;
        double share = (double)clusters[i].population / sampled;
        double difference = hypot(color.a - accent.a, color.b - accent.b);
        if (share >= 0.03 && difference >= 0.055) {
            double score = sqrt(share) * difference;
            if (score > secondary_score) {
                secondary = color;
                secondary_score = score;
            }
        }
        double warmth = color.a - 0.35 * fabs(color.b - 0.035);
        if (share >= 0.01 && warmth > 0.035 && warmth * sqrt(share) > urgent_score) {
            urgent = color;
            urgent_score = warmth * sqrt(share);
        }
    }

    double target = fmax(3, config->contrast);
    uint32_t shadow = dark ? 0 : 0xffffff;
    uint32_t light = shadow ^ 0xffffffu;
    uint32_t background = palette_tone(base, dark ? 0.17 + 0.16 * base.l : 0.88 + 0.09 * base.l, 0.30);
    background = ensure_contrast(background, light, target, shadow, linear);
    uint32_t foreground = palette_tone(accent, dark ? 0.91 + 0.05 * base.l : 0.16 + 0.12 * base.l, 0.20);
    foreground = ensure_contrast(foreground, background, target, light, linear);
    uint32_t main_accent = palette_tone(accent, dark ? 0.56 + 0.23 * accent.l : 0.32 + 0.24 * accent.l, 0.95);
    uint32_t highlight = palette_tone(secondary, dark ? 0.72 + 0.16 * secondary.l :
                                      0.24 + 0.16 * secondary.l, 0.75);
    uint32_t selection = palette_tone(accent, dark ? 0.62 + 0.12 * accent.l :
                                      0.39 + 0.10 * accent.l, 0.80);
    selection = ensure_contrast(selection, background, 2.15, light, linear);
    selection = ensure_contrast(selection, shadow, target, light, linear);
    uint32_t muted = tint_rgb(background, foreground, 0.57);
    uint32_t inactive = tint_rgb(background, foreground, 0.29);

    palette->rgb[PALETTE_BACKGROUND] = background;
    palette->rgb[PALETTE_FOREGROUND] = foreground;
    palette->rgb[PALETTE_ACCENT] = ensure_contrast(main_accent, background, target, light, linear);
    palette->rgb[PALETTE_HIGHLIGHT] = ensure_contrast(highlight, background, target, light, linear);
    palette->rgb[PALETTE_MUTED] = ensure_contrast(muted, background, fmin(3.4, target * 0.8), light, linear);
    palette->rgb[PALETTE_SELECTION_BACKGROUND] = selection;
    palette->rgb[PALETTE_SELECTION_FOREGROUND] = ensure_contrast(background, selection, target, shadow, linear);
    palette->rgb[PALETTE_BORDER_ACTIVE] = ensure_contrast(main_accent, background, 3, light, linear);
    palette->rgb[PALETTE_BORDER_INACTIVE] = ensure_contrast(inactive, background, 1.5, light, linear);
    uint32_t attention = palette_tone(urgent, dark ? 0.59 + 0.16 * urgent.l : 0.37 + 0.13 * urgent.l, 1.05);
    palette->rgb[PALETTE_URGENT] = ensure_contrast(attention, background, target, light, linear);
}

static bool generate_palette(const struct image *image, uint32_t background,
                             const struct color_config *config, struct palette *palette)
{
    double linear[256];
    linear_channels(linear);
    struct color_cluster clusters[COLOR_CLUSTERS] = {0};
    size_t count = 1, sampled = 0;
    if (image && image->pixels) {
        uint64_t pixels = (uint64_t)image->width * image->height;
        size_t capacity = (size_t)(pixels < config->samples ? pixels : config->samples);
        struct color_point *samples = malloc(capacity * sizeof *samples);
        if (!samples)
            return false;
        sampled = collect_color_samples(image, background, config->samples, linear, samples);
        if (sampled)
            count = cluster_colors(samples, sampled, clusters);
        free(samples);
    }
    if (!sampled) {
        clusters[0] = (struct color_cluster){to_oklab(background, linear), 1};
        sampled = 1;
    }
    semantic_palette(palette, clusters, count, sampled, config, linear);
    return true;
}

struct request {
    enum wall_command command;
    const char *mode;
    const char *path;
};

static const char *next_field(const char **cursor, const char *end)
{
    const char *field = *cursor;
    const char *nul = memchr(field, 0, (size_t)(end - field));
    if (!nul)
        return NULL;
    *cursor = nul + 1;
    return field;
}

static bool parse_request(const char *packet, size_t length, struct request *request)
{
    const char *cursor = packet, *end = packet + length;
    const char *version = next_field(&cursor, end);
    const char *verb = next_field(&cursor, end);
    if (!version || strcmp(version, "WALL1") || !verb)
        return false;
    *request = (struct request){0};
    if (strcmp(verb, "SET") == 0) {
        request->command = WALL_SET;
        const char *name = next_field(&cursor, end);
        request->path = next_field(&cursor, end);
        enum mode mode = MODE_FILL;
        if (!name || (*name && !parse_mode(name, &mode)) ||
            !request->path || request->path[0] != '/' || strlen(request->path) > WALL_PATH_MAX)
            return false;
        if (*name)
            request->mode = mode_names[mode];
    } else if (strcmp(verb, "GET") == 0) {
        request->command = WALL_GET;
    } else if (strcmp(verb, "RELOAD") == 0) {
        request->command = WALL_RELOAD;
    } else if (strcmp(verb, "STOP") == 0) {
        request->command = WALL_STOP;
    }
    return request->command != WALL_RENDER && cursor == end;
}

static size_t encode_request(char *packet, enum wall_command command,
                             const char *mode, const char *path)
{
    static const char *const verbs[] = {
        [WALL_SET] = "SET", [WALL_GET] = "GET",
        [WALL_RELOAD] = "RELOAD", [WALL_STOP] = "STOP",
    };
    if (command < WALL_SET || command > WALL_STOP)
        return 0;
    int length;
    if (command == WALL_SET) {
        if (!path || path[0] != '/' || strlen(path) > WALL_PATH_MAX ||
            (mode && strlen(mode) > 7))
            return 0;
        length = snprintf(packet, WALL_PACKET_MAX, "WALL1%cSET%c%s%c%s",
                          0, 0, mode ? mode : "", 0, path);
    } else {
        length = snprintf(packet, WALL_PACKET_MAX, "WALL1%c%s", 0, verbs[command]);
    }
    return length >= 0 && length < WALL_PACKET_MAX ? (size_t)length + 1 : 0;
}

static char *state_directory(void)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char *directory;
    if (xdg && xdg[0] == '/') {
        if (asprintf(&directory, "%s/wall", xdg) >= 0)
            return directory;
    } else if (home && home[0] == '/') {
        if (asprintf(&directory, "%s/.local/state/wall", home) >= 0)
            return directory;
    }
    return NULL;
}

static char *wall_cache_directory(void)
{
    const char *base = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    char *directory;
    if (base && *base == '/') {
        if (asprintf(&directory, "%s/wall", base) >= 0)
            return directory;
    } else if (home && *home == '/') {
        if (asprintf(&directory, "%s/.cache/wall", home) >= 0)
            return directory;
    } else {
        errno = ENOENT;
    }
    return NULL;
}

static char *theme_palette_path(void)
{
    char *directory = wall_cache_directory(), *path = NULL;
    if (directory && asprintf(&path, "%s/colors.conf", directory) < 0)
        path = NULL;
    free(directory);
    return path;
}

static int read_state(const char *directory, struct config *config)
{
    if (!directory)
        return 0;
    char *path;
    if (asprintf(&path, "%s/selection", directory) < 0)
        return -1;
    struct stat st;
    int fd = open_regular(path, &st);
    int error = errno;
    free(path);
    if (fd < 0) {
        errno = error;
        return error == ENOENT ? 0 : -1;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    char packet[WALL_PACKET_MAX + 1];
    size_t length = fread(packet, 1, sizeof packet, file);
    error = ferror(file) ? errno : 0;
    fclose(file);
    if (error) {
        errno = error;
        return -1;
    }
    struct request request;
    enum mode mode;
    if (length > WALL_PACKET_MAX || !parse_request(packet, length, &request) ||
        request.command != WALL_SET || !request.mode || !parse_mode(request.mode, &mode)) {
        errno = EINVAL;
        return -1;
    }
    char *selected = strdup(request.path);
    if (!selected)
        return -1;
    free(config->path);
    config->path = selected;
    config->mode = mode;
    return 1;
}

static bool create_state_directory(const char *directory)
{
    char *path = strdup(directory);
    if (!path)
        return false;
    for (char *slash = path + 1; (slash = strchr(slash, '/')); ++slash) {
        *slash = '\0';
        if (mkdir(path, 0700) < 0 && errno != EEXIST) {
            int error = errno;
            free(path);
            errno = error;
            return false;
        }
        *slash = '/';
    }
    free(path);
    if (mkdir(directory, 0700) < 0 && errno != EEXIST)
        return false;
    struct stat st;
    if (lstat(directory, &st) < 0)
        return false;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        errno = EACCES;
        return false;
    }
    return true;
}

static bool replace_contents(const char *directory, const char *name, const void *data, size_t length)
{
    if (!directory) {
        errno = ENOENT;
        return false;
    }
    if (!create_state_directory(directory))
        return false;
    char *target, *temporary;
    if (asprintf(&target, "%s/%s", directory, name) < 0)
        return false;
    if (asprintf(&temporary, "%s/.%s-XXXXXX", directory, name) < 0) {
        free(target);
        return false;
    }
    int fd = mkostemp(temporary, O_CLOEXEC);
    int error = errno;
    if (fd >= 0) {
        FILE *file = fdopen(fd, "wb");
        if (file) {
            error = 0;
            if (fwrite(data, 1, length, file) != length ||
                fflush(file) == EOF || fsync(fd) < 0)
                error = errno ? errno : EIO;
            if (fclose(file) == EOF && !error)
                error = errno;
        } else {
            error = errno;
            close(fd);
        }
        if (!error && rename(temporary, target) < 0)
            error = errno;
        if (error)
            unlink(temporary);
    }
    free(target);
    free(temporary);
    errno = error;
    return !error;
}

static bool save_state(const char *directory, const struct config *config)
{
    char packet[WALL_PACKET_MAX];
    size_t length = encode_request(packet, WALL_SET, mode_names[config->mode], config->path);
    if (!length) {
        errno = EINVAL;
        return false;
    }
    return replace_contents(directory, "selection", packet, length);
}

static size_t format_palette(const struct palette *palette, char text[PALETTE_TEXT_SIZE])
{
    static const char header[] = "[colors]\n";
    memcpy(text, header, sizeof header);
    size_t used = sizeof header - 1;
    for (size_t i = 0; i < PALETTE_COUNT; ++i) {
        int length = snprintf(text + used, PALETTE_TEXT_SIZE - used, "%s = #%06x\n",
                              palette_names[i], (unsigned)palette->rgb[i]);
        if (length < 0 || (size_t)length >= PALETTE_TEXT_SIZE - used)
            return 0;
        used += (size_t)length;
    }
    return used;
}

static bool save_palette(const struct palette *palette)
{
    char text[PALETTE_TEXT_SIZE];
    size_t length = format_palette(palette, text);
    if (!length) {
        errno = EOVERFLOW;
        return false;
    }
    char *directory = wall_cache_directory();
    if (!directory)
        return false;
    bool saved = replace_contents(directory, "colors.conf", text, length);
    int error = errno;
    free(directory);
    errno = error;
    return saved;
}

static bool parse_theme_palette(FILE *file, uint32_t rgb[PALETTE_COUNT])
{
    char text[1025];
    size_t length = fread(text, 1, sizeof text, file);
    if (length == sizeof text || !feof(file) || ferror(file) || memchr(text, '\0', length))
        return false;
    text[length] = '\0';

    uint32_t colors[PALETTE_COUNT] = {0};
    unsigned seen = 0;
    bool section = false;
    char *cursor = text, *line;
    while ((line = strsep(&cursor, "\n"))) {
        char *key = trim(line);
        if (!*key || *key == '#' || *key == ';')
            continue;
        if (!section) {
            if (strcmp(key, "[colors]"))
                return false;
            section = true;
            continue;
        }
        char *value = strchr(key, '=');
        if (!value)
            return false;
        *value++ = '\0';
        key = trim(key);
        value = trim(value);
        size_t i = 0;
        while (i < PALETTE_COUNT && strcmp(key, palette_names[i]))
            ++i;
        if (i == PALETTE_COUNT || (seen & (1u << i)))
            return false;
        if (!parse_background(value, &colors[i]))
            return false;
        seen |= 1u << i;
    }
    if (seen != (1u << PALETTE_COUNT) - 1)
        return false;
    memcpy(rgb, colors, sizeof colors);
    return true;
}

static int64_t monotonic_ns(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0)
        return -1;
    return (int64_t)time.tv_sec * NSEC_PER_SEC + time.tv_nsec;
}

static int64_t monotonic_ms(void)
{
    int64_t time = monotonic_ns();
    return time < 0 ? -1 : time / NSEC_PER_MSEC;
}

static uint64_t hash_path(const char *path)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *byte = (const unsigned char *)path; *byte; ++byte)
        hash = (hash ^ *byte) * UINT64_C(1099511628211);
    return hash;
}

static char *display_socket(void)
{
    const char *inherited = getenv("WAYLAND_SOCKET");
    if (inherited) {
        char *end;
        errno = 0;
        long fd = strtol(inherited, &end, 10);
        if (errno || end == inherited || *end || fd < 0 || fd > INT_MAX) {
            errno = EINVAL;
            return NULL;
        }
        struct sockaddr_un peer = {0};
        socklen_t size = sizeof peer;
        if (getpeername((int)fd, (struct sockaddr *)&peer, &size) < 0)
            return NULL;
        if (peer.sun_family != AF_UNIX || peer.sun_path[0] != '/' ||
            !memchr(peer.sun_path, 0, sizeof peer.sun_path)) {
            errno = ENOTSUP;
            return NULL;
        }
        return realpath(peer.sun_path, NULL);
    }
    const char *display = getenv("WAYLAND_DISPLAY");
    if (!display || !*display)
        display = "wayland-0";
    if (display[0] == '/')
        return realpath(display, NULL);
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') {
        errno = EINVAL;
        return NULL;
    }
    char *path;
    if (asprintf(&path, "%s/%s", runtime, display) < 0)
        return NULL;
    char *resolved = realpath(path, NULL);
    int error = errno;
    free(path);
    errno = error;
    return resolved;
}

static char *ipc_socket_path(const char *display)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') {
        errno = EINVAL;
        return NULL;
    }
    struct stat st;
    if (stat(runtime, &st) < 0)
        return NULL;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) {
        errno = EACCES;
        return NULL;
    }
    char *path;
    if (asprintf(&path, "%s/wall-%016llx.sock", runtime, (unsigned long long)hash_path(display)) < 0)
        return NULL;
    return path;
}

static bool unix_address(struct sockaddr_un *address, const char *path, socklen_t *size)
{
    size_t length = strlen(path);
    if (length >= sizeof address->sun_path) {
        errno = ENAMETOOLONG;
        return false;
    }
    *address = (struct sockaddr_un){.sun_family = AF_UNIX};
    memcpy(address->sun_path, path, length + 1);
    *size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
    return true;
}

static bool same_user(int fd)
{
    struct ucred peer;
    socklen_t size = sizeof peer;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0)
        return false;
    if (size != sizeof peer || peer.uid != geteuid()) {
        errno = EACCES;
        return false;
    }
    return true;
}

static bool unlink_stale_socket(int fd, const char *path,
                                const struct sockaddr_un *address, socklen_t size)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return errno == ENOENT;
    if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
        errno = EEXIST;
        return false;
    }
    if (connect(fd, (const struct sockaddr *)address, size) == 0 || errno == EAGAIN) {
        errno = EADDRINUSE;
        return false;
    }
    if (errno == ENOENT)
        return true;
    if (errno != ECONNREFUSED)
        return false;
    struct stat current;
    if (lstat(path, &current) < 0)
        return errno == ENOENT;
    if (current.st_dev != st.st_dev || current.st_ino != st.st_ino) {
        errno = EAGAIN;
        return false;
    }
    return unlink(path) == 0;
}

static int lock_session_file(const char *path)
{
    int error;
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0)
        goto fail;
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
        (st.st_mode & 0077)) {
        errno = EACCES;
        goto fail;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        return fd;
    if (errno == EWOULDBLOCK)
        errno = EADDRINUSE;

fail:
    error = errno;
    close(fd);
    errno = error;
    return -1;
}

static int lock_browser(const char *display)
{
    char *endpoint = ipc_socket_path(display);
    if (!endpoint)
        return -1;
    char *path;
    int length = asprintf(&path, "%s.browser.lock", endpoint);
    int error = errno;
    free(endpoint);
    if (length < 0) {
        errno = error;
        return -1;
    }
    int lock = lock_session_file(path);
    error = errno;
    free(path);
    errno = error;
    return lock;
}

static bool start_ipc(struct app *app, const char *display)
{
    char *path = ipc_socket_path(display);
    if (!path)
        return false;
    struct sockaddr_un address;
    socklen_t size;
    int probe = -1, error;
    bool bound = false;
    if (!unix_address(&address, path, &size))
        goto fail;
    char *lock_path;
    if (asprintf(&lock_path, "%s.lock", path) < 0)
        goto fail;
    app->lock = lock_session_file(lock_path);
    error = errno;
    free(lock_path);
    errno = error;
    if (app->lock < 0)
        goto fail;
    struct stat st;

    int flags = SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC;
    probe = socket(AF_UNIX, flags, 0);
    if (probe < 0 || !unlink_stale_socket(probe, path, &address, size))
        goto fail;
    close(probe);
    probe = -1;
    app->listener = socket(AF_UNIX, flags, 0);
    if (app->listener < 0)
        goto fail;
    mode_t previous = umask(0177);
    int result = bind(app->listener, (const struct sockaddr *)&address, size);
    error = errno;
    umask(previous);
    errno = error;
    if (result < 0)
        goto fail;
    bound = true;
    if (lstat(path, &st) < 0 || listen(app->listener, WALL_IPC_CLIENTS) < 0)
        goto fail;
    app->socket_device = st.st_dev;
    app->socket_inode = st.st_ino;
    app->socket_path = path;
    return true;

fail:
    error = errno;
    if (probe >= 0)
        close(probe);
    if (bound)
        unlink(path);
    free(path);
    errno = error;
    return false;
}

static void interpolate_rgba(const struct image *image, double x, double y, double rgba[4])
{
    x = fmax(0, fmin(x, image->width - 1));
    y = fmax(0, fmin(y, image->height - 1));
    uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y;
    uint32_t x1 = x0 + (x0 + 1 < image->width);
    uint32_t y1 = y0 + (y0 + 1 < image->height);
    double dx = x - x0, dy = y - y0;
    double weights[] = {(1 - dx) * (1 - dy), dx * (1 - dy), (1 - dx) * dy, dx * dy};
    const unsigned char *pixels[] = {
        image->pixels + ((size_t)y0 * image->width + x0) * 4,
        image->pixels + ((size_t)y0 * image->width + x1) * 4,
        image->pixels + ((size_t)y1 * image->width + x0) * 4,
        image->pixels + ((size_t)y1 * image->width + x1) * 4,
    };
    for (size_t i = 0; i < 4; ++i) {
        for (size_t channel = 0; channel < 4; ++channel)
            rgba[channel] += pixels[i][channel] * weights[i];
    }
}

static uint32_t sample(const struct image *image, double x, double y, uint32_t background)
{
    double rgba[4] = {0};
    interpolate_rgba(image, x, y, rgba);
    double uncovered = 1 - rgba[3] / 255;
    uint32_t pixel = 0xff000000;
    for (size_t channel = 0; channel < 3; ++channel) {
        unsigned shift = (unsigned)(2 - channel) * 8;
        unsigned base = (background >> shift) & 0xff;
        pixel |= (uint32_t)(rgba[channel] + base * uncovered + 0.5) << shift;
    }
    return pixel;
}

static int image_edge(double edge, int limit)
{
    return (int)fmax(0, fmin(ceil(edge - 0.5), limit));
}

static void paint(uint32_t *pixels, int width, int height, const struct config *config,
                  const struct image *image)
{
    uint32_t background = 0xff000000 | config->background;
    if (!image->pixels) {
        for (size_t i = 0; i < (size_t)width * height; ++i)
            pixels[i] = background;
        return;
    }

    double scale_x = (double)width / image->width;
    double scale_y = (double)height / image->height;
    switch (config->mode) {
    case MODE_FILL:
        scale_x = scale_y = fmax(scale_x, scale_y);
        break;
    case MODE_FIT:
        scale_x = scale_y = fmin(scale_x, scale_y);
        break;
    case MODE_CENTER:
        scale_x = scale_y = 1;
        break;
    case MODE_STRETCH:
    case MODE_SOLID:
        break;
    }

    double image_width = image->width * scale_x;
    double image_height = image->height * scale_y;
    double left = (width - image_width) * 0.5;
    double top = (height - image_height) * 0.5;
    if (config->mode == MODE_CENTER) {
        left = floor(left);
        top = floor(top);
    }
    int x0 = image_edge(left, width), x1 = image_edge(left + image_width, width);
    int y0 = image_edge(top, height), y1 = image_edge(top + image_height, height);

    /* Preserve a visible row or column when fitting a very narrow image. */
    if (x0 == x1) {
        x0 = (width - 1) / 2;
        x1 = x0 + 1;
    }
    if (y0 == y1) {
        y0 = (height - 1) / 2;
        y1 = y0 + 1;
    }

    for (int y = 0; y < height; ++y) {
        uint32_t *row = pixels + (size_t)y * width;
        if (y < y0 || y >= y1) {
            for (int x = 0; x < width; ++x)
                row[x] = background;
            continue;
        }
        for (int x = 0; x < x0; ++x)
            row[x] = background;
        double image_y = y1 - y0 == 1 && image_height < 1
                       ? (image->height - 1) * 0.5 : (y + 0.5 - top) / scale_y - 0.5;
        for (int x = x0; x < x1; ++x) {
            double image_x = x1 - x0 == 1 && image_width < 1
                           ? (image->width - 1) * 0.5 : (x + 0.5 - left) / scale_x - 0.5;
            row[x] = sample(image, image_x, image_y, background);
        }
        for (int x = x1; x < width; ++x)
            row[x] = background;
    }
}

static void destroy_buffer(struct buffer *buffer)
{
    wl_buffer_destroy(buffer->proxy);
    munmap(buffer->pixels, (size_t)buffer->size);
    wl_list_remove(&buffer->link);
    free(buffer);
}

static bool buffer_held(const struct output *output, const struct buffer *buffer)
{
    return buffer->busy || buffer == output->shown || buffer == output->prepared ||
           buffer == output->animation_old || buffer == output->animation_final;
}

static void prune_buffers(struct output *output)
{
    struct buffer *buffer, *next;
    bool spare = false;
    wl_list_for_each_safe(buffer, next, &output->buffers, link) {
        if (buffer_held(output, buffer))
            continue;
        if (buffer->retired || spare ||
            (output->shown && (buffer->width != output->shown->width ||
                               buffer->height != output->shown->height))) {
            destroy_buffer(buffer);
        } else {
            spare = true;
        }
    }
}

static void buffer_release(void *data, struct wl_buffer *proxy)
{
    (void)proxy;
    struct buffer *buffer = data;
    buffer->busy = false;
    prune_buffers(buffer->output);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static bool buffer_available(const struct output *output, unsigned limit)
{
    struct buffer *buffer;
    unsigned held = 0;
    wl_list_for_each(buffer, &output->buffers, link) {
        if (buffer_held(output, buffer) && ++held >= limit)
            return false;
    }
    return true;
}

/* The renderer's wallpapers are opaque. The browser uses premultiplied ARGB
   so its background can follow [appearance] opacity. */
static struct buffer *allocate_buffer(struct wl_shm *shm, int width, int height, bool alpha)
{
    int stride = width * 4;
    int size = stride * height;
    int fd = memfd_create("wall-buffer", MFD_CLOEXEC);
    if (fd < 0)
        return NULL;

    struct wl_shm_pool *pool = NULL;
    uint32_t *pixels = MAP_FAILED;
    int error = posix_fallocate(fd, 0, size);
    if (error == 0) {
        pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (pixels != MAP_FAILED)
            pool = wl_shm_create_pool(shm, fd, size);
        if (!pool)
            error = errno;
    }
    close(fd);
    if (!pool) {
        if (pixels != MAP_FAILED)
            munmap(pixels, (size_t)size);
        errno = error;
        return NULL;
    }

    struct buffer *buffer = calloc(1, sizeof *buffer);
    if (buffer) {
        buffer->proxy = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                  alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
    }
    wl_shm_pool_destroy(pool);
    if (!buffer || !buffer->proxy) {
        munmap(pixels, (size_t)size);
        free(buffer);
        errno = ENOMEM;
        return NULL;
    }

    buffer->pixels = pixels;
    buffer->width = width;
    buffer->height = height;
    buffer->size = size;
    wl_list_init(&buffer->link);
    return buffer;
}

static struct buffer *make_buffer(struct output *output, int width, int height, unsigned limit)
{
    if (!buffer_available(output, limit)) {
        errno = EAGAIN;
        return NULL;
    }
    struct buffer *buffer, *next;
    wl_list_for_each_safe(buffer, next, &output->buffers, link) {
        if (!buffer_held(output, buffer)) {
            if (!buffer->retired && buffer->width == width && buffer->height == height)
                return buffer;
            destroy_buffer(buffer);
        }
    }
    buffer = allocate_buffer(output->app->shm, width, height, false);
    if (buffer) {
        buffer->output = output;
        wl_list_insert(&output->buffers, &buffer->link);
        wl_buffer_add_listener(buffer->proxy, &buffer_listener, buffer);
    }
    return buffer;
}

static uint32_t scaled_dimension(uint32_t size, uint32_t numerator, uint32_t denominator)
{
    uint64_t value = ((uint64_t)size * numerator + denominator / 2) / denominator;
    return (uint32_t)(value > UINT32_MAX ? UINT32_MAX : value);
}

static struct geometry output_geometry(const struct output *output)
{
    struct geometry geometry = {
        .width = output->width,
        .height = output->height,
        .fractional = output->fractional_units != 0,
    };
    uint32_t denominator = 1;
    if (geometry.fractional) {
        geometry.scale = output->fractional_units;
        denominator = FRACTIONAL_DENOMINATOR;
    } else {
        geometry.scale = (uint32_t)(output->preferred_scale > 0
                                  ? output->preferred_scale : output->state.scale);
    }

    /* A zero configure dimension leaves that dimension up to the client. */
    uint32_t width = output->state.width, height = output->state.height;
    if (output->state.transform == WL_OUTPUT_TRANSFORM_90 ||
        output->state.transform == WL_OUTPUT_TRANSFORM_270 ||
        output->state.transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
        output->state.transform == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        width = output->state.height;
        height = output->state.width;
    }
    if (!geometry.width)
        geometry.width = scaled_dimension(width, denominator, geometry.scale);
    if (!geometry.height)
        geometry.height = scaled_dimension(height, denominator, geometry.scale);
    return geometry;
}

static bool same_geometry(struct geometry a, struct geometry b)
{
    return a.width == b.width && a.height == b.height &&
           a.scale == b.scale && a.fractional == b.fractional;
}

static void ack_configure(struct output *output)
{
    if (output->configure_pending) {
        zwlr_layer_surface_v1_ack_configure(output->layer, output->serial);
        output->configure_pending = false;
    }
}

static struct buffer *prepare_frame(struct output *output, struct geometry geometry,
                                    const struct config *config, const struct image *image)
{
    uint32_t denominator = geometry.fractional ? FRACTIONAL_DENOMINATOR : 1;
    uint32_t width = scaled_dimension(geometry.width, geometry.scale, denominator);
    uint32_t height = scaled_dimension(geometry.height, geometry.scale, denominator);
    if (geometry.width > INT32_MAX || geometry.height > INT32_MAX ||
        !width || !height || width > INT32_MAX / 4 || height > INT32_MAX / (width * 4)) {
        errno = EOVERFLOW;
        return NULL;
    }

    struct buffer *buffer = make_buffer(output, (int)width, (int)height, STATIC_BUFFERS);
    if (buffer)
        paint(buffer->pixels, (int)width, (int)height, config, image);
    return buffer;
}

static void present(struct output *output, struct geometry geometry, struct buffer *buffer,
                    bool final)
{
    ack_configure(output);
    if (geometry.fractional)
        wp_viewport_set_destination(output->viewport, (int32_t)geometry.width, (int32_t)geometry.height);
    wl_surface_set_buffer_scale(output->surface, geometry.fractional ? 1 : (int32_t)geometry.scale);
    buffer->busy = true;
    wl_surface_attach(output->surface, buffer->proxy, 0, 0);
    if (wl_surface_get_version(output->surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
        wl_surface_damage_buffer(output->surface, 0, 0, buffer->width, buffer->height);
    else
        wl_surface_damage(output->surface, 0, 0, (int32_t)geometry.width, (int32_t)geometry.height);
    wl_surface_commit(output->surface);
    output->shown = buffer;
    output->committed = geometry;
    output->committed_revision = final ? output->app->revision : 0;
    prune_buffers(output);
}

static bool ready_output(const struct output *output, struct geometry *geometry)
{
    if (!output->layer || !output->configured)
        return false;
    *geometry = output_geometry(output);
    return geometry->width && geometry->height;
}

static double ease_progress(enum transition_easing easing, double progress)
{
    switch (easing) {
    case EASE_IN:
        return progress * progress;
    case EASE_OUT:
        return progress * (2 - progress);
    case EASE_IN_OUT:
        return progress * progress * (3 - 2 * progress);
    case EASE_LINEAR:
        return progress;
    }
    return progress;
}

static uint32_t mix_pixels(uint32_t old, uint32_t target, unsigned weight)
{
    unsigned inverse = 256 - weight;
    uint32_t rb = ((old & 0xff00ffu) * inverse + (target & 0xff00ffu) * weight + 0x800080u) >> 8;
    uint32_t green = ((old & 0xff00u) * inverse + (target & 0xff00u) * weight + 0x8000u) >> 8;
    return 0xff000000u | (rb & 0xff00ffu) | (green & 0xff00u);
}

static uint32_t mask_pixel(uint32_t old, uint32_t target, double distance)
{
    if (distance <= -0.5)
        return old;
    if (distance >= 0.5)
        return target;
    return mix_pixels(old, target, (unsigned)((distance + 0.5) * 256 + 0.5));
}

static void paint_slide(struct buffer *frame, const struct buffer *old, const struct buffer *target,
                        double progress, unsigned direction)
{
    int width = frame->width, height = frame->height;
    size_t stride = (size_t)width;
    bool vertical = direction & 1u, reversed = direction & 2u;
    int length = vertical ? height : width;
    int incoming = (int)(progress * length + 0.5);
    int remaining = length - incoming;

    if (vertical) {
        size_t new_pixels = (size_t)incoming * stride, old_pixels = (size_t)remaining * stride;
        if (reversed) {
            memcpy(frame->pixels, old->pixels + new_pixels, old_pixels * 4);
            memcpy(frame->pixels + old_pixels, target->pixels, new_pixels * 4);
        } else {
            memcpy(frame->pixels, target->pixels + old_pixels, new_pixels * 4);
            memcpy(frame->pixels + new_pixels, old->pixels, old_pixels * 4);
        }
        return;
    }

    for (int y = 0; y < height; ++y) {
        uint32_t *row = frame->pixels + (size_t)y * stride;
        const uint32_t *from = old->pixels + (size_t)y * stride;
        const uint32_t *to = target->pixels + (size_t)y * stride;
        if (reversed) {
            memcpy(row, from + incoming, (size_t)remaining * 4);
            memcpy(row + remaining, to, (size_t)incoming * 4);
        } else {
            memcpy(row, to + remaining, (size_t)incoming * 4);
            memcpy(row + incoming, from, (size_t)remaining * 4);
        }
    }
}

static int clamp_column(double column, int width)
{
    if (column <= 0)
        return 0;
    return column >= width ? width : (int)column;
}

static void paint_edge_row(uint32_t *dest, const uint32_t *old, const uint32_t *target,
                           int width, double edge, double slope, bool reverse)
{
    double half_edge = 0.5 / slope;
    int first = clamp_column(floor(edge - half_edge + 0.5), width);
    int last = clamp_column(ceil(edge + half_edge - 0.5), width);
    if (reverse) {
        memcpy(dest, old, (size_t)(width - last) * sizeof *dest);
        memcpy(dest + width - first, target + width - first, (size_t)first * sizeof *dest);
    } else {
        memcpy(dest, target, (size_t)first * sizeof *dest);
        memcpy(dest + last, old + last, (size_t)(width - last) * sizeof *dest);
    }
    for (int x = first; x < last; ++x) {
        int column = reverse ? width - 1 - x : x;
        dest[column] = mask_pixel(old[column], target[column], (edge - x - 0.5) * slope);
    }
}

static void paint_center_row(uint32_t *dest, const uint32_t *old, const uint32_t *target,
                             int width, double outer, double inner, double size,
                             double y_distance, bool circle)
{
    if (outer <= 0) {
        memcpy(dest, old, (size_t)width * sizeof *dest);
        return;
    }
    double center = width * 0.5;
    int first = clamp_column(floor(center - outer - 0.5), width);
    int last = clamp_column(ceil(center + outer + 0.5), width);
    int interior_first = clamp_column(ceil(center - inner - 0.5), width);
    int interior_last = clamp_column(floor(center + inner + 0.5), width);
    memcpy(dest, old, (size_t)first * sizeof *dest);
    memcpy(dest + last, old + last, (size_t)(width - last) * sizeof *dest);
    for (int x = first; x < last; ++x) {
        if (inner > 0 && x == interior_first && interior_last > interior_first) {
            memcpy(dest + x, target + x, (size_t)(interior_last - x) * sizeof *dest);
            x = interior_last - 1;
            continue;
        }
        double dx = x + 0.5 - center;
        double rect_x = size - fabs(dx);
        double distance = circle ? size - sqrt(dx * dx + y_distance) :
                          rect_x < y_distance ? rect_x : y_distance;
        dest[x] = mask_pixel(old[x], target[x], distance);
    }
}

static void paint_mask(struct buffer *frame, const struct buffer *old, const struct buffer *target,
                       const struct animation *animation, double progress)
{
    double half_width = frame->width * 0.5, half_height = frame->height * 0.5;
    double radius = hypot(half_width, half_height) * progress;
    double inner = fmax(0, radius - 0.5), outer = radius + 0.5;
    double inner_squared = inner * inner, outer_squared = outer * outer;
    double grow_width = half_width * progress, grow_height = half_height * progress;
    double diagonal = ((double)frame->width + frame->height) * progress;
    bool flip_x = animation->direction & 1u, flip_y = animation->direction & 2u;
    bool vertical_wipe = animation->direction & 1u;
    double edge = (vertical_wipe ? frame->height : frame->width) * progress;

    for (int y = 0; y < frame->height; ++y) {
        double center_y = y + 0.5;
        double dy = center_y - half_height;
        double from_edge = flip_y ? frame->height - center_y : center_y;
        size_t offset = (size_t)y * frame->width;
        uint32_t *dest = frame->pixels + offset;
        const uint32_t *from = old->pixels + offset, *to = target->pixels + offset;

        switch (animation->effect) {
        case TRANSITION_CIRCLE: {
            double squared_y = dy * dy;
            double row_outer = squared_y < outer_squared ? sqrt(outer_squared - squared_y) : 0;
            double row_inner = squared_y < inner_squared ? sqrt(inner_squared - squared_y) : 0;
            paint_center_row(dest, from, to, frame->width, row_outer, row_inner, radius, squared_y, true);
            break;
        }
        case TRANSITION_WIPE:
            if (vertical_wipe) {
                double distance = edge - from_edge;
                if (distance <= -0.5 || distance >= 0.5) {
                    memcpy(dest, distance >= 0.5 ? to : from, (size_t)frame->width * sizeof *dest);
                } else {
                    for (int x = 0; x < frame->width; ++x)
                        dest[x] = mask_pixel(from[x], to[x], distance);
                }
            } else {
                paint_edge_row(dest, from, to, frame->width, edge, 1, flip_y);
            }
            break;
        case TRANSITION_DIAGONAL:
            paint_edge_row(dest, from, to, frame->width, diagonal - from_edge, 0.7071067811865475, flip_x);
            break;
        case TRANSITION_GROW: {
            double rect_y = grow_height - fabs(dy);
            paint_center_row(dest, from, to, frame->width,
                             rect_y <= -0.5 ? 0 : grow_width + 0.5,
                             rect_y >= 0.5 ? grow_width - 0.5 : 0, grow_width, rect_y, false);
            break;
        }
        default:
            break;
        }
    }
}

static void paint_animation(struct output *output, struct buffer *frame, double progress)
{
    const struct animation *animation = &output->app->animation;
    const struct buffer *old = output->animation_old, *target = output->animation_final;
    if (animation->effect == TRANSITION_SLIDE) {
        paint_slide(frame, old, target, progress, animation->direction);
    } else if (animation->effect == TRANSITION_FADE) {
        unsigned weight = (unsigned)(progress * 256 + 0.5);
        size_t pixels = (size_t)frame->width * frame->height;
        for (size_t i = 0; i < pixels; ++i)
            frame->pixels[i] = mix_pixels(old->pixels[i], target->pixels[i], weight);
    } else {
        paint_mask(frame, old, target, animation, progress);
    }
}

static void animation_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)time;
    struct output *output = data;
    wl_callback_destroy(callback);
    output->frame_callback = NULL;
}

static const struct wl_callback_listener animation_frame_listener = {.done = animation_frame_done};

static void clear_output_animation(struct output *output)
{
    if (output->frame_callback) {
        wl_callback_destroy(output->frame_callback);
        output->frame_callback = NULL;
    }
    output->animation_old = NULL;
    output->animation_final = NULL;
    struct buffer *buffer;
    wl_list_for_each(buffer, &output->buffers, link) {
        if (buffer != output->shown || output->committed_revision != output->app->revision)
            buffer->retired = true;
    }
    prune_buffers(output);
}

static void advance_animation(struct app *app, int64_t now)
{
    struct animation *animation = &app->animation;
    if (animation->effect == TRANSITION_NONE)
        return;
    bool active = false;
    struct output *output;
    wl_list_for_each(output, &app->outputs, link) {
        struct buffer *target = output->animation_final;
        if (!target)
            continue;
        int64_t time = monotonic_ns();
        if (time > now)
            now = time;
        struct geometry geometry;
        if (!ready_output(output, &geometry) || !same_geometry(geometry, output->committed)) {
            clear_output_animation(output);
            continue;
        }
        if (now >= animation->end) {
            present(output, geometry, target, true);
            clear_output_animation(output);
            continue;
        }
        if (animation->end - now < animation->interval) {
            output->next_frame = animation->end;
            active = true;
            continue;
        }
        if (!output->frame_callback && now >= output->next_frame &&
            buffer_available(output, ANIMATION_BUFFERS)) {
            struct buffer *frame = make_buffer(output, target->width, target->height, ANIMATION_BUFFERS);
            if (!frame) {
                present(output, geometry, target, true);
                clear_output_animation(output);
                continue;
            }
            double elapsed = (double)(now - animation->start) / (animation->end - animation->start);
            paint_animation(output, frame, ease_progress(animation->easing, elapsed));
            int64_t submitted = monotonic_ns();
            if (submitted >= animation->end) {
                present(output, geometry, target, true);
                clear_output_animation(output);
                continue;
            }
            int64_t finished = submitted < 0 ? now : submitted;
            output->next_frame = finished + animation->interval;
            if (output->next_frame <= animation->end) {
                output->frame_callback = wl_surface_frame(output->surface);
                if (output->frame_callback)
                    wl_callback_add_listener(output->frame_callback, &animation_frame_listener, output);
                present(output, geometry, frame, false);
            }
        }
        active = true;
    }
    if (!active)
        *animation = (struct animation){0};
}

static int64_t animation_deadline(const struct app *app)
{
    if (app->animation.effect == TRANSITION_NONE)
        return 0;
    int64_t deadline = app->animation.end;
    const struct output *output;
    wl_list_for_each(output, &app->outputs, link) {
        if (output->animation_final && !output->frame_callback && output->next_frame < deadline &&
            buffer_available(output, ANIMATION_BUFFERS))
            deadline = output->next_frame;
    }
    return (deadline + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC;
}

static struct animation choose_animation(const struct transition_config *config, int64_t now)
{
    if (!config->enabled || !config->duration_ms || config->type == TRANSITION_NONE || now < 0)
        return (struct animation){0};

    enum transition_effect effect = config->type;
    uint32_t random;
    if (getrandom(&random, sizeof random, GRND_NONBLOCK) != (ssize_t)sizeof random)
        random = (uint32_t)now ^ (uint32_t)(now >> 32) ^ (uint32_t)getpid();
    if (effect == TRANSITION_RANDOM) {
        enum transition_effect enabled[TRANSITION_RANDOM - 1];
        size_t count = 0;
        for (int i = TRANSITION_FADE; i < TRANSITION_RANDOM; ++i) {
            if (config->effects & (1u << i))
                enabled[count++] = (enum transition_effect)i;
        }
        if (!count)
            return (struct animation){0};
        effect = enabled[(random >> 2) % count];
    }
    return (struct animation){
        .effect = effect, .easing = config->easing, .direction = random & 3u,
        .start = now, .end = now + (int64_t)config->duration_ms * NSEC_PER_MSEC,
        .interval = (NSEC_PER_SEC + config->fps - 1) / config->fps,
    };
}

static void show_prepared_wallpaper(struct app *app, bool animate)
{
    struct animation animation = animate ? choose_animation(&app->config.transition, monotonic_ns()) :
                                          (struct animation){0};
    bool active = false;
    struct output *output;
    wl_list_for_each(output, &app->outputs, link) {
        struct buffer *target = output->prepared;
        if (!target)
            continue;
        struct geometry geometry = output_geometry(output);
        struct buffer *old = output->shown;
        output->prepared = NULL;
        if (animation.effect != TRANSITION_NONE && old && same_geometry(geometry, output->committed) &&
            old->width == target->width && old->height == target->height &&
            memcmp(old->pixels, target->pixels, (size_t)target->size) != 0) {
            output->animation_old = old;
            output->animation_final = target;
            output->next_frame = animation.start + animation.interval;
            active = true;
        } else {
            present(output, geometry, target, true);
        }
    }
    if (active)
        app->animation = animation;
}

static void render_outputs(struct app *app)
{
    struct image image = {0};
    bool decoded = !app->snapshot;
    bool decode_failed = false;
    struct output *output;
    wl_list_for_each(output, &app->outputs, link) {
        struct geometry geometry;
        if (output->animation_final || !ready_output(output, &geometry))
            continue;
        if (output->committed_revision == app->revision &&
            same_geometry(output->committed, geometry)) {
            output->attempted_revision = 0;
            if (output->configure_pending) {
                ack_configure(output);
                wl_surface_commit(output->surface);
            }
            continue;
        }
        if (output->attempted_revision == app->revision &&
            same_geometry(output->attempted, geometry))
            continue;

        /* Allow a replacement even if the compositor still holds the current buffer. */
        if (!buffer_available(output, STATIC_BUFFERS))
            continue;
        output->attempted = geometry;
        output->attempted_revision = app->revision;
        if (!decoded && !decode_failed) {
            decoded = decode_image(app->snapshot, &image, app->config.path);
            decode_failed = !decoded;
        }
        if (decoded) {
            struct buffer *buffer = prepare_frame(output, geometry, &app->config, &image);
            if (buffer)
                present(output, geometry, buffer, true);
            else
                fprintf(stderr, "wall: output %u: shared-memory buffer: %s\n",
                        output->name, strerror(errno));
        }
    }

    free(image.pixels);
}

static bool outputs_available(struct app *app)
{
    struct output *output;
    struct geometry geometry;
    wl_list_for_each(output, &app->outputs, link) {
        if (ready_output(output, &geometry) && !buffer_available(output, STATIC_BUFFERS))
            return false;
    }
    return true;
}

static bool same_color_settings(const struct color_config *a, const struct color_config *b)
{
    return a->enabled == b->enabled && a->theme == b->theme &&
           a->samples == b->samples && a->contrast == b->contrast;
}

static void publish_pending_palette(struct app *app)
{
    if (!app->palette_pending || app->animation.effect != TRANSITION_NONE)
        return;

    app->palette_pending = false;
    /* Submit final surface requests before the cache write when the socket can take them. */
    if (wl_display_flush(app->display) < 0 && errno != EAGAIN)
        return;
    if (!save_palette(&app->pending_palette))
        fprintf(stderr, "wall: cannot publish wallpaper colors: %s\n", strerror(errno));
}

static bool apply_wallpaper(struct app *app, struct config *config, bool save,
                            char *error, size_t error_size)
{
    struct image image = {0};
    FILE *snapshot = NULL;
    bool success = false;
    bool solid = config->mode == MODE_SOLID || !config->path;
    bool unchanged = app->revision && app->config.mode == config->mode &&
                     app->config.background == config->background &&
                     ((!config->path && !app->config.path) ||
                      (config->path && app->config.path &&
                       strcmp(config->path, app->config.path) == 0));
    if (!solid || save) {
        snapshot = snapshot_image(config->path);
        if (!snapshot) {
            snprintf(error, error_size, "cannot read wallpaper image: %s", strerror(errno));
            goto done;
        }
        if (!solid)
            unchanged = unchanged && app->snapshot && same_snapshot(app->snapshot, snapshot);
        if ((solid || !unchanged) && !decode_image(snapshot, &image, config->path)) {
            snprintf(error, error_size, "cannot decode wallpaper image as PNG or JPEG");
            goto done;
        }
    }

    bool animate = app->revision && !unchanged;
    bool regenerate = config->colors.enabled &&
                      (!unchanged || !same_color_settings(&config->colors, &app->config.colors));
    struct output *output;
    struct geometry geometry;
    if (unchanged) {
        wl_list_for_each(output, &app->outputs, link) {
            if (ready_output(output, &geometry) &&
                (output->committed_revision != app->revision ||
                 !same_geometry(output->committed, geometry))) {
                unchanged = false;
                break;
            }
        }
    }
    if (!unchanged) {
        if (!solid && !image.pixels && !decode_image(snapshot, &image, config->path)) {
            snprintf(error, error_size, "cannot decode wallpaper image as PNG or JPEG");
            goto done;
        }
        if (solid) {
            free(image.pixels);
            image = (struct image){0};
        }
        wl_list_for_each(output, &app->outputs, link) {
            if (!ready_output(output, &geometry))
                continue;
            output->prepared = prepare_frame(output, geometry, config, &image);
            if (!output->prepared) {
                snprintf(error, error_size, "cannot render output %u: %s",
                         output->name, strerror(errno));
                goto done;
            }
        }
    }
    if (!regenerate) {
        free(image.pixels);
        image.pixels = NULL;
    }
    /* Queue surface commits only after every frame and the state file are ready. */
    int wayland_error = wl_display_get_error(app->display);
    if (wayland_error) {
        snprintf(error, error_size, "Wayland connection failed: %s", strerror(wayland_error));
        goto done;
    }
    if (save && !save_state(app->state_dir, config)) {
        snprintf(error, error_size, "cannot save wallpaper state: %s", strerror(errno));
        goto done;
    }

    app->palette_pending = false;
    if (regenerate) {
        /* Geometry-only updates never enter this path. Palette settings may change
         * without requiring replacement buffers, in which case the snapshot is decoded here. */
        if (solid || image.pixels || decode_image(snapshot, &image, config->path)) {
            if (generate_palette(solid ? NULL : &image, config->background,
                                 &config->colors, &app->pending_palette)) {
                app->palette_pending = true;
            } else {
                fprintf(stderr, "wall: cannot generate wallpaper colors: %s\n", strerror(errno));
            }
        }
    }
    free(image.pixels);
    image.pixels = NULL;
    if (!unchanged) {
        free(app->config.path);
        if (app->snapshot)
            fclose(app->snapshot);
        app->config = *config;
        config->path = NULL;
        app->snapshot = solid ? NULL : snapshot;
        if (!solid)
            snapshot = NULL;
        ++app->revision;
        show_prepared_wallpaper(app, animate);
    } else {
        app->config.transition = config->transition;
        app->config.colors = config->colors;
    }
    publish_pending_palette(app);
    success = true;

done:
    if (!success) {
        wl_list_for_each(output, &app->outputs, link) {
            if (output->prepared) {
                destroy_buffer(output->prepared);
                output->prepared = NULL;
            }
        }
    }
    if (snapshot)
        fclose(snapshot);
    free(image.pixels);
    return success;
}

static bool apply_config(struct app *app, bool startup, char *error, size_t error_size)
{
    struct config config = default_config;
    if (!load_config(&config, &app->options, NULL)) {
        snprintf(error, error_size, "invalid configuration; see renderer log");
        free(config.path);
        return false;
    }
    struct config configured = config;
    int selected = 0;
    if (startup && (!app->options.image_path || !app->options.mode)) {
        config.path = configured.path ? strdup(configured.path) : NULL;
        if (configured.path && !config.path) {
            snprintf(error, error_size, "cannot read saved selection: %s", strerror(errno));
            free(configured.path);
            return false;
        }
        selected = read_state(app->state_dir, &config);
        if (selected < 0)
            fprintf(stderr, "wall: ignoring saved selection: %s\n", strerror(errno));
    } else {
        configured.path = NULL;
    }

    bool ok = apply_options(&config, &app->options);
    if (ok)
        ok = apply_wallpaper(app, &config, false, error, error_size);
    else
        snprintf(error, error_size, "invalid wallpaper options; see renderer log");
    free(config.path);
    if (!ok && selected > 0) {
        fprintf(stderr, "wall: saved selection failed, trying configuration: %s\n", error);
        ok = apply_options(&configured, &app->options);
        if (ok)
            ok = apply_wallpaper(app, &configured, false, error, error_size);
        else
            snprintf(error, error_size, "invalid wallpaper options; see renderer log");
    }
    free(configured.path);
    return ok;
}

static void destroy_surface(struct output *output)
{
    if (output->frame_callback)
        wl_callback_destroy(output->frame_callback);
    if (output->fractional_scale)
        wp_fractional_scale_v1_destroy(output->fractional_scale);
    if (output->viewport)
        wp_viewport_destroy(output->viewport);
    if (output->layer)
        zwlr_layer_surface_v1_destroy(output->layer);
    if (output->surface)
        wl_surface_destroy(output->surface);
    output->fractional_scale = NULL;
    output->viewport = NULL;
    output->layer = NULL;
    output->surface = NULL;
    output->frame_callback = NULL;
    output->animation_old = NULL;
    output->animation_final = NULL;
    output->shown = NULL;
    output->prepared = NULL;

    struct buffer *buffer, *next;
    wl_list_for_each_safe(buffer, next, &output->buffers, link)
        destroy_buffer(buffer);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                            uint32_t serial, uint32_t width, uint32_t height)
{
    (void)layer;
    struct output *output = data;
    output->width = width;
    output->height = height;
    output->serial = serial;
    output->configured = true;
    output->configure_pending = true;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
    (void)layer;
    struct output *output = data;
    output->closed = true;
    destroy_surface(output);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void surface_output(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
static void surface_scale(void *data, struct wl_surface *surface, int32_t factor)
{
    (void)surface;
    struct output *output = data;
    if (factor > 0)
        output->preferred_scale = factor;
}

static void surface_transform(void *data, struct wl_surface *surface, uint32_t transform)
{
    (void)data;
    (void)surface;
    (void)transform;
}
#endif

static const struct wl_surface_listener surface_listener = {
    .enter = surface_output,
    .leave = surface_output,
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
    .preferred_buffer_scale = surface_scale,
    .preferred_buffer_transform = surface_transform,
#endif
};

static void fractional_scale(void *data, struct wp_fractional_scale_v1 *proxy, uint32_t scale)
{
    (void)proxy;
    struct output *output = data;
    if (scale)
        output->fractional_units = scale;
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = fractional_scale,
};

static bool create_surface(struct output *output)
{
    struct app *app = output->app;
    output->surface = wl_compositor_create_surface(app->compositor);
    if (!output->surface)
        return false;
    wl_surface_add_listener(output->surface, &surface_listener, output);

    output->layer = zwlr_layer_shell_v1_get_layer_surface(app->shell, output->surface,
        output->proxy, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wall");
    if (!output->layer)
        return false;
    zwlr_layer_surface_v1_add_listener(output->layer, &layer_listener, output);

    struct wl_region *region = wl_compositor_create_region(app->compositor);
    if (!region)
        return false;
    wl_surface_set_input_region(output->surface, region);
    wl_region_destroy(region);

    if (app->viewporter && app->fractional_manager) {
        output->viewport = wp_viewporter_get_viewport(app->viewporter, output->surface);
        output->fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(app->fractional_manager,
                                                               output->surface);
        if (!output->viewport || !output->fractional_scale)
            return false;
        wp_fractional_scale_v1_add_listener(output->fractional_scale, &fractional_listener, output);
    }

    zwlr_layer_surface_v1_set_size(output->layer, 0, 0);
    zwlr_layer_surface_v1_set_anchor(output->layer,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(output->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(output->layer,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(output->surface);
    return true;
}

static void output_done(void *data, struct wl_output *proxy)
{
    (void)proxy;
    struct output *output = data;
    output->state = output->pending_state;
}

static void output_properties(void *data, struct wl_output *proxy,
                              int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
                              int32_t subpixel, const char *make, const char *model, int32_t transform)
{
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    struct output *output = data;
    output->pending_state.transform = transform;
    if (wl_output_get_version(proxy) < WL_OUTPUT_DONE_SINCE_VERSION)
        output_done(data, proxy);
}

static void output_mode(void *data, struct wl_output *proxy, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)refresh;
    struct output *output = data;
    if ((flags & WL_OUTPUT_MODE_CURRENT) && width > 0 && height > 0) {
        output->pending_state.width = (uint32_t)width;
        output->pending_state.height = (uint32_t)height;
        if (wl_output_get_version(proxy) < WL_OUTPUT_DONE_SINCE_VERSION)
            output_done(data, proxy);
    }
}

static void output_scale(void *data, struct wl_output *proxy, int32_t factor)
{
    (void)proxy;
    struct output *output = data;
    if (factor > 0)
        output->pending_state.scale = factor;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_properties,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static bool add_output(struct app *app, uint32_t name, uint32_t version)
{
    struct output *output = calloc(1, sizeof *output);
    if (!output)
        return false;

    output->proxy = wl_registry_bind(app->registry, name, &wl_output_interface,
                                     version < 3 ? version : 3);
    if (!output->proxy) {
        free(output);
        return false;
    }
    output->app = app;
    output->name = name;
    output->pending_state.scale = output->state.scale = 1;
    wl_list_init(&output->buffers);
    wl_list_insert(&app->outputs, &output->link);
    wl_output_add_listener(output->proxy, &output_listener, output);
    return true;
}

static void remove_output(struct output *output)
{
    destroy_surface(output);
    if (wl_output_get_version(output->proxy) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
        wl_output_release(output->proxy);
    else
        wl_output_destroy(output->proxy);
    wl_list_remove(&output->link);
    free(output);
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    struct app *app = data;
    if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!add_output(app, name, version))
            app->registry_failed = true;
    } else if (!app->compositor && version >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION &&
               strcmp(interface, wl_compositor_interface.name) == 0) {
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
        uint32_t maximum = WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION;
#else
        uint32_t maximum = WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION;
#endif
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                            version < maximum ? version : maximum);
    } else if (!app->shm && strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!app->shell && strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        uint32_t maximum = ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION;
        app->shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
                                      version < maximum ? version : maximum);
    } else if (!app->viewporter && strcmp(interface, wp_viewporter_interface.name) == 0) {
        app->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (!app->fractional_manager &&
               strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        app->fractional_manager =
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    }
}

static void registry_removed(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)registry;
    struct app *app = data;
    struct output *output;
    wl_list_for_each(output, &app->outputs, link) {
        if (output->name == name) {
            remove_output(output);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_removed,
};

static bool connect_wayland(struct app *app)
{
    app->display = wl_display_connect(NULL);
    if (!app->display) {
        fprintf(stderr, "wall: connecting to Wayland: %s\n", strerror(errno));
        return false;
    }
    app->registry = wl_display_get_registry(app->display);
    if (!app->registry) {
        fprintf(stderr, "wall: could not create the Wayland registry\n");
        return false;
    }
    wl_registry_add_listener(app->registry, &registry_listener, app);
    if (wl_display_roundtrip(app->display) < 0) {
        fprintf(stderr, "wall: reading the Wayland registry: %s\n", strerror(errno));
        return false;
    }
    if (!app->compositor || !app->shm || !app->shell) {
        fprintf(stderr, "wall: Wayland requires wl_compositor v3, wl_shm and wlr-layer-shell\n");
        return false;
    }
    return true;
}

static void cleanup(struct app *app)
{
    struct output *output, *next;
    wl_list_for_each_safe(output, next, &app->outputs, link)
        remove_output(output);
    if (app->fractional_manager)
        wp_fractional_scale_manager_v1_destroy(app->fractional_manager);
    if (app->viewporter)
        wp_viewporter_destroy(app->viewporter);
    if (app->shell) {
        if (zwlr_layer_shell_v1_get_version(app->shell) >= ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION)
            zwlr_layer_shell_v1_destroy(app->shell);
        else
            wl_proxy_destroy((struct wl_proxy *)app->shell);
    }
    if (app->shm)
        wl_shm_destroy(app->shm);
    if (app->compositor)
        wl_compositor_destroy(app->compositor);
    if (app->registry)
        wl_registry_destroy(app->registry);
    if (app->display)
        wl_display_disconnect(app->display);
    if (app->snapshot)
        fclose(app->snapshot);
    free(app->config.path);
}

static int signal_fd(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
        return -1;
    return signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
}

static int collect_signals(int fd, bool *reload, bool *stop)
{
    struct signalfd_siginfo info;
    for (;;) {
        ssize_t size = read(fd, &info, sizeof info);
        if (size == (ssize_t)sizeof info) {
            if (info.ssi_signo == SIGHUP)
                *reload = true;
            else
                *stop = true;
        } else if (size < 0 && errno == EINTR) {
            continue;
        } else if (size < 0 && errno == EAGAIN) {
            return 0;
        } else {
            if (size >= 0)
                errno = EIO;
            return -1;
        }
    }
}

#define IPC_READ_TIMEOUT 5000
#define IPC_CHANGE_TIMEOUT 15000
#define IPC_REPLY_TIMEOUT 60000
#define INITIAL_OUTPUT_TIMEOUT 1500

static ssize_t receive_packet(int fd, char *packet)
{
    struct iovec buffer = {.iov_base = packet, .iov_len = WALL_PACKET_MAX};
    struct msghdr message = {.msg_iov = &buffer, .msg_iovlen = 1};
    ssize_t length = recvmsg(fd, &message, 0);
    if (length >= 0 && (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        errno = EMSGSIZE;
        return -1;
    }
    return length;
}

static void send_reply(int fd, const char *error, const struct config *config)
{
    char packet[WALL_PACKET_MAX];
    int length;
    if (error)
        length = snprintf(packet, sizeof packet, "ERR%c%s", 0, error);
    else if (config)
        length = snprintf(packet, sizeof packet, "OK%c%s%c%s", 0, mode_names[config->mode],
                          0, config->path ? config->path : "");
    else
        length = snprintf(packet, sizeof packet, "OK");
    if (length >= 0 && (size_t)length < sizeof packet)
        (void)send(fd, packet, (size_t)length + 1, MSG_NOSIGNAL);
    close(fd);
}

static void close_ipc(struct app *app)
{
    if (app->pending.command != WALL_RENDER && app->pending.fd >= 0)
        send_reply(app->pending.fd, "renderer is stopping", NULL);
    for (size_t i = 0; i < WALL_IPC_CLIENTS; ++i) {
        if (app->clients[i].fd >= 0)
            close(app->clients[i].fd);
    }
    if (app->socket_path) {
        struct stat st;
        if (lstat(app->socket_path, &st) == 0 &&
            st.st_dev == app->socket_device && st.st_ino == app->socket_inode)
            unlink(app->socket_path);
        free(app->socket_path);
    }
    if (app->listener >= 0)
        close(app->listener);
    if (app->lock >= 0)
        close(app->lock);
    if (app->stop_client >= 0)
        send_reply(app->stop_client, NULL, NULL);
}

static int accept_clients(struct app *app, int64_t now)
{
    for (size_t accepted = 0; accepted < WALL_IPC_CLIENTS; ++accepted) {
        int fd = accept4(app->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
            return errno == EAGAIN || errno == EINTR || errno == ECONNABORTED ? 0 : -1;
        if (!same_user(fd)) {
            close(fd);
            continue;
        }
        size_t slot = 0;
        while (slot < WALL_IPC_CLIENTS && app->clients[slot].fd >= 0)
            ++slot;
        if (slot == WALL_IPC_CLIENTS) {
            send_reply(fd, "too many connected clients", NULL);
            return 0;
        }
        app->clients[slot] = (struct ipc_client){.fd = fd, .deadline = now + IPC_READ_TIMEOUT};
    }
    return 0;
}

static void queue_change(struct app *app, const struct request *request, int fd, int64_t now)
{
    if (app->pending.command != WALL_RENDER && app->pending.fd >= 0)
        send_reply(app->pending.fd, "wallpaper change superseded by a newer request", NULL);
    app->pending.command = request->command;
    app->pending.fd = fd;
    app->pending.deadline = now + IPC_CHANGE_TIMEOUT;
    app->pending.mode = request->mode;
    if (request->path)
        strcpy(app->pending.path, request->path);
}

static void queue_reload(struct app *app, bool *reload, int64_t now)
{
    if (*reload && app->revision) {
        const struct request request = {.command = WALL_RELOAD};
        queue_change(app, &request, -1, now);
        *reload = false;
    }
}

static void read_client(struct app *app, struct ipc_client *client, int64_t now, bool *stop)
{
    char packet[WALL_PACKET_MAX];
    int fd = client->fd;
    ssize_t length = receive_packet(fd, packet);
    if (length < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    client->fd = -1;
    if (length < 0) {
        if (errno == EMSGSIZE)
            send_reply(fd, "request packet is too large", NULL);
        else
            close(fd);
        return;
    }

    struct request request;
    if (!parse_request(packet, (size_t)length, &request)) {
        send_reply(fd, "invalid WALL1 request or packet length", NULL);
    } else if (request.command == WALL_GET) {
        send_reply(fd, NULL, &app->config);
    } else if (request.command == WALL_STOP) {
        app->stop_client = fd;
        *stop = true;
    } else {
        queue_change(app, &request, fd, now);
    }
}

static void process_change(struct app *app, int64_t now)
{
    struct pending_change *change = &app->pending;
    if (change->command == WALL_RENDER)
        return;

    char error[sizeof ((struct wall_reply *)0)->error] = "";
    if (now >= change->deadline) {
        snprintf(error, sizeof error, "timed out waiting for a compositor buffer release");
    } else if (app->animation.effect != TRANSITION_NONE || !outputs_available(app)) {
        return;
    } else if (change->command == WALL_RELOAD) {
        apply_config(app, false, error, sizeof error);
    } else {
        struct config config = app->config;
        config.path = strdup(change->path);
        if (!config.path) {
            snprintf(error, sizeof error, "cannot allocate wallpaper path: %s", strerror(errno));
        } else {
            if (change->mode)
                parse_mode(change->mode, &config.mode);
            apply_wallpaper(app, &config, true, error, sizeof error);
            free(config.path);
        }
    }
    if (*error)
        fprintf(stderr, "wall: %s\n", error);
    if (change->fd >= 0)
        send_reply(change->fd, *error ? error : NULL, NULL);
    change->command = WALL_RENDER;
    change->fd = -1;
}

enum { POLL_DISPLAY, POLL_SIGNALS, POLL_LISTENER, POLL_CLIENTS };

static int wait_for_events(struct app *app, int signals, int64_t startup_deadline,
                           bool *reload, bool *stop)
{
    struct wl_display *display = app->display;
    struct pollfd fds[POLL_CLIENTS + WALL_IPC_CLIENTS] = {
        [POLL_DISPLAY] = {.fd = wl_display_get_fd(display), .events = POLLIN},
        [POLL_SIGNALS] = {.fd = signals, .events = POLLIN},
        [POLL_LISTENER] = {.fd = app->revision ? app->listener : -1, .events = POLLIN},
    };
    int64_t deadline = app->revision ? 0 : startup_deadline;
    if (app->pending.command != WALL_RENDER)
        deadline = app->pending.deadline;
    int64_t next_frame = animation_deadline(app);
    if (next_frame && (!deadline || next_frame < deadline))
        deadline = next_frame;
    for (size_t i = 0; i < WALL_IPC_CLIENTS; ++i) {
        const struct ipc_client *client = &app->clients[i];
        fds[POLL_CLIENTS + i] = (struct pollfd){.fd = client->fd, .events = POLLIN};
        if (client->fd >= 0 && (!deadline || client->deadline < deadline))
            deadline = client->deadline;
    }
    int64_t now = monotonic_ms();
    if (now < 0)
        return -1;
    int timeout = !deadline ? -1 : deadline > now ? (int)(deadline - now) : 0;
    if (wl_display_prepare_read(display) < 0)
        return 0;
    if (wl_display_flush(display) < 0) {
        if (errno != EAGAIN) {
            int error = errno;
            wl_display_cancel_read(display);
            errno = error;
            return -1;
        }
        fds[POLL_DISPLAY].events |= POLLOUT;
    }

    if (poll(fds, sizeof fds / sizeof fds[0], timeout) < 0) {
        int error = errno;
        wl_display_cancel_read(display);
        errno = error;
        return error == EINTR ? 0 : -1;
    }
    if (fds[POLL_DISPLAY].revents & POLLIN) {
        if (wl_display_read_events(display) < 0)
            return -1;
    } else {
        wl_display_cancel_read(display);
    }
    if ((fds[POLL_DISPLAY].revents | fds[POLL_SIGNALS].revents |
         fds[POLL_LISTENER].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
        errno = EIO;
        return -1;
    }
    if ((fds[POLL_SIGNALS].revents & POLLIN) && collect_signals(signals, reload, stop) < 0)
        return -1;
    now = monotonic_ms();
    if (now < 0)
        return -1;
    if (!*stop)
        queue_reload(app, reload, now);
    for (size_t i = 0; i < WALL_IPC_CLIENTS && !*stop; ++i) {
        struct ipc_client *client = &app->clients[i];
        if (client->fd < 0)
            continue;
        short events = fds[POLL_CLIENTS + i].revents;
        if (now >= client->deadline) {
            send_reply(client->fd, "timed out waiting for a request", NULL);
            client->fd = -1;
        } else if (events & POLLIN) {
            read_client(app, client, now, stop);
        } else if (events & (POLLERR | POLLHUP | POLLNVAL)) {
            close(client->fd);
            client->fd = -1;
        }
    }
    if (!*stop && (fds[POLL_LISTENER].revents & POLLIN))
        return accept_clients(app, now);
    return 0;
}

static bool initial_outputs_ready(struct app *app)
{
    struct output *output;
    struct geometry geometry;
    wl_list_for_each(output, &app->outputs, link) {
        if (!output->closed && !ready_output(output, &geometry))
            return false;
    }
    return true;
}

static int run(struct app *app, int signals)
{
    bool reload = false, stop = false;
    int64_t now = monotonic_ms();
    if (now < 0)
        return -1;
    int64_t startup_deadline = now + INITIAL_OUTPUT_TIMEOUT;
    while (!stop) {
        if (wl_display_dispatch_pending(app->display) < 0)
            return -1;
        if (app->registry_failed) {
            errno = ENOMEM;
            return -1;
        }

        struct output *output;
        wl_list_for_each(output, &app->outputs, link) {
            if (!output->surface && !output->closed && !create_surface(output)) {
                errno = ENOMEM;
                return -1;
            }
        }
        now = monotonic_ms();
        if (now < 0)
            return -1;

        /* Collect the initial output configurations before decoding the wallpaper. */
        if (!app->revision && (initial_outputs_ready(app) || now >= startup_deadline)) {
            char error[192];
            if (!apply_config(app, true, error, sizeof error)) {
                fprintf(stderr, "wall: %s; using the default background\n", error);
                ++app->revision;
            }
        }
        if (app->revision) {
            queue_reload(app, &reload, now);
            if (app->animation.effect != TRANSITION_NONE) {
                int64_t time = monotonic_ns();
                if (time < 0)
                    return -1;
                advance_animation(app, time);
            }
            if (app->palette_pending && app->animation.effect == TRANSITION_NONE) {
                render_outputs(app);
                publish_pending_palette(app);
            }
            process_change(app, now);
            if (app->pending.command == WALL_RENDER || app->animation.effect != TRANSITION_NONE)
                render_outputs(app);
        }
        if (wait_for_events(app, signals, startup_deadline, &reload, &stop) < 0)
            return -1;
    }
    return 0;
}

static int parse_reply(const char *packet, size_t length, enum wall_command command,
                       struct wall_reply *reply)
{
    const char *cursor = packet, *end = packet + length;
    const char *status = next_field(&cursor, end);
    if (status && strcmp(status, "ERR") == 0) {
        const char *message = next_field(&cursor, end);
        if (message && *message && strlen(message) < sizeof reply->error && cursor == end) {
            strcpy(reply->error, message);
            return 1;
        }
    } else if (status && strcmp(status, "OK") == 0) {
        if (command == WALL_GET) {
            const char *mode = next_field(&cursor, end);
            const char *path = next_field(&cursor, end);
            if (!mode || !parse_mode(mode, &reply->mode) || !path ||
                (*path && path[0] != '/') || strlen(path) > WALL_PATH_MAX) {
                errno = EPROTO;
                return -1;
            }
            strcpy(reply->path, path);
        }
        if (cursor == end)
            return 0;
    }
    errno = EPROTO;
    return -1;
}

static int ipc_request_display(const char *display, enum wall_command command,
                               const char *path, const char *mode, struct wall_reply *reply)
{
    char packet[WALL_PACKET_MAX];
    size_t length = encode_request(packet, command, mode, path);
    if (!length || !reply) {
        errno = EINVAL;
        return -1;
    }
    *reply = (struct wall_reply){0};
    char *endpoint = ipc_socket_path(display);
    if (!endpoint)
        return -1;

    struct sockaddr_un address;
    socklen_t size;
    bool named = unix_address(&address, endpoint, &size);
    free(endpoint);
    if (!named)
        return -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    int result = -1;
    if (connect(fd, (const struct sockaddr *)&address, size) == 0 && same_user(fd)) {
        ssize_t sent = send(fd, packet, length, MSG_NOSIGNAL);
        if (sent == (ssize_t)length) {
            struct pollfd event = {.fd = fd, .events = POLLIN};
            int ready = poll(&event, 1, IPC_REPLY_TIMEOUT);
            if (ready > 0) {
                ssize_t received = receive_packet(fd, packet);
                if (received > 0)
                    result = parse_reply(packet, (size_t)received, command, reply);
                else if (received == 0)
                    errno = ECONNRESET;
            } else if (ready == 0) {
                errno = ETIMEDOUT;
            }
        } else if (sent >= 0) {
            errno = EIO;
        }
    }
    int error = errno;
    close(fd);
    errno = error;
    return result;
}

int wall_ipc_request(enum wall_command command, const char *path, const char *mode,
                     struct wall_reply *reply)
{
    char *display = display_socket();
    if (!display)
        return -1;
    int result = ipc_request_display(display, command, path, mode, reply);
    int error = errno;
    free(display);
    errno = error;
    return result;
}

static int browser_run(const struct options *options);

static int print_palette(const struct palette *palette)
{
    char text[PALETTE_TEXT_SIZE];
    size_t length = format_palette(palette, text);
    if (!length || fwrite(text, 1, length, stdout) != length || fflush(stdout) == EOF) {
        fprintf(stderr, "wall: cannot print wallpaper colors: %s\n",
                strerror(length ? errno : EOVERFLOW));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int saved_palette_command(void)
{
    char *path = theme_palette_path();
    if (!path) {
        fprintf(stderr, "wall: cannot resolve palette path: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    int result = EXIT_FAILURE;
    struct stat st;
    int fd = open_regular(path, &st);
    if (fd < 0) {
        if (errno == ENOENT)
            fprintf(stderr, "wall: no generated palette at %s\n", path);
        else
            fprintf(stderr, "wall: %s: %s\n", path, strerror(errno));
    } else {
        FILE *file = fdopen(fd, "rb");
        if (!file) {
            fprintf(stderr, "wall: %s: %s\n", path, strerror(errno));
            close(fd);
        } else {
            struct palette palette;
            bool valid = parse_theme_palette(file, palette.rgb);
            fclose(file);
            if (valid)
                result = print_palette(&palette);
            else
                fprintf(stderr, "wall: %s: invalid generated palette\n", path);
        }
    }
    free(path);
    return result;
}

static int generate_colors_command(const char *argument, const struct options *options)
{
    struct config config = default_config;
    bool configured = load_config(&config, options, NULL);
    free(config.path);
    if (!configured)
        return EXIT_FAILURE;

    char *path = *argument ? absolute_path(argument) : NULL;
    if (!path) {
        fprintf(stderr, "wall: wallpaper path: %s\n", *argument ? strerror(errno) : "empty path");
        return EXIT_FAILURE;
    }
    struct stat st;
    int fd = open_regular(path, &st);
    int result = EXIT_FAILURE;
    if (fd < 0) {
        fprintf(stderr, "wall: %s: %s\n", path, strerror(errno));
    } else if (st.st_size > (off_t)MAX_SNAPSHOT_BYTES) {
        fprintf(stderr, "wall: %s: image file exceeds the size limit\n", path);
        close(fd);
    } else {
        FILE *file = fdopen(fd, "rb");
        if (!file) {
            fprintf(stderr, "wall: %s: %s\n", path, strerror(errno));
            close(fd);
        } else {
            struct image image = {0};
            bool decoded = decode_image(file, &image, path);
            fclose(file);
            if (decoded) {
                struct palette palette;
                if (generate_palette(&image, config.background, &config.colors, &palette))
                    result = print_palette(&palette);
                else
                    fprintf(stderr, "wall: cannot generate wallpaper colors: %s\n", strerror(errno));
            }
            free(image.pixels);
        }
    }
    free(path);
    return result;
}

static void usage(FILE *stream)
{
    fputs("Usage: wall [-c PATH] [-i PATH] [-m MODE] [-h]\n"
          "       wall --set PATH [--mode MODE]\n"
          "       wall --get | --reload | --stop\n"
          "       wall --browse [-c PATH]\n"
          "       wall --colors\n"
          "       wall --generate-colors PATH [-c PATH]\n"
          "  -c PATH  Use an alternative configuration file\n"
          "  -i PATH  Override the wallpaper image\n"
          "  -m MODE  Override the mode (fill, fit, stretch, center, solid)\n"
          "  -h       Show this help\n", stream);
}

static int control(enum wall_command command, const char *path, const char *mode)
{
    char *absolute = NULL;
    if (command == WALL_SET) {
        absolute = *path ? absolute_path(path) : NULL;
        if (!absolute) {
            fprintf(stderr, "wall: wallpaper path: %s\n", *path ? strerror(errno) : "empty path");
            return EXIT_FAILURE;
        }
    }
    struct wall_reply reply;
    int result = wall_ipc_request(command, absolute, mode, &reply);
    int error = errno;
    free(absolute);
    if (result == 1) {
        fprintf(stderr, "wall: %s\n", reply.error);
        return EXIT_FAILURE;
    }
    if (result < 0) {
        if (error == ENOENT || error == ECONNREFUSED)
            fprintf(stderr, "wall: no renderer is running in this Wayland session\n");
        else
            fprintf(stderr, "wall: cannot communicate with renderer: %s\n", strerror(error));
        return EXIT_FAILURE;
    }
    if (command == WALL_GET)
        printf("path = %s\nmode = %s\n", reply.path, mode_names[reply.mode]);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    enum { OPT_BROWSE = 1000, OPT_COLORS, OPT_GENERATE_COLORS };
    static const struct option long_options[] = {
        {"set", required_argument, NULL, WALL_SET},
        {"get", no_argument, NULL, WALL_GET},
        {"reload", no_argument, NULL, WALL_RELOAD},
        {"stop", no_argument, NULL, WALL_STOP},
        {"browse", no_argument, NULL, OPT_BROWSE},
        {"colors", no_argument, NULL, OPT_COLORS},
        {"generate-colors", required_argument, NULL, OPT_GENERATE_COLORS},
        {"mode", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    struct app app = {
        .config = default_config, .listener = -1, .lock = -1, .stop_client = -1,
    };
    wl_list_init(&app.outputs);
    for (size_t i = 0; i < WALL_IPC_CLIENTS; ++i)
        app.clients[i].fd = -1;

    enum wall_command command = WALL_RENDER;
    int local_command = 0;
    const char *path = NULL;
    const char *palette_path = NULL;
    int option;
    while ((option = getopt_long(argc, argv, "c:i:m:h", long_options, NULL)) != -1) {
        switch (option) {
        case OPT_BROWSE:
        case OPT_COLORS:
        case OPT_GENERATE_COLORS:
            if (local_command) {
                fprintf(stderr, "wall: specify one command\n");
                return EXIT_FAILURE;
            }
            local_command = option;
            if (option == OPT_GENERATE_COLORS)
                palette_path = optarg;
            break;
        case WALL_SET:
        case WALL_GET:
        case WALL_RELOAD:
        case WALL_STOP:
            if (command != WALL_RENDER) {
                fprintf(stderr, "wall: specify one control command\n");
                return EXIT_FAILURE;
            }
            command = (enum wall_command)option;
            if (command == WALL_SET)
                path = optarg;
            break;
        case 'c':
            app.options.config_path = optarg;
            break;
        case 'i':
            app.options.image_path = optarg;
            break;
        case 'm':
            app.options.mode = optarg;
            break;
        case 'h':
            usage(stdout);
            return EXIT_SUCCESS;
        default:
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    enum mode mode;
    if (app.options.mode && !parse_mode(app.options.mode, &mode)) {
        fprintf(stderr, "wall: invalid mode: %s\n", app.options.mode);
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (optind < argc) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (local_command) {
        if (command != WALL_RENDER || app.options.image_path || app.options.mode ||
            (local_command == OPT_COLORS && app.options.config_path)) {
            usage(stderr);
            return EXIT_FAILURE;
        }
        if (local_command == OPT_BROWSE)
            return browser_run(&app.options);
        if (local_command == OPT_COLORS)
            return saved_palette_command();
        return generate_colors_command(palette_path, &app.options);
    }
    if (command != WALL_RENDER) {
        if (app.options.config_path || app.options.image_path ||
            (command != WALL_SET && app.options.mode)) {
            usage(stderr);
            return EXIT_FAILURE;
        }
        return control(command, path, app.options.mode);
    }

    int signals = signal_fd();
    if (signals < 0) {
        fprintf(stderr, "wall: setting up signal notifications: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    int status = EXIT_FAILURE;
    char *display = display_socket();
    if (!display) {
        fprintf(stderr, "wall: finding Wayland session: %s\n", strerror(errno));
    } else if (!start_ipc(&app, display)) {
        if (errno == EADDRINUSE)
            fprintf(stderr, "wall: a renderer is already running in this Wayland session\n");
        else
            fprintf(stderr, "wall: cannot create control socket: %s\n", strerror(errno));
    } else if (connect_wayland(&app)) {
        app.state_dir = state_directory();
        if (run(&app, signals) == 0)
            status = EXIT_SUCCESS;
        else
            fprintf(stderr, "wall: event loop: %s\n", strerror(errno));
    }
    cleanup(&app);
    close_ipc(&app);
    free(app.state_dir);
    free(display);
    close(signals);
    return status;
}

/* The browser uses the same binary as the renderer and owns its Wayland connection. */
#define BROWSER_MAX_SIZE 4096
#define CATALOG_BYTES (32u * 1024 * 1024)
#define CATALOG_LIMIT 100000u
#define DIRECTORY_BYTES (8u * 1024 * 1024)
#define THUMBNAIL_BYTES (40u * 1024 * 1024)
#define THUMBNAIL_SLOTS 128
#define DISK_SLOTS 1024
#define BROWSER_PADDING 12
#define BROWSER_GAP 8

enum browser_task { TASK_SCAN, TASK_THUMBNAIL, TASK_IPC, TASK_COUNT };

struct browser_job {
    pid_t pid;
    int notify, result;
};

struct catalog_header {
    uint32_t count, skipped, limited, reserved;
};

struct catalog_entry {
    uint32_t offset, relative, name;
    uint64_t device, inode;
};

struct thumbnail_size {
    uint32_t width, height;
};

struct thumbnail {
    uint32_t entry;
    uint64_t used;
    struct thumbnail_size size;
    uint32_t *pixels;
    int watch;
};

struct browser_layout {
    int columns, column_space, thumb_height, row_height, top, bottom, max_scroll;
};

struct theme_watch_entry {
    int wd;
    size_t offset, length;
    bool leaf;
};

struct theme_watch {
    int fd;
    char *path;
    struct theme_watch_entry *entries;
    size_t count;
};

static void disarm_theme_watch(struct theme_watch *watch)
{
    if (watch->fd >= 0)
        close(watch->fd);
    free(watch->entries);
    watch->entries = NULL;
    watch->count = 0;
    watch->fd = -1;
}

static void close_theme_watch(struct theme_watch *watch)
{
    disarm_theme_watch(watch);
    free(watch->path);
    watch->path = NULL;
}

static void arm_theme_watch(struct theme_watch *watch)
{
    disarm_theme_watch(watch);
    if (!watch->path)
        return;
    char *prefix = strdup(watch->path);
    if (!prefix)
        return;

    size_t capacity = 1;
    for (const char *slash = prefix; (slash = strchr(slash, '/')); ++slash)
        ++capacity;
    watch->entries = calloc(capacity, sizeof *watch->entries);
    if (watch->entries)
        watch->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (watch->fd < 0)
        goto done;

    /* Watch each existing parent so a missing or replaced directory can be found. */
    const uint32_t changes = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                             IN_MOVE_SELF | IN_DELETE_SELF | IN_ONLYDIR | IN_MASK_ADD;
    char *name = prefix + 1;
    while (*name) {
        name += strspn(name, "/");
        if (!*name)
            break;
        const char *end = strchr(name, '/');
        char *cut = name == prefix + 1 ? name : name - 1;
        char saved = *cut;
        size_t length = end ? (size_t)(end - name) : strlen(name);
        *cut = '\0';
        int wd = inotify_add_watch(watch->fd, prefix, changes | (end ? 0 : IN_CLOSE_WRITE));
        *cut = saved;
        if (wd < 0)
            break;
        watch->entries[watch->count++] = (struct theme_watch_entry){
            .wd = wd, .offset = (size_t)(name - prefix), .length = length, .leaf = !end,
        };
        if (!end)
            break;
        name = (char *)end + 1;
    }

done:
    free(prefix);
    if (!watch->count)
        disarm_theme_watch(watch);
}

static bool theme_watch_changed(struct theme_watch *watch, short events)
{
    if (events & (POLLERR | POLLHUP | POLLNVAL)) {
        arm_theme_watch(watch);
        return true;
    }
    if (!(events & POLLIN))
        return false;

    char bytes[8192];
    ssize_t used = read(watch->fd, bytes, sizeof bytes);
    if (used < 0 && (errno == EAGAIN || errno == EINTR))
        return false;
    bool changed = false, rebuild = used <= 0;
    for (size_t offset = 0; used > 0 && offset + sizeof(struct inotify_event) <= (size_t)used;) {
        struct inotify_event event;
        memcpy(&event, bytes + offset, sizeof event);
        offset += sizeof event;
        if (event.len > (size_t)used - offset)
            break;
        const char *name = bytes + offset;
        offset += event.len;

        if (event.mask & (IN_Q_OVERFLOW | IN_IGNORED | IN_MOVE_SELF | IN_DELETE_SELF | IN_UNMOUNT)) {
            rebuild = true;
            break;
        }
        if (!event.len)
            continue;
        size_t length = strnlen(name, event.len);
        for (size_t i = 0; i < watch->count; ++i) {
            const struct theme_watch_entry *entry = &watch->entries[i];
            if (entry->wd != event.wd || entry->length != length ||
                memcmp(name, watch->path + entry->offset, length))
                continue;
            changed = true;
            if (!entry->leaf)
                rebuild = true;
        }
    }
    if (rebuild)
        arm_theme_watch(watch);
    return changed || rebuild;
}

struct browser {
    struct browser_config config;
    struct theme_watch theme_watch;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *shell;
    struct wl_surface *surface, *cursor_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_seat *seat;
    uint32_t seat_name;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_cursor_theme *cursor_theme;
    struct wl_cursor *cursor;
    struct xkb_context *xkb;
    struct xkb_state *keys;
    FT_Library freetype;
    FT_Face face;
    struct buffer *frames[2];
    struct wl_callback *frame_callback;

    struct browser_job jobs[TASK_COUNT];
    int signals, notify_files, session_lock;
    bool configured, dirty, done, editing;
    int width, height, requested_width, requested_height;
    struct browser_layout layout;
    int scroll;
    double pointer_x, pointer_y, wheel;
    size_t last_clicked;
    uint32_t last_click_time;

    void *catalog;
    size_t catalog_size;
    const struct catalog_entry *entries;
    size_t count, match_count, selected;
    uint32_t *matches;
    uint32_t scan_skipped;
    bool scan_limited;

    char query[256];
    char status[256];
    char *cache_directory, *current_path, *requested_path;
    char *display_name;
    int session_error;
    uint64_t current_device, current_inode;
    struct thumbnail thumbnails[THUMBNAIL_SLOTS];
    size_t thumb_bytes;
    uint64_t use_clock;
    uint64_t catalog_generation, thumbnail_generation;
    uint32_t thumbnail_entry;
    enum wall_command ipc_command;
    const char *error;
};

static bool write_bytes(int fd, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    while (size) {
        ssize_t written = write(fd, bytes, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        bytes += written;
        size -= (size_t)written;
    }
    return true;
}

static bool read_bytes(int fd, void *data, size_t size)
{
    unsigned char *bytes = data;
    while (size) {
        ssize_t length = read(fd, bytes, size);
        if (length < 0 && errno == EINTR)
            continue;
        if (length <= 0)
            return false;
        bytes += length;
        size -= (size_t)length;
    }
    return true;
}

static bool start_job(struct browser *browser, enum browser_task task,
                      bool (*work)(const void *, int), const void *argument)
{
    int result = memfd_create("wall-browser-job", MFD_CLOEXEC);
    int pipe_fds[2];
    if (result < 0)
        return false;
    if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
        close(result);
        return false;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pipe_fds[0]);
        close(wl_display_get_fd(browser->display));
        close(browser->signals);
        if (browser->session_lock >= 0)
            close(browser->session_lock);
        if (browser->theme_watch.fd >= 0)
            close(browser->theme_watch.fd);
        if (task != TASK_THUMBNAIL && browser->notify_files >= 0)
            close(browser->notify_files);
        for (size_t i = 0; i < TASK_COUNT; ++i) {
            if (browser->jobs[i].pid) {
                close(browser->jobs[i].notify);
                close(browser->jobs[i].result);
            }
        }
        sigset_t mask;
        sigemptyset(&mask);
        sigprocmask(SIG_SETMASK, &mask, NULL);
        /* The write end stays open until the result is complete. */
        _exit(work(argument, result) ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    close(pipe_fds[1]);
    if (pid < 0) {
        close(pipe_fds[0]);
        close(result);
        return false;
    }
    browser->jobs[task] = (struct browser_job){.pid = pid, .notify = pipe_fds[0], .result = result};
    return true;
}

static int finish_job(struct browser_job *job, bool cancel)
{
    if (!job->pid)
        return -1;
    if (cancel)
        kill(job->pid, SIGKILL);
    int status;
    pid_t result;
    do {
        result = waitpid(job->pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    close(job->notify);
    int fd = job->result;
    *job = (struct browser_job){0};
    if (cancel || result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS ||
        lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct scan_item {
    char *path;
    uint32_t relative;
    dev_t device;
    ino_t inode;
};

struct scan_directory {
    char *path;
    uint32_t relative;
};

struct scan {
    struct scan_item *items;
    size_t count, capacity, bytes;
    struct scan_directory *pending;
    size_t pending_count, pending_capacity, pending_bytes;
    uint32_t skipped;
    bool limited;
};

static const char *filename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void *grow_array(void *array, size_t *capacity, size_t count, size_t element)
{
    if (count < *capacity)
        return array;
    size_t next = *capacity ? *capacity * 2 : 64;
    if (next <= count || next > SIZE_MAX / element)
        return NULL;
    void *expanded = realloc(array, next * element);
    if (expanded)
        *capacity = next;
    return expanded;
}

static bool queue_directory(struct scan *scan, char *path, uint32_t relative)
{
    size_t bytes = strlen(path) + 1 + sizeof *scan->pending;
    if (bytes > DIRECTORY_BYTES - scan->pending_bytes)
        return false;
    struct scan_directory *expanded = grow_array(scan->pending, &scan->pending_capacity,
                                                  scan->pending_count, sizeof *scan->pending);
    if (!expanded)
        return false;
    scan->pending = expanded;
    scan->pending[scan->pending_count++] = (struct scan_directory){path, relative};
    scan->pending_bytes += bytes;
    return true;
}

static bool supported_file(int directory, const char *name, struct stat *st)
{
    int fd = openat(directory, name, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    unsigned char signature[8];
    ssize_t length = 0;
    if (fstat(fd, st) == 0 && S_ISREG(st->st_mode))
        length = read(fd, signature, sizeof signature);
    close(fd);
    return (length == 8 && png_sig_cmp(signature, 0, sizeof signature) == 0) ||
           (length >= 2 && signature[0] == 0xff && signature[1] == 0xd8);
}

static void scan_directory(struct scan *scan, const struct scan_directory *directory, bool recursive)
{
    int fd = open(directory->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *stream = fd < 0 ? NULL : fdopendir(fd);
    if (!stream) {
        if (fd >= 0)
            close(fd);
        ++scan->skipped;
        return;
    }
    const char *separator = !strcmp(directory->path, "/") ? "" : "/";
    while (!scan->limited) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) {
            if (errno)
                ++scan->skipped;
            break;
        }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;

        struct stat st;
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
            continue;
        bool child = S_ISDIR(st.st_mode);
        if (S_ISLNK(st.st_mode) && fstatat(fd, name, &st, 0) < 0)
            continue;
        if (child ? !recursive :
            (!S_ISREG(st.st_mode) || !supported_file(fd, name, &st)))
            continue;
        char *path;
        if (asprintf(&path, "%s%s%s", directory->path, separator, name) < 0) {
            scan->limited = true;
            break;
        }
        size_t length = strlen(path);
        if (length > WALL_PATH_MAX) {
            ++scan->skipped;
            free(path);
            continue;
        }
        if (child) {
            if (!queue_directory(scan, path, directory->relative)) {
                scan->limited = true;
                free(path);
            }
        } else {
            size_t bytes = length + 1 + sizeof(struct catalog_entry);
            struct scan_item *expanded = NULL;
            if (bytes <= CATALOG_BYTES - scan->bytes && scan->count < CATALOG_LIMIT)
                expanded = grow_array(scan->items, &scan->capacity, scan->count, sizeof *scan->items);
            if (!expanded) {
                scan->limited = true;
                free(path);
            } else {
                scan->items = expanded;
                scan->items[scan->count++] = (struct scan_item){path, directory->relative, st.st_dev, st.st_ino};
                scan->bytes += bytes;
            }
        }
    }
    closedir(stream);
}

static int compare_identity(const void *left, const void *right)
{
    const struct scan_item *a = left, *b = right;
    if (a->device != b->device)
        return a->device < b->device ? -1 : 1;
    if (a->inode != b->inode)
        return a->inode < b->inode ? -1 : 1;
    int path = strcmp(a->path, b->path);
    if (path)
        return path;
    return (a->relative > b->relative) - (a->relative < b->relative);
}

static int compare_filename(const void *left, const void *right)
{
    const struct scan_item *a = left, *b = right;
    int name = strcmp(filename(a->path), filename(b->path));
    return name ? name : strcmp(a->path, b->path);
}

static bool publish_scan(struct scan *scan, int fd)
{
    if (scan->count > 1)
        qsort(scan->items, scan->count, sizeof *scan->items, compare_identity);
    size_t unique = 0;
    for (size_t i = 0; i < scan->count; ++i) {
        struct scan_item *item = &scan->items[i];
        if (unique && scan->items[unique - 1].device == item->device &&
            scan->items[unique - 1].inode == item->inode)
            free(item->path);
        else
            scan->items[unique++] = *item;
    }
    scan->count = unique;
    if (unique > 1)
        qsort(scan->items, unique, sizeof *scan->items, compare_filename);

    struct catalog_header header = {(uint32_t)unique, scan->skipped, scan->limited, 0};
    if (!write_bytes(fd, &header, sizeof header))
        return false;
    size_t offset = sizeof header + unique * sizeof(struct catalog_entry);
    for (size_t i = 0; i < unique; ++i) {
        const char *path = scan->items[i].path;
        struct catalog_entry entry = {
            .offset = (uint32_t)offset, .relative = scan->items[i].relative,
            .name = (uint32_t)(filename(path) - path),
            .device = (uint64_t)scan->items[i].device, .inode = (uint64_t)scan->items[i].inode,
        };
        if (!write_bytes(fd, &entry, sizeof entry))
            return false;
        offset += strlen(path) + 1;
    }
    for (size_t i = 0; i < unique; ++i) {
        const char *path = scan->items[i].path;
        if (!write_bytes(fd, path, strlen(path) + 1))
            return false;
    }
    return true;
}

static bool discover_images(const void *argument, int fd)
{
    const struct browser_config *config = argument;
    struct scan scan = {0};
    char *list = strdup(config->directories ? config->directories : "~/Pictures/Wallpapers");
    if (!list)
        return false;
    char *cursor = list, *root;
    while (!scan.limited && (root = strsep(&cursor, ","))) {
        root = trim(root);
        if (!*root)
            continue;
        char *expanded = absolute_path(root);
        char *canonical = expanded ? realpath(expanded, NULL) : NULL;
        free(expanded);
        if (!canonical) {
            ++scan.skipped;
            continue;
        }
        size_t length = strlen(canonical);
        uint32_t relative = (uint32_t)(length + (length != 1));
        if (length > WALL_PATH_MAX) {
            ++scan.skipped;
            free(canonical);
        } else if (!queue_directory(&scan, canonical, relative)) {
            scan.limited = true;
            free(canonical);
        }
    }
    free(list);
    while (scan.pending_count && !scan.limited) {
        struct scan_directory directory = scan.pending[--scan.pending_count];
        scan.pending_bytes -= strlen(directory.path) + 1 + sizeof directory;
        scan_directory(&scan, &directory, config->recursive);
        free(directory.path);
    }
    bool ok = publish_scan(&scan, fd);
    for (size_t i = 0; i < scan.pending_count; ++i)
        free(scan.pending[i].path);
    for (size_t i = 0; i < scan.count; ++i)
        free(scan.items[i].path);
    free(scan.items);
    free(scan.pending);
    return ok;
}

struct thumbnail_request {
    const char *path, *cache_directory;
    int width, height, notify_files;
};

struct thumbnail_result {
    struct thumbnail_size size;
    int watch;
};

static char *thumbnail_directory(void)
{
    char *base = wall_cache_directory();
    char *directory = NULL;
    if (base && asprintf(&directory, "%s/thumbnails", base) < 0)
        directory = NULL;
    free(base);
    return directory;
}

enum { CACHE_KEY_LENGTH = 11 };

static void thumbnail_key(uint64_t key[CACHE_KEY_LENGTH], const struct thumbnail_request *request,
                          const struct stat *st)
{
    const uint64_t fields[CACHE_KEY_LENGTH] = {
        UINT64_C(0x57414c4c54485532), hash_path(request->path), (uint64_t)st->st_dev,
        (uint64_t)st->st_ino, (uint64_t)st->st_size, (uint64_t)st->st_mtim.tv_sec,
        (uint64_t)st->st_mtim.tv_nsec, (uint64_t)st->st_ctim.tv_sec, (uint64_t)st->st_ctim.tv_nsec,
        (uint64_t)request->width, (uint64_t)request->height,
    };
    memcpy(key, fields, sizeof fields);
}

static size_t thumbnail_bytes(struct thumbnail_size size)
{
    if (!size.width || !size.height || size.width > 512 || size.height > 512)
        return 0;
    return (size_t)size.width * size.height * sizeof(uint32_t);
}

static uint32_t *read_thumbnail(const char *path, const uint64_t key[CACHE_KEY_LENGTH],
                                 struct thumbnail_size *size)
{
    if (!path)
        return NULL;
    struct stat st;
    int fd = open_regular(path, &st);
    if (fd < 0)
        return NULL;
    uint64_t cached[CACHE_KEY_LENGTH];
    uint32_t *pixels = NULL;
    if (read_bytes(fd, cached, sizeof cached) && !memcmp(cached, key, sizeof cached) &&
        read_bytes(fd, size, sizeof *size)) {
        size_t bytes = thumbnail_bytes(*size);
        if (bytes && size->width <= key[9] && size->height <= key[10] &&
            st.st_size == (off_t)(sizeof cached + sizeof *size + bytes)) {
            pixels = malloc(bytes);
            if (pixels && !read_bytes(fd, pixels, bytes)) {
                free(pixels);
                pixels = NULL;
            }
        }
    }
    close(fd);
    return pixels;
}

static char *thumbnail_temporary(const char *directory, pid_t pid)
{
    char *name;
    if (!directory || asprintf(&name, "%s/.thumb-%ld", directory, (long)pid) < 0)
        return NULL;
    return name;
}

static void save_thumbnail(const char *directory, const char *path,
                           const uint64_t key[CACHE_KEY_LENGTH],
                           struct thumbnail_size size, const uint32_t *pixels)
{
    if (!path || !create_state_directory(directory))
        return;
    char *temporary = thumbnail_temporary(directory, getpid());
    if (!temporary)
        return;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        bool ok = write_bytes(fd, key, CACHE_KEY_LENGTH * sizeof *key) &&
                  write_bytes(fd, &size, sizeof size) &&
                  write_bytes(fd, pixels, thumbnail_bytes(size));
        if (close(fd) == 0 && ok)
            rename(temporary, path);
        unlink(temporary);
    }
    free(temporary);
}

static uint32_t *downsample(const struct thumbnail_request *request, const struct image *source,
                            struct thumbnail_size *size)
{
    double scale = fmin(1, fmin((double)request->width / source->width,
                               (double)request->height / source->height));
    size->width = (uint32_t)fmax(1, floor(source->width * scale + 0.5));
    size->height = (uint32_t)fmax(1, floor(source->height * scale + 0.5));
    size_t bytes = thumbnail_bytes(*size);
    uint32_t *pixels = bytes ? malloc(bytes) : NULL;
    if (!pixels)
        return NULL;
    double sx = (double)source->width / size->width;
    double sy = (double)source->height / size->height;
    for (uint32_t y = 0; y < size->height; ++y) {
        for (uint32_t x = 0; x < size->width; ++x) {
            double rgba[4] = {0};
            interpolate_rgba(source, (x + 0.5) * sx - 0.5, (y + 0.5) * sy - 0.5, rgba);
            uint32_t pixel = (uint32_t)(rgba[3] + 0.5) << 24;
            for (unsigned channel = 0; channel < 3; ++channel)
                pixel |= (uint32_t)(rgba[channel] + 0.5) << ((2 - channel) * 8);
            pixels[(size_t)y * size->width + x] = pixel;
        }
    }
    return pixels;
}

static bool generate_thumbnail(const void *argument, int result_fd)
{
    const struct thumbnail_request *request = argument;
    struct thumbnail_result result = {.watch = -1};
    struct stat st;
    int fd = open_regular(request->path, &st);
    if (fd < 0)
        return write_bytes(result_fd, &result, sizeof result);
    if (request->notify_files >= 0)
        result.watch = inotify_add_watch(request->notify_files, request->path,
                                        IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF);

    uint64_t key[CACHE_KEY_LENGTH];
    thumbnail_key(key, request, &st);
    char *cache_path = NULL;
    if (request->cache_directory &&
        asprintf(&cache_path, "%s/thumb-%04x", request->cache_directory,
                 (unsigned)(key[1] % DISK_SLOTS)) < 0)
        cache_path = NULL;
    uint32_t *pixels = read_thumbnail(cache_path, key, &result.size);
    bool generated = false;
    if (!pixels && st.st_size <= (off_t)MAX_SNAPSHOT_BYTES) {
        FILE *file = fdopen(fd, "rb");
        if (file) {
            fd = -1;
            struct image image = {0};
            if (decode_image(file, &image, request->path)) {
                pixels = downsample(request, &image, &result.size);
                free(image.pixels);
                generated = pixels != NULL;
            }
            fclose(file);
        }
    }
    if (fd >= 0)
        close(fd);
    if (pixels) {
        struct stat current;
        uint64_t actual[CACHE_KEY_LENGTH];
        if (stat(request->path, &current) == 0)
            thumbnail_key(actual, request, &current);
        else
            memset(actual, 0, sizeof actual);
        if (memcmp(actual, key, sizeof key)) {
            free(pixels);
            pixels = NULL;
        } else if (generated) {
            save_thumbnail(request->cache_directory, cache_path, key, result.size, pixels);
        }
    }
    if (!pixels)
        result.size = (struct thumbnail_size){0};
    bool ok = write_bytes(result_fd, &result, sizeof result) &&
              (!pixels || write_bytes(result_fd, pixels, thumbnail_bytes(result.size)));
    free(cache_path);
    free(pixels);
    return ok;
}

struct browser_ipc_request {
    enum wall_command command;
    const char *path, *display;
    int session_error;
};

struct browser_ipc_result {
    int result, error;
    struct wall_reply reply;
    uint64_t device, inode;
};

static bool browser_ipc_work(const void *argument, int fd)
{
    const struct browser_ipc_request *request = argument;
    struct browser_ipc_result result = {0};
    if (request->display) {
        result.result = ipc_request_display(request->display, request->command, request->path, NULL, &result.reply);
        result.error = errno;
    } else {
        result.result = -1;
        result.error = request->session_error;
    }
    if (!result.result) {
        const char *path = request->command == WALL_GET ? result.reply.path : request->path;
        struct stat st;
        if (path && *path && stat(path, &st) == 0) {
            result.device = (uint64_t)st.st_dev;
            result.inode = (uint64_t)st.st_ino;
        }
    }
    return write_bytes(fd, &result, sizeof result);
}

static const char *entry_path(const struct browser *browser, uint32_t index)
{
    return (const char *)browser->catalog + browser->entries[index].offset;
}

static int smaller(int a, int b) { return a < b ? a : b; }
static int larger(int a, int b) { return a > b ? a : b; }

static int column_offset(const struct browser_layout *layout, int column)
{
    return layout->column_space * column / layout->columns + column * BROWSER_GAP;
}

static int column_width(const struct browser_layout *layout, int column)
{
    return column_offset(layout, column + 1) - column_offset(layout, column) - BROWSER_GAP;
}

static void update_layout(struct browser *browser)
{
    const struct browser_config *c = &browser->config;
    int usable = larger(1, browser->width - 2 * BROWSER_PADDING);
    int columns = larger(1, (usable + BROWSER_GAP) / (c->thumbnail_width + BROWSER_GAP));
    if (c->columns)
        columns = smaller(c->columns, columns);
    int column_space = usable - (columns - 1) * BROWSER_GAP;
    int line = c->font_size + 6;
    int thumb_height = larger(1, column_space * c->thumbnail_height / (columns * c->thumbnail_width));
    int row = thumb_height + line + BROWSER_GAP;
    int top = BROWSER_PADDING + 3 * line + 12;
    int bottom = larger(top, browser->height - line - BROWSER_PADDING);
    int rows = (int)((browser->match_count + columns - 1) / columns);
    browser->layout = (struct browser_layout){
        columns, column_space, thumb_height, row, top, bottom,
        larger(0, rows * row - (bottom - top)),
    };
    browser->scroll = smaller(browser->scroll, browser->layout.max_scroll);
}

static void visible_selection(struct browser *browser)
{
    if (!browser->match_count)
        return;
    const struct browser_layout *l = &browser->layout;
    int first = (int)(browser->selected / l->columns) * l->row_height;
    int space = l->bottom - l->top;
    if (first < browser->scroll || l->row_height >= space)
        browser->scroll = first;
    else if (first + l->row_height > browser->scroll + space)
        browser->scroll = first + l->row_height - space;
    browser->scroll = smaller(browser->scroll, l->max_scroll);
}

static size_t fold_text(const char *text, wchar_t *folded, size_t capacity)
{
    mbstate_t state = {0};
    size_t position = 0, left = strlen(text);
    while (left && position + 1 < capacity) {
        wchar_t symbol;
        size_t count = mbrtowc(&symbol, text, left, &state);
        if (count == (size_t)-1 || count == (size_t)-2 || count == 0) {
            memset(&state, 0, sizeof state);
            symbol = (unsigned char)*text;
            count = 1;
        }
        folded[position++] = (wchar_t)towlower((wint_t)symbol);
        text += count;
        left -= count;
    }
    folded[position] = 0;
    return position;
}

static void filter_entries(struct browser *browser, const char *selected_path)
{
    browser->selected = browser->match_count = 0;
    browser->scroll = 0;
    browser->last_clicked = SIZE_MAX;
    wchar_t query[sizeof browser->query];
    size_t query_length = fold_text(browser->query, query, sizeof query / sizeof query[0]);
    for (size_t i = 0; i < browser->count; ++i) {
        const char *path = entry_path(browser, (uint32_t)i);
        const char *relative = path + browser->entries[i].relative;
        bool matches = !query_length || strcasestr(relative, browser->query);
        if (!matches) {
            bool non_ascii = false;
            for (const unsigned char *byte = (const unsigned char *)relative; *byte; ++byte) {
                if (*byte >= 128) {
                    non_ascii = true;
                    break;
                }
            }
            if (non_ascii) {
                wchar_t folded[WALL_PATH_MAX + 1];
                fold_text(relative, folded, sizeof folded / sizeof folded[0]);
                matches = wcsstr(folded, query) != NULL;
            }
        }
        if (matches) {
            if (selected_path && !strcmp(path, selected_path))
                browser->selected = browser->match_count;
            browser->matches[browser->match_count++] = (uint32_t)i;
        }
    }
    update_layout(browser);
    visible_selection(browser);
    browser->dirty = true;
}

static const char *selected_path(const struct browser *browser)
{
    return browser->match_count ? entry_path(browser, browser->matches[browser->selected]) : NULL;
}

static void visible_range(const struct browser *browser, size_t *first, size_t *end)
{
    const struct browser_layout *l = &browser->layout;
    *first = *end = 0;
    int space = l->bottom - l->top;
    if (space <= 0)
        return;
    *first = (size_t)(browser->scroll / l->row_height) * l->columns;
    size_t last = (size_t)((browser->scroll + space - 1) / l->row_height + 1) * l->columns;
    *end = last < browser->match_count ? last : browser->match_count;
}

static bool visible_entry(const struct browser *browser, uint32_t entry)
{
    size_t first, end;
    visible_range(browser, &first, &end);
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        if (browser->matches[middle] == entry)
            return true;
        if (browser->matches[middle] < entry)
            first = middle + 1;
        else
            end = middle;
    }
    return false;
}

static struct thumbnail *find_thumbnail(struct browser *browser, uint32_t entry)
{
    for (size_t i = 0; i < THUMBNAIL_SLOTS; ++i) {
        struct thumbnail *thumb = &browser->thumbnails[i];
        if (thumb->used && thumb->entry == entry)
            return thumb;
    }
    return NULL;
}

static void drop_thumbnail(struct browser *browser, struct thumbnail *thumb)
{
    if (!thumb->used)
        return;
    int watch = thumb->watch;
    browser->thumb_bytes -= thumbnail_bytes(thumb->size);
    free(thumb->pixels);
    *thumb = (struct thumbnail){0};
    if (watch >= 0 && browser->notify_files >= 0) {
        for (size_t i = 0; i < THUMBNAIL_SLOTS; ++i) {
            if (browser->thumbnails[i].used && browser->thumbnails[i].watch == watch)
                return;
        }
        inotify_rm_watch(browser->notify_files, watch);
    }
}

static struct thumbnail *oldest_thumbnail(struct browser *browser)
{
    struct thumbnail *oldest = NULL;
    for (size_t i = 0; i < THUMBNAIL_SLOTS; ++i) {
        struct thumbnail *thumb = &browser->thumbnails[i];
        if (thumb->used && !visible_entry(browser, thumb->entry) &&
            (!oldest || thumb->used < oldest->used))
            oldest = thumb;
    }
    return oldest;
}

static struct thumbnail *reserve_thumbnail(struct browser *browser, size_t bytes)
{
    while (bytes > THUMBNAIL_BYTES - browser->thumb_bytes) {
        struct thumbnail *old = oldest_thumbnail(browser);
        if (!old)
            return NULL;
        drop_thumbnail(browser, old);
    }
    for (size_t i = 0; i < THUMBNAIL_SLOTS; ++i) {
        if (!browser->thumbnails[i].used)
            return &browser->thumbnails[i];
    }
    struct thumbnail *old = oldest_thumbnail(browser);
    if (old)
        drop_thumbnail(browser, old);
    return old;
}

static void remove_thumbnail_temporary(const struct browser *browser, pid_t pid)
{
    char *name = thumbnail_temporary(browser->cache_directory, pid);
    if (name) {
        unlink(name);
        free(name);
    }
}

static void reset_thumbnails(struct browser *browser, bool cancel)
{
    if (cancel && browser->jobs[TASK_THUMBNAIL].pid) {
        pid_t pid = browser->jobs[TASK_THUMBNAIL].pid;
        finish_job(&browser->jobs[TASK_THUMBNAIL], true);
        remove_thumbnail_temporary(browser, pid);
    }
    if (browser->notify_files >= 0)
        close(browser->notify_files);
    browser->notify_files = -1;
    for (size_t i = 0; i < THUMBNAIL_SLOTS; ++i)
        drop_thumbnail(browser, &browser->thumbnails[i]);
}

static void schedule_thumbnail(struct browser *browser)
{
    if (!browser->configured || browser->jobs[TASK_THUMBNAIL].pid)
        return;
    size_t first, end;
    visible_range(browser, &first, &end);
    size_t priority = browser->selected;
    if (priority < first || priority >= end || find_thumbnail(browser, browser->matches[priority])) {
        priority = first;
        while (priority < end && find_thumbnail(browser, browser->matches[priority]))
            ++priority;
    }
    if (priority >= end)
        return;
    size_t maximum = (size_t)browser->config.thumbnail_width * browser->config.thumbnail_height * 4;
    if (!reserve_thumbnail(browser, maximum))
        return;
    browser->thumbnail_entry = browser->matches[priority];
    browser->thumbnail_generation = browser->catalog_generation;
    const struct thumbnail_request request = {
        entry_path(browser, browser->thumbnail_entry), browser->cache_directory,
        browser->config.thumbnail_width, browser->config.thumbnail_height,
        browser->notify_files,
    };
    if (!start_job(browser, TASK_THUMBNAIL, generate_thumbnail, &request)) {
        snprintf(browser->status, sizeof browser->status, "Cannot start thumbnail worker");
        struct thumbnail *slot = reserve_thumbnail(browser, 0);
        if (slot)
            *slot = (struct thumbnail){.entry = browser->thumbnail_entry, .used = ++browser->use_clock, .watch = -1};
        browser->dirty = true;
    }
}

static void receive_thumbnail(struct browser *browser)
{
    pid_t pid = browser->jobs[TASK_THUMBNAIL].pid;
    int fd = finish_job(&browser->jobs[TASK_THUMBNAIL], false);
    if (fd < 0)
        remove_thumbnail_temporary(browser, pid);
    if (browser->thumbnail_generation != browser->catalog_generation) {
        if (fd >= 0)
            close(fd);
        return;
    }
    struct thumbnail_result result = {.watch = -1};
    uint32_t *pixels = NULL;
    if (fd >= 0 && read_bytes(fd, &result, sizeof result)) {
        size_t bytes = thumbnail_bytes(result.size);
        if (bytes) {
            pixels = malloc(bytes);
            if (pixels && !read_bytes(fd, pixels, bytes)) {
                free(pixels);
                pixels = NULL;
            }
        }
    }
    if (fd >= 0)
        close(fd);
    if (!pixels)
        result.size = (struct thumbnail_size){0};
    size_t bytes = thumbnail_bytes(result.size);
    struct thumbnail *slot = reserve_thumbnail(browser, bytes);
    if (slot) {
        *slot = (struct thumbnail){browser->thumbnail_entry, ++browser->use_clock,
                                   result.size, pixels, result.watch};
        browser->thumb_bytes += bytes;
    } else {
        if (result.watch >= 0 && browser->notify_files >= 0)
            inotify_rm_watch(browser->notify_files, result.watch);
        free(pixels);
    }
    browser->dirty = true;
}

static void invalidate_thumbnails(struct browser *browser)
{
    unsigned char events[4096];
    ssize_t received = read(browser->notify_files, events, sizeof events);
    if (received <= 0)
        return;
    for (size_t offset = 0; offset + sizeof(struct inotify_event) <= (size_t)received;) {
        struct inotify_event event;
        memcpy(&event, events + offset, sizeof event);
        offset += sizeof event + event.len;
        if (event.mask & IN_IGNORED)
            continue;
        for (size_t j = 0; j < THUMBNAIL_SLOTS; ++j) {
            struct thumbnail *thumb = &browser->thumbnails[j];
            if (thumb->used && (thumb->watch == event.wd || (event.mask & IN_Q_OVERFLOW))) {
                drop_thumbnail(browser, thumb);
                browser->dirty = true;
            }
        }
    }
}

static void start_scan(struct browser *browser)
{
    if (browser->jobs[TASK_SCAN].pid)
        return;
    if (!start_job(browser, TASK_SCAN, discover_images, &browser->config))
        snprintf(browser->status, sizeof browser->status, "Cannot start directory scan");
    browser->dirty = true;
}

static bool load_catalog(struct browser *browser, int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(struct catalog_header) ||
        st.st_size > (off_t)(CATALOG_BYTES + sizeof(struct catalog_header)))
        return false;
    size_t size = (size_t)st.st_size;
    void *mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED)
        return false;

    const struct catalog_header *header = mapping;
    const struct catalog_entry *entries = (const void *)(header + 1);
    uint32_t *matches = NULL;
    if (header->count > CATALOG_LIMIT ||
        header->count > (size - sizeof *header) / sizeof *entries)
        goto fail;
    size_t offset = sizeof *header + header->count * sizeof *entries;
    for (size_t i = 0; i < header->count; ++i) {
        if (entries[i].offset != offset || offset >= size)
            goto fail;
        const char *path = (const char *)mapping + offset;
        const char *end = memchr(path, 0, size - offset);
        if (!end || entries[i].relative > entries[i].name || entries[i].name >= (size_t)(end - path))
            goto fail;
        offset += (size_t)(end - path) + 1;
    }
    if (offset != size)
        goto fail;
    if (header->count) {
        matches = malloc(header->count * sizeof *matches);
        if (!matches)
            goto fail;
    }

    const char *previous = selected_path(browser);
    char *selection = previous ? strdup(previous) : NULL;
    ++browser->catalog_generation;
    reset_thumbnails(browser, false);
    browser->notify_files = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (browser->catalog)
        munmap(browser->catalog, browser->catalog_size);
    free(browser->matches);
    browser->catalog = mapping;
    browser->catalog_size = size;
    browser->entries = entries;
    browser->count = header->count;
    browser->scan_skipped = header->skipped;
    browser->scan_limited = header->limited;
    browser->matches = matches;
    filter_entries(browser, selection);
    free(selection);
    return true;

fail:
    munmap(mapping, size);
    return false;
}

static void receive_scan(struct browser *browser)
{
    int fd = finish_job(&browser->jobs[TASK_SCAN], false);
    if (fd < 0 || !load_catalog(browser, fd))
        snprintf(browser->status, sizeof browser->status, "Could not read the wallpaper catalog");
    if (fd >= 0)
        close(fd);
    browser->dirty = true;
}

static void start_browser_ipc(struct browser *browser, enum wall_command command, const char *path)
{
    if (browser->jobs[TASK_IPC].pid)
        return;
    char *copy = path ? strdup(path) : NULL;
    const struct browser_ipc_request request = {
        command, copy, browser->display_name, browser->session_error,
    };
    if ((path && !copy) || !start_job(browser, TASK_IPC, browser_ipc_work, &request)) {
        free(copy);
        snprintf(browser->status, sizeof browser->status, "Cannot start renderer request");
    } else {
        free(browser->requested_path);
        browser->requested_path = copy;
        browser->ipc_command = command;
        if (command == WALL_SET)
            snprintf(browser->status, sizeof browser->status, "Applying wallpaper…");
    }
    browser->dirty = true;
}

static void receive_browser_ipc(struct browser *browser)
{
    int fd = finish_job(&browser->jobs[TASK_IPC], false);
    struct browser_ipc_result result;
    if (fd < 0 || !read_bytes(fd, &result, sizeof result)) {
        snprintf(browser->status, sizeof browser->status, "Could not read the renderer response");
    } else if (result.result == 1) {
        snprintf(browser->status, sizeof browser->status, "%s", result.reply.error);
    } else if (result.result < 0) {
        if (result.error == ENOENT || result.error == ECONNREFUSED)
            snprintf(browser->status, sizeof browser->status,
                     "No wallpaper renderer is running in this Wayland session.");
        else
            snprintf(browser->status, sizeof browser->status, "Renderer request failed: %s", strerror(result.error));
    } else if (browser->ipc_command == WALL_GET) {
        free(browser->current_path);
        browser->current_path = *result.reply.path ? strdup(result.reply.path) : NULL;
        browser->current_device = result.device;
        browser->current_inode = result.inode;
    } else {
        free(browser->current_path);
        browser->current_path = browser->requested_path;
        browser->requested_path = NULL;
        browser->current_device = result.device;
        browser->current_inode = result.inode;
        snprintf(browser->status, sizeof browser->status, "Wallpaper applied");
        browser->done = browser->config.close_on_apply;
    }
    if (fd >= 0)
        close(fd);
    free(browser->requested_path);
    browser->requested_path = NULL;
    browser->dirty = true;
}

struct canvas {
    struct buffer *buffer;
    int top, bottom;
};

static void rectangle(struct canvas *canvas, int x, int y, int width, int height, uint32_t color)
{
    struct buffer *buffer = canvas->buffer;
    int right = smaller(x + width, buffer->width);
    int bottom = smaller(y + height, canvas->bottom);
    int left = larger(0, x);
    for (int row = larger(y, canvas->top); row < bottom; ++row) {
        uint32_t *pixels = buffer->pixels + (size_t)row * buffer->width;
        for (int column = left; column < right; ++column)
            pixels[column] = color | 0xff000000;
    }
}

static void outline(struct canvas *canvas, int x, int y, int width, int height, uint32_t color)
{
    rectangle(canvas, x, y, width, 1, color);
    rectangle(canvas, x, y + height - 1, width, 1, color);
    rectangle(canvas, x, y, 1, height, color);
    rectangle(canvas, x + width - 1, y, 1, height, color);
}

/* Premultiplied "over": an opaque color with coverage alpha on top of base,
   which may be translucent. */
static uint32_t blend(uint32_t base, uint32_t foreground, unsigned alpha)
{
    foreground |= 0xff000000;
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        unsigned bg = (base >> shift) & 255;
        unsigned fg = (foreground >> shift) & 255;
        result |= ((bg * (255 - alpha) + fg * alpha + 127) / 255) << shift;
    }
    return result;
}

static void draw_text(struct canvas *canvas, FT_Face face, int x, int y, int limit,
                      const char *text, uint32_t color)
{
    if (limit <= 0)
        return;
    struct buffer *buffer = canvas->buffer;
    int right = smaller(buffer->width, x + limit);
    int left = larger(0, x);
    int baseline = y + (int)(face->size->metrics.ascender >> 6);
    mbstate_t state = {0};
    size_t remaining = strlen(text);
    while (remaining && x < right) {
        wchar_t character;
        size_t consumed = mbrtowc(&character, text, remaining, &state);
        if (consumed == (size_t)-1 || consumed == (size_t)-2 || consumed == 0) {
            memset(&state, 0, sizeof state);
            character = 0xfffd;
            consumed = 1;
        }
        text += consumed;
        remaining -= consumed;
        if (iswcntrl((wint_t)character))
            character = L' ';
        if (FT_Load_Char(face, (FT_ULong)character, FT_LOAD_RENDER))
            continue;

        FT_GlyphSlot glyph = face->glyph;
        const FT_Bitmap *bitmap = &glyph->bitmap;
        int glyph_x = x + glyph->bitmap_left, glyph_y = baseline - glyph->bitmap_top;
        int last_row = smaller((int)bitmap->rows, canvas->bottom - glyph_y);
        int last_column = smaller((int)bitmap->width, right - glyph_x);
        int pitch = bitmap->pitch;
        for (int row = larger(0, canvas->top - glyph_y); row < last_row; ++row) {
            const unsigned char *src = bitmap->buffer + (pitch >= 0 ?
                (size_t)row * pitch : (size_t)(bitmap->rows - 1 - row) * (unsigned)-pitch);
            uint32_t *dst = buffer->pixels + (size_t)(glyph_y + row) * buffer->width;
            for (int column = larger(0, left - glyph_x); column < last_column; ++column) {
                unsigned coverage = 0;
                if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY && bitmap->num_grays > 1)
                    coverage = src[column] * 255u / (bitmap->num_grays - 1);
                else if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
                    coverage = (src[column / 8] & (0x80u >> (column % 8))) ? 255 : 0;
                if (coverage)
                    dst[glyph_x + column] = blend(dst[glyph_x + column], color, coverage);
            }
        }
        x += (int)(glyph->advance.x >> 6);
    }
}

static void draw_thumbnail(struct canvas *canvas, const struct thumbnail *thumb,
                           int left, int top, int width, int height)
{
    const struct thumbnail_size size = thumb->size;
    double ratio = fmin((double)width / size.width, (double)height / size.height);
    int w = larger(1, (int)floor(size.width * ratio + 0.5));
    int h = larger(1, (int)floor(size.height * ratio + 0.5));
    int x = left + (width - w) / 2, y = top + (height - h) / 2;
    for (int row = larger(y, canvas->top); row < smaller(y + h, canvas->bottom); ++row) {
        size_t sy = (size_t)(row - y) * size.height / h;
        const uint32_t *source = thumb->pixels + sy * size.width;
        uint32_t *target = canvas->buffer->pixels + (size_t)row * canvas->buffer->width;
        for (int column = larger(0, x); column < smaller(x + w, canvas->buffer->width); ++column) {
            uint32_t pixel = source[(size_t)(column - x) * size.width / w];
            unsigned alpha = pixel >> 24;
            if (!alpha)
                continue;
            if (alpha < 255) {
                uint32_t background = target[column];
                uint32_t rgb = 0;
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    unsigned old = (background >> shift) & 255u;
                    unsigned channel = ((pixel >> shift) & 255u) + (old * (255 - alpha) + 127) / 255;
                    rgb |= (uint32_t)smaller((int)channel, 255) << shift;
                }
                pixel = rgb;
            }
            target[column] = pixel;
        }
    }
}

static void paint_browser(struct browser *browser, struct buffer *buffer)
{
    const struct browser_config *c = &browser->config;
    const struct browser_layout *l = &browser->layout;
    const int padding = BROWSER_PADDING, line = c->font_size + 6;
    const int usable = larger(0, buffer->width - 2 * padding);
    struct canvas canvas = {.buffer = buffer, .bottom = buffer->height};
    /* Only the background is translucent, as in deal. Text, outlines and
       thumbnails are drawn opaque on top. */
    unsigned opacity = (unsigned)(c->opacity * 255.0 + 0.5);
    uint32_t background = (uint32_t)opacity << 24;
    for (unsigned shift = 0; shift < 24; shift += 8)
        background |= ((((c->background >> shift) & 255u) * opacity + 127) / 255) << shift;
    for (int row = 0; row < buffer->height; ++row) {
        uint32_t *pixels = buffer->pixels + (size_t)row * buffer->width;
        for (int column = 0; column < buffer->width; ++column)
            pixels[column] = background;
    }
    draw_text(&canvas, browser->face, padding, padding, usable, "Wallpaper browser", c->highlight);

    int search_y = padding + line;
    outline(&canvas, padding, search_y, usable, line + 5, browser->editing ? c->highlight : c->muted);
    char search[sizeof browser->query + 16];
    snprintf(search, sizeof search, "Search: %s%s", browser->query, browser->editing ? "_" : "");
    draw_text(&canvas, browser->face, padding + 6, search_y + 3, larger(0, usable - 12),
              search, c->foreground);

    char count[384];
    const char *detail = browser->status;
    char scan_info[128];
    if (browser->jobs[TASK_SCAN].pid && !*detail)
        detail = "Scanning directories…";
    else if (!*detail && browser->scan_limited)
        detail = "Collection limit reached";
    else if (!*detail && browser->scan_skipped) {
        snprintf(scan_info, sizeof scan_info, "%u paths skipped", browser->scan_skipped);
        detail = scan_info;
    }
    snprintf(count, sizeof count, "%zu of %zu wallpapers%s%s",
             browser->match_count, browser->count, *detail ? "  |  " : "", detail);
    draw_text(&canvas, browser->face, padding, search_y + line + 9, usable, count,
              *browser->status ? c->foreground : c->muted);

    canvas.top = smaller(l->top, buffer->height);
    canvas.bottom = smaller(l->bottom, buffer->height);
    size_t first, end;
    visible_range(browser, &first, &end);
    for (size_t i = first; i < end; ++i) {
        int column = (int)(i % l->columns);
        int x = padding + column_offset(l, column);
        int width = column_width(l, column);
        int y = l->top + (int)(i / l->columns) * l->row_height - browser->scroll;
        uint32_t entry = browser->matches[i];
        const char *path = entry_path(browser, entry);
        bool current = (browser->current_path && !strcmp(browser->current_path, path)) ||
                       (browser->current_inode && browser->current_inode == browser->entries[entry].inode &&
                        browser->current_device == browser->entries[entry].device);
        struct thumbnail *thumb = find_thumbnail(browser, entry);
        if (thumb) {
            thumb->used = ++browser->use_clock;
            if (thumb->pixels)
                draw_thumbnail(&canvas, thumb, x, y, width, l->thumb_height);
            else
                draw_text(&canvas, browser->face, x + 4, y + 4, width - 8,
                          "Unavailable", c->muted);
        }
        if (i == browser->selected || current)
            outline(&canvas, x, y, width, l->thumb_height,
                    i == browser->selected ? c->highlight : c->muted);
        int name_y = y + l->thumb_height + 2;
        int inset = current ? smaller(width / 2, 8 * (c->font_size / 2 + 1)) : 0;
        draw_text(&canvas, browser->face, x + 1, name_y, width - inset - 1,
                  path + browser->entries[entry].name, i == browser->selected ? c->highlight : c->foreground);
        if (current && inset)
            draw_text(&canvas, browser->face, x + width - inset, name_y, inset,
                      "Current", c->highlight);
    }
    if (!browser->match_count && !browser->jobs[TASK_SCAN].pid)
        draw_text(&canvas, browser->face, padding, l->top + 4, usable,
                  *browser->query ? "No matching wallpapers" : "No wallpapers found", c->muted);

    canvas.top = smaller(l->bottom, buffer->height);
    canvas.bottom = buffer->height;
    draw_text(&canvas, browser->face, padding, l->bottom + 3, usable,
              "Arrows / hjkl  Move   Enter  Apply   /  Search   r  Rescan   Esc / q  Close", c->muted);
}

static bool setup_font(struct browser *browser)
{
    const char *name = browser->config.font ? browser->config.font : "monospace";
    if (FT_Init_FreeType(&browser->freetype))
        return false;
    FcConfig *config = FcInitLoadConfigAndFonts();
    if (!config)
        return false;
    FcPattern *pattern = FcNameParse((const FcChar8 *)name);
    FcPattern *match = NULL;
    if (pattern) {
        FcResult result;
        FcConfigSubstitute(config, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        match = FcFontMatch(config, pattern, &result);
        FcPatternDestroy(pattern);
    }
    bool ok = false;
    if (match) {
        FcChar8 *path;
        int index = 0;
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        if (FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch &&
            !FT_New_Face(browser->freetype, (const char *)path, index, &browser->face) &&
            !FT_Set_Pixel_Sizes(browser->face, 0, (FT_UInt)browser->config.font_size))
            ok = true;
        FcPatternDestroy(match);
    }
    FcConfigDestroy(config);
    return ok;
}

static void browser_release(void *data, struct wl_buffer *proxy)
{
    (void)proxy;
    struct buffer *buffer = data;
    buffer->busy = false;
}

static const struct wl_buffer_listener browser_buffer_listener = {.release = browser_release};

static void browser_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)time;
    struct browser *browser = data;
    wl_callback_destroy(callback);
    browser->frame_callback = NULL;
}

static const struct wl_callback_listener browser_frame_listener = {.done = browser_frame_done};

static bool draw_browser(struct browser *browser)
{
    if (!browser->configured || !browser->dirty || browser->frame_callback)
        return true;
    struct buffer *buffer = NULL;
    for (size_t i = 0; i < 2; ++i) {
        struct buffer *frame = browser->frames[i];
        if (frame && !frame->busy && (frame->width != browser->width || frame->height != browser->height)) {
            destroy_buffer(frame);
            browser->frames[i] = NULL;
        } else if (frame && !frame->busy) {
            buffer = frame;
        }
    }
    for (size_t i = 0; !buffer && i < 2; ++i) {
        if (browser->frames[i])
            continue;
        buffer = allocate_buffer(browser->shm, browser->width, browser->height, true);
        if (!buffer)
            return false;
        wl_buffer_add_listener(buffer->proxy, &browser_buffer_listener, buffer);
        browser->frames[i] = buffer;
    }
    if (!buffer)
        return true;

    paint_browser(browser, buffer);
    buffer->busy = true;
    wl_surface_attach(browser->surface, buffer->proxy, 0, 0);
    wl_surface_damage(browser->surface, 0, 0, buffer->width, buffer->height);
    browser->frame_callback = wl_surface_frame(browser->surface);
    if (browser->frame_callback)
        wl_callback_add_listener(browser->frame_callback, &browser_frame_listener, browser);
    wl_surface_commit(browser->surface);
    browser->dirty = false;
    return true;
}

static void apply_selection(struct browser *browser)
{
    const char *path = selected_path(browser);
    if (path)
        start_browser_ipc(browser, WALL_SET, path);
}

static void move_selection(struct browser *browser, int delta)
{
    if (!browser->match_count)
        return;
    int selected = (int)browser->selected + delta;
    browser->selected = (size_t)larger(0, smaller(selected, (int)browser->match_count - 1));
    visible_selection(browser);
    browser->dirty = true;
}

static void browser_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                          int32_t fd, uint32_t size)
{
    (void)keyboard;
    struct browser *browser = data;
    struct xkb_state *keys = NULL;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 1 && size <= 16 * 1024 * 1024) {
        const char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(
                browser->xkb, map, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (keymap) {
                keys = xkb_state_new(keymap);
                xkb_keymap_unref(keymap);
            }
            munmap((void *)map, size);
        }
    }
    close(fd);
    xkb_state_unref(browser->keys);
    browser->keys = keys;
    if (!keys) {
        snprintf(browser->status, sizeof browser->status, "No usable keyboard map from the compositor");
        browser->dirty = true;
    }
}

static void browser_keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                                   struct wl_surface *surface, struct wl_array *keys)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void browser_keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                                   struct wl_surface *surface)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void edit_search(struct browser *browser, xkb_keycode_t key, xkb_keysym_t symbol)
{
    size_t length = strlen(browser->query);
    if (symbol == XKB_KEY_BackSpace && length) {
        --length;
        while (length && ((unsigned char)browser->query[length] & 0xc0) == 0x80)
            --length;
        browser->query[length] = 0;
    } else if (symbol != XKB_KEY_BackSpace) {
        char text[64];
        if (xkb_state_mod_name_is_active(browser->keys, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
            return;
        int bytes = xkb_state_key_get_utf8(browser->keys, key, text, sizeof text);
        if (bytes <= 0 || (size_t)bytes >= sizeof text || length + (size_t)bytes >= sizeof browser->query ||
            (unsigned char)text[0] < 32 || text[0] == 127)
            return;
        memcpy(browser->query + length, text, (size_t)bytes + 1);
    } else {
        return;
    }
    filter_entries(browser, selected_path(browser));
}

static void browser_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                       uint32_t time, uint32_t code, uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    struct browser *browser = data;
    if (browser->done || !browser->keys || state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    xkb_keycode_t key = code + 8;
    xkb_keysym_t symbol = xkb_state_key_get_one_sym(browser->keys, key);
    int columns = browser->layout.columns;
    if (symbol == XKB_KEY_Escape) {
        if (browser->editing)
            browser->editing = false;
        else
            browser->done = true;
        browser->dirty = true;
    } else if (symbol == XKB_KEY_Return || symbol == XKB_KEY_KP_Enter) {
        apply_selection(browser);
    } else if (symbol == XKB_KEY_Left || (!browser->editing && symbol == XKB_KEY_h)) {
        move_selection(browser, -1);
    } else if (symbol == XKB_KEY_Right || (!browser->editing && symbol == XKB_KEY_l)) {
        move_selection(browser, 1);
    } else if (symbol == XKB_KEY_Up || (!browser->editing && symbol == XKB_KEY_k)) {
        move_selection(browser, -columns);
    } else if (symbol == XKB_KEY_Down || (!browser->editing && symbol == XKB_KEY_j)) {
        move_selection(browser, columns);
    } else if (browser->editing) {
        edit_search(browser, key, symbol);
    } else if (symbol == XKB_KEY_slash) {
        browser->editing = true;
        browser->dirty = true;
    } else if (symbol == XKB_KEY_r) {
        start_scan(browser);
    } else if (symbol == XKB_KEY_q) {
        browser->done = true;
    }
}

static void browser_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                             uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)keyboard;
    (void)serial;
    struct browser *browser = data;
    if (browser->keys)
        xkb_state_update_mask(browser->keys, depressed, latched, locked, 0, 0, group);
}

static void browser_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener browser_keyboard_listener = {
    .keymap = browser_keymap, .enter = browser_keyboard_enter, .leave = browser_keyboard_leave,
    .key = browser_key, .modifiers = browser_modifiers, .repeat_info = browser_repeat,
};

static void browser_pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                                  struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    struct browser *browser = data;
    if (surface != browser->surface)
        return;
    browser->pointer_x = wl_fixed_to_double(x);
    browser->pointer_y = wl_fixed_to_double(y);
    if (browser->cursor && browser->cursor->image_count && browser->cursor_surface) {
        struct wl_cursor_image *image = browser->cursor->images[0];
        struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
        if (buffer) {
            wl_pointer_set_cursor(pointer, serial, browser->cursor_surface, image->hotspot_x, image->hotspot_y);
            wl_surface_attach(browser->cursor_surface, buffer, 0, 0);
            wl_surface_damage(browser->cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
            wl_surface_commit(browser->cursor_surface);
        }
    }
}

static void browser_pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                                  struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    struct browser *browser = data;
    browser->pointer_x = browser->pointer_y = -1;
    browser->last_clicked = SIZE_MAX;
}

static void browser_pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                                   wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)time;
    struct browser *browser = data;
    browser->pointer_x = wl_fixed_to_double(x);
    browser->pointer_y = wl_fixed_to_double(y);
}

static void browser_pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                                   uint32_t time, uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)serial;
    struct browser *browser = data;
    if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED || !browser->configured)
        return;
    const struct browser_layout *l = &browser->layout;
    double x = browser->pointer_x - BROWSER_PADDING;
    double y = browser->pointer_y;
    int search = BROWSER_PADDING + browser->config.font_size + 6;
    if (x >= 0 && x < browser->width - 2 * BROWSER_PADDING &&
        y >= search && y < search + browser->config.font_size + 11) {
        browser->last_clicked = SIZE_MAX;
        browser->editing = true;
        browser->dirty = true;
        return;
    }
    if (x < 0 || y < l->top || y >= l->bottom)
        return;
    int grid_y = (int)y - l->top + browser->scroll;
    if (grid_y % l->row_height >= l->row_height - BROWSER_GAP)
        return;
    int column;
    for (column = 0; column < l->columns; ++column) {
        int left = column_offset(l, column);
        if (x >= left && x < left + column_width(l, column))
            break;
    }
    if (column == l->columns)
        return;
    size_t index = (size_t)(grid_y / l->row_height) * l->columns + column;
    if (index >= browser->match_count)
        return;
    bool double_click = index == browser->last_clicked && time - browser->last_click_time <= 350;
    browser->selected = index;
    browser->editing = false;
    browser->last_clicked = double_click ? SIZE_MAX : index;
    browser->last_click_time = time;
    browser->dirty = true;
    if (double_click)
        apply_selection(browser);
}

static void browser_pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                                 uint32_t axis, wl_fixed_t value)
{
    (void)pointer;
    (void)time;
    struct browser *browser = data;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || !browser->configured)
        return;
    browser->wheel += wl_fixed_to_double(value) * 3;
    int delta = (int)browser->wheel;
    browser->wheel -= delta;
    int next = larger(0, smaller(browser->layout.max_scroll, browser->scroll + delta));
    if (next != browser->scroll) {
        browser->scroll = next;
        browser->last_clicked = SIZE_MAX;
        browser->dirty = true;
    }
}

static const struct wl_pointer_listener browser_pointer_listener = {
    .enter = browser_pointer_enter, .leave = browser_pointer_leave,
    .motion = browser_pointer_motion, .button = browser_pointer_button, .axis = browser_pointer_axis,
};

static void browser_drop_keyboard(struct browser *browser)
{
    if (browser->keyboard) {
        if (wl_keyboard_get_version(browser->keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
            wl_keyboard_release(browser->keyboard);
        else
            wl_keyboard_destroy(browser->keyboard);
        browser->keyboard = NULL;
    }
    xkb_state_unref(browser->keys);
    browser->keys = NULL;
}

static void browser_drop_pointer(struct browser *browser)
{
    if (!browser->pointer)
        return;
    if (wl_pointer_get_version(browser->pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
        wl_pointer_release(browser->pointer);
    else
        wl_pointer_destroy(browser->pointer);
    browser->pointer = NULL;
}

static void browser_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct browser *browser = data;
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!browser->keyboard) {
            browser->keyboard = wl_seat_get_keyboard(seat);
            if (browser->keyboard)
                wl_keyboard_add_listener(browser->keyboard, &browser_keyboard_listener, browser);
        }
    } else {
        browser_drop_keyboard(browser);
    }
    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        if (!browser->pointer) {
            browser->pointer = wl_seat_get_pointer(seat);
            if (browser->pointer)
                wl_pointer_add_listener(browser->pointer, &browser_pointer_listener, browser);
        }
    } else {
        browser_drop_pointer(browser);
    }
}

static void browser_seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener browser_seat_listener = {
    .capabilities = browser_capabilities, .name = browser_seat_name,
};

static void browser_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener browser_shell_listener = {.ping = browser_ping};

static void browser_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    struct browser *browser = data;
    xdg_surface_ack_configure(surface, serial);
    if (browser->requested_width > 0)
        browser->width = smaller(browser->requested_width, BROWSER_MAX_SIZE);
    if (browser->requested_height > 0)
        browser->height = smaller(browser->requested_height, BROWSER_MAX_SIZE);
    browser->configured = true;
    browser->dirty = true;
    update_layout(browser);
    visible_selection(browser);
}

static const struct xdg_surface_listener browser_surface_listener = {.configure = browser_configure};

static void browser_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                       int32_t width, int32_t height, struct wl_array *states)
{
    (void)toplevel;
    (void)states;
    struct browser *browser = data;
    browser->requested_width = width;
    browser->requested_height = height;
}

static void browser_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    struct browser *browser = data;
    browser->done = true;
}

static const struct xdg_toplevel_listener browser_toplevel_listener = {
    .configure = browser_toplevel_configure, .close = browser_toplevel_close,
};

static void browser_global(void *data, struct wl_registry *registry, uint32_t name,
                           const char *interface, uint32_t version)
{
    struct browser *b = data;
    if (!b->compositor && !strcmp(interface, wl_compositor_interface.name)) {
        b->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if (!b->shm && !strcmp(interface, wl_shm_interface.name)) {
        b->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!b->shell && !strcmp(interface, xdg_wm_base_interface.name)) {
        b->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        if (b->shell)
            xdg_wm_base_add_listener(b->shell, &browser_shell_listener, b);
    } else if (!b->seat && !strcmp(interface, wl_seat_interface.name)) {
        b->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 4 ? version : 4);
        if (b->seat) {
            b->seat_name = name;
            wl_seat_add_listener(b->seat, &browser_seat_listener, b);
        }
    }
}

static void browser_global_removed(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)registry;
    struct browser *browser = data;
    if (browser->seat && browser->seat_name == name) {
        browser_drop_keyboard(browser);
        browser_drop_pointer(browser);
        wl_seat_destroy(browser->seat);
        browser->seat = NULL;
    }
}

static const struct wl_registry_listener browser_registry_listener = {
    .global = browser_global, .global_remove = browser_global_removed,
};

static bool browser_wayland(struct browser *b)
{
    b->display = wl_display_connect(NULL);
    if (!b->display) {
        b->error = "cannot connect to the Wayland session";
        return false;
    }
    b->registry = wl_display_get_registry(b->display);
    if (b->registry) {
        wl_registry_add_listener(b->registry, &browser_registry_listener, b);
        if (wl_display_roundtrip(b->display) < 0) {
            b->error = "cannot read the Wayland registry";
            return false;
        }
    }
    if (!b->compositor || !b->shm || !b->shell) {
        b->error = "browser requires wl_compositor, wl_shm and xdg-shell";
        return false;
    }
    b->surface = wl_compositor_create_surface(b->compositor);
    if (b->surface)
        b->xdg_surface = xdg_wm_base_get_xdg_surface(b->shell, b->surface);
    if (b->xdg_surface)
        b->toplevel = xdg_surface_get_toplevel(b->xdg_surface);
    if (!b->toplevel) {
        b->error = "cannot create the Wayland browser window";
        return false;
    }
    wl_surface_add_listener(b->surface, &surface_listener, b);
    xdg_surface_add_listener(b->xdg_surface, &browser_surface_listener, b);
    xdg_toplevel_add_listener(b->toplevel, &browser_toplevel_listener, b);
    xdg_toplevel_set_title(b->toplevel, "Wallpaper browser");
    xdg_toplevel_set_app_id(b->toplevel, "wall-browser");
    xdg_toplevel_set_min_size(b->toplevel, 280, larger(210, 5 * (b->config.font_size + 6) + 48));
    xdg_toplevel_set_max_size(b->toplevel, BROWSER_MAX_SIZE, BROWSER_MAX_SIZE);
    b->cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), 24, b->shm);
    if (b->cursor_theme) {
        b->cursor = wl_cursor_theme_get_cursor(b->cursor_theme, "left_ptr");
        if (b->cursor)
            b->cursor_surface = wl_compositor_create_surface(b->compositor);
    }
    wl_surface_commit(b->surface);
    return true;
}

static void refresh_browser_palette(struct browser *browser)
{
    const char *path = browser->theme_watch.path;
    if (!path)
        return;
    struct stat st;
    int fd = open_regular(path, &st);
    if (fd < 0)
        return;
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        return;
    }
    struct palette palette;
    bool valid = parse_theme_palette(file, palette.rgb);
    fclose(file);
    if (!valid)
        return;

    struct browser_config *config = &browser->config;
    const struct {
        uint32_t *color;
        enum palette_role role;
    } mapping[] = {
        {&config->background, PALETTE_BACKGROUND}, {&config->foreground, PALETTE_FOREGROUND},
        {&config->highlight, PALETTE_HIGHLIGHT}, {&config->muted, PALETTE_MUTED},
    };
    for (size_t i = 0; i < sizeof mapping / sizeof mapping[0]; ++i) {
        uint32_t color = palette.rgb[mapping[i].role];
        if (*mapping[i].color != color) {
            *mapping[i].color = color;
            browser->dirty = true;
        }
    }
}

static int browser_events(struct browser *b)
{
    enum { DISPLAY, SIGNALS, NOTIFY_FILES, NOTIFY_THEME, TASKS };
    struct pollfd fds[TASKS + TASK_COUNT] = {
        [DISPLAY] = {.fd = wl_display_get_fd(b->display), .events = POLLIN},
        [SIGNALS] = {.fd = b->signals, .events = POLLIN},
        [NOTIFY_FILES] = {.fd = b->notify_files, .events = POLLIN},
        [NOTIFY_THEME] = {.fd = b->theme_watch.fd, .events = POLLIN},
    };
    for (size_t i = 0; i < TASK_COUNT; ++i) {
        const struct browser_job *job = &b->jobs[i];
        fds[TASKS + i] = (struct pollfd){.fd = job->pid ? job->notify : -1, .events = POLLIN};
    }

    if (wl_display_prepare_read(b->display) < 0)
        return 0;
    if (wl_display_flush(b->display) < 0) {
        if (errno != EAGAIN) {
            wl_display_cancel_read(b->display);
            return -1;
        }
        fds[DISPLAY].events |= POLLOUT;
    }
    if (poll(fds, sizeof fds / sizeof fds[0], -1) < 0) {
        int error = errno;
        wl_display_cancel_read(b->display);
        errno = error;
        return error == EINTR ? 0 : -1;
    }
    if (fds[DISPLAY].revents & POLLIN) {
        if (wl_display_read_events(b->display) < 0)
            return -1;
    } else {
        wl_display_cancel_read(b->display);
    }
    if ((fds[DISPLAY].revents | fds[SIGNALS].revents) & (POLLHUP | POLLERR | POLLNVAL)) {
        errno = EIO;
        return -1;
    }

    if (fds[SIGNALS].revents & POLLIN) {
        bool reload = false;
        if (collect_signals(b->signals, &reload, &b->done) < 0)
            return -1;
        if (b->done)
            return 0;
        if (reload)
            start_scan(b);
    }
    if (fds[TASKS + TASK_THUMBNAIL].revents)
        receive_thumbnail(b);
    if (fds[NOTIFY_FILES].revents & POLLIN)
        invalidate_thumbnails(b);
    if (fds[NOTIFY_FILES].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        close(b->notify_files);
        b->notify_files = -1;
    }
    if (theme_watch_changed(&b->theme_watch, fds[NOTIFY_THEME].revents))
        refresh_browser_palette(b);
    if (fds[TASKS + TASK_SCAN].revents)
        receive_scan(b);
    if (fds[TASKS + TASK_IPC].revents)
        receive_browser_ipc(b);
    return 0;
}

static void browser_cleanup(struct browser *b)
{
    close_theme_watch(&b->theme_watch);
    reset_thumbnails(b, true);
    for (size_t i = 0; i < TASK_COUNT; ++i)
        finish_job(&b->jobs[i], true);
    if (b->signals >= 0)
        close(b->signals);
    if (b->catalog)
        munmap(b->catalog, b->catalog_size);
    free(b->matches);
    free(b->current_path);
    free(b->requested_path);
    free(b->config.directories);
    free(b->config.font);
    free(b->cache_directory);
    free(b->display_name);
    if (b->face)
        FT_Done_Face(b->face);
    if (b->freetype)
        FT_Done_FreeType(b->freetype);

    browser_drop_keyboard(b);
    browser_drop_pointer(b);
    xkb_context_unref(b->xkb);
    if (b->seat)
        wl_seat_destroy(b->seat);
    if (b->cursor_surface)
        wl_surface_destroy(b->cursor_surface);
    if (b->cursor_theme)
        wl_cursor_theme_destroy(b->cursor_theme);
    if (b->toplevel)
        xdg_toplevel_destroy(b->toplevel);
    if (b->xdg_surface)
        xdg_surface_destroy(b->xdg_surface);
    if (b->frame_callback)
        wl_callback_destroy(b->frame_callback);
    if (b->surface)
        wl_surface_destroy(b->surface);
    for (size_t i = 0; i < 2; ++i) {
        if (b->frames[i])
            destroy_buffer(b->frames[i]);
    }
    if (b->shell)
        xdg_wm_base_destroy(b->shell);
    if (b->shm)
        wl_shm_destroy(b->shm);
    if (b->compositor)
        wl_compositor_destroy(b->compositor);
    if (b->registry)
        wl_registry_destroy(b->registry);
    if (b->display)
        wl_display_disconnect(b->display);
    if (b->session_lock >= 0)
        close(b->session_lock);
}

static int browser_run(const struct options *options)
{
    struct browser browser = {
        .config = default_browser, .signals = -1, .notify_files = -1, .session_lock = -1,
        .theme_watch = {.fd = -1},
        .width = 1352, .height = 624, .last_clicked = SIZE_MAX,
        .pointer_x = -1, .pointer_y = -1,
    };
    struct config wallpaper = default_config;
    bool ok = load_config(&wallpaper, options, &browser.config);
    free(wallpaper.path);
    if (!ok) {
        browser.error = "cannot load browser configuration";
        goto end;
    }
    browser.display_name = display_socket();
    browser.session_error = errno;
    if (browser.display_name) {
        browser.session_lock = lock_browser(browser.display_name);
        if (browser.session_lock < 0) {
            if (errno != EADDRINUSE)
                browser.error = "cannot lock the wallpaper browser for this session";
            goto end;
        }
    }
    if (browser.config.wallpaper_theme) {
        browser.theme_watch.path = theme_palette_path();
        arm_theme_watch(&browser.theme_watch);
        refresh_browser_palette(&browser);
    }
    setlocale(LC_CTYPE, "");
    if (MB_CUR_MAX == 1)
        setlocale(LC_CTYPE, "C.UTF-8");
    browser.signals = signal_fd();
    if (browser.signals < 0) {
        browser.error = "cannot create browser signal notifications";
        goto end;
    }
    browser.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!browser.xkb) {
        browser.error = "cannot initialize XKB";
        goto end;
    }
    if (!setup_font(&browser)) {
        browser.error = "cannot open the configured font";
        goto end;
    }
    if (!browser_wayland(&browser))
        goto end;
    if (browser.config.cache)
        browser.cache_directory = thumbnail_directory();
    update_layout(&browser);
    start_scan(&browser);
    start_browser_ipc(&browser, WALL_GET, NULL);
    while (!browser.done) {
        if (wl_display_dispatch_pending(browser.display) < 0) {
            browser.error = "Wayland connection closed";
            break;
        }
        if (browser.done)
            break;
        schedule_thumbnail(&browser);
        if (!draw_browser(&browser)) {
            browser.error = "cannot allocate a browser frame";
            break;
        }
        if (browser_events(&browser) < 0) {
            browser.error = "cannot process browser events";
            break;
        }
    }

end:
    if (browser.error)
        fprintf(stderr, "wall: %s\n", browser.error);
    browser_cleanup(&browser);
    return browser.error ? EXIT_FAILURE : EXIT_SUCCESS;
}
