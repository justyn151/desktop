#define _GNU_SOURCE
#include "terminal.h"
#include "desk.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "primary-selection-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

enum {
    INITIAL_COLUMNS = 80,
    INITIAL_ROWS = 24,
    DEFAULT_FONT_SIZE = 14,
    MIN_FONT_SIZE = 6,
    MAX_FONT_SIZE = 96,
    DEFAULT_PADDING = 8,
    DEFAULT_BLINK_INTERVAL = 500,
    DEFAULT_CURSOR_THICKNESS = 2,
    FIRST_GLYPH = ' ',
    LAST_GLYPH = '~'
};

static const TerminalCell blank_cell = {
    .character = ' ', .foreground = {220, 229, 237}, .background = {16, 22, 30}, .width = 1
};
static const char deal_version[] = "0.13.2";
static const char compatibility_term[] = "xterm-color";
static const char default_title[] = "deal";
static const char default_font_family[] = "monospace";

/* Screen buffer */

static size_t screen_row_index(const TerminalScreen *screen, size_t row)
{
    size_t index = screen->first_row + row;
    return index < screen->rows ? index : index - screen->rows;
}

static TerminalCell *screen_row(const TerminalScreen *screen, size_t row)
{
    return screen->cells + screen_row_index(screen, row) * screen->columns;
}

static bool *screen_wrap(const TerminalScreen *screen, size_t row)
{
    return &screen->wrapped[screen_row_index(screen, row)];
}

static void screen_fill(TerminalCell *cells, size_t start, size_t end, TerminalCell fill)
{
    for (size_t i = start; i < end; ++i) {
        cells[i] = fill;
    }
}

void terminal_screen_clear(TerminalScreen *screen, TerminalCell fill)
{
    screen_fill(screen->cells, 0, screen->columns * screen->rows, fill);
    if (screen->wrapped != NULL) memset(screen->wrapped, 0, screen->rows * sizeof(*screen->wrapped));
    screen->first_row = 0;
}

void terminal_screen_set(TerminalScreen *screen, size_t column, size_t row,
                         TerminalCell cell)
{
    if (column < screen->columns && row < screen->rows) {
        screen_row(screen, row)[column] = cell;
    }
}

static bool screen_resized(TerminalScreen *resized, const TerminalScreen *screen,
                           size_t columns, size_t rows, size_t first_row, TerminalCell fill)
{
    *resized = (TerminalScreen){.columns = columns, .rows = rows};
    if (columns != 0 && rows != 0) {
        if (rows > SIZE_MAX / sizeof(TerminalCell) / columns) {
            return false;
        }
        resized->cells = malloc(columns * rows * sizeof(TerminalCell));
        resized->wrapped = calloc(rows, sizeof(*resized->wrapped));
        if (resized->cells == NULL || resized->wrapped == NULL) {
            terminal_screen_destroy(resized);
            return false;
        }
        size_t copy_columns = columns < screen->columns ? columns : screen->columns;
        size_t available_rows = screen->rows - first_row;
        size_t copy_rows = rows < available_rows ? rows : available_rows;
        for (size_t row = 0; row < rows; ++row) {
            TerminalCell *line = resized->cells + row * columns;
            size_t copied = row < copy_rows ? copy_columns : 0;
            if (copied != 0) {
                memcpy(line, screen_row(screen, first_row + row), copied * sizeof(*line));
                if (line[copied - 1].width == 2) line[copied - 1] = fill;
                resized->wrapped[row] = *screen_wrap(screen, first_row + row);
            }
            screen_fill(line, copied, columns, fill);
        }
    }

    return true;
}

bool terminal_screen_resize(TerminalScreen *screen, size_t columns, size_t rows)
{
    if (columns == screen->columns && rows == screen->rows) return true;
    TerminalScreen resized;
    if (!screen_resized(&resized, screen, columns, rows, 0, blank_cell)) return false;
    terminal_screen_destroy(screen);
    *screen = resized;
    return true;
}

void terminal_screen_destroy(TerminalScreen *screen)
{
    free(screen->cells);
    free(screen->wrapped);
    *screen = (TerminalScreen){0};
}

static const TerminalColor ansi_colors[] = {
    {24, 29, 36}, {209, 86, 87}, {114, 190, 125}, {218, 176, 82},
    {104, 155, 225}, {183, 125, 214}, {101, 189, 202}, {211, 218, 228},
    {106, 116, 131}, {255, 126, 124}, {155, 229, 166}, {245, 199, 117},
    {150, 190, 255}, {219, 165, 249}, {138, 228, 241}, {250, 252, 255}
};

/* Color sources retained by screen and history cells. */
/* SOURCE_RGB only appears in underline_source, where zero follows the foreground. */
enum { SOURCE_FG = 1, SOURCE_BG, SOURCE_ANSI, SOURCE_RGB = 127, SOURCE_DIM = 128 };
enum { TITLE_LIMIT = 512, LINK_LIMIT = 2048, LINK_SLOTS = 512, OSC_LIMIT = 2304 };
/* OSC 52 payloads use their own buffer: 16 MiB of base64 is 12 MiB of text. */
enum { OSC52_LIMIT = 16 * 1024 * 1024, SYNC_TIMEOUT = 150 };
enum { OSC52_CLIPBOARD = 1, OSC52_PRIMARY = 2 };

/* Startup configuration */

typedef enum { CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BAR } CursorStyle;
static const char *const cursor_style_names[] = {"block", "underline", "bar"};
enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4 };
static const char default_zoom_in[] = "Ctrl+Shift+=";
static const char default_zoom_out[] = "Ctrl+Shift+-";
static const char default_zoom_reset[] = "Ctrl+Shift+0";
static const char default_reload[] = "Ctrl+Shift+R";
static const char default_fullscreen[] = "F11";
static const char default_new_window[] = "Ctrl+Shift+N";
static const char default_new_tab[] = "Ctrl+Shift+T";
static const char default_close_tab[] = "Ctrl+Shift+W";
static const char default_next_tab[] = "Ctrl+Tab";
static const char default_previous_tab[] = "Ctrl+Shift+Tab";

typedef struct {
    xkb_keysym_t key;
    unsigned int modifiers;
} Shortcut;

static xkb_keysym_t shortcut_key(xkb_keysym_t key)
{
    if (key >= XKB_KEY_A && key <= XKB_KEY_Z) return XKB_KEY_a + (key - XKB_KEY_A);
    if (key == XKB_KEY_plus) return XKB_KEY_equal;
    if (key == XKB_KEY_underscore) return XKB_KEY_minus;
    return key;
}

static unsigned int modifier_parse(const char *text, size_t length)
{
    if (length == 4 && memcmp(text, "Ctrl", 4) == 0) return MOD_CTRL;
    if (length == 5 && memcmp(text, "Shift", 5) == 0) return MOD_SHIFT;
    if (length == 3 && memcmp(text, "Alt", 3) == 0) return MOD_ALT;
    return 0;
}

static bool shortcut_parse(const char *text, Shortcut *shortcut, bool allow_bare)
{
    unsigned int modifiers = 0;
    const char *part;
    while ((part = strchr(text, '+')) != NULL && part != text) {
        unsigned int modifier = modifier_parse(text, (size_t)(part - text));
        if (modifier == 0 || (modifiers & modifier)) return false;
        modifiers |= modifier;
        text = part + 1;
    }
    if ((!allow_bare && modifiers == 0) || *text == '\0') return false;
    unsigned char first = (unsigned char)*text;
    xkb_keysym_t key = first > ' ' && first <= '~' && text[1] == '\0' ? first : xkb_keysym_from_name(text, XKB_KEYSYM_NO_FLAGS);
    if (key == XKB_KEY_NoSymbol) return false;
    *shortcut = (Shortcut){shortcut_key(key), modifiers};
    return true;
}

typedef struct {
    int horizontal, vertical;
} Padding;

typedef struct {
    const char *config_path, *directory, *title, *app_id;
    char **command;
    int columns, rows;
    bool diagnostics;
} Options;

typedef struct {
    char *title;
    char *app_id;
    char *shell;
    char *font_family;
    const Options *options;
    bool custom_term, audible_bell, visual_bell, bell_urgency;
    int font_size, letter_spacing, line_spacing;
    int zoom_min, zoom_max, zoom_step;
    Padding padding;
    int columns, rows;
    int history_capacity;
    int scroll_lines, click_interval;
    float opacity;
    int blink_interval, cursor_thickness;
    TerminalColor foreground;
    TerminalColor background;
    TerminalColor cursor;
    TerminalColor selection_foreground;
    TerminalColor selection_background;
    TerminalColor palette[16];
    unsigned int mouse_override, link_modifier;
    CursorStyle cursor_style;
    bool blink, decorations;
    bool scroll_on_input, primary_selection, bold_bright, osc52;
    bool theme_wallpaper; /* [theme] source = wallpaper */
    bool theme_loaded; /* The colors came from a wallpaper palette. */
    uint16_t palette_set; /* [palette] entries given in the file, one bit per index. */
    Shortcut zoom_in, zoom_out, zoom_reset, reload, fullscreen, new_window;
    Shortcut new_tab, close_tab, next_tab, previous_tab;
    bool dynamic_title, mouse_reporting, hyperlinks;
} Config;

static Config config_defaults(void)
{
    Config defaults = {.padding = {DEFAULT_PADDING, DEFAULT_PADDING}, .font_size = DEFAULT_FONT_SIZE,
                    .zoom_min = MIN_FONT_SIZE, .zoom_max = MAX_FONT_SIZE, .zoom_step = 1,
                    .columns = INITIAL_COLUMNS, .rows = INITIAL_ROWS, .decorations = true, .opacity = 1.0f,
                    .blink_interval = DEFAULT_BLINK_INTERVAL, .cursor_thickness = DEFAULT_CURSOR_THICKNESS,
                    .history_capacity = 10000, .scroll_on_input = true, .scroll_lines = 3,
                    .click_interval = 350, .primary_selection = true, .bold_bright = true, .osc52 = true,
                    .foreground = blank_cell.foreground,
                    .background = blank_cell.background, .cursor = blank_cell.foreground,
                    .cursor_style = CURSOR_UNDERLINE, .selection_foreground = {16, 22, 30},
                    .selection_background = {138, 193, 222},
                    .dynamic_title = true, .mouse_reporting = true, .hyperlinks = true,
                    .mouse_override = MOD_SHIFT, .link_modifier = MOD_CTRL};
    memcpy(defaults.palette, ansi_colors, sizeof defaults.palette);
    shortcut_parse(default_zoom_in, &defaults.zoom_in, false);
    shortcut_parse(default_zoom_out, &defaults.zoom_out, false);
    shortcut_parse(default_zoom_reset, &defaults.zoom_reset, false);
    shortcut_parse(default_reload, &defaults.reload, false);
    shortcut_parse(default_fullscreen, &defaults.fullscreen, true);
    shortcut_parse(default_new_window, &defaults.new_window, true);
    shortcut_parse(default_new_tab, &defaults.new_tab, true);
    shortcut_parse(default_close_tab, &defaults.close_tab, true);
    shortcut_parse(default_next_tab, &defaults.next_tab, true);
    shortcut_parse(default_previous_tab, &defaults.previous_tab, true);
    return defaults;
}

static TerminalColor cell_color(const Config *config, uint8_t source, TerminalColor rgb)
{
    switch (source & ~SOURCE_DIM) {
    case 0: return rgb;
    case SOURCE_FG: rgb = config->foreground; break;
    case SOURCE_BG: rgb = config->background; break;
    default: rgb = config->palette[(source & ~SOURCE_DIM) - SOURCE_ANSI]; break;
    }
    if (source & SOURCE_DIM) {
        rgb.red /= 2;
        rgb.green /= 2;
        rgb.blue /= 2;
    }
    return rgb;
}

static void config_destroy(Config *config)
{
    free(config->title);
    free(config->app_id);
    free(config->shell);
    free(config->font_family);
}

static char *config_path(const char *filename, bool quiet)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *suffix = "/deal/";
    if (base == NULL || base[0] != '/') {
        if (!quiet && base != NULL && *base != '\0') {
            fprintf(stderr, "deal: ignoring non-absolute XDG_CONFIG_HOME\n");
        }
        base = getenv("HOME");
        suffix = "/.config/deal/";
        if (base == NULL || *base == '\0') {
            return NULL;
        }
        if (base[0] != '/') {
            if (!quiet) fprintf(stderr, "deal: ignoring non-absolute HOME for configuration\n");
            return NULL;
        }
    }
    char *path;
    if (asprintf(&path, "%s%s%s", base, suffix, filename) < 0) {
        fprintf(stderr, "deal: could not allocate the configuration path\n");
        return NULL;
    }
    return path;
}

static int config_make_parents(char *path)
{
    for (char *slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        int result = mkdir(path, 0700);
        *slash = '/';
        if (result < 0 && errno != EEXIST) return -1;
    }
    return 0;
}

static int config_write_file(char *path, int (*write_contents)(FILE *, const void *),
                             const void *data)
{
    if (config_make_parents(path) < 0) return -1;
    char *temporary;
    if (asprintf(&temporary, "%s.XXXXXX", path) < 0) return -1;
    int error = 0;
    int fd = mkstemp(temporary);
    if (fd < 0) {
        error = errno;
    } else {
        FILE *file = fdopen(fd, "w");
        if (file == NULL) {
            error = errno;
            close(fd);
        } else {
            if (write_contents(file, data) < 0) error = errno;
            if (fclose(file) == EOF && error == 0) error = errno;
            if (error == 0 && rename(temporary, path) < 0) error = errno;
        }
        unlink(temporary);
    }
    free(temporary);
    if (error == 0) return 0;
    errno = error;
    return -1;
}

typedef struct {
    int configured, size;
} SavedZoom;

static int config_write_zoom(FILE *file, const void *data)
{
    const SavedZoom *zoom = data;
    return fprintf(file, "%d:%d\n", zoom->configured, zoom->size);
}

static int config_zoom_size(const Config *config, int64_t size)
{
    if (size < config->zoom_min) return config->zoom_min;
    if (size > config->zoom_max) return config->zoom_max;
    return (int)size;
}

static int config_read_zoom(const char *path, const Config *config)
{
    int configured = config->font_size;
    if (path == NULL) return configured;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return configured;
    struct stat attributes;
    FILE *file = fstat(fd, &attributes) == 0 && S_ISREG(attributes.st_mode) && attributes.st_size <= 32
        ? fdopen(fd, "r") : NULL;
    if (file == NULL) {
        close(fd);
        return configured;
    }
    SavedZoom zoom;
    char extra;
    bool valid = fscanf(file, "%3d:%3d %c", &zoom.configured, &zoom.size, &extra) == 2 &&
                 zoom.configured == configured && zoom.size >= MIN_FONT_SIZE && zoom.size <= MAX_FONT_SIZE;
    fclose(file);
    return valid ? config_zoom_size(config, zoom.size) : configured;
}

static void config_save_zoom(char *path, int configured, int size)
{
    SavedZoom zoom = {configured, size};
    if (path != NULL && config_write_file(path, config_write_zoom, &zoom) < 0) {
        fprintf(stderr, "deal: could not save zoom in %s: %s\n", path, strerror(errno));
    }
}

static int hex_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static bool config_parse_color(const char *value, size_t length, TerminalColor *color)
{
    if (length != 7 || value[0] != '#') return false;
    unsigned char components[3];
    for (size_t i = 0; i < 3; ++i) {
        int high = hex_digit((unsigned char)value[1 + i * 2]);
        int low = hex_digit((unsigned char)value[2 + i * 2]);
        if (high < 0 || low < 0) return false;
        components[i] = (unsigned char)(high * 16 + low);
    }
    *color = (TerminalColor){components[0], components[1], components[2]};
    return true;
}

enum ConfigType {
    CFG_INT, CFG_BOOL, CFG_STRING, CFG_SHELL, CFG_COLOR, CFG_OPACITY,
    CFG_PADDING, CFG_SHORTCUT, CFG_OPTIONAL_SHORTCUT, CFG_MODIFIER, CFG_CURSOR, CFG_TERM, CFG_THEME
};

typedef struct {
    const char *section, *key;
    enum ConfigType type;
    size_t offset;
    int low, high;
} ConfigKey;

#define SETTING(section, key, type, field) {section, key, type, offsetof(Config, field), 0, 0}
#define INTEGER(section, key, field, low, high) {section, key, CFG_INT, offsetof(Config, field), low, high}
static const ConfigKey config_keys[] = {
    SETTING("window", "title", CFG_STRING, title),
    SETTING("window", "app_id", CFG_STRING, app_id),
    SETTING("window", "dynamic_title", CFG_BOOL, dynamic_title),
    SETTING("window", "padding", CFG_PADDING, padding),
    INTEGER("window", "padding_horizontal", padding.horizontal, 0, 128),
    INTEGER("window", "padding_vertical", padding.vertical, 0, 128),
    SETTING("window", "opacity", CFG_OPACITY, opacity),
    SETTING("window", "decorations", CFG_BOOL, decorations),
    INTEGER("window", "columns", columns, 1, 512),
    INTEGER("window", "rows", rows, 1, 256),
    SETTING("font", "family", CFG_STRING, font_family),
    INTEGER("font", "size", font_size, MIN_FONT_SIZE, MAX_FONT_SIZE),
    INTEGER("font", "zoom_min", zoom_min, MIN_FONT_SIZE, MAX_FONT_SIZE),
    INTEGER("font", "zoom_max", zoom_max, MIN_FONT_SIZE, MAX_FONT_SIZE),
    INTEGER("font", "zoom_step", zoom_step, 1, MAX_FONT_SIZE - MIN_FONT_SIZE),
    INTEGER("font", "letter_spacing", letter_spacing, -16, 32),
    INTEGER("font", "line_spacing", line_spacing, -32, 64),
    SETTING("cursor", "style", CFG_CURSOR, cursor_style),
    SETTING("cursor", "blink", CFG_BOOL, blink),
    INTEGER("cursor", "blink_interval", blink_interval, 50, 5000),
    INTEGER("cursor", "thickness", cursor_thickness, 1, 32),
    SETTING("colors", "foreground", CFG_COLOR, foreground),
    SETTING("colors", "background", CFG_COLOR, background),
    SETTING("colors", "cursor", CFG_COLOR, cursor),
    SETTING("colors", "bold_is_bright", CFG_BOOL, bold_bright),
    SETTING("selection", "foreground", CFG_COLOR, selection_foreground),
    SETTING("selection", "background", CFG_COLOR, selection_background),
    SETTING("selection", "primary", CFG_BOOL, primary_selection),
    INTEGER("selection", "click_interval", click_interval, 100, 1000),
    SETTING("selection", "osc52", CFG_BOOL, osc52),
    SETTING("terminal", "shell", CFG_SHELL, shell),
    SETTING("terminal", "term", CFG_TERM, custom_term),
    SETTING("theme", "source", CFG_THEME, theme_wallpaper),
    INTEGER("scrollback", "capacity", history_capacity, 0, 1000000),
    SETTING("scrollback", "scroll_on_input", CFG_BOOL, scroll_on_input),
    INTEGER("scrollback", "scroll_lines", scroll_lines, 1, 100),
    SETTING("mouse", "reporting", CFG_BOOL, mouse_reporting),
    SETTING("mouse", "selection_override", CFG_MODIFIER, mouse_override),
    SETTING("hyperlinks", "enabled", CFG_BOOL, hyperlinks),
    SETTING("hyperlinks", "modifier", CFG_MODIFIER, link_modifier),
    SETTING("bell", "audible", CFG_BOOL, audible_bell),
    SETTING("bell", "visual", CFG_BOOL, visual_bell),
    SETTING("bell", "urgency", CFG_BOOL, bell_urgency),
    SETTING("shortcuts", "zoom_in", CFG_SHORTCUT, zoom_in),
    SETTING("shortcuts", "zoom_out", CFG_SHORTCUT, zoom_out),
    SETTING("shortcuts", "zoom_reset", CFG_SHORTCUT, zoom_reset),
    SETTING("keybinds", "reload_config", CFG_SHORTCUT, reload),
    SETTING("keybinds", "fullscreen", CFG_OPTIONAL_SHORTCUT, fullscreen),
    SETTING("keybinds", "new_window", CFG_OPTIONAL_SHORTCUT, new_window),
    SETTING("keybinds", "new_tab", CFG_OPTIONAL_SHORTCUT, new_tab),
    SETTING("keybinds", "close_tab", CFG_OPTIONAL_SHORTCUT, close_tab),
    SETTING("keybinds", "next_tab", CFG_OPTIONAL_SHORTCUT, next_tab),
    SETTING("keybinds", "previous_tab", CFG_OPTIONAL_SHORTCUT, previous_tab),
};
#undef INTEGER
#undef SETTING

static bool osc_text_valid(const char *text, size_t length, bool url);

static char *config_trim(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}

static bool config_parse_int(const char *text, int low, int high, int *number)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < low || value > high) return false;
    *number = (int)value;
    return true;
}

static const char *config_set_value(const ConfigKey *key, Config *config, const char *value,
                                    char expected[80])
{
    void *field = (char *)config + key->offset;
    size_t length = strlen(value);
    switch (key->type) {
    case CFG_INT:
        if (config_parse_int(value, key->low, key->high, field)) return NULL;
        snprintf(expected, 80, "expected an integer from %d to %d", key->low, key->high);
        return expected;
    case CFG_BOOL:
        if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
            *(bool *)field = value[0] == 't';
            return NULL;
        }
        return "expected true or false";
    case CFG_STRING:
    case CFG_SHELL: {
        bool shell = key->type == CFG_SHELL;
        if (shell) {
            struct stat st;
            if (length >= PATH_MAX || value[0] != '/' || stat(value, &st) < 0 ||
                !S_ISREG(st.st_mode) || access(value, X_OK) < 0)
                return "expected an absolute executable path";
        } else if (length == 0 || length > TITLE_LIMIT || !osc_text_valid(value, length, false)) {
            return "expected a nonempty UTF-8 string up to 512 bytes";
        }
        char *copy = strdup(value);
        if (copy == NULL) return "out of memory";
        free(*(char **)field);
        *(char **)field = copy;
        return NULL;
    }
    case CFG_COLOR:
        return config_parse_color(value, length, field) ? NULL : "expected a #RRGGBB color";
    case CFG_OPACITY: {
        char *end;
        errno = 0;
        double number = strtod(value, &end);
        if (errno || end == value || *end || !(number >= 0.0 && number <= 1.0))
            return "expected a number from 0.0 to 1.0";
        *(float *)field = (float)number;
        return NULL;
    }
    case CFG_PADDING: {
        int pixels;
        if (!config_parse_int(value, 0, 128, &pixels)) return "expected an integer from 0 to 128";
        *(Padding *)field = (Padding){pixels, pixels};
        return NULL;
    }
    case CFG_SHORTCUT:
    case CFG_OPTIONAL_SHORTCUT:
        if (key->type == CFG_OPTIONAL_SHORTCUT && strcmp(value, "false") == 0) {
            *(Shortcut *)field = (Shortcut){0};
            return NULL;
        }
        if (length <= 64 && shortcut_parse(value, field, key->type == CFG_OPTIONAL_SHORTCUT))
            return NULL;
        return key->type == CFG_OPTIONAL_SHORTCUT ? "expected a shortcut such as F11, or false"
                                                   : "expected a shortcut such as Ctrl+Shift+R";
    case CFG_MODIFIER: {
        unsigned int modifier = modifier_parse(value, length);
        if (!modifier) return "expected Shift, Alt, or Ctrl";
        *(unsigned int *)field = modifier;
        return NULL;
    }
    case CFG_CURSOR:
        for (size_t i = 0; i < sizeof cursor_style_names / sizeof *cursor_style_names; ++i) {
            if (strcmp(value, cursor_style_names[i]) == 0) {
                *(CursorStyle *)field = (CursorStyle)i;
                return NULL;
            }
        }
        return "expected block, underline, or bar";
    case CFG_THEME:
        if (strcmp(value, "manual") != 0 && strcmp(value, "wallpaper") != 0)
            return "expected manual or wallpaper";
        *(bool *)field = strcmp(value, "wallpaper") == 0;
        return NULL;
    case CFG_TERM:
        if (strcmp(value, "deal") != 0 && strcmp(value, compatibility_term) != 0)
            return "expected deal or xterm-color";
        *(bool *)field = strcmp(value, "deal") == 0;
        return NULL;
    }
    return "unknown setting type";
}

static bool config_parse_file(FILE *file, Config *config, const char *path)
{
    char line[PATH_MAX + 256], section[32] = "";
    size_t number = 0, zoom_bounds_line = 0;
    for (;;) {
        size_t length = 0;
        int byte;
        const char *error = NULL;
        char expected[80];
        while ((byte = fgetc(file)) != EOF && byte != '\n') {
            if (byte == 0 || length == sizeof line - 1) {
                error = "line is too long or contains a NUL byte";
                break;
            }
            line[length++] = (char)byte;
        }
        if (ferror(file)) error = "could not read configuration";
        if (byte == EOF && length == 0 && error == NULL) break;
        ++number;
        line[length] = '\0';
        char *key = config_trim(line), *value = NULL;
        if (error != NULL) goto invalid;
        if (*key == '\0' || *key == '#' || *key == ';') continue;
        if (*key == '[') {
            char *end = strrchr(key, ']');
            if (end == NULL || end[1] != '\0') {
                error = "expected [section]";
                goto invalid;
            }
            *end = '\0';
            key = config_trim(key + 1);
            bool known = strcmp(key, "palette") == 0;
            for (size_t i = 0; i < sizeof config_keys / sizeof *config_keys; ++i)
                known |= strcmp(key, config_keys[i].section) == 0;
            if (!known) {
                error = "unknown section";
                goto invalid;
            }
            snprintf(section, sizeof section, "%s", key);
            continue;
        }
        value = strchr(key, '=');
        if (!value || !*section) {
            error = "expected key = value inside a section";
            goto invalid;
        }
        *value++ = '\0';
        key = config_trim(key);
        value = config_trim(value);
        if (*value == '"' || *value == '\'') {
            length = strlen(value);
            if (length < 2 || value[length - 1] != value[0]) {
                error = "unterminated quoted value";
                goto invalid;
            }
            value[length - 1] = '\0';
            ++value;
        }
        if (strcmp(section, "palette") == 0) {
            int index;
            if (!config_parse_int(key, 1, 16, &index)) error = "expected a palette index from 1 to 16";
            else if (config->palette_set |= (uint16_t)(1u << (index - 1)),
                     !config_parse_color(value, strlen(value), &config->palette[index - 1]))
                error = "expected a #RRGGBB color";
        } else {
            error = "unknown setting";
            for (size_t i = 0; i < sizeof config_keys / sizeof *config_keys; ++i) {
                if (strcmp(section, config_keys[i].section) == 0 && strcmp(key, config_keys[i].key) == 0) {
                    error = config_set_value(&config_keys[i], config, value, expected);
                    if (config_keys[i].offset == offsetof(Config, font_size) ||
                        config_keys[i].offset == offsetof(Config, zoom_min) ||
                        config_keys[i].offset == offsetof(Config, zoom_max))
                        zoom_bounds_line = number;
                    break;
                }
            }
        }
        if (error == NULL) continue;
invalid:
        fprintf(stderr, "deal: %s:%zu: %s%s%s: %s\n", path, number, section,
                *section ? "." : "", key, error);
        return false;
    }
    if (config->font_size < config->zoom_min || config->font_size > config->zoom_max) {
        fprintf(stderr, "deal: %s:%zu: [font] requires zoom_min <= size <= zoom_max\n", path, zoom_bounds_line);
        return false;
    }
    return true;
}

static bool config_read(Config *config, bool startup)
{
    const char *explicit_path = config->options->config_path;
    char *default_path = explicit_path == NULL ? config_path("deal.conf", false) : NULL;
    const char *path = explicit_path != NULL ? explicit_path : default_path;
    if (path == NULL) return startup;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    bool okay = false;
    if (fd < 0) {
        okay = startup && explicit_path == NULL && errno == ENOENT;
        if (!okay) fprintf(stderr, "deal: %s: %s\n", path, strerror(errno));
    } else {
        struct stat attributes;
        if (fstat(fd, &attributes) < 0 || !S_ISREG(attributes.st_mode)) {
            fprintf(stderr, "deal: %s: configuration must be a readable regular file\n", path);
            close(fd);
        } else {
            FILE *file = fdopen(fd, "r");
            if (file == NULL) {
                fprintf(stderr, "deal: %s: %s\n", path, strerror(errno));
                close(fd);
            } else {
                okay = config_parse_file(file, config, path);
                fclose(file);
            }
        }
    }
    free(default_path);
    return okay;
}

static void config_overrides(Config *config)
{
    const Options *options = config->options;
    if (options == NULL) return;
    if (options->columns != 0) config->columns = options->columns;
    if (options->rows != 0) config->rows = options->rows;
}

/* Wallpaper theme. wall publishes ten color roles. The sixteen ANSI colors
   keep their usual hues, so red still reads as an error, with lightness and
   chroma chosen in Oklab for contrast against the wallpaper's background.
   The ANSI slot nearest the accent's hue takes the accent's hue instead. */

typedef struct {
    double l, a, b;
} Oklab;

static double srgb_linear(double c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double linear_srgb(double c)
{
    return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

static Oklab oklab_from_color(TerminalColor color)
{
    double r = srgb_linear(color.red / 255.0), g = srgb_linear(color.green / 255.0),
           b = srgb_linear(color.blue / 255.0);
    double l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    double m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    double s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return (Oklab){0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                   1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                   0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

/* Convert to linear sRGB and report whether the color is inside the gamut. */
static bool oklab_linear(Oklab color, double rgb[3])
{
    double l = color.l + 0.3963377774 * color.a + 0.2158037573 * color.b;
    double m = color.l - 0.1055613458 * color.a - 0.0638541728 * color.b;
    double s = color.l - 0.0894841775 * color.a - 1.2914855480 * color.b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    rgb[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    rgb[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    rgb[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    for (int i = 0; i < 3; ++i) if (rgb[i] < -0.0001 || rgb[i] > 1.0001) return false;
    return true;
}

static TerminalColor color_from_oklab(Oklab color)
{
    double rgb[3];
    oklab_linear(color, rgb);
    unsigned char out[3];
    for (int i = 0; i < 3; ++i) {
        double value = linear_srgb(rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i]);
        out[i] = (unsigned char)(value * 255.0 + 0.5);
    }
    return (TerminalColor){out[0], out[1], out[2]};
}

static double color_luminance(TerminalColor color)
{
    return 0.2126 * srgb_linear(color.red / 255.0) + 0.7152 * srgb_linear(color.green / 255.0) +
           0.0722 * srgb_linear(color.blue / 255.0);
}

static double color_contrast(TerminalColor a, TerminalColor b)
{
    double x = color_luminance(a) + 0.05, y = color_luminance(b) + 0.05;
    return x > y ? x / y : y / x;
}

/* The most saturated color with this lightness and hue, up to chroma. */
static TerminalColor oklch_color(double lightness, double chroma, double hue)
{
    double radians = hue * 3.14159265358979323846 / 180.0, rgb[3];
    double low = 0, high = chroma;
    if (!oklab_linear((Oklab){lightness, chroma * cos(radians), chroma * sin(radians)}, rgb)) {
        for (int i = 0; i < 16; ++i) {
            double middle = (low + high) / 2;
            if (oklab_linear((Oklab){lightness, middle * cos(radians), middle * sin(radians)}, rgb)) low = middle;
            else high = middle;
        }
        chroma = low;
    }
    return color_from_oklab((Oklab){lightness, chroma * cos(radians), chroma * sin(radians)});
}

/* Move lightness away from the background until the color reaches target. */
static TerminalColor theme_hue(double hue, double chroma, double lightness, TerminalColor background,
                               bool dark, double target, double *chosen)
{
    TerminalColor color = oklch_color(lightness, chroma, hue);
    while (color_contrast(color, background) < target && lightness > 0.02 && lightness < 0.98) {
        lightness += dark ? 0.01 : -0.01;
        color = oklch_color(lightness, chroma, hue);
    }
    if (chosen != NULL) *chosen = lightness;
    return color;
}

static TerminalColor rgb_color(uint32_t rgb)
{
    return (TerminalColor){(unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8), (unsigned char)rgb};
}

static TerminalColor oklab_shift(TerminalColor color, double amount)
{
    Oklab value = oklab_from_color(color);
    value.l += amount;
    return color_from_oklab(value);
}

static void theme_apply(Config *config, const DeskPalette *palette)
{
    TerminalColor background = rgb_color(palette->rgb[DESK_BACKGROUND]);
    TerminalColor foreground = rgb_color(palette->rgb[DESK_FOREGROUND]);
    TerminalColor accent = rgb_color(palette->rgb[DESK_ACCENT]);
    TerminalColor muted = rgb_color(palette->rgb[DESK_MUTED]);
    bool dark = oklab_from_color(background).l < 0.5;
    config->background = background;
    config->foreground = foreground;
    config->cursor = accent;
    config->selection_background = rgb_color(palette->rgb[DESK_SELECTION_BACKGROUND]);
    config->selection_foreground = rgb_color(palette->rgb[DESK_SELECTION_FOREGROUND]);

    /* Red, green, yellow, blue, magenta and cyan as Oklab hues in degrees. */
    double hues[6] = {27, 145, 95, 255, 330, 200};
    double chroma[6] = {0.13, 0.13, 0.13, 0.13, 0.13, 0.11};
    Oklab tone = oklab_from_color(accent);
    double accent_chroma = hypot(tone.a, tone.b);
    if (accent_chroma >= 0.06) {
        double accent_hue = atan2(tone.b, tone.a) * 180.0 / 3.14159265358979323846;
        if (accent_hue < 0) accent_hue += 360;
        int nearest = 0;
        double distance = 360;
        for (int i = 0; i < 6; ++i) {
            double gap = fabs(hues[i] - accent_hue);
            if (gap > 180) gap = 360 - gap;
            if (gap < distance) {
                distance = gap;
                nearest = i;
            }
        }
        if (distance < 35) {
            hues[nearest] = accent_hue;
            if (accent_chroma > chroma[nearest]) chroma[nearest] = accent_chroma < 0.18 ? accent_chroma : 0.18;
        }
    }

    TerminalColor ansi[16];
    for (int i = 0; i < 6; ++i) {
        double lightness;
        ansi[1 + i] = theme_hue(hues[i], chroma[i], dark ? 0.68 : 0.52, background, dark, 4.5, &lightness);
        ansi[9 + i] = theme_hue(hues[i], chroma[i] + 0.02, lightness + (dark ? 0.08 : -0.06), background, dark,
                                5.5, NULL);
    }
    if (dark) {
        ansi[0] = oklab_shift(background, 0.08);
        ansi[7] = oklab_shift(foreground, -0.10);
        ansi[15] = foreground;
    } else {
        ansi[0] = foreground;
        ansi[7] = oklab_shift(background, -0.12);
        ansi[15] = oklab_shift(background, -0.05);
    }
    ansi[8] = muted;
    for (int i = 0; i < 16; ++i) {
        if (!(config->palette_set & (1u << i))) config->palette[i] = ansi[i];
    }
}

/* Replace the configured colors with the wallpaper palette when enabled.
   Return false when the palette is missing or invalid. */
static bool config_theme(Config *config)
{
    if (!config->theme_wallpaper) return true;
    char *path = desk_palette_path();
    DeskPalette palette;
    int error = desk_palette_load(path, &palette);
    if (error == 0) theme_apply(config, &palette);
    else fprintf(stderr, "deal: %s: %s, keeping the current colors\n", path != NULL ? path : "wallpaper palette",
                 error == EINVAL ? "not a complete wallpaper palette" : strerror(error));
    free(path);
    return error == 0;
}

/* After a failed palette load, keep the wallpaper colors already in use
   rather than falling back to deal.conf. Explicit [palette] entries still apply. */
static void config_keep_theme(Config *next, const Config *current)
{
    next->background = current->background;
    next->foreground = current->foreground;
    next->cursor = current->cursor;
    next->selection_background = current->selection_background;
    next->selection_foreground = current->selection_foreground;
    for (int i = 0; i < 16; ++i) {
        if (!(next->palette_set & (1u << i))) next->palette[i] = current->palette[i];
    }
}

static bool config_load(Config *config, const Options *options)
{
    *config = config_defaults();
    config->options = options;
    if (!config_read(config, true)) {
        config_destroy(config);
        *config = config_defaults();
        config->options = options;
        if (options->config_path != NULL) return false;
    }
    config_overrides(config);
    config->theme_loaded = config->theme_wallpaper && config_theme(config);
    return true;
}

/* Scrollback and viewport */

typedef struct {
    TerminalCell *cells;
    size_t columns;
    size_t stored;
    bool wrapped, has_links;
} HistoryLine;

typedef struct {
    HistoryLine *lines;
    size_t capacity, head, count, offset, stored_cells;
    uint64_t first_live;
} History;

typedef struct {
    uint64_t line;
    size_t column;
} SelectionPoint;

typedef struct {
    SelectionPoint first, end;
} TextRange;

typedef enum { SELECT_CHARACTER, SELECT_WORD, SELECT_LINE } SelectionMode;

typedef struct {
    TextRange anchor, range;
    SelectionPoint origin;
    SelectionMode mode;
    bool dragging, active, alternate;
} Selection;

typedef struct {
    const TerminalCell *cells;
    size_t columns;
    bool wrapped;
    size_t stored;
} LineView;

typedef struct {
    const TerminalScreen *screen;
    const History *history;
    const Selection *selection;
    uint64_t first_live, first_visible;
    TerminalCell blank;
    uint16_t hovered_link;
} TerminalView;

static void history_clear(History *history)
{
    for (size_t i = 0; i < history->count; ++i) {
        HistoryLine *line = &history->lines[(history->head + i) % history->capacity];
        free(line->cells);
        *line = (HistoryLine){0};
    }
    history->head = history->count = history->offset = history->stored_cells = 0;
}

static bool color_equal(TerminalColor a, TerminalColor b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static bool default_blank(const TerminalCell *cell, TerminalCell blank)
{
    return !cell->written && cell->character == ' ' && cell->width == 1 && !cell->underline &&
           !cell->strikethrough && !cell->overline &&
           !cell->combining[0] && !cell->combining[1] && !cell->hyperlink &&
           cell->foreground_source == blank.foreground_source &&
           cell->background_source == blank.background_source &&
           (cell->foreground_source || color_equal(cell->foreground, blank.foreground)) &&
           (cell->background_source || color_equal(cell->background, blank.background));
}

static size_t line_content(const TerminalCell *cells, size_t length, TerminalCell blank)
{
    while (length > 0 && default_blank(&cells[length - 1], blank)) --length;
    return length;
}

static bool history_write(History *history, const TerminalCell *source, size_t columns,
                          size_t stored, bool wrapped, bool track_links)
{
    if (history->capacity == 0) return true;
    size_t slot = (history->head + history->count) % history->capacity;
    if (history->count == history->capacity) history->head = (history->head + 1) % history->capacity;
    else ++history->count;
    HistoryLine *line = &history->lines[slot];
    history->stored_cells -= line->stored;
    bool okay = true;
    if (stored == 0) {
        free(line->cells);
        line->cells = NULL;
    } else if (stored != line->stored) {
        TerminalCell *storage = realloc(line->cells, stored * sizeof(*storage));
        if (storage != NULL) line->cells = storage;
        else if (stored > line->stored) {
            free(line->cells);
            line->cells = NULL;
            stored = 0;
            okay = false;
        }
    }
    if (stored != 0) memcpy(line->cells, source, stored * sizeof(*line->cells));
    line->columns = columns;
    line->stored = stored;
    line->wrapped = wrapped;
    line->has_links = false;
    if (track_links) {
        for (size_t i = 0; i < stored && !line->has_links; ++i) {
            line->has_links = source[i].hyperlink != 0;
        }
    }
    history->stored_cells += stored;
    const size_t limit = (64u * 1024u * 1024u) / sizeof(TerminalCell);
    while (history->stored_cells > limit) {
        HistoryLine *oldest = &history->lines[history->head];
        history->stored_cells -= oldest->stored;
        free(oldest->cells);
        *oldest = (HistoryLine){0};
        history->head = (history->head + 1) % history->capacity;
        --history->count;
    }
    return okay;
}

static void history_push(History *history, const TerminalScreen *screen, TerminalCell blank, bool track_links)
{
    ++history->first_live;
    if (history->capacity == 0) return;
    const TerminalCell *line = screen_row(screen, 0);
    size_t stored = line_content(line, screen->columns, blank);
    history_write(history, line, screen->columns, stored, *screen_wrap(screen, 0), track_links);
    if (history->offset > 0) {
        if (history->offset < history->count) ++history->offset;
        else history->offset = history->count;
    }
}

static LineView view_line(const TerminalView *view, uint64_t line)
{
    if (line < view->first_live) {
        const History *history = view->history;
        uint64_t distance = view->first_live - line;
        if (history == NULL || distance > history->count) return (LineView){0};
        const HistoryLine *entry = &history->lines[(history->head + history->count - (size_t)distance) % history->capacity];
        return (LineView){entry->cells, entry->columns, entry->wrapped, entry->stored};
    }
    uint64_t index = line - view->first_live;
    if (index >= view->screen->rows) return (LineView){0};
    size_t row = (size_t)index;
    return (LineView){screen_row(view->screen, row), view->screen->columns,
                      *screen_wrap(view->screen, row), view->screen->columns};
}

static const TerminalCell *view_cell(const TerminalView *view, const LineView *line, size_t column)
{
    return column < line->stored ? &line->cells[column] : &view->blank;
}

static bool point_before(SelectionPoint a, SelectionPoint b)
{
    return a.line < b.line || (a.line == b.line && a.column < b.column);
}

static void view_selection_columns(const TerminalView *view, uint64_t line, size_t columns,
                                   size_t *first, size_t *end)
{
    *first = *end = 0;
    if (view->selection == NULL || !view->selection->active) return;
    TextRange range = view->selection->range;
    if (line < range.first.line || line > range.end.line) return;
    if (line == range.first.line) *first = range.first.column < columns ? range.first.column : columns;
    *end = (line == range.end.line && range.end.column < columns) ? range.end.column : columns;
}

/* System fonts and OpenGL rendering */

typedef struct {
    int width;
    int height;
    int baseline;
} FontMetrics;

typedef struct {
    int left, top, right, bottom;
} GlyphBounds;

typedef struct {
    FT_Face face;
    bool attempted;
    GlyphBounds box;
    GlyphBounds block;
} FontFace;

typedef struct {
    uint64_t used;
    uint32_t codepoint;
    unsigned char span, style;
    bool mark;
    int x, y, width, height;
} CachedGlyph;

/* A bold, italic or bold italic variant of the primary family. A missing face
   draws the regular face with FreeType synthesis instead. */
typedef struct {
    FontFace font;
    unsigned int synthesis;
    bool attempted;
} StyledFace;

typedef struct {
    FT_Library library;
    FcConfig *config;
    FcFontSet *matches;
    FontFace *faces;
    size_t primary;
    StyledFace styled[4];
    int size, x_height;
    FontMetrics metrics;
    GLuint atlas;
    int atlas_size, tile_columns;
    CachedGlyph *cache;
    size_t cache_size;
    uint64_t access;
    unsigned char *pixels;
    bool drawing;
} FontRenderer;

/* The atlas uses fixed tiles so an uncommon glyph can evict one cache entry
   without moving other glyphs or invalidating the whole screen. */
enum { GLYPH_CACHE_WAYS = 4, GLYPH_ATLAS_LIMIT = 2048 };

static GlyphBounds bitmap_bounds(FT_GlyphSlot slot)
{
    GlyphBounds bounds = {slot->bitmap_left, -slot->bitmap_top, 0, 0};
    bounds.right = bounds.left + (int)slot->bitmap.width;
    bounds.bottom = bounds.top + (int)slot->bitmap.rows;
    return bounds;
}

static bool font_load_glyph(FT_Face face, uint32_t codepoint, unsigned int synthesis)
{
    FT_UInt index = FT_Get_Char_Index(face, codepoint);
    if (index == 0) return false;
    if (synthesis == 0) {
        if (FT_Load_Glyph(face, index, FT_LOAD_RENDER) != 0) return false;
    } else {
        if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) != 0) return false;
        if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            if (synthesis & TERMINAL_STYLE_ITALIC) FT_GlyphSlot_Oblique(face->glyph);
            if (synthesis & TERMINAL_STYLE_BOLD) FT_GlyphSlot_Embolden(face->glyph);
        }
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return false;
    }
    const FT_Bitmap *bitmap = &face->glyph->bitmap;
    if (bitmap->width > 4096 || bitmap->rows > 4096 ||
        face->glyph->bitmap_left < -4096 || face->glyph->bitmap_left > 4096 ||
        face->glyph->bitmap_top < -4096 || face->glyph->bitmap_top > 4096) return false;
    if (bitmap->width == 0 || bitmap->rows == 0) return true;
    return bitmap->buffer != NULL &&
           (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY || bitmap->pixel_mode == FT_PIXEL_MODE_MONO);
}

static unsigned char glyph_alpha(const FT_Bitmap *bitmap, const unsigned char *row, int x)
{
    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) return row[x / 8] & (0x80u >> (x % 8)) ? 255 : 0;
    if (bitmap->num_grays > 1 && bitmap->num_grays < 256) {
        return (unsigned char)(255u * row[x] / (bitmap->num_grays - 1));
    }
    return row[x];
}

static GlyphBounds font_ink_bounds(FT_GlyphSlot slot)
{
    GlyphBounds bounds = bitmap_bounds(slot);
    const FT_Bitmap *bitmap = &slot->bitmap;
    if (bitmap->width == 0 || bitmap->rows == 0) return bounds;
    unsigned int peak = 0;
    for (int y = 0; y < (int)bitmap->rows; ++y) {
        const unsigned char *row = bitmap->buffer + (ptrdiff_t)y * bitmap->pitch;
        for (int x = 0; x < (int)bitmap->width; ++x) {
            unsigned int alpha = glyph_alpha(bitmap, row, x);
            if (alpha > peak) peak = alpha;
        }
    }
    if (peak == 0) return bounds;
    unsigned int threshold = (4 * peak + 4) / 5;
    GlyphBounds ink = {bounds.right, bounds.bottom, bounds.left, bounds.top};
    for (int y = 0; y < (int)bitmap->rows; ++y) {
        const unsigned char *row = bitmap->buffer + (ptrdiff_t)y * bitmap->pitch;
        for (int x = 0; x < (int)bitmap->width; ++x) {
            /* Faint edge pixels should not create seams between grid graphics. */
            if (glyph_alpha(bitmap, row, x) < threshold) continue;
            int left = bounds.left + x, top = bounds.top + y;
            if (left < ink.left) ink.left = left;
            if (left >= ink.right) ink.right = left + 1;
            if (top < ink.top) ink.top = top;
            if (top >= ink.bottom) ink.bottom = top + 1;
        }
    }
    return ink.right > ink.left && ink.bottom > ink.top ? ink : bounds;
}

static void font_graphic_bounds(FontFace *font)
{
    FT_Face face = font->face;
    if (font_load_glyph(face, 0x2500, 0)) {
        GlyphBounds line = font_ink_bounds(face->glyph);
        font->box.left = line.left;
        font->box.right = line.right;
    }
    if (font_load_glyph(face, 0x2502, 0)) {
        GlyphBounds line = font_ink_bounds(face->glyph);
        font->box.top = line.top;
        font->box.bottom = line.bottom;
    }
    if (font_load_glyph(face, 0x2588, 0)) font->block = font_ink_bounds(face->glyph);
}

static FontFace *font_open_face(FontRenderer *renderer, size_t index)
{
    FontFace *font = &renderer->faces[index];
    if (!font->attempted) {
        FcPattern *pattern = renderer->matches->fonts[index];
        FcChar8 *path;
        int face_index = 0;
        font->attempted = true;
        if (FcPatternGetString(pattern, FC_FILE, 0, &path) != FcResultMatch) return NULL;
        FcPatternGetInteger(pattern, FC_INDEX, 0, &face_index);
        if (FT_New_Face(renderer->library, (const char *)path, face_index, &font->face) != 0) {
            font->face = NULL;
        } else if (FT_Select_Charmap(font->face, FT_ENCODING_UNICODE) != 0 ||
                   FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)renderer->size) != 0) {
            FT_Done_Face(font->face);
            font->face = NULL;
        } else {
            font_graphic_bounds(font);
        }
    }
    return font->face != NULL ? font : NULL;
}

static bool font_measure(FT_Face face, FontMetrics *metrics)
{
    FT_Long advance = 0;
    for (uint32_t cp = FIRST_GLYPH; cp <= LAST_GLYPH; ++cp) {
        FT_UInt index = FT_Get_Char_Index(face, cp);
        if (index == 0 || FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) != 0) return false;
        if (face->glyph->advance.x > advance) advance = face->glyph->advance.x;
    }
    FT_Size_Metrics size = face->size->metrics;
    if (advance <= 0 || advance > 256 * 64 || size.ascender <= 0 || size.ascender > 256 * 64 ||
        size.descender > 0 || size.descender < -256 * 64 || size.height > 512 * 64) return false;
    int ascent = (int)((size.ascender + 63) / 64);
    int descent = (int)((-size.descender + 63) / 64);
    int height = (int)((size.height + 63) / 64);
    if (height < ascent + descent) height = ascent + descent;
    *metrics = (FontMetrics){(int)((advance + 63) / 64), height, ascent + (height - ascent - descent) / 2};
    return true;
}

static const char font_spacing_error[] = "font spacing leaves invalid cell dimensions";

static bool font_apply_spacing(FontMetrics *metrics, const Config *config)
{
    FontMetrics adjusted = {
        .width = metrics->width + config->letter_spacing,
        .height = metrics->height + config->line_spacing,
        .baseline = metrics->baseline + config->line_spacing / 2
    };
    if (adjusted.width < 1 || adjusted.height < 1 ||
        adjusted.baseline < 0 || adjusted.baseline >= adjusted.height) return false;
    *metrics = adjusted;
    return true;
}

static const char *font_open(FontRenderer *renderer, const Config *config, FcConfig *font_config)
{
    renderer->size = config->font_size;
    if (FT_Init_FreeType(&renderer->library) != 0) return "could not initialize FreeType";
    renderer->config = font_config != NULL ? FcConfigReference(font_config) : FcInitLoadConfigAndFonts();
    if (renderer->config == NULL) return "could not initialize Fontconfig";
    FcPattern *pattern = FcPatternCreate();
    if (pattern == NULL) return "could not allocate a font pattern";
    const char *family = config->font_family != NULL ? config->font_family : default_font_family;
    if (!FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)family) ||
        (strcmp(family, default_font_family) != 0 &&
         !FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)default_font_family)) ||
        !FcPatternAddDouble(pattern, FC_PIXEL_SIZE, renderer->size) ||
        !FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_REGULAR) ||
        !FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN) ||
        !FcPatternAddBool(pattern, FC_SCALABLE, FcTrue) ||
        !FcConfigSubstitute(renderer->config, pattern, FcMatchPattern)) {
        FcPatternDestroy(pattern);
        return "could not prepare the font pattern";
    }
    FcDefaultSubstitute(pattern);
    FcResult result;
    renderer->matches = FcFontSort(renderer->config, pattern, FcFalse, NULL, &result);
    FcPatternDestroy(pattern);
    if (renderer->matches == NULL || renderer->matches->nfont == 0) return "no usable system fonts found";
    renderer->faces = calloc((size_t)renderer->matches->nfont, sizeof(*renderer->faces));
    if (renderer->faces == NULL) return "could not allocate font fallback state";
    for (size_t i = 0; i < (size_t)renderer->matches->nfont; ++i) {
        FontFace *candidate = font_open_face(renderer, i);
        if (candidate != NULL && font_measure(candidate->face, &renderer->metrics)) {
            renderer->primary = i;
            FT_Face face = candidate->face;
            FT_UInt x = FT_Get_Char_Index(face, 'x');
            renderer->x_height = x != 0 && FT_Load_Glyph(face, x, FT_LOAD_DEFAULT) == 0
                ? (int)((face->glyph->metrics.horiBearingY + 32) / 64) : 0;
            if (renderer->x_height <= 0 || renderer->x_height > renderer->metrics.baseline) {
                renderer->x_height = renderer->metrics.baseline / 2;
            }
            return font_apply_spacing(&renderer->metrics, config) ? NULL : font_spacing_error;
        }
    }
    return "could not open a system font with printable ASCII glyphs";
}

static const char *font_make_atlas(FontRenderer *renderer)
{
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    /* A power-of-two texture also works on older compatibility contexts. */
    int size = GLYPH_ATLAS_LIMIT;
    while (size > limit && size > 0) size /= 2;
    int tile_width = 2 * renderer->metrics.width;
    renderer->atlas_size = size;
    renderer->tile_columns = size / tile_width;
    size_t tiles = (size_t)renderer->tile_columns * (size / renderer->metrics.height);
    renderer->cache_size = tiles - tiles % GLYPH_CACHE_WAYS;
    if (renderer->cache_size == 0) return "OpenGL texture size is too small for the configured font";
    renderer->cache = calloc(renderer->cache_size, sizeof(*renderer->cache));
    renderer->pixels = malloc((size_t)tile_width * renderer->metrics.height);
    if (renderer->cache == NULL || renderer->pixels == NULL) return "could not allocate the glyph cache";
    glGenTextures(1, &renderer->atlas);
    glBindTexture(GL_TEXTURE_2D, renderer->atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, size, size, 0, GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
    if (renderer->atlas == 0 || glGetError() != GL_NO_ERROR) return "could not create the OpenGL glyph atlas";
    return NULL;
}

static void font_close(FontRenderer *renderer)
{
    if (renderer->atlas != 0) glDeleteTextures(1, &renderer->atlas);
    free(renderer->cache);
    free(renderer->pixels);
    if (renderer->faces != NULL) {
        for (int i = 0; i < renderer->matches->nfont; ++i) {
            if (renderer->faces[i].face != NULL) FT_Done_Face(renderer->faces[i].face);
        }
    }
    free(renderer->faces);
    for (size_t i = 1; i < sizeof renderer->styled / sizeof renderer->styled[0]; ++i) {
        FontFace *styled = &renderer->styled[i].font;
        if (styled->face != NULL) FT_Done_Face(styled->face);
    }
    if (renderer->matches != NULL) FcFontSetDestroy(renderer->matches);
    if (renderer->config != NULL) FcConfigDestroy(renderer->config);
    if (renderer->library != NULL) FT_Done_FreeType(renderer->library);
}

static FontFace *font_find_glyph(FontRenderer *renderer, uint32_t codepoint, unsigned int synthesis)
{
    FontFace *primary = &renderer->faces[renderer->primary];
    if (font_load_glyph(primary->face, codepoint, synthesis)) return primary;
    for (size_t i = 0; i < (size_t)renderer->matches->nfont; ++i) {
        if (i == renderer->primary) continue;
        FcCharSet *charset;
        if (FcPatternGetCharSet(renderer->matches->fonts[i], FC_CHARSET, 0, &charset) == FcResultMatch &&
            !FcCharSetHasChar(charset, codepoint)) continue;
        FontFace *font = font_open_face(renderer, i);
        if (font != NULL && font_load_glyph(font->face, codepoint, synthesis)) return font;
    }
    return NULL;
}

static bool font_same_file(FcPattern *a, FcPattern *b)
{
    FcChar8 *first, *second;
    int first_index = 0, second_index = 0;
    if (FcPatternGetString(a, FC_FILE, 0, &first) != FcResultMatch ||
        FcPatternGetString(b, FC_FILE, 0, &second) != FcResultMatch) return false;
    FcPatternGetInteger(a, FC_INDEX, 0, &first_index);
    FcPatternGetInteger(b, FC_INDEX, 0, &second_index);
    return first_index == second_index && strcmp((const char *)first, (const char *)second) == 0;
}

/* Match a styled face from the primary family. Fontconfig substitutes the
   nearest face, so a different family, the regular file, or a weight or
   slant that falls short of the request selects synthesis instead. */
static StyledFace *font_styled(FontRenderer *renderer, unsigned int style)
{
    StyledFace *styled = &renderer->styled[style];
    if (styled->attempted) return styled;
    styled->attempted = true;
    styled->synthesis = style;
    FcPattern *primary = renderer->matches->fonts[renderer->primary];
    FcChar8 *family;
    if (FcPatternGetString(primary, FC_FAMILY, 0, &family) != FcResultMatch) return styled;
    FcPattern *pattern = FcPatternCreate();
    if (pattern == NULL) return styled;
    bool bold = style & TERMINAL_STYLE_BOLD, italic = style & TERMINAL_STYLE_ITALIC;
    FcPattern *match = NULL;
    FcResult result;
    if (FcPatternAddString(pattern, FC_FAMILY, family) &&
        FcPatternAddDouble(pattern, FC_PIXEL_SIZE, renderer->size) &&
        FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR) &&
        FcPatternAddInteger(pattern, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN) &&
        FcPatternAddBool(pattern, FC_SCALABLE, FcTrue) &&
        FcConfigSubstitute(renderer->config, pattern, FcMatchPattern)) {
        FcDefaultSubstitute(pattern);
        match = FcFontMatch(renderer->config, pattern, &result);
    }
    FcPatternDestroy(pattern);
    if (match == NULL) return styled;
    FcChar8 *matched_family, *path;
    int weight = FC_WEIGHT_REGULAR, slant = FC_SLANT_ROMAN, index = 0;
    bool usable = FcPatternGetString(match, FC_FAMILY, 0, &matched_family) == FcResultMatch &&
                  strcmp((const char *)matched_family, (const char *)family) == 0 &&
                  !font_same_file(match, primary) &&
                  FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch;
    if (usable) {
        FcPatternGetInteger(match, FC_WEIGHT, 0, &weight);
        FcPatternGetInteger(match, FC_SLANT, 0, &slant);
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        bool real_bold = weight >= FC_WEIGHT_DEMIBOLD, real_italic = slant != FC_SLANT_ROMAN;
        /* A face that supplies neither requested property adds nothing. */
        if ((bold && real_bold) || (italic && real_italic)) {
            FT_Face face;
            if (FT_New_Face(renderer->library, (const char *)path, index, &face) == 0) {
                if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0 &&
                    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)renderer->size) == 0) {
                    styled->font.face = face;
                    font_graphic_bounds(&styled->font);
                    styled->synthesis = (bold && !real_bold ? TERMINAL_STYLE_BOLD : 0) |
                                        (italic && !real_italic ? TERMINAL_STYLE_ITALIC : 0);
                } else FT_Done_Face(face);
            }
        }
    }
    FcPatternDestroy(match);
    return styled;
}

static FontFace *font_find_styled(FontRenderer *renderer, uint32_t codepoint, unsigned int style)
{
    if (style != 0) {
        StyledFace *styled = font_styled(renderer, style);
        if (styled->font.face != NULL && font_load_glyph(styled->font.face, codepoint, styled->synthesis)) {
            return &styled->font;
        }
    }
    return font_find_glyph(renderer, codepoint, style);
}

static int scale_edge(int coordinate, int start, int end, int pixels)
{
    int delta = (coordinate - start) * pixels;
    int half = (end - start) / 2;
    return (delta + (delta < 0 ? -half : half)) / (end - start);
}

static GlyphBounds glyph_placement(const FontRenderer *renderer, const FontFace *font,
                                    uint32_t codepoint, unsigned char span, bool mark)
{
    FT_GlyphSlot slot = font->face->glyph;
    GlyphBounds source = bitmap_bounds(slot);
    int width = renderer->metrics.width * span;
    int height = renderer->metrics.height;
    /* Use the font's full-length strokes to map box and block elements to the
       cell edges, including when the fallback face has a different advance. */
    GlyphBounds reference = {0};
    if (codepoint >= 0x2500 && codepoint <= 0x257f) reference = font->box;
    if (codepoint >= 0x2580 && codepoint <= 0x259f) reference = font->block;
    GlyphBounds target;
    if (reference.right > reference.left) {
        target.left = scale_edge(source.left, reference.left, reference.right, width);
        target.right = scale_edge(source.right, reference.left, reference.right, width);
    } else {
        FT_Pos advance = slot->advance.x / 64;
        if (advance < 0 || advance > 4096) advance = width;
        target.left = mark ? (width - (int)slot->bitmap.width) / 2
                           : source.left + (width - (int)advance) / 2;
        target.right = target.left + (int)slot->bitmap.width;
    }
    if (reference.bottom > reference.top) {
        target.top = scale_edge(source.top, reference.top, reference.bottom, height);
        target.bottom = scale_edge(source.bottom, reference.top, reference.bottom, height);
    } else {
        target.top = source.top + renderer->metrics.baseline;
        target.bottom = source.bottom + renderer->metrics.baseline;
    }
    if (target.right <= target.left) target.right = target.left + 1;
    if (target.bottom <= target.top) target.bottom = target.top + 1;
    return target;
}

static void glyph_tile(const FontRenderer *renderer, size_t slot, int *x, int *y)
{
    *x = (int)(slot % (size_t)renderer->tile_columns) * 2 * renderer->metrics.width;
    *y = (int)(slot / (size_t)renderer->tile_columns) * renderer->metrics.height;
}

static void glyph_upload(FontRenderer *renderer, CachedGlyph *glyph, size_t slot)
{
    uint32_t codepoint = glyph->codepoint;
    FontFace *font = font_find_styled(renderer, codepoint, glyph->style);
    if (font == NULL && !glyph->mark) {
        codepoint = 0xfffd;
        font = font_find_styled(renderer, codepoint, glyph->style);
        if (font == NULL) {
            codepoint = '?';
            font = font_find_styled(renderer, codepoint, glyph->style);
        }
    }
    if (font == NULL) return;
    const FT_Bitmap *bitmap = &font->face->glyph->bitmap;
    if (bitmap->width == 0 || bitmap->rows == 0) return;
    GlyphBounds target = glyph_placement(renderer, font, codepoint, glyph->span, glyph->mark);
    int cell_width = renderer->metrics.width * glyph->span;
    int cell_height = renderer->metrics.height;
    /* Italic glyphs lean past a narrow cell. The tile holds two cell widths,
       so let them overhang half a cell on each side. */
    int first = 0, last = cell_width;
    if ((glyph->style & TERMINAL_STYLE_ITALIC) && glyph->span == 1) {
        first = -(renderer->metrics.width / 2);
        last = 2 * renderer->metrics.width + first;
    }
    int left = target.left > first ? target.left : first;
    int top = target.top > 0 ? target.top : 0;
    int right = target.right < last ? target.right : last;
    int bottom = target.bottom < cell_height ? target.bottom : cell_height;
    if (right <= left || bottom <= top) return;
    glyph->x = left;
    glyph->y = top;
    glyph->width = right - left;
    glyph->height = bottom - top;
    unsigned char *dest = renderer->pixels;
    for (int y = top; y < bottom; ++y) {
        int sy = (int)((int64_t)(y - target.top) * bitmap->rows / (target.bottom - target.top));
        const unsigned char *row = bitmap->buffer + (ptrdiff_t)sy * bitmap->pitch;
        for (int x = left; x < right; ++x) {
            int sx = (int)((int64_t)(x - target.left) * bitmap->width / (target.right - target.left));
            *dest++ = glyph_alpha(bitmap, row, sx);
        }
    }
    int x, y;
    glyph_tile(renderer, slot, &x, &y);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, glyph->width, glyph->height, GL_ALPHA,
                    GL_UNSIGNED_BYTE, renderer->pixels);
}

static void glyph_flush(FontRenderer *renderer)
{
    if (renderer->drawing) {
        glEnd();
        renderer->drawing = false;
    }
}

static size_t glyph_cached(FontRenderer *renderer, uint32_t codepoint, unsigned char span,
                           unsigned char style, bool mark)
{
    uint32_t hash = (codepoint << 4) | ((uint32_t)style << 2) | (span == 2 ? 2u : 0u) | mark;
    hash = (hash ^ (hash >> 16)) * UINT32_C(0x7feb352d);
    hash ^= hash >> 15;
    size_t bucket = (hash % (renderer->cache_size / GLYPH_CACHE_WAYS)) * GLYPH_CACHE_WAYS;
    size_t target = bucket;
    for (size_t slot = bucket; slot < bucket + GLYPH_CACHE_WAYS; ++slot) {
        CachedGlyph *glyph = &renderer->cache[slot];
        if (glyph->used != 0 && glyph->codepoint == codepoint && glyph->span == span &&
            glyph->style == style && glyph->mark == mark) {
            glyph->used = ++renderer->access;
            return slot;
        }
        if (glyph->used < renderer->cache[target].used) target = slot;
    }
    /* Previously submitted quads must finish using this tile before it changes. */
    glyph_flush(renderer);
    CachedGlyph *entry = &renderer->cache[target];
    *entry = (CachedGlyph){.used = ++renderer->access, .codepoint = codepoint, .span = span,
                           .style = style, .mark = mark};
    glyph_upload(renderer, entry, target);
    return target;
}

static void render_color(TerminalColor color)
{
    glColor3ub(color.red, color.green, color.blue);
}

static void render_rectangle(int x, int y, int width, int height)
{
    glVertex2i(x, y);
    glVertex2i(x + width, y);
    glVertex2i(x + width, y + height);
    glVertex2i(x, y + height);
}

static int grid_extent(int pixels, int padding)
{
    return pixels > 2 * padding ? pixels - 2 * padding : 0;
}

/* The grid holds whole cells. Split the remaining pixels between both sides
   so the margins stay even instead of collecting at the right and bottom. */
static Padding grid_padding(Padding padding, int width, int height, const FontMetrics *metrics,
                            const TerminalScreen *screen)
{
    int spare_x = grid_extent(width, padding.horizontal) - (int)screen->columns * metrics->width;
    int spare_y = grid_extent(height, padding.vertical) - (int)screen->rows * metrics->height;
    if (spare_x > 0) padding.horizontal += spare_x / 2;
    if (spare_y > 0) padding.vertical += spare_y / 2;
    return padding;
}

static void render_glyph(FontRenderer *renderer, uint32_t codepoint, unsigned char span,
                         unsigned char style, bool mark, TerminalColor foreground, int x, int y)
{
    /* Slanted grid graphics would no longer meet their neighbours. */
    if ((codepoint >= 0x2500 && codepoint <= 0x259f) || (codepoint >= 0xe0b0 && codepoint <= 0xe0bf)) {
        style &= ~TERMINAL_STYLE_ITALIC;
    }
    size_t slot = glyph_cached(renderer, codepoint, span, style, mark);
    const CachedGlyph *glyph = &renderer->cache[slot];
    if (glyph->width == 0) return;
    if (!renderer->drawing) {
        glBegin(GL_QUADS);
        renderer->drawing = true;
    }
    render_color(foreground);
    int ax, ay;
    glyph_tile(renderer, slot, &ax, &ay);
    GLfloat u0 = (GLfloat)ax / renderer->atlas_size;
    GLfloat v0 = (GLfloat)ay / renderer->atlas_size;
    GLfloat u1 = (GLfloat)(ax + glyph->width) / renderer->atlas_size;
    GLfloat v1 = (GLfloat)(ay + glyph->height) / renderer->atlas_size;
    x += glyph->x;
    y += glyph->y;
    glTexCoord2f(u0, v0); glVertex2i(x, y);
    glTexCoord2f(u1, v0); glVertex2i(x + glyph->width, y);
    glTexCoord2f(u1, v1); glVertex2i(x + glyph->width, y + glyph->height);
    glTexCoord2f(u0, v1); glVertex2i(x, y + glyph->height);
}

static int decoration_thickness(int cell_height)
{
    int thickness = (cell_height + 8) / 18;
    return thickness > 0 ? thickness : 1;
}

/* Draw one cell's underline inside a GL_QUADS batch. Patterns use the
   absolute x position so they continue across neighbouring cells. */
static void render_underline(unsigned int style, int x, int y, int width, int cell_width,
                             int cell_height, int origin)
{
    int thickness = decoration_thickness(cell_height);
    int base = cell_height - 1 - thickness;
    if (base < 0) base = 0;
    switch (style) {
    case TERMINAL_UNDERLINE_DOUBLE:
        render_rectangle(x, y + base, width, thickness);
        if (base >= 2 * thickness) render_rectangle(x, y + base - 2 * thickness, width, thickness);
        break;
    case TERMINAL_UNDERLINE_CURLY: {
        int amplitude = cell_height / 8 > thickness + 1 ? cell_height / 8 : thickness + 1;
        if (amplitude > base + thickness) amplitude = base + thickness;
        int rise = amplitude - thickness;
        for (int i = 0; i < width; ++i) {
            int phase = (x - origin + i) % cell_width;
            double angle = 2.0 * 3.14159265358979323846 * (phase + 0.5) / cell_width;
            int lift = (int)(rise * (1.0 - cos(angle)) / 2.0 + 0.5);
            render_rectangle(x + i, y + base - lift, 1, thickness + (rise > 0));
        }
        break;
    }
    case TERMINAL_UNDERLINE_DOTTED:
        for (int i = 0; i < width; ++i) {
            if ((x - origin + i) % (2 * thickness) < thickness) render_rectangle(x + i, y + base, 1, thickness);
        }
        break;
    case TERMINAL_UNDERLINE_DASHED:
        for (int offset = 0; offset < width; offset += cell_width) {
            int inset = cell_width / 6;
            render_rectangle(x + offset + inset, y + base, cell_width - 2 * inset, thickness);
        }
        break;
    default:
        render_rectangle(x, y + base, width, thickness);
        break;
    }
}

static TerminalColor underline_color(const Config *config, const TerminalCell *cell, TerminalColor foreground)
{
    if (cell->underline_source == 0) return foreground;
    if (cell->underline_source == SOURCE_RGB) return cell->underline_color;
    return cell_color(config, cell->underline_source, cell->underline_color);
}

static void render_screen(FontRenderer *renderer, const TerminalView *view, int width, int height,
                          size_t cursor_column, size_t cursor_row, bool cursor_visible,
                          CursorStyle cursor_style, const Config *config, float opacity)
{
    const TerminalScreen *screen = view->screen;
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    TerminalColor background = config->background;
    /* Wayland buffers use premultiplied alpha. */
    float background_scale = opacity / 255.0f;
    glClearColor(background.red * background_scale, background.green * background_scale,
                 background.blue * background_scale, opacity);
    glClear(GL_COLOR_BUFFER_BIT);

    Padding padding = grid_padding(config->padding, width, height, &renderer->metrics, screen);
    int pad_x = padding.horizontal, pad_y = padding.vertical;
    int cell_width = renderer->metrics.width;
    int cell_height = renderer->metrics.height;
    glScissor(pad_x, pad_y, grid_extent(width, pad_x), grid_extent(height, pad_y));
    glEnable(GL_SCISSOR_TEST);

    bool show_cursor = cursor_visible && view->first_visible == view->first_live &&
                       cursor_column < screen->columns && cursor_row < screen->rows;
    size_t block_row = SIZE_MAX, block_first = 0, block_end = 0;
    if (show_cursor && cursor_style == CURSOR_BLOCK) {
        const TerminalCell *cells = screen_row(screen, cursor_row);
        block_row = cursor_row;
        block_first = cursor_column - (cells[cursor_column].width == 0 && cursor_column > 0);
        block_end = block_first + cells[block_first].width;
    }
    bool any_strikethrough = false;
    glBegin(GL_QUADS);
    for (size_t row = 0; row < screen->rows; ++row) {
        uint64_t line = view->first_visible + row;
        LineView source = view_line(view, line);
        size_t selected_first, selected_end;
        view_selection_columns(view, line, source.columns, &selected_first, &selected_end);
        int y = pad_y + (int)row * cell_height;
        size_t start = 0;
        TerminalColor run = background;
        bool solid_run = false;
        bool has_decoration = false;
        for (size_t column = 0; column < screen->columns; ++column) {
            const TerminalCell *cell = view_cell(view, &source, column);
            bool under_block = row == block_row && column >= block_first && column < block_end;
            bool selected = column >= selected_first && column < selected_end;
            TerminalColor bg = selected ? config->selection_background : cell_color(config, cell->background_source, cell->background);
            if (under_block) bg = config->cursor;
            /* With opacity, colored backgrounds stay opaque so highlights remain
               readable. A background that only repeats the default color, as
               TUIs that paint every cell do, shares the window's opacity. */
            bool solid = opacity < 1.0f
                ? selected || under_block ||
                  (cell->background_source != SOURCE_BG && !color_equal(bg, background))
                : !color_equal(bg, background);
            if (solid != solid_run || (solid && !color_equal(bg, run))) {
                if (solid_run) {
                    render_color(run);
                    render_rectangle(pad_x + (int)start * cell_width, y, (int)(column - start) * cell_width, cell_height);
                }
                run = bg;
                solid_run = solid;
                start = column;
            }
            has_decoration |= cell->underline || cell->overline ||
                              (view->hovered_link && cell->hyperlink == view->hovered_link);
            any_strikethrough |= cell->strikethrough;
        }
        if (solid_run) {
            render_color(run);
            render_rectangle(pad_x + (int)start * cell_width, y, (int)(screen->columns - start) * cell_width, cell_height);
        }
        if (!has_decoration) continue;
        for (size_t column = 0; column < screen->columns && column < source.stored; ++column) {
            const TerminalCell *cell = &source.cells[column];
            bool hover = view->hovered_link && cell->hyperlink == view->hovered_link;
            if (!cell->underline && !cell->overline && !hover) continue;
            bool selected = column >= selected_first && column < selected_end;
            bool under_block = row == block_row && column >= block_first && column < block_end;
            TerminalColor fg = selected ? config->selection_foreground : cell_color(config, cell->foreground_source, cell->foreground);
            TerminalColor bg = selected ? config->selection_background : cell_color(config, cell->background_source, cell->background);
            int x = pad_x + (int)column * cell_width;
            if (cell->overline) {
                render_color(under_block ? bg : fg);
                render_rectangle(x, y, cell_width, decoration_thickness(cell_height));
            }
            if (cell->underline) {
                render_color(under_block ? bg : underline_color(config, cell, fg));
                render_underline(cell->underline, x, y, cell_width, cell_width, cell_height, pad_x);
            } else if (hover) {
                render_color(under_block ? bg : fg);
                render_rectangle(x, y + (cell_height > 1 ? cell_height - 2 : 0),
                                 cell_width > 1 ? cell_width - 1 : cell_width, 1);
            }
        }
    }
    glEnd();

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, renderer->atlas);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (size_t row = 0; row < screen->rows; ++row) {
        uint64_t line = view->first_visible + row;
        LineView source = view_line(view, line);
        size_t selected_first, selected_end;
        view_selection_columns(view, line, source.columns, &selected_first, &selected_end);
        int y = pad_y + (int)row * cell_height;
        for (size_t column = 0; column < screen->columns && column < source.stored; ++column) {
            const TerminalCell *cell = &source.cells[column];
            if (cell->width == 0 || column + cell->width > screen->columns) continue;
            bool glyph = cell->character != ' ' && cell->character != 0x2800;
            if ((!glyph && !cell->combining[0]) || cell->concealed) continue;
            int x = pad_x + (int)column * cell_width;
            bool selected = column >= selected_first && column < selected_end;
            TerminalColor foreground = selected ? config->selection_foreground : cell_color(config, cell->foreground_source, cell->foreground);
            if (row == block_row && column == block_first) foreground = selected ? config->selection_background : cell_color(config, cell->background_source, cell->background);
            if (glyph) {
                render_glyph(renderer, cell->character, cell->width, cell->style, false, foreground, x, y);
            }
            for (size_t i = 0; i < sizeof cell->combining / sizeof cell->combining[0] && cell->combining[i]; ++i) {
                render_glyph(renderer, cell->combining[i], cell->width, cell->style, true, foreground, x, y);
            }
        }
    }
    glyph_flush(renderer);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);

    /* Strikethrough crosses the glyphs, so it follows them. */
    if (any_strikethrough) {
        int thickness = decoration_thickness(cell_height);
        int offset = renderer->metrics.baseline - renderer->x_height / 2 - thickness / 2;
        if (offset < 0) offset = 0;
        glBegin(GL_QUADS);
        for (size_t row = 0; row < screen->rows; ++row) {
            uint64_t line = view->first_visible + row;
            LineView source = view_line(view, line);
            size_t selected_first, selected_end;
            view_selection_columns(view, line, source.columns, &selected_first, &selected_end);
            int y = pad_y + (int)row * cell_height + offset;
            for (size_t column = 0; column < screen->columns && column < source.stored; ++column) {
                const TerminalCell *cell = &source.cells[column];
                if (!cell->strikethrough) continue;
                bool selected = column >= selected_first && column < selected_end;
                bool under_block = row == block_row && column >= block_first && column < block_end;
                TerminalColor color = selected ? config->selection_foreground
                                               : cell_color(config, cell->foreground_source, cell->foreground);
                if (under_block) {
                    color = selected ? config->selection_background
                                     : cell_color(config, cell->background_source, cell->background);
                }
                render_color(color);
                render_rectangle(pad_x + (int)column * cell_width, y, cell_width, thickness);
            }
        }
        glEnd();
    }

    if (show_cursor && block_row == SIZE_MAX) {
        int x = pad_x + (int)cursor_column * cell_width;
        int y = pad_y + (int)cursor_row * cell_height;
        render_color(config->cursor);
        glBegin(GL_QUADS);
        int thickness = config->cursor_thickness;
        if (cursor_style == CURSOR_UNDERLINE) {
            if (thickness > cell_height) thickness = cell_height;
            render_rectangle(x, y + cell_height - thickness, cell_width, thickness);
        } else {
            if (thickness > cell_width) thickness = cell_width;
            render_rectangle(x, y, thickness, cell_height);
        }
        glEnd();
    }
    glDisable(GL_SCISSOR_TEST);
}

/* Platform and window management */

typedef enum { KEY_PRESS, KEY_REPEAT, KEY_REPEAT_SHORTCUT } KeyEvent;
typedef enum { SCROLL_NONE, SCROLL_DETENTS, SCROLL_WHEEL_DISTANCE, SCROLL_DISTANCE } ScrollMode;
enum { WHEEL_DETENT = 120 };

typedef struct {
    xkb_keycode_t key;
    int32_t rate, delay;
    int64_t first, deadline;
    bool shortcut;
} KeyRepeat;

typedef struct {
    double distance, remainder;
    int64_t value120, discrete;
    ScrollMode mode;
    bool has_value120, has_discrete, stopped, zoom, wheel_source;
} PointerAxis;

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct xdg_wm_base *wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_callback *frame;
    struct wl_seat *seat;
    struct wl_data_device_manager *clipboard_manager;
    struct zwp_primary_selection_device_manager_v1 *primary_manager;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct xkb_context *xkb;
    struct xkb_state *keys;
    struct xkb_compose_table *compose_table;
    struct xkb_compose_state *compose;
    const char *compose_locale;
    const char *composed; /* UTF-8 result of a finished compose sequence during key_pressed. */
    xkb_keysym_t composed_key;
    struct xdg_activation_v1 *activation;
    struct xdg_activation_token_v1 *activation_token;
    uint32_t seat_name, clipboard_name, primary_name, activation_name;
    uint32_t input_serial; /* The latest serial from a key, button or keyboard focus event. */
    KeyRepeat repeat;
    PointerAxis scroll[2];
    double pointer_x, pointer_y;
    bool pointer_inside;
    struct wl_egl_window *window;
    EGLDisplay egl;
    EGLContext context;
    EGLSurface egl_surface;
    EGLint egl_major, egl_minor;
    bool egl_initialized;
    int width, height, pending_width, pending_height;
    bool configured, needs_redraw, closed, focused, fullscreen, pending_fullscreen;
    const char *error;
    char egl_error[160];
    bool (*key_pressed)(void *data, xkb_keycode_t key, KeyEvent event, uint32_t serial);
    void (*scrolled)(void *data, uint32_t axis, int steps, unsigned int modifiers);
    void (*pointer_button_event)(void *data, uint32_t button, bool pressed, uint32_t serial, uint32_t time);
    void (*pointer_moved)(void *data);
    void (*pointer_left)(void *data);
    void *input_data;
} Platform;

static int64_t monotonic_milliseconds(void);

static const char *platform_app_id(const Config *config)
{
    if (config->options->app_id != NULL) return config->options->app_id;
    return config->app_id != NULL ? config->app_id : "deal";
}

static void platform_title(const Platform *platform, const Config *config, const char *dynamic)
{
    const char *title = config->title != NULL ? config->title : default_title;
    if (config->options->title != NULL) title = config->options->title;
    if (*dynamic) title = dynamic;
    xdg_toplevel_set_title(platform->toplevel, title);
}

static void platform_size_hints(const Platform *platform, Padding padding, const FontMetrics *metrics)
{
    xdg_toplevel_set_min_size(platform->toplevel, 2 * padding.horizontal + metrics->width,
                             2 * padding.vertical + metrics->height);
}

static void platform_fullscreen(const Platform *platform)
{
    if (platform->fullscreen) xdg_toplevel_unset_fullscreen(platform->toplevel);
    else xdg_toplevel_set_fullscreen(platform->toplevel, NULL);
}

static int initial_extent(int cells, int cell_size, int padding)
{
    int fit = grid_extent(4096, padding) / cell_size;
    if (fit == 0) return 4096;
    return 2 * padding + (fit < cells ? fit : cells) * cell_size;
}

static unsigned int keyboard_modifiers(struct xkb_state *keys)
{
    if (keys == NULL) return 0;
    unsigned int modifiers = 0;
    if (xkb_state_mod_name_is_active(keys, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= MOD_SHIFT;
    if (xkb_state_mod_name_is_active(keys, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= MOD_CTRL;
    if (xkb_state_mod_name_is_active(keys, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        modifiers |= MOD_ALT;
    return modifiers;
}

static void platform_repeat_arm(KeyRepeat *repeat, int64_t now)
{
    if (now < 0 || repeat->rate <= 0) {
        repeat->key = 0;
        return;
    }
    repeat->first = repeat->deadline = now + repeat->delay;
}

static void platform_repeat_key(Platform *platform, int64_t now)
{
    KeyRepeat *repeat = &platform->repeat;
    if (repeat->key == 0 || now < repeat->deadline) return;
    /* Skip missed ticks and keep the compositor's rate without accumulating rounding error. */
    int64_t elapsed = now - repeat->first;
    int64_t tick = elapsed * repeat->rate / 1000 + 1;
    repeat->deadline = repeat->first + (tick * 1000 + repeat->rate - 1) / repeat->rate;
    platform->key_pressed(platform->input_data, repeat->key,
                          repeat->shortcut ? KEY_REPEAT_SHORTCUT : KEY_REPEAT, 0);
}

static void platform_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                            int32_t fd, uint32_t size)
{
    (void)keyboard;
    Platform *platform = data;
    struct xkb_state *keys = NULL;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 1) {
        void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(platform->xkb, map, size - 1,
                XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (keymap != NULL) {
                keys = xkb_state_new(keymap);
                xkb_keymap_unref(keymap);
            }
            munmap(map, size);
        }
    }
    close(fd);
    xkb_state_unref(platform->keys);
    platform->keys = keys;
    platform->repeat.key = 0;
    if (platform->compose != NULL) xkb_compose_state_reset(platform->compose);
    platform->scroll[0].remainder = platform->scroll[1].remainder = 0;
    if (keys == NULL) fprintf(stderr, "deal: Wayland did not provide a usable keyboard keymap\n");
}

static void platform_keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                                    struct wl_surface *surface, struct wl_array *keys)
{
    (void)keyboard;
    (void)keys;
    Platform *platform = data;
    platform->focused = surface == platform->surface;
    platform->repeat.key = 0;
    if (platform->focused) platform->input_serial = serial;
}

static void platform_keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                                    struct wl_surface *surface)
{
    (void)keyboard;
    (void)serial;
    Platform *platform = data;
    if (surface == platform->surface) {
        platform->focused = false;
        platform->repeat.key = 0;
        platform->scroll[0].remainder = platform->scroll[1].remainder = 0;
        if (platform->compose != NULL) xkb_compose_state_reset(platform->compose);
        if (platform->keys != NULL) xkb_state_update_mask(platform->keys, 0, 0, 0, 0, 0, 0);
    }
}

static void platform_keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                                  uint32_t time, uint32_t key, uint32_t state)
{
    (void)keyboard;
    (void)time;
    Platform *platform = data;
    if (key > UINT32_MAX - 8) return;
    xkb_keycode_t code = key + 8;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED && platform->repeat.key == code)
        platform->repeat.key = 0;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || platform->keys == NULL ||
        !platform->focused || platform->key_pressed == NULL) return;
    platform->input_serial = serial;
    char composed[64];
    bool finished = false;
    if (platform->compose != NULL &&
        xkb_compose_state_feed(platform->compose, xkb_state_key_get_one_sym(platform->keys, code)) ==
            XKB_COMPOSE_FEED_ACCEPTED) {
        switch (xkb_compose_state_get_status(platform->compose)) {
        case XKB_COMPOSE_COMPOSING:
            /* Dead keys and compose prefixes produce no input of their own. */
            platform->repeat.key = 0;
            return;
        case XKB_COMPOSE_CANCELLED:
            xkb_compose_state_reset(platform->compose);
            platform->repeat.key = 0;
            return;
        case XKB_COMPOSE_COMPOSED: {
            int length = xkb_compose_state_get_utf8(platform->compose, composed, sizeof composed);
            platform->composed_key = xkb_compose_state_get_one_sym(platform->compose);
            xkb_compose_state_reset(platform->compose);
            if (length <= 0 || (size_t)length >= sizeof composed) {
                if (platform->composed_key == XKB_KEY_NoSymbol) return;
                composed[0] = 0;
            }
            platform->composed = composed;
            finished = true;
            break;
        }
        case XKB_COMPOSE_NOTHING:
            break;
        }
    }
    bool shortcut = platform->key_pressed(platform->input_data, code, KEY_PRESS, serial);
    platform->composed = NULL;
    /* A composed character comes from several keys, so it never repeats. */
    if (!finished && xkb_keymap_key_repeats(xkb_state_get_keymap(platform->keys), code)) {
        platform->repeat.key = code;
        platform->repeat.shortcut = shortcut;
        platform_repeat_arm(&platform->repeat, monotonic_milliseconds());
    }
}

static void platform_keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                                        uint32_t depressed, uint32_t latched, uint32_t locked,
                                        uint32_t group)
{
    (void)keyboard;
    (void)serial;
    Platform *platform = data;
    if (platform->keys != NULL) {
        unsigned int before = keyboard_modifiers(platform->keys);
        xkb_state_update_mask(platform->keys, depressed, latched, locked, 0, 0, group);
        if ((before ^ keyboard_modifiers(platform->keys)) & MOD_CTRL)
            platform->scroll[0].remainder = platform->scroll[1].remainder = 0;
    }
}

static void platform_keyboard_repeat(void *data, struct wl_keyboard *keyboard,
                                     int32_t rate, int32_t delay)
{
    (void)keyboard;
    Platform *platform = data;
    KeyRepeat *repeat = &platform->repeat;
    repeat->rate = rate > 1000 ? 1000 : rate;
    repeat->delay = delay >= 0 ? delay : 0;
    if (repeat->key != 0) platform_repeat_arm(repeat, monotonic_milliseconds());
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = platform_keymap, .enter = platform_keyboard_enter, .leave = platform_keyboard_leave,
    .key = platform_keyboard_key, .modifiers = platform_keyboard_modifiers,
    .repeat_info = platform_keyboard_repeat
};

static void platform_drop_keyboard(Platform *platform)
{
    if (platform->keyboard != NULL) {
        if (wl_keyboard_get_version(platform->keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
            wl_keyboard_release(platform->keyboard);
        else wl_keyboard_destroy(platform->keyboard);
        platform->keyboard = NULL;
    }
    xkb_state_unref(platform->keys);
    platform->keys = NULL;
    platform->focused = false;
    platform->repeat = (KeyRepeat){0};
    platform->scroll[0].remainder = platform->scroll[1].remainder = 0;
}

static void platform_pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                                    wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)time;
    Platform *platform = data;
    platform->pointer_x = wl_fixed_to_double(x);
    platform->pointer_y = wl_fixed_to_double(y);
    if (platform->pointer_inside && platform->pointer_moved != NULL)
        platform->pointer_moved(platform->input_data);
}

static void platform_pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                                   struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)serial;
    Platform *platform = data;
    platform->pointer_inside = surface == platform->surface;
    memset(platform->scroll, 0, sizeof platform->scroll);
    platform->pointer_x = wl_fixed_to_double(x);
    platform->pointer_y = wl_fixed_to_double(y);
}

static void platform_pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                                   struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    Platform *platform = data;
    if (surface == platform->surface) {
        platform->pointer_inside = false;
        memset(platform->scroll, 0, sizeof platform->scroll);
        if (platform->pointer_left != NULL) platform->pointer_left(platform->input_data);
    }
}

static void platform_pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                                    uint32_t time, uint32_t button, uint32_t state)
{
    (void)pointer;
    Platform *platform = data;
    platform->input_serial = serial;
    if (platform->pointer_inside && platform->pointer_button_event != NULL)
        platform->pointer_button_event(platform->input_data, button,
                                       state == WL_POINTER_BUTTON_STATE_PRESSED, serial, time);
}

static void platform_scroll_frame(Platform *platform, uint32_t index)
{
    PointerAxis *axis = &platform->scroll[index];
    bool detents = axis->has_value120 || axis->has_discrete;
    if (platform->pointer_inside && (detents || axis->distance != 0)) {
        unsigned int modifiers = keyboard_modifiers(platform->keys);
        bool zoom = (modifiers & MOD_CTRL) != 0;
        ScrollMode mode = detents ? SCROLL_DETENTS :
                          axis->wheel_source ? SCROLL_WHEEL_DISTANCE : SCROLL_DISTANCE;
        if (mode != axis->mode || zoom != axis->zoom) axis->remainder = 0;
        axis->mode = mode;
        axis->zoom = zoom;
        /* Prefer modern detents, then legacy detents, then continuous distance. */
        double movement = axis->has_value120 ? (double)axis->value120 :
                          axis->has_discrete ? (double)axis->discrete : axis->distance;
        double threshold = detents ? WHEEL_DETENT : zoom && mode == SCROLL_DISTANCE ? 48.0 : 15.0;
        double total = axis->remainder + movement;
        double whole = total / threshold;
        int steps = whole >= INT_MAX ? INT_MAX : whole <= -INT_MAX ? -INT_MAX : (int)whole;
        axis->remainder = whole >= INT_MAX || whole <= -INT_MAX ? 0 : total - steps * threshold;
        if (steps != 0 && platform->scrolled != NULL)
            platform->scrolled(platform->input_data, index, steps, modifiers);
    }
    if (axis->stopped) axis->remainder = 0;
    axis->distance = 0;
    axis->value120 = axis->discrete = 0;
    axis->has_value120 = axis->has_discrete = axis->stopped = axis->wheel_source = false;
}

static void platform_pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)pointer;
    Platform *platform = data;
    for (uint32_t axis = 0; axis < sizeof platform->scroll / sizeof *platform->scroll; ++axis)
        platform_scroll_frame(platform, axis);
}

static void platform_pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                                 uint32_t axis, wl_fixed_t value)
{
    (void)time;
    Platform *platform = data;
    if (axis >= sizeof platform->scroll / sizeof *platform->scroll) return;
    platform->scroll[axis].distance += wl_fixed_to_double(value);
    if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION)
        platform_scroll_frame(platform, axis);
}

static void platform_pointer_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)pointer;
    Platform *platform = data;
    bool wheel = source == WL_POINTER_AXIS_SOURCE_WHEEL;
#ifdef WL_POINTER_AXIS_SOURCE_WHEEL_TILT_SINCE_VERSION
    wheel |= source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT;
#endif
    for (size_t i = 0; i < sizeof platform->scroll / sizeof *platform->scroll; ++i)
        platform->scroll[i].wheel_source = wheel;
}

static void platform_pointer_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
    (void)pointer;
    (void)time;
    Platform *platform = data;
    if (axis < sizeof platform->scroll / sizeof *platform->scroll) platform->scroll[axis].stopped = true;
}

static void platform_pointer_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t steps)
{
    (void)pointer;
    Platform *platform = data;
    if (axis < sizeof platform->scroll / sizeof *platform->scroll) {
        platform->scroll[axis].has_discrete = true;
        platform->scroll[axis].discrete += (int64_t)steps * WHEEL_DETENT;
    }
}

#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
static void platform_pointer_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120)
{
    (void)pointer;
    Platform *platform = data;
    if (axis < sizeof platform->scroll / sizeof *platform->scroll) {
        platform->scroll[axis].has_value120 = true;
        platform->scroll[axis].value120 += value120;
    }
}
#define PLATFORM_SEAT_VERSION WL_POINTER_AXIS_VALUE120_SINCE_VERSION
#else
#define PLATFORM_SEAT_VERSION WL_POINTER_AXIS_DISCRETE_SINCE_VERSION
#endif

static const struct wl_pointer_listener pointer_listener = {
    .enter = platform_pointer_enter, .leave = platform_pointer_leave,
    .motion = platform_pointer_motion, .button = platform_pointer_button,
    .axis = platform_pointer_axis, .frame = platform_pointer_frame,
    .axis_source = platform_pointer_source, .axis_stop = platform_pointer_stop,
    .axis_discrete = platform_pointer_discrete,
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    .axis_value120 = platform_pointer_value120,
#endif
};

static void platform_drop_pointer(Platform *platform)
{
    if (platform->pointer == NULL) return;
    if (wl_pointer_get_version(platform->pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
        wl_pointer_release(platform->pointer);
    else wl_pointer_destroy(platform->pointer);
    platform->pointer = NULL;
    platform->pointer_inside = false;
    memset(platform->scroll, 0, sizeof platform->scroll);
    if (platform->pointer_left != NULL) platform->pointer_left(platform->input_data);
}

static void platform_drop_seat(Platform *platform)
{
    platform_drop_keyboard(platform);
    platform_drop_pointer(platform);
    if (platform->seat == NULL) return;
    if (wl_seat_get_version(platform->seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
        wl_seat_release(platform->seat);
    else wl_seat_destroy(platform->seat);
    platform->seat = NULL;
}

static void platform_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    Platform *platform = data;
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
        platform_drop_keyboard(platform);
    } else if (platform->keyboard == NULL) {
        platform->keyboard = wl_seat_get_keyboard(seat);
        if (platform->keyboard == NULL ||
            wl_keyboard_add_listener(platform->keyboard, &keyboard_listener, platform) < 0)
            platform->error = "could not initialize the Wayland keyboard";
    }
    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER)) {
        platform_drop_pointer(platform);
    } else if (platform->pointer == NULL) {
        platform->pointer = wl_seat_get_pointer(seat);
        if (platform->pointer == NULL ||
            wl_pointer_add_listener(platform->pointer, &pointer_listener, platform) < 0)
            platform->error = "could not initialize the Wayland pointer";
    }
}

static void platform_seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = platform_seat_capabilities, .name = platform_seat_name
};

static void platform_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {.ping = platform_ping};

static void platform_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    Platform *platform = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0 && platform->compositor == NULL) {
        platform->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                                version < 4 ? version : 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0 && platform->wm_base == NULL) {
        platform->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        if (platform->wm_base == NULL ||
            xdg_wm_base_add_listener(platform->wm_base, &wm_base_listener, platform) < 0)
            platform->error = "could not initialize xdg-shell";
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && platform->seat == NULL) {
        platform->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                         version < PLATFORM_SEAT_VERSION ? version : PLATFORM_SEAT_VERSION);
        platform->seat_name = name;
        if (platform->seat == NULL || wl_seat_add_listener(platform->seat, &seat_listener, platform) < 0)
            platform->error = "could not initialize the Wayland seat";
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0 && platform->clipboard_manager == NULL) {
        platform->clipboard_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface,
                                                       version < 2 ? version : 2);
        platform->clipboard_name = name;
    } else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0 &&
               platform->primary_manager == NULL) {
        platform->primary_manager = wl_registry_bind(registry, name,
                                                     &zwp_primary_selection_device_manager_v1_interface, 1);
        platform->primary_name = name;
    } else if (strcmp(interface, xdg_activation_v1_interface.name) == 0 && platform->activation == NULL) {
        platform->activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
        platform->activation_name = name;
    }
}

static void platform_drop_activation(Platform *platform)
{
    if (platform->activation_token != NULL) xdg_activation_token_v1_destroy(platform->activation_token);
    if (platform->activation != NULL) xdg_activation_v1_destroy(platform->activation);
    platform->activation_token = NULL;
    platform->activation = NULL;
}

static void platform_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)registry;
    Platform *platform = data;
    if (platform->seat != NULL && platform->seat_name == name) platform_drop_seat(platform);
    if (platform->clipboard_manager != NULL && platform->clipboard_name == name) {
        wl_data_device_manager_destroy(platform->clipboard_manager);
        platform->clipboard_manager = NULL;
    }
    if (platform->primary_manager != NULL && platform->primary_name == name) {
        zwp_primary_selection_device_manager_v1_destroy(platform->primary_manager);
        platform->primary_manager = NULL;
    }
    if (platform->activation != NULL && platform->activation_name == name) platform_drop_activation(platform);
}

static void platform_activation_done(void *data, struct xdg_activation_token_v1 *token, const char *name)
{
    Platform *platform = data;
    if (token == platform->activation_token) {
        if (platform->activation != NULL && platform->surface != NULL) {
            xdg_activation_v1_activate(platform->activation, name, platform->surface);
        }
        platform->activation_token = NULL;
    }
    xdg_activation_token_v1_destroy(token);
}

static const struct xdg_activation_token_v1_listener activation_token_listener = {
    .done = platform_activation_done
};

/* Ask for attention without an input serial. dwl marks an unfocused window
   urgent instead of focusing it, and ignores the request when focused. */
static void platform_request_attention(Platform *platform)
{
    if (platform->activation == NULL || platform->activation_token != NULL || platform->surface == NULL) return;
    struct xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(platform->activation);
    if (token == NULL) return;
    xdg_activation_token_v1_add_listener(token, &activation_token_listener, platform);
    xdg_activation_token_v1_set_surface(token, platform->surface);
    xdg_activation_token_v1_commit(token);
    platform->activation_token = token;
}

/* deal runs with a C.UTF-8 character type, so the Compose table follows the
   user's environment instead, the same order setlocale would use. */
static void platform_open_compose(Platform *platform)
{
    const char *candidates[] = {getenv("LC_ALL"), getenv("LC_CTYPE"), getenv("LANG"), "en_US.UTF-8"};
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
        const char *locale = candidates[i];
        if (locale == NULL || *locale == 0) continue;
        platform->compose_table = xkb_compose_table_new_from_locale(platform->xkb, locale,
                                                                    XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (platform->compose_table == NULL) continue;
        platform->compose = xkb_compose_state_new(platform->compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
        if (platform->compose != NULL) {
            platform->compose_locale = locale;
            return;
        }
        xkb_compose_table_unref(platform->compose_table);
        platform->compose_table = NULL;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = platform_global, .global_remove = platform_global_remove
};

static void platform_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    Platform *platform = data;
    xdg_surface_ack_configure(surface, serial);
    if (platform->pending_width > 0) platform->width = platform->pending_width;
    if (platform->pending_height > 0) platform->height = platform->pending_height;
    platform->fullscreen = platform->pending_fullscreen;
    platform->configured = true;
    platform->needs_redraw = true;
}

static const struct xdg_surface_listener surface_listener = {.configure = platform_configure};

static void platform_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                        int32_t width, int32_t height, struct wl_array *states)
{
    (void)toplevel;
    Platform *platform = data;
    platform->pending_width = width;
    platform->pending_height = height;
    platform->pending_fullscreen = false;
    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN) platform->pending_fullscreen = true;
    }
}

static void platform_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    Platform *platform = data;
    platform->closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = platform_toplevel_configure, .close = platform_toplevel_close
};

static const char *platform_open(Platform *platform, const Config *config, const FontMetrics *metrics)
{
    platform->display = wl_display_connect(NULL);
    if (platform->display == NULL) return "could not connect to the Wayland display";
    platform->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (platform->xkb == NULL) return "could not create the XKB context";
    platform_open_compose(platform);
    platform->registry = wl_display_get_registry(platform->display);
    if (platform->registry == NULL ||
        wl_registry_add_listener(platform->registry, &registry_listener, platform) < 0 ||
        wl_display_roundtrip(platform->display) < 0)
        return "could not discover Wayland globals";
    if (platform->error != NULL) return platform->error;
    if (platform->compositor == NULL || platform->wm_base == NULL)
        return "the Wayland compositor must provide wl_compositor and xdg_wm_base";

    platform->surface = wl_compositor_create_surface(platform->compositor);
    if (platform->surface == NULL) return "could not create the Wayland surface";
    platform->xdg_surface = xdg_wm_base_get_xdg_surface(platform->wm_base, platform->surface);
    if (platform->xdg_surface == NULL ||
        xdg_surface_add_listener(platform->xdg_surface, &surface_listener, platform) < 0)
        return "could not create the xdg surface";
    platform->toplevel = xdg_surface_get_toplevel(platform->xdg_surface);
    if (platform->toplevel == NULL ||
        xdg_toplevel_add_listener(platform->toplevel, &toplevel_listener, platform) < 0)
        return "could not create the xdg toplevel";

    platform->width = initial_extent(config->columns, metrics->width, config->padding.horizontal);
    platform->height = initial_extent(config->rows, metrics->height, config->padding.vertical);
    platform_title(platform, config, "");
    xdg_toplevel_set_app_id(platform->toplevel, platform_app_id(config));
    platform_size_hints(platform, config->padding, metrics);
    wl_surface_commit(platform->surface);
    while (!platform->configured && !platform->closed && platform->error == NULL) {
        if (wl_display_dispatch(platform->display) < 0)
            return "lost the Wayland connection waiting for the initial configure";
    }
    return platform->error;
}

static const char *platform_egl_error(Platform *platform, const char *message)
{
    snprintf(platform->egl_error, sizeof platform->egl_error, "%s (EGL error 0x%04x)",
             message, (unsigned int)eglGetError());
    return platform->egl_error;
}

static const char *platform_open_egl(Platform *platform)
{
    platform->window = wl_egl_window_create(platform->surface, platform->width, platform->height);
    if (platform->window == NULL) return "could not create the Wayland EGL window";
    platform->egl = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, platform->display, NULL);
    if (platform->egl == EGL_NO_DISPLAY ||
        !eglInitialize(platform->egl, &platform->egl_major, &platform->egl_minor))
        return platform_egl_error(platform, "could not initialize Wayland EGL");
    platform->egl_initialized = true;

    if (!eglBindAPI(EGL_OPENGL_API)) return platform_egl_error(platform, "could not bind desktop OpenGL");
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    EGLConfig config;
    EGLint count;
    if (!eglChooseConfig(platform->egl, attributes, &config, 1, &count))
        return platform_egl_error(platform, "could not select a Wayland EGL configuration");
    if (count == 0) return "no Wayland EGL configuration supports desktop OpenGL with RGBA";

    /* OpenGL 2.1 preserves the fixed-function renderer and alpha-only glyph textures. */
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    platform->context = eglCreateContext(platform->egl, config, EGL_NO_CONTEXT, context_attributes);
    if (platform->context == EGL_NO_CONTEXT)
        return platform_egl_error(platform, "could not create a compatible desktop OpenGL context");
    platform->egl_surface = eglCreatePlatformWindowSurface(platform->egl, config, platform->window, NULL);
    if (platform->egl_surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(platform->egl, platform->egl_surface, platform->egl_surface, platform->context))
        return platform_egl_error(platform, "could not activate the Wayland EGL window surface");
    /* Wayland frame callbacks throttle drawing even when the driver allows immediate swaps. */
    eglSwapInterval(platform->egl, 0);
    return NULL;
}

static void platform_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)time;
    Platform *platform = data;
    wl_callback_destroy(callback);
    platform->frame = NULL;
}

static const struct wl_callback_listener frame_listener = {.done = platform_frame_done};

static const char *platform_swap(Platform *platform)
{
    /* A resize can present before the previous callback fires. Only the
       newest callback paces the next frame. */
    if (platform->frame != NULL) wl_callback_destroy(platform->frame);
    platform->frame = wl_surface_frame(platform->surface);
    if (platform->frame == NULL || wl_callback_add_listener(platform->frame, &frame_listener, platform) < 0)
        return "could not request a Wayland frame callback";
    if (!eglSwapBuffers(platform->egl, platform->egl_surface))
        return platform_egl_error(platform, "could not present the terminal frame");
    return NULL;
}

static void platform_close(Platform *platform)
{
    if (platform->egl_initialized) {
        eglMakeCurrent(platform->egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (platform->egl_surface != EGL_NO_SURFACE) eglDestroySurface(platform->egl, platform->egl_surface);
        if (platform->context != EGL_NO_CONTEXT) eglDestroyContext(platform->egl, platform->context);
        eglTerminate(platform->egl);
        eglReleaseThread();
    }
    if (platform->window != NULL) wl_egl_window_destroy(platform->window);
    if (platform->frame != NULL) wl_callback_destroy(platform->frame);
    platform_drop_seat(platform);
    if (platform->clipboard_manager != NULL) wl_data_device_manager_destroy(platform->clipboard_manager);
    if (platform->primary_manager != NULL)
        zwp_primary_selection_device_manager_v1_destroy(platform->primary_manager);
    platform_drop_activation(platform);
    xkb_compose_state_unref(platform->compose);
    xkb_compose_table_unref(platform->compose_table);
    xkb_context_unref(platform->xkb);
    if (platform->toplevel != NULL) xdg_toplevel_destroy(platform->toplevel);
    if (platform->xdg_surface != NULL) xdg_surface_destroy(platform->xdg_surface);
    if (platform->surface != NULL) wl_surface_destroy(platform->surface);
    if (platform->wm_base != NULL) xdg_wm_base_destroy(platform->wm_base);
    if (platform->compositor != NULL) wl_compositor_destroy(platform->compositor);
    if (platform->registry != NULL) wl_registry_destroy(platform->registry);
    if (platform->display != NULL) {
        wl_display_flush(platform->display);
        wl_display_disconnect(platform->display);
    }
}

typedef enum { COLOR_DEFAULT, COLOR_INDEXED, COLOR_RGB } ColorKind;

typedef struct {
    ColorKind kind;
    unsigned char index;
    TerminalColor rgb;
} ColorSpec;

typedef struct {
    ColorSpec foreground;
    ColorSpec background;
    ColorSpec underline_color;
    unsigned char underline; /* TERMINAL_UNDERLINE_* */
    bool bold;
    bool dim;
    bool italic;
    bool inverse;
    bool strikethrough;
    bool overline;
    bool concealed;
} Rendition;

typedef struct {
    size_t column;
    size_t row;
    Rendition rendition;
    bool origin;
    bool wrap_pending;
    bool line_drawing[2];
    unsigned int charset;
} TerminalCursor;

typedef struct {
    TerminalScreen screen;
    TerminalCursor cursor;
    TerminalCursor saved;
    size_t scroll_top;
    size_t scroll_bottom;
    size_t combining_index;
    bool can_combine;
    uint16_t active_link;
    CursorStyle cursor_style;
    bool cursor_visible;
    bool cursor_blink;
} TerminalBuffer;

typedef enum {
    PARSE_TEXT, PARSE_ESCAPE, PARSE_ESCAPE_INTERMEDIATE, PARSE_CSI,
    PARSE_STRING, PARSE_STRING_ESCAPE
} ParseState;

typedef struct {
    ParseState state;
    unsigned int parameters[32];
    bool subparameter[32];
    size_t count;
    unsigned char prefix;
    unsigned char intermediate;
    bool started;
    bool has_subparameters;
    bool unsupported;
    bool osc;
    bool osc52; /* The string continues in Terminal.osc52 after "52;". */
    char string[OSC_LIMIT + 1];
    size_t string_length;
} Parser;

typedef struct {
    uint32_t codepoint;
    uint32_t minimum;
    unsigned int remaining;
} Utf8Decoder;

typedef struct {
    TerminalBuffer primary;
    TerminalBuffer alternate;
    TerminalCursor saved_1049;
    History history;
    Selection selection;
    TerminalCell defaults;
    TerminalColor cursor_color, palette[16];
    char dynamic_title[TITLE_LIMIT + 1];
    char **links;
    size_t link_count;
    bool allow_title, title_dirty, allow_links, links_dirty;
    bool focus_reporting, mouse_sgr;
    unsigned int mouse_mode, mouse_generation;
    uint16_t hovered_link;
    CursorStyle default_cursor_style;
    Parser parser;
    Utf8Decoder utf8;
    uint32_t last_character;
    unsigned char tab_stops[USHRT_MAX / 8 + 1];
    void (*reply)(void *context, const char *bytes, size_t length);
    void *reply_context;
    bool using_alternate;
    bool mode_1049;
    bool autowrap;
    bool default_blink;
    bool application_arrows;
    bool application_keypad;
    bool insert_mode;
    bool bracketed_paste, bell_pending;
    bool bold_bright;
    bool synchronized; /* DECSET 2026 holds frames until reset or SYNC_TIMEOUT. */
    bool allow_osc52;
    char *osc52;
    size_t osc52_length, osc52_capacity;
    /* A decoded OSC 52 write waiting for the Wayland side. */
    unsigned char *clipboard_write;
    size_t clipboard_write_length;
    unsigned int clipboard_targets;
    char directory[PATH_MAX]; /* From OSC 7, empty when unknown or remote. */
} Terminal;

static size_t minimum(size_t a, size_t b)
{
    return a < b ? a : b;
}

static TerminalBuffer *terminal_buffer(Terminal *terminal)
{
    return terminal->using_alternate ? &terminal->alternate : &terminal->primary;
}

static TerminalView terminal_view(Terminal *terminal)
{
    TerminalView view = {.screen = &terminal_buffer(terminal)->screen, .blank = terminal->defaults,
                         .hovered_link = terminal->hovered_link};
    if (!terminal->using_alternate) {
        view.history = &terminal->history;
        view.first_live = terminal->history.first_live;
        view.first_visible = view.first_live - terminal->history.offset;
    }
    if (terminal->selection.alternate == terminal->using_alternate) view.selection = &terminal->selection;
    return view;
}

static const char *link_url(const Terminal *terminal, uint16_t id)
{
    return terminal->links != NULL && id && id <= LINK_SLOTS ? terminal->links[id - 1] : NULL;
}

static void links_clear(Terminal *terminal)
{
    if (terminal->links == NULL) return;
    for (size_t i = 0; i < LINK_SLOTS; ++i) free(terminal->links[i]);
    free(terminal->links);
    terminal->links = NULL;
    terminal->link_count = 0;
    terminal->links_dirty = false;
    terminal->hovered_link = 0;
}

static void links_mark(bool used[LINK_SLOTS + 1], const TerminalCell *cells, size_t count)
{
    for (size_t i = 0; i < count; ++i) used[cells[i].hyperlink] = true;
}

static void links_collect(Terminal *terminal)
{
    terminal->links_dirty = false;
    if (terminal->links == NULL) return;
    bool used[LINK_SLOTS + 1] = {false};
    const TerminalBuffer *buffers[] = {&terminal->primary, &terminal->alternate};
    for (size_t i = 0; i < 2; ++i) {
        const TerminalScreen *screen = &buffers[i]->screen;
        used[buffers[i]->active_link] = true;
        links_mark(used, screen->cells, screen->columns * screen->rows);
    }
    const History *history = &terminal->history;
    for (size_t i = 0; i < history->count; ++i) {
        const HistoryLine *line = &history->lines[(history->head + i) % history->capacity];
        if (line->has_links) links_mark(used, line->cells, line->stored);
    }
    for (size_t i = 0; i < LINK_SLOTS; ++i) {
        if (!used[i + 1] && terminal->links[i] != NULL) {
            free(terminal->links[i]);
            terminal->links[i] = NULL;
            --terminal->link_count;
        }
    }
    if (terminal->link_count == 0) links_clear(terminal);
}

static uint16_t link_add(Terminal *terminal, const char *url)
{
    if (terminal->links == NULL) {
        terminal->links = calloc(LINK_SLOTS, sizeof(*terminal->links));
        if (terminal->links == NULL) return 0;
    }
    size_t opening = LINK_SLOTS;
    for (size_t i = 0; i < LINK_SLOTS; ++i) {
        const char *stored = terminal->links[i];
        if (stored == NULL && opening == LINK_SLOTS) opening = i;
        if (stored != NULL && strcmp(stored, url) == 0) return (uint16_t)(i + 1);
    }
    if (opening == LINK_SLOTS) return 0; /* Collection is paced by the event loop. */
    char *stored = strdup(url);
    if (stored == NULL) return 0;
    terminal->links[opening] = stored;
    ++terminal->link_count;
    return (uint16_t)(opening + 1);
}

static void terminal_configure(Terminal *terminal, const Config *config)
{
    terminal->defaults.foreground = config->foreground;
    terminal->defaults.background = config->background;
    terminal->cursor_color = config->cursor;
    memcpy(terminal->palette, config->palette, sizeof terminal->palette);
    terminal->bold_bright = config->bold_bright;
    terminal->allow_osc52 = config->osc52;
    if (terminal->default_cursor_style != config->cursor_style) {
        terminal->default_cursor_style = config->cursor_style;
        terminal->primary.cursor_style = terminal->alternate.cursor_style = config->cursor_style;
    }
    if (terminal->default_blink != config->blink) {
        terminal->default_blink = config->blink;
        terminal->primary.cursor_blink = terminal->alternate.cursor_blink = config->blink;
    }
    terminal->allow_title = config->dynamic_title;
    if (!terminal->allow_title) terminal->dynamic_title[0] = 0;
    terminal->title_dirty = true;
    terminal->allow_links = config->hyperlinks;
    if (!terminal->allow_links) {
        terminal->primary.active_link = terminal->alternate.active_link = 0;
        terminal->links_dirty |= terminal->links != NULL;
        terminal->hovered_link = 0;
    }
    ++terminal->mouse_generation;
}

static void selection_damage(Terminal *terminal, const TerminalBuffer *buffer, size_t row,
                              size_t first, size_t end)
{
    Selection *selection = &terminal->selection;
    bool alternate = buffer == &terminal->alternate;
    if (!selection->active || selection->dragging || selection->alternate != alternate) return;
    uint64_t line = row + (alternate ? 0 : terminal->history.first_live);
    if (point_before((SelectionPoint){line, first}, selection->range.end) &&
        point_before(selection->range.first, (SelectionPoint){line, end})) selection->active = false;
}

static void terminal_reset_modes(Terminal *terminal)
{
    terminal->using_alternate = false;
    terminal->mode_1049 = false;
    terminal->autowrap = true;
    terminal->primary.cursor_visible = terminal->alternate.cursor_visible = true;
    terminal->primary.cursor_style = terminal->alternate.cursor_style = terminal->default_cursor_style;
    terminal->primary.cursor_blink = terminal->alternate.cursor_blink = terminal->default_blink;
    terminal->application_arrows = terminal->application_keypad = terminal->insert_mode = false;
    terminal->bracketed_paste = terminal->focus_reporting = terminal->mouse_sgr = false;
    terminal->mouse_mode = 0;
    terminal->synchronized = false;
    ++terminal->mouse_generation;
    terminal->last_character = 0;
    memset(terminal->tab_stops, 1, sizeof terminal->tab_stops);
}

static void terminal_init(Terminal *terminal, TerminalColor foreground, TerminalColor background,
                          TerminalColor cursor, CursorStyle style, bool blink)
{
    *terminal = (Terminal){.defaults = {.character = ' ', .foreground = foreground,
                                       .background = background, .width = 1,
                                       .foreground_source = SOURCE_FG, .background_source = SOURCE_BG},
                           .cursor_color = cursor, .default_cursor_style = style,
                           .default_blink = blink, .allow_title = true, .allow_links = true};
    memcpy(terminal->palette, ansi_colors, sizeof terminal->palette);
    terminal_reset_modes(terminal);
}

static bool terminal_reset(Terminal *terminal)
{
    terminal_reset_modes(terminal);
    terminal->selection = (Selection){0};
    terminal->history.offset = 0;
    if (terminal->dynamic_title[0]) {
        terminal->dynamic_title[0] = '\0';
        terminal->title_dirty = true;
    }
    TerminalBuffer *buffers[] = {&terminal->primary, &terminal->alternate};
    for (size_t i = 0; i < sizeof buffers / sizeof buffers[0]; ++i) {
        TerminalBuffer *buffer = buffers[i];
        terminal_screen_clear(&buffer->screen, terminal->defaults);
        buffer->cursor = buffer->saved = (TerminalCursor){0};
        buffer->can_combine = false;
        buffer->active_link = 0;
        buffer->scroll_top = 0;
        buffer->scroll_bottom = buffer->screen.rows - 1;
    }
    return true;
}

static TerminalColor palette_color(unsigned int index)
{
    if (index < 16) return ansi_colors[index];
    if (index < 232) {
        static const unsigned char levels[] = {0, 95, 135, 175, 215, 255};
        unsigned int cube = index - 16;
        return (TerminalColor){levels[cube / 36], levels[cube / 6 % 6], levels[cube % 6]};
    }
    unsigned char gray = (unsigned char)(8 + 10 * (index - 232));
    return (TerminalColor){gray, gray, gray};
}

static uint8_t color_source(ColorSpec spec, uint8_t fallback, bool bold)
{
    if (spec.kind == COLOR_DEFAULT) return bold ? SOURCE_ANSI + 15 : fallback;
    if (spec.kind != COLOR_INDEXED || spec.index >= 16) return 0;
    return (uint8_t)(SOURCE_ANSI + spec.index + (bold && spec.index < 8 ? 8 : 0));
}

static TerminalColor resolve_color(const Terminal *terminal, ColorSpec spec, uint8_t source)
{
    switch (source) {
    case 0: return spec.kind == COLOR_RGB ? spec.rgb : palette_color(spec.index);
    case SOURCE_FG: return terminal->defaults.foreground;
    case SOURCE_BG: return terminal->defaults.background;
    default: return terminal->palette[source - SOURCE_ANSI];
    }
}

static TerminalCell terminal_cell(const Terminal *terminal, const TerminalBuffer *buffer,
                                  uint32_t character, unsigned char width)
{
    const Rendition *rendition = &buffer->cursor.rendition;
    uint8_t fs = color_source(rendition->foreground, SOURCE_FG, rendition->bold && terminal->bold_bright);
    uint8_t bs = color_source(rendition->background, SOURCE_BG, false);
    TerminalColor fg = resolve_color(terminal, rendition->foreground, fs);
    TerminalColor bg = resolve_color(terminal, rendition->background, bs);
    if (rendition->dim) {
        if (fs) fs |= SOURCE_DIM;
        fg.red /= 2;
        fg.green /= 2;
        fg.blue /= 2;
    }
    uint8_t us = 0;
    TerminalColor underline = {0};
    if (rendition->underline_color.kind != COLOR_DEFAULT) {
        us = color_source(rendition->underline_color, 0, false);
        underline = resolve_color(terminal, rendition->underline_color, us);
        if (us == 0) us = SOURCE_RGB;
    }
    return (TerminalCell){.character = character, .foreground = rendition->inverse ? bg : fg,
                          .background = rendition->inverse ? fg : bg,
                          .foreground_source = rendition->inverse ? bs : fs,
                          .background_source = rendition->inverse ? fs : bs,
                          .underline = rendition->underline, .underline_source = us,
                          .underline_color = underline,
                          .style = (rendition->bold ? TERMINAL_STYLE_BOLD : 0) |
                                   (rendition->italic ? TERMINAL_STYLE_ITALIC : 0),
                          .strikethrough = rendition->strikethrough, .overline = rendition->overline,
                          .concealed = rendition->concealed, .width = width};
}

static TerminalCell terminal_blank(const Terminal *terminal, const TerminalBuffer *buffer)
{
    TerminalCell cell = terminal_cell(terminal, buffer, ' ', 1);
    /* Erasure keeps only the colors. */
    cell.underline = TERMINAL_UNDERLINE_NONE;
    cell.underline_source = 0;
    cell.underline_color = (TerminalColor){0};
    cell.style = 0;
    cell.strikethrough = cell.overline = cell.concealed = false;
    return cell;
}

static void cursor_resized(TerminalCursor *cursor, size_t first_row, size_t columns, size_t rows)
{
    if (cursor->column != columns - 1) cursor->wrap_pending = false;
    cursor->column = minimum(cursor->column, columns - 1);
    cursor->row = minimum(cursor->row - minimum(cursor->row, first_row), rows - 1);
}

static void buffer_resize_commit(TerminalBuffer *buffer, TerminalScreen resized, size_t first_row)
{
    bool full_region = buffer->scroll_top == 0 && buffer->scroll_bottom + 1 >= buffer->screen.rows;
    terminal_screen_destroy(&buffer->screen);
    buffer->screen = resized;
    buffer->can_combine = false;
    cursor_resized(&buffer->cursor, first_row, resized.columns, resized.rows);
    cursor_resized(&buffer->saved, first_row, resized.columns, resized.rows);
    buffer->scroll_top = minimum(buffer->scroll_top - minimum(buffer->scroll_top, first_row),
                                 resized.rows - 1);
    buffer->scroll_bottom = minimum(buffer->scroll_bottom - minimum(buffer->scroll_bottom, first_row),
                                    resized.rows - 1);
    if (full_region || buffer->scroll_top >= buffer->scroll_bottom) {
        buffer->scroll_top = 0;
        buffer->scroll_bottom = resized.rows - 1;
    }
}

/* Reflow the primary transcript as a stream of logical lines. The first pass
   maps screen anchors, and the second builds only the rows that can be kept. */
typedef struct {
    uint64_t source_line;
    size_t source_column, row, column;
    bool pending, mapped, wrap;
} ReflowAnchor;

typedef struct {
    size_t width, row, column, live_top, keep_from;
    TerminalCell *cells;
    TerminalScreen *screen;
    History *history;
    TerminalCell blank;
    bool track_links;
} Reflow;

static bool reflow_row(Reflow *flow, bool wrapped)
{
    if (flow->cells != NULL && flow->row >= flow->keep_from) {
        if (flow->row < flow->live_top) {
            size_t stored = line_content(flow->cells, flow->column, flow->blank);
            if (!history_write(flow->history, flow->cells, flow->width, stored, wrapped,
                               flow->track_links)) return false;
        } else if (flow->row - flow->live_top < flow->screen->rows) {
            size_t row = flow->row - flow->live_top;
            memcpy(screen_row(flow->screen, row), flow->cells, flow->column * sizeof(*flow->cells));
            *screen_wrap(flow->screen, row) = wrapped && row + 1 < flow->screen->rows;
        }
    }
    ++flow->row;
    flow->column = 0;
    return true;
}

static void reflow_map(Reflow *flow, ReflowAnchor *anchor, size_t advance)
{
    size_t column = flow->column + advance;
    anchor->row = flow->row;
    anchor->column = minimum(column, flow->width - 1);
    anchor->wrap = column == flow->width;
    anchor->mapped = true;
}

static bool reflow_lines(Reflow *flow, const TerminalView *view, size_t last_live,
                         ReflowAnchor *anchors, size_t anchor_count)
{
    uint64_t first = view->first_live - view->history->count;
    uint64_t last = view->first_live + last_live;
    for (uint64_t row = first; row <= last; ++row) {
        LineView line = view_line(view, row);
        size_t length = row < view->first_live ? line.stored :
                       line_content(line.cells, line.stored, flow->blank);
        size_t used = length;
        for (size_t a = 0; a < anchor_count; ++a) {
            ReflowAnchor *anchor = &anchors[a];
            if (anchor->source_line == row && !line.wrapped && !anchor->pending)
                used = used > anchor->source_column ? used : anchor->source_column;
        }
        for (size_t i = 0; i < used;) {
            TerminalCell cell = i < length ? line.cells[i] : flow->blank;
            size_t span = cell.width == 2 && i + 1 < length && line.cells[i + 1].width == 0 ? 2 : 1;
            bool wide = span == 2 || cell.narrowed;
            size_t placed = wide && flow->width > 1 ? 2 : 1;
            if (flow->column + placed > flow->width && !reflow_row(flow, true)) return false;
            for (size_t a = 0; a < anchor_count; ++a) {
                ReflowAnchor *anchor = &anchors[a];
                if (anchor->source_line == row && anchor->source_column >= i &&
                    anchor->source_column < i + span) {
                    size_t advance = anchor->pending ? placed : minimum(anchor->source_column - i, placed - 1);
                    reflow_map(flow, anchor, advance);
                }
            }
            if (flow->cells != NULL) {
                if (cell.width == 0) cell = flow->blank;
                cell.width = (unsigned int)placed;
                cell.narrowed = wide && placed == 1;
                flow->cells[flow->column] = cell;
                if (placed == 2) {
                    cell.character = 0;
                    cell.width = 0;
                    cell.combining[0] = cell.combining[1] = 0;
                    flow->cells[flow->column + 1] = cell;
                }
            }
            flow->column += placed;
            i += span;
        }
        for (size_t a = 0; a < anchor_count; ++a) {
            ReflowAnchor *anchor = &anchors[a];
            if (anchor->source_line == row && !anchor->mapped) reflow_map(flow, anchor, 0);
        }
        if ((!line.wrapped || row == last) && !reflow_row(flow, false)) return false;
    }
    return true;
}

static void reflow_cursor(TerminalCursor *cursor, ReflowAnchor anchor, size_t top,
                          const TerminalScreen *screen)
{
    if (!anchor.mapped) {
        cursor_resized(cursor, 0, screen->columns, screen->rows);
        return;
    }
    cursor->column = anchor.column;
    cursor->row = minimum(anchor.row - minimum(anchor.row, top), screen->rows - 1);
    cursor->wrap_pending = anchor.wrap && anchor.row >= top && anchor.row - top < screen->rows;
}

static bool primary_needs_reflow(const TerminalView *view, TerminalCell blank,
                                 size_t width, size_t *last_live)
{
    bool reflow = false;
    for (size_t i = 0; i < view->screen->rows; ++i) {
        const TerminalCell *line = screen_row(view->screen, i);
        size_t used = line_content(line, view->screen->columns, blank);
        bool wrapped = *screen_wrap(view->screen, i);
        if ((used || wrapped) && i > *last_live) *last_live = i;
        reflow |= wrapped || used > width || (used != 0 && view->screen->columns == 1);
    }
    const History *history = view->history;
    for (size_t i = 0; i < history->count && !reflow; ++i) {
        const HistoryLine *line = &history->lines[(history->head + i) % history->capacity];
        reflow = line->wrapped || line->stored > width || (line->stored && line->columns == 1);
    }
    return reflow;
}

static bool primary_reflow(Terminal *terminal, const TerminalView *view, size_t last_live,
                           TerminalScreen *screen, History *history)
{
    const TerminalBuffer *primary = &terminal->primary;
    TerminalCursor cursors[] = {primary->cursor, primary->saved, terminal->saved_1049};
    ReflowAnchor anchors[4] = {{0}};
    for (size_t i = 0; i < 3; ++i) {
        anchors[i].source_line = view->first_live + cursors[i].row;
        anchors[i].source_column = cursors[i].column;
        anchors[i].pending = cursors[i].wrap_pending;
    }
    anchors[3].source_line = view->first_visible;
    Reflow flow = {.width = screen->columns, .blank = terminal->defaults};
    reflow_lines(&flow, view, last_live, anchors, 4);
    size_t top = flow.row > screen->rows ? flow.row - screen->rows : 0;
    top = minimum(top, anchors[0].row);
    size_t capacity = terminal->history.capacity;
    *history = (History){.capacity = capacity, .first_live = top};
    if (capacity != 0) {
        history->lines = calloc(capacity, sizeof(*history->lines));
        if (history->lines == NULL) return false;
    }
    flow = (Reflow){.width = screen->columns, .blank = terminal->defaults, .screen = screen,
                    .history = history, .live_top = top, .keep_from = top - minimum(top, capacity),
                    .track_links = terminal->links != NULL};
    flow.cells = malloc(screen->columns * sizeof(*flow.cells));
    bool okay = flow.cells != NULL && reflow_lines(&flow, view, last_live, anchors, 4);
    free(flow.cells);
    if (!okay) {
        history_clear(history);
        free(history->lines);
        return false;
    }
    if (terminal->history.offset && anchors[3].row < top)
        history->offset = minimum(top - anchors[3].row, history->count);
    reflow_cursor(&terminal->primary.cursor, anchors[0], top, screen);
    reflow_cursor(&terminal->primary.saved, anchors[1], top, screen);
    reflow_cursor(&terminal->saved_1049, anchors[2], top, screen);
    return true;
}

static bool terminal_resize(Terminal *terminal, size_t columns, size_t rows)
{
    if (columns == 0 || rows == 0 || columns > USHRT_MAX || rows > USHRT_MAX) return false;
    TerminalBuffer *primary = &terminal->primary;
    TerminalBuffer *alternate = &terminal->alternate;
    if (primary->screen.columns == columns && primary->screen.rows == rows) return true;

    /* Allocate both buffers before changing viewport or cursor state. */
    size_t primary_first = primary->cursor.row - minimum(primary->cursor.row, rows - 1);
    size_t alternate_first = alternate->cursor.row - minimum(alternate->cursor.row, rows - 1);
    size_t last_live = primary->cursor.row;
    TerminalView view = {.screen = &primary->screen, .history = &terminal->history,
                         .first_live = terminal->history.first_live,
                         .first_visible = terminal->history.first_live - terminal->history.offset};
    bool reflow = primary->screen.columns != 0 && primary->screen.columns != columns &&
                  primary->scroll_top == 0 && primary->scroll_bottom + 1 == primary->screen.rows &&
                  !primary->cursor.origin && primary_needs_reflow(&view, terminal->defaults, columns, &last_live);
    TerminalScreen primary_resized, alternate_resized, empty = {0};
    if (!screen_resized(&primary_resized, reflow ? &empty : &primary->screen, columns, rows,
                        reflow ? 0 : primary_first, reflow ? terminal->defaults : terminal_blank(terminal, primary)))
        return false;
    if (!screen_resized(&alternate_resized, &alternate->screen, columns, rows, alternate_first,
                        terminal_blank(terminal, alternate))) {
        terminal_screen_destroy(&primary_resized);
        return false;
    }
    History next;
    if (reflow && !primary_reflow(terminal, &view, last_live, &primary_resized, &next)) {
        terminal_screen_destroy(&primary_resized);
        terminal_screen_destroy(&alternate_resized);
        return false;
    }
    terminal->links_dirty |= terminal->links != NULL;
    buffer_resize_commit(primary, primary_resized, reflow ? 0 : primary_first);
    buffer_resize_commit(alternate, alternate_resized, alternate_first);
    if (reflow) {
        history_clear(&terminal->history);
        free(terminal->history.lines);
        terminal->history = next;
    } else cursor_resized(&terminal->saved_1049, primary_first, columns, rows);
    if (reflow || terminal->selection.dragging || terminal->selection.alternate ||
        terminal->selection.range.end.line >= terminal->history.first_live) {
        terminal->selection = (Selection){0};
    }
    return true;
}

static bool terminal_move(TerminalBuffer *buffer, size_t column, size_t row)
{
    TerminalCursor *cursor = &buffer->cursor;
    buffer->can_combine = false;
    column = minimum(column, buffer->screen.columns - 1);
    row = minimum(row, buffer->screen.rows - 1);
    bool changed = column != cursor->column || row != cursor->row;
    cursor->column = column;
    cursor->row = row;
    cursor->wrap_pending = false;
    return changed;
}

static bool terminal_position(TerminalBuffer *buffer, size_t column, size_t row)
{
    if (buffer->cursor.origin) {
        row = buffer->scroll_top + minimum(row, buffer->scroll_bottom - buffer->scroll_top);
    }
    return terminal_move(buffer, column, row);
}

static bool terminal_vertical(TerminalBuffer *buffer, size_t column, size_t amount, bool down)
{
    size_t row = buffer->cursor.row;
    size_t top = 0, bottom = buffer->screen.rows - 1;
    if (row >= buffer->scroll_top && row <= buffer->scroll_bottom) {
        top = buffer->scroll_top;
        bottom = buffer->scroll_bottom;
    }
    row = down ? row + minimum(amount, bottom - row) : row - minimum(amount, row - top);
    return terminal_move(buffer, column, row);
}

static void terminal_scroll(Terminal *terminal, TerminalBuffer *buffer, size_t top, size_t bottom,
                            size_t amount, bool down, bool archive)
{
    buffer->can_combine = false;
    TerminalScreen *screen = &buffer->screen;
    size_t height = bottom - top + 1;
    amount = minimum(amount, height);
    if (archive) {
        history_push(&terminal->history, screen, terminal->defaults, terminal->links != NULL);
    } else if ((buffer == &terminal->alternate) == terminal->selection.alternate) {
        uint64_t first = buffer == &terminal->primary ? terminal->history.first_live : 0;
        if (terminal->selection.range.first.line <= first + bottom &&
            terminal->selection.range.end.line >= first + top) terminal->selection = (Selection){0};
    }
    size_t retained = height - amount;
    if (height == screen->rows) {
        screen->first_row = screen_row_index(screen, down ? retained : amount);
    } else if (amount != 0) {
        /* Partial regions move in logical order even when their physical rows wrap. */
        for (size_t i = 0; i < retained; ++i) {
            size_t row = top + (down ? retained - 1 - i : i);
            size_t from = row + (down ? 0 : amount);
            size_t to = row + (down ? amount : 0);
            memcpy(screen_row(screen, to), screen_row(screen, from), screen->columns * sizeof(TerminalCell));
            *screen_wrap(screen, to) = *screen_wrap(screen, from);
        }
    }
    TerminalCell fill = terminal_blank(terminal, buffer);
    size_t fill_start = top + (down ? 0 : retained);
    for (size_t row = fill_start; row < fill_start + amount; ++row) {
        screen_fill(screen_row(screen, row), 0, screen->columns, fill);
        *screen_wrap(screen, row) = false;
    }
}

static bool terminal_index(Terminal *terminal, TerminalBuffer *buffer, bool down, bool wrapped)
{
    TerminalCursor *cursor = &buffer->cursor;
    cursor->wrap_pending = false;
    if (down) *screen_wrap(&buffer->screen, cursor->row) = wrapped;
    if (cursor->row == (down ? buffer->scroll_bottom : buffer->scroll_top)) {
        bool archive = down && buffer == &terminal->primary && buffer->scroll_top == 0 &&
                       buffer->scroll_bottom + 1 == buffer->screen.rows;
        terminal_scroll(terminal, buffer, buffer->scroll_top, buffer->scroll_bottom, 1, !down, archive);
        return true;
    }
    size_t row = cursor->row;
    if (down) row += row + 1 < buffer->screen.rows;
    else row -= row > 0;
    return terminal_move(buffer, cursor->column, row);
}

/* Editing at either half of a wide character clears its partner. */
static void screen_break_wide(TerminalCell *line, size_t columns, size_t column, TerminalCell fill)
{
    if (line[column].width == 0 && column > 0) line[column - 1] = fill;
    if (line[column].width == 2 && column + 1 < columns) line[column + 1] = fill;
}

static void screen_repair_line(TerminalCell *line, size_t columns, TerminalCell fill)
{
    for (size_t column = 0; column < columns; ++column) {
        if (line[column].width == 2 && column + 1 < columns && line[column + 1].width == 0) {
            ++column;
        } else if (line[column].width != 1) {
            line[column] = fill;
        }
    }
}

static void terminal_chars(Terminal *terminal, TerminalBuffer *buffer, size_t amount,
                            unsigned char operation)
{
    buffer->can_combine = false;
    TerminalScreen *screen = &buffer->screen;
    size_t row = buffer->cursor.row;
    TerminalCell *line = screen_row(screen, row);
    size_t start = buffer->cursor.column;
    size_t end = screen->columns;
    TerminalCell fill = terminal_blank(terminal, buffer);
    amount = minimum(amount, end - start);
    size_t fill_start = start;
    if (operation != 'X' && amount < end - start) {
        /* Break a pair only where the shift cuts between its two cells. */
        if (line[start].width == 0) {
            screen_break_wide(line, end, start, fill);
            line[start] = fill;
        }
        size_t cut = operation == '@' ? end - amount : start + amount;
        if (line[cut].width == 0) {
            screen_break_wide(line, end, cut, fill);
            line[cut] = fill;
        }
        size_t moved = (end - start - amount) * sizeof(*screen->cells);
        memmove(line + start + (operation == '@' ? amount : 0),
                line + start + (operation == 'P' ? amount : 0), moved);
    }
    if (operation == 'P') fill_start = end - amount;
    if (amount != 0) {
        if (operation == 'X' || amount == end - start) {
            screen_break_wide(line, end, fill_start, fill);
            screen_break_wide(line, end, fill_start + amount - 1, fill);
        }
        screen_fill(line, fill_start, fill_start + amount, fill);
    }
    screen_repair_line(line, end, fill);
    selection_damage(terminal, buffer, row, start > 0 ? start - 1 : 0,
                     operation == 'X' ? minimum(start + amount + 1, end) : end);
    buffer->cursor.wrap_pending = false;
}

static bool terminal_combine(Terminal *terminal, TerminalBuffer *buffer, uint32_t character)
{
    /* Format controls and variation selectors are zero-width but aren't marks.
       Store a short run of actual overlays on the last written base cell. */
    if (!buffer->can_combine || character == 0x034f || character == 0xfeff ||
        (character >= 0x180b && character <= 0x180f) ||
        (character >= 0x200b && character <= 0x200f) ||
        (character >= 0x202a && character <= 0x202e) ||
        (character >= 0x2060 && character <= 0x206f) ||
        (character >= 0xfe00 && character <= 0xfe0f) ||
        (character >= 0xe0000 && character <= 0xe01ef)) return false;
    size_t row = buffer->combining_index / buffer->screen.columns;
    size_t column = buffer->combining_index % buffer->screen.columns;
    TerminalCell *base = &screen_row(&buffer->screen, row)[column];
    for (size_t i = 0; i < sizeof base->combining / sizeof base->combining[0]; ++i) {
        if (base->combining[i] == 0) {
            selection_damage(terminal, buffer, row, column, column + base->width);
            base->combining[i] = character;
            return true;
        }
    }
    return false;
}

static bool terminal_print(Terminal *terminal, uint32_t character)
{
    TerminalBuffer *buffer = terminal_buffer(terminal);
    TerminalCursor *cursor = &buffer->cursor;
    TerminalScreen *screen = &buffer->screen;
    int columns = character < 0x80 ? 1 : wcwidth((wchar_t)character);
    if (columns == 0) return terminal_combine(terminal, buffer, character);
    unsigned char width = columns == 2 ? 2 : 1;

    if (width == 2 && (screen->columns == 1 ||
                      (!terminal->autowrap && cursor->column + 1 == screen->columns))) {
        width = 1;
    }
    bool narrowed = width == 1 && columns == 2;
    if ((terminal->autowrap && cursor->wrap_pending) ||
        (width == 2 && cursor->column + 1 == screen->columns)) {
        cursor->column = 0;
        terminal_index(terminal, buffer, true, true);
    }
    if (terminal->insert_mode) terminal_chars(terminal, buffer, width, '@');
    size_t column = cursor->column;
    TerminalCell *line = screen_row(screen, cursor->row);
    TerminalCell *target = &line[column];
    bool wide_neighbor = width == 2 && target[1].width == 2;
    if (terminal->selection.active &&
        (target->character != character || target->width != width || !target->written || target->combining[0])) {
        size_t first = target->width == 0 && column > 0 ? column - 1 : column;
        size_t end = column + (target->width == 2 ? 2 : width) + wide_neighbor;
        selection_damage(terminal, buffer, cursor->row, first, end);
    }
    if (target->width != 1 || (width == 2 && target[1].width != 1)) {
        TerminalCell fill = terminal_blank(terminal, buffer);
        screen_break_wide(line, screen->columns, column, fill);
        if (width == 2) screen_break_wide(line, screen->columns, column + 1, fill);
    }
    TerminalCell cell = terminal_cell(terminal, buffer, character, width);
    cell.written = true;
    cell.narrowed = narrowed;
    cell.hyperlink = buffer->active_link;
    target[0] = cell;
    if (width == 2) {
        cell.character = 0;
        cell.width = 0;
        target[1] = cell;
    }
    buffer->combining_index = cursor->row * screen->columns + column;
    buffer->can_combine = true;
    if (cursor->column + width < screen->columns) cursor->column += width;
    else {
        cursor->column = screen->columns - 1;
        cursor->wrap_pending = terminal->autowrap;
    }
    terminal->last_character = character;
    return true;
}

static bool terminal_erase(Terminal *terminal, TerminalBuffer *buffer, bool display, unsigned int mode)
{
    if (display && mode == 3 && buffer == &terminal->primary) {
        history_clear(&terminal->history);
        return true;
    }
    if (mode > 2) return false;
    buffer->can_combine = false;
    TerminalScreen *screen = &buffer->screen;
    size_t line = buffer->cursor.row * screen->columns;
    size_t position = line + buffer->cursor.column;
    size_t start = mode == 0 ? position : display ? 0 : line;
    size_t end = mode == 1 ? position + 1 : display ? screen->rows * screen->columns : line + screen->columns;
    TerminalCell fill = terminal_blank(terminal, buffer);
    size_t first_row = start / screen->columns, last_row = (end - 1) / screen->columns;
    for (size_t row = first_row; row <= last_row; ++row) {
        TerminalCell *cells = screen_row(screen, row);
        size_t first = row == first_row ? start % screen->columns : 0;
        size_t last = row == last_row ? (end - 1) % screen->columns + 1 : screen->columns;
        selection_damage(terminal, buffer, row, first - (first > 0 && cells[first].width == 0),
                         minimum(screen->columns, last + (cells[last - 1].width == 2)));
        screen_break_wide(cells, screen->columns, first, fill);
        screen_break_wide(cells, screen->columns, last - 1, fill);
        screen_fill(cells, first, last, fill);
        if (first == 0 && last == screen->columns) *screen_wrap(screen, row) = false;
    }
    return true;
}

static bool terminal_tab(Terminal *terminal, TerminalBuffer *buffer, size_t amount, bool forward)
{
    size_t column = buffer->cursor.column;
    size_t edge = forward ? buffer->screen.columns - 1 : 0;
    while (amount != 0 && column != edge) {
        column = forward ? column + 1 : column - 1;
        if (terminal->tab_stops[column / 8] & (1u << (column % 8))) --amount;
    }
    return terminal_move(buffer, column, buffer->cursor.row);
}

/* Incremental VT parser */

static ColorSpec indexed_color(unsigned int index)
{
    return (ColorSpec){.kind = COLOR_INDEXED, .index = (unsigned char)index};
}

static size_t sgr_extended(ColorSpec *target, const Parser *parser, size_t first, size_t end)
{
    const unsigned int *p = parser->parameters;
    bool colon = first + 1 < end && parser->subparameter[first + 1];
    size_t next = first + 1;
    if (next == end) return end;
    unsigned int format = p[next++];
    if (format == 5 && next < end) {
        if (p[next] <= 255) *target = indexed_color(p[next]);
        ++next;
    } else if (format == 2) {
        if (colon && end - next == 4) {
            if (p[next++] != 0) return end;
        }
        if (end - next >= 3) {
            if (p[next] <= 255 && p[next + 1] <= 255 && p[next + 2] <= 255) {
                *target = (ColorSpec){.kind = COLOR_RGB,
                    .rgb = {(unsigned char)p[next], (unsigned char)p[next + 1], (unsigned char)p[next + 2]}};
            }
            next += 3;
        } else next = end;
    }
    return colon ? end : next;
}

static void terminal_sgr(Rendition *rendition, const Parser *parser)
{
    for (size_t i = 0; i < parser->count;) {
        unsigned int code = parser->parameters[i];
        size_t end = i + 1;
        while (end < parser->count && parser->subparameter[end]) ++end;
        if (code == 38 || code == 48 || code == 58) {
            ColorSpec *target = code == 38 ? &rendition->foreground
                               : code == 48 ? &rendition->background : &rendition->underline_color;
            size_t limit = end > i + 1 ? end : parser->count;
            i = sgr_extended(target, parser, i, limit);
            continue;
        }
        if (end > i + 1) {
            /* 4:0 through 4:5 select an underline style. */
            if (code == 4 && parser->parameters[i + 1] <= TERMINAL_UNDERLINE_DASHED) {
                rendition->underline = (unsigned char)parser->parameters[i + 1];
            }
            i = end;
            continue;
        }
        switch (code) {
        case 0: *rendition = (Rendition){0}; break;
        case 1: rendition->bold = true; break;
        case 2: rendition->dim = true; break;
        case 3: rendition->italic = true; break;
        case 4: rendition->underline = TERMINAL_UNDERLINE_SINGLE; break;
        case 7: rendition->inverse = true; break;
        case 8: rendition->concealed = true; break;
        case 9: rendition->strikethrough = true; break;
        case 21: rendition->underline = TERMINAL_UNDERLINE_DOUBLE; break;
        case 22: rendition->bold = rendition->dim = false; break;
        case 23: rendition->italic = false; break;
        case 24: rendition->underline = TERMINAL_UNDERLINE_NONE; break;
        case 27: rendition->inverse = false; break;
        case 28: rendition->concealed = false; break;
        case 29: rendition->strikethrough = false; break;
        case 39: rendition->foreground = (ColorSpec){0}; break;
        case 49: rendition->background = (ColorSpec){0}; break;
        case 53: rendition->overline = true; break;
        case 55: rendition->overline = false; break;
        case 59: rendition->underline_color = (ColorSpec){0}; break;
        /* Blink (5, 6 and 25) is accepted and deliberately not animated. */
        default:
            if (code >= 30 && code <= 37) rendition->foreground = indexed_color(code - 30);
            else if (code >= 40 && code <= 47) rendition->background = indexed_color(code - 40);
            else if (code >= 90 && code <= 97) rendition->foreground = indexed_color(code - 90 + 8);
            else if (code >= 100 && code <= 107) rendition->background = indexed_color(code - 100 + 8);
            break;
        }
        i = end;
    }
}

static bool terminal_restore(TerminalBuffer *buffer, TerminalCursor saved)
{
    buffer->can_combine = false;
    buffer->cursor = saved;
    if (buffer->cursor.origin) {
        size_t row = minimum(buffer->cursor.row, buffer->scroll_bottom);
        if (row < buffer->scroll_top) row = buffer->scroll_top;
        if (row != buffer->cursor.row) buffer->cursor.wrap_pending = false;
        buffer->cursor.row = row;
    }
    return true;
}

static bool terminal_alternate(Terminal *terminal, unsigned int mode, bool set)
{
    bool changed = terminal->using_alternate != set;
    if (mode == 1049 && set && !terminal->mode_1049) {
        terminal->saved_1049 = terminal->primary.cursor;
        terminal->mode_1049 = true;
        terminal_screen_clear(&terminal->alternate.screen, terminal->defaults);
        terminal->alternate.active_link = 0;
        terminal_position(&terminal->alternate, 0, 0);
        terminal->alternate.cursor_style = terminal->primary.cursor_style;
        terminal->alternate.cursor_visible = terminal->primary.cursor_visible;
        terminal->alternate.cursor_blink = terminal->primary.cursor_blink;
        changed = true;
    } else if (mode == 1047 && !set && terminal->using_alternate) {
        terminal_screen_clear(&terminal->alternate.screen, terminal->defaults);
        terminal->alternate.active_link = 0;
        terminal->alternate.cursor.wrap_pending = false;
    }
    terminal->using_alternate = set;
    if (!set && terminal->mode_1049) {
        terminal->mode_1049 = false;
        changed |= terminal_restore(&terminal->primary, terminal->saved_1049);
    }
    if (changed) {
        terminal_buffer(terminal)->can_combine = false;
        terminal->selection.dragging = false;
        if (set && terminal->selection.alternate && mode == 1049) terminal->selection.active = false;
    }
    return changed;
}

static bool terminal_mode(Terminal *terminal, const Parser *parser, bool set)
{
    bool changed = false;
    for (size_t i = 0; i < parser->count; ++i) {
        unsigned int mode = parser->parameters[i];
        if (parser->prefix == 0) {
            if (mode == 4) terminal->insert_mode = set;
            continue;
        }
        TerminalBuffer *buffer = terminal_buffer(terminal);
        switch (mode) {
        case 1: terminal->application_arrows = set; break;
        case 6:
            buffer->cursor.origin = set;
            changed |= terminal_position(buffer, 0, 0);
            break;
        case 7:
            if (terminal->autowrap != set) buffer->cursor.wrap_pending = false;
            terminal->autowrap = set;
            break;
        case 12:
            changed |= buffer->cursor_blink != set;
            buffer->cursor_blink = set;
            break;
        case 25:
            changed |= buffer->cursor_visible != set;
            buffer->cursor_visible = set;
            break;
        case 66: terminal->application_keypad = set; break;
        case 1000: case 1002: case 1003: {
            unsigned int next = set ? mode : terminal->mouse_mode == mode ? 0 : terminal->mouse_mode;
            if (terminal->mouse_mode != next) ++terminal->mouse_generation;
            terminal->mouse_mode = next;
            break;
        }
        case 1004: terminal->focus_reporting = set; break;
        case 1006:
            if (terminal->mouse_sgr != set) ++terminal->mouse_generation;
            terminal->mouse_sgr = set;
            break;
        case 2004: terminal->bracketed_paste = set; break;
        case 2026:
            changed |= terminal->synchronized && !set;
            terminal->synchronized = set;
            break;
        case 47: case 1047: case 1049:
            changed |= terminal_alternate(terminal, mode, set);
            break;
        case 1048:
            if (set) buffer->saved = buffer->cursor;
            else changed |= terminal_restore(buffer, buffer->saved);
            break;
        }
    }
    return changed;
}

static void terminal_reply(Terminal *terminal, const char *bytes)
{
    if (terminal->reply != NULL) terminal->reply(terminal->reply_context, bytes, strlen(bytes));
}

static void terminal_query(Terminal *terminal, const TerminalBuffer *buffer, unsigned char final,
                           unsigned char prefix, unsigned int parameter)
{
    char response[48];
    if (final == 'n' && (prefix == 0 || prefix == '?')) {
        if (parameter == 5 && prefix == 0) terminal_reply(terminal, "\033[0n");
        if (parameter == 6) {
            size_t row = buffer->cursor.row;
            if (buffer->cursor.origin) row -= buffer->scroll_top;
            snprintf(response, sizeof response, "\033[%s%zu;%zuR", prefix == '?' ? "?" : "",
                     row + 1, buffer->cursor.column + 1);
            terminal_reply(terminal, response);
        }
    } else if (final == 'c' && parameter == 0) {
        if (prefix == 0) terminal_reply(terminal, "\033[?1;2c");
    } else if (final == 't' && prefix == 0 && parameter == 18) {
        snprintf(response, sizeof response, "\033[8;%zu;%zut", buffer->screen.rows,
                 buffer->screen.columns);
        terminal_reply(terminal, response);
    }
}

static void terminal_mode_query(Terminal *terminal, const TerminalBuffer *buffer,
                                unsigned char prefix, unsigned int mode)
{
    unsigned int status = 0;
    if (prefix == '?') {
        switch (mode) {
        case 1: status = terminal->application_arrows ? 1 : 2; break;
        case 6: status = buffer->cursor.origin ? 1 : 2; break;
        case 7: status = terminal->autowrap ? 1 : 2; break;
        case 12: status = buffer->cursor_blink ? 1 : 2; break;
        case 25: status = buffer->cursor_visible ? 1 : 2; break;
        case 47: case 1047: status = terminal->using_alternate ? 1 : 2; break;
        case 66: status = terminal->application_keypad ? 1 : 2; break;
        case 1000: case 1002: case 1003: status = terminal->mouse_mode == mode ? 1 : 2; break;
        case 1004: status = terminal->focus_reporting ? 1 : 2; break;
        case 1006: status = terminal->mouse_sgr ? 1 : 2; break;
        case 1049: status = terminal->mode_1049 ? 1 : 2; break;
        case 2004: status = terminal->bracketed_paste ? 1 : 2; break;
        case 2026: status = terminal->synchronized ? 1 : 2; break;
        }
    } else if (prefix == 0 && mode == 4) status = terminal->insert_mode ? 1 : 2;
    char response[32];
    snprintf(response, sizeof response, "\033[%s%u;%u$y", prefix == '?' ? "?" : "", mode, status);
    terminal_reply(terminal, response);
}

static bool terminal_cursor_style(TerminalBuffer *buffer, unsigned int style)
{
    static const CursorStyle styles[] = {CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BAR};
    if (style > 6) return false;
    if (style == 0) style = 1;
    buffer->cursor_style = styles[(style - 1) / 2];
    buffer->cursor_blink = style % 2 != 0;
    return true;
}

/* DECSTR resets editing modes and cursor attributes while preserving the
   active screen, its contents, and the cursor position. */
static bool terminal_soft_reset(Terminal *terminal, TerminalBuffer *buffer)
{
    terminal->autowrap = true;
    terminal->insert_mode = terminal->application_arrows = terminal->application_keypad = false;
    buffer->scroll_top = 0;
    buffer->scroll_bottom = buffer->screen.rows - 1;
    buffer->cursor_visible = true;
    buffer->cursor_style = terminal->default_cursor_style;
    buffer->cursor_blink = terminal->default_blink;
    buffer->cursor = (TerminalCursor){.column = buffer->cursor.column, .row = buffer->cursor.row};
    buffer->saved = (TerminalCursor){0};
    buffer->can_combine = false;
    return true;
}

static bool terminal_csi(Terminal *terminal, unsigned char final)
{
    const Parser *parser = &terminal->parser;
    if (parser->unsupported || (parser->has_subparameters && final != 'm')) return false;
    TerminalBuffer *buffer = terminal_buffer(terminal);
    unsigned int first = parser->parameters[0];
    unsigned int second = parser->count > 1 ? parser->parameters[1] : 0;
    size_t offset = first > 0 ? first - 1 : 0;
    size_t amount = first > 0 ? first : 1;
    size_t column = buffer->cursor.column, row = buffer->cursor.row;
    if (parser->intermediate != 0) {
        if (parser->intermediate == '!' && parser->prefix == 0 && final == 'p' && first == 0)
            return terminal_soft_reset(terminal, buffer);
        if (parser->intermediate == ' ' && parser->prefix == 0 && final == 'q') {
            return terminal_cursor_style(buffer, first);
        }
        if (parser->intermediate == '$' && final == 'p' && (parser->prefix == 0 || parser->prefix == '?')) {
            terminal_mode_query(terminal, buffer, parser->prefix, first);
        }
        return false;
    }
    if ((final == 'h' || final == 'l') && (parser->prefix == 0 || parser->prefix == '?')) {
        return terminal_mode(terminal, parser, final == 'h');
    }
    if (final == 'n' || final == 'c' || final == 't') {
        terminal_query(terminal, buffer, final, parser->prefix, first);
        return false;
    }
    if (parser->prefix != 0) return false;
    switch (final) {
    case 'H': case 'f': return terminal_position(buffer, second > 0 ? second - 1 : 0, offset);
    case 'A': return terminal_vertical(buffer, column, amount, false);
    case 'B': case 'e': return terminal_vertical(buffer, column, amount, true);
    case 'C': case 'a': return terminal_move(buffer, column + amount, row);
    case 'D': return terminal_move(buffer, column - minimum(column, amount), row);
    case 'E': return terminal_vertical(buffer, 0, amount, true);
    case 'F': return terminal_vertical(buffer, 0, amount, false);
    case 'G': case '`': return terminal_move(buffer, offset, row);
    case 'd': return terminal_position(buffer, column, offset);
    case 'I': return terminal_tab(terminal, buffer, amount, true);
    case 'Z': return terminal_tab(terminal, buffer, amount, false);
    case 'J': return terminal_erase(terminal, buffer, true, first);
    case 'K': return terminal_erase(terminal, buffer, false, first);
    case '@': case 'P': case 'X':
        terminal_chars(terminal, buffer, amount, final);
        return true;
    case 'L': case 'M':
        if (row < buffer->scroll_top || row > buffer->scroll_bottom) return false;
        terminal_scroll(terminal, buffer, row, buffer->scroll_bottom, amount, final == 'L', false);
        terminal_move(buffer, 0, row);
        return true;
    case 'S': case 'T':
        terminal_scroll(terminal, buffer, buffer->scroll_top, buffer->scroll_bottom, amount, final == 'T', false);
        return true;
    case 'b':
        if (terminal->last_character == 0) return false;
        amount = minimum(amount, buffer->screen.columns * buffer->screen.rows);
        while (amount-- != 0) terminal_print(terminal, terminal->last_character);
        return true;
    case 'g':
        if (first == 0) terminal->tab_stops[column / 8] &= (unsigned char)~(1u << (column % 8));
        else if (first == 3) memset(terminal->tab_stops, 0, sizeof terminal->tab_stops);
        break;
    case 'r': {
        size_t bottom = second > 0 ? minimum(second - 1, buffer->screen.rows - 1) : buffer->screen.rows - 1;
        if (offset < bottom || (offset == 0 && buffer->screen.rows == 1)) {
            buffer->scroll_top = offset;
            buffer->scroll_bottom = bottom;
            return terminal_position(buffer, 0, 0);
        }
        break;
    }
    case 's': buffer->saved = buffer->cursor; break;
    case 'u': return terminal_restore(buffer, buffer->saved);
    case 'm': terminal_sgr(&buffer->cursor.rendition, parser); break;
    }
    return false;
}

static bool parser_csi_byte(Terminal *terminal, unsigned char byte)
{
    Parser *parser = &terminal->parser;
    if (byte >= 0x40 && byte <= 0x7e) {
        parser->state = PARSE_TEXT;
        return terminal_csi(terminal, byte);
    }
    if (byte >= 0x20 && byte <= 0x2f && parser->intermediate == 0) {
        parser->intermediate = byte;
    } else if (parser->intermediate != 0) {
        parser->unsupported = true;
    } else if (byte >= '0' && byte <= '9') {
        unsigned int *value = &parser->parameters[parser->count - 1];
        *value = (unsigned int)minimum(*value * 10 + (byte - '0'), USHRT_MAX);
    } else if ((byte == ';' || byte == ':') && parser->count < sizeof parser->parameters / sizeof parser->parameters[0]) {
        parser->parameters[parser->count] = 0;
        parser->subparameter[parser->count++] = byte == ':';
        parser->has_subparameters |= byte == ':';
    } else if (byte >= 0x3c && byte <= 0x3f && !parser->started) {
        parser->prefix = byte;
    } else {
        parser->unsupported = true;
    }
    parser->started = true;
    return false;
}

static bool terminal_control(Terminal *terminal, unsigned char byte)
{
    TerminalBuffer *buffer = terminal_buffer(terminal);
    size_t column = buffer->cursor.column, row = buffer->cursor.row;
    switch (byte) {
    case '\a': terminal->bell_pending = true; break;
    case '\r': return terminal_move(buffer, 0, row);
    case '\n': case '\v': case '\f': return terminal_index(terminal, buffer, true, false);
    case '\b': return terminal_move(buffer, column - (column > 0), row);
    case '\t': return terminal_tab(terminal, buffer, 1, true);
    case 0x0e: buffer->cursor.charset = 1; break;
    case 0x0f: buffer->cursor.charset = 0; break;
    }
    return false;
}

static bool parser_escape_byte(Terminal *terminal, unsigned char byte)
{
    Parser *parser = &terminal->parser;
    TerminalBuffer *buffer = terminal_buffer(terminal);
    parser->state = PARSE_TEXT;
    switch (byte) {
    case '[':
        parser->state = PARSE_CSI;
        parser->count = 1;
        parser->parameters[0] = 0;
        parser->subparameter[0] = false;
        parser->prefix = parser->intermediate = 0;
        parser->started = parser->has_subparameters = parser->unsupported = false;
        break;
    case ']': case 'P': case 'X': case '^': case '_':
        parser->state = PARSE_STRING;
        parser->osc = byte == ']';
        parser->osc52 = false;
        parser->unsupported = false;
        parser->string_length = 0;
        terminal->osc52_length = 0;
        break;
    case '7': buffer->saved = buffer->cursor; break;
    case '8': return terminal_restore(buffer, buffer->saved);
    case 'E':
        buffer->cursor.column = 0;
        terminal_index(terminal, buffer, true, false);
        return true;
    case 'D': return terminal_index(terminal, buffer, true, false);
    case 'M': return terminal_index(terminal, buffer, false, false);
    case 'H': {
        size_t column = buffer->cursor.column;
        terminal->tab_stops[column / 8] |= (unsigned char)(1u << (column % 8));
        break;
    }
    case '=': terminal->application_keypad = true; break;
    case '>': terminal->application_keypad = false; break;
    case 'Z': terminal_reply(terminal, "\033[?1;2c"); break;
    case 'c': return terminal_reset(terminal);
    default:
        if (byte >= 0x20 && byte <= 0x2f) {
            parser->intermediate = byte;
            parser->state = PARSE_ESCAPE_INTERMEDIATE;
        }
        break;
    }
    return false;
}

static void parser_escape_intermediate(Terminal *terminal, unsigned char byte)
{
    Parser *parser = &terminal->parser;
    if (byte < 0x30) {
        parser->intermediate = 0;
        return;
    }
    if ((parser->intermediate == '(' || parser->intermediate == ')') && (byte == '0' || byte == 'B')) {
        TerminalBuffer *buffer = terminal_buffer(terminal);
        buffer->cursor.line_drawing[parser->intermediate == ')'] = byte == '0';
    }
    parser->state = PARSE_TEXT;
}

/* Reject invalid UTF-8 and controls before using text in a window title or hyperlink. */
static bool osc_text_valid(const char *text, size_t length, bool url)
{
    for (size_t i = 0; i < length;) {
        unsigned char lead = (unsigned char)text[i++];
        uint32_t cp = lead, minimum = 0;
        unsigned int rest = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { cp &= 0x1f; rest = 1; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { cp &= 0x0f; rest = 2; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { cp &= 0x07; rest = 3; minimum = 0x10000; }
        else if (lead >= 0x80) return false;
        if (rest > length - i) return false;
        while (rest-- > 0) {
            unsigned char next = (unsigned char)text[i++];
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) ||
            (cp >= 0x2028 && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069)) return false;
        if (url && (cp == ' ' || cp == '\\' || cp == 0xa0 || cp == 0x1680 ||
                    (cp >= 0x2000 && cp <= 0x200f) || (cp >= 0x2028 && cp <= 0x202f) ||
                    cp == 0x205f || (cp >= 0x2060 && cp <= 0x206f) || cp == 0x3000)) return false;
    }
    return true;
}

static bool link_valid(const char *url, size_t length)
{
    if (length > LINK_LIMIT || !osc_text_valid(url, length, true)) return false;
    size_t scheme = strncasecmp(url, "https://", 8) == 0 ? 8 :
                    strncasecmp(url, "http://", 7) == 0 ? 7 : 0;
    return scheme && scheme < length && strcspn(url + scheme, "/?#") != 0;
}

static bool osc52_append(Terminal *terminal, unsigned char byte)
{
    if (terminal->osc52_length >= OSC52_LIMIT) return false;
    if (terminal->osc52_length == terminal->osc52_capacity) {
        size_t capacity = terminal->osc52_capacity ? terminal->osc52_capacity * 2 : 4096;
        char *grown = realloc(terminal->osc52, capacity);
        if (grown == NULL) return false;
        terminal->osc52 = grown;
        terminal->osc52_capacity = capacity;
    }
    terminal->osc52[terminal->osc52_length++] = (char)byte;
    return true;
}

static int base64_value(unsigned char byte)
{
    if (byte >= 'A' && byte <= 'Z') return byte - 'A';
    if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
    if (byte >= '0' && byte <= '9') return byte - '0' + 52;
    if (byte == '+') return 62;
    if (byte == '/') return 63;
    return -1;
}

/* OSC 52 ; targets ; base64. Only writes are supported. Reading the
   clipboard would let any program, including a remote one, take its contents. */
static void osc52_finish(Terminal *terminal)
{
    const char *data = terminal->osc52;
    size_t length = terminal->osc52_length;
    const char *separator = length != 0 ? memchr(data, ';', length) : NULL;
    if (!terminal->allow_osc52 || separator == NULL) return;
    unsigned int targets = 0;
    for (const char *target = data; target < separator; ++target) {
        if (*target == 'c') targets |= OSC52_CLIPBOARD;
        else if (*target == 'p' || *target == 's') targets |= OSC52_PRIMARY;
    }
    if (separator == data) targets = OSC52_CLIPBOARD;
    const unsigned char *encoded = (const unsigned char *)separator + 1;
    size_t count = length - (size_t)(separator + 1 - data);
    if (targets == 0 || count == 0 || (count == 1 && encoded[0] == '?')) return;
    unsigned char *decoded = malloc(count / 4 * 3 + 3);
    if (decoded == NULL) return;
    size_t written = 0;
    uint32_t bits = 0;
    int pending = 0;
    bool padding = false;
    for (size_t i = 0; i < count; ++i) {
        if (encoded[i] == '=') {
            padding = true;
            continue;
        }
        int value = base64_value(encoded[i]);
        if (value < 0 || padding) {
            free(decoded);
            return;
        }
        bits = (bits << 6) | (uint32_t)value;
        pending += 6;
        if (pending >= 8) {
            pending -= 8;
            decoded[written++] = (unsigned char)(bits >> pending);
        }
    }
    if (written == 0 || memchr(decoded, 0, written) != NULL) {
        free(decoded);
        return;
    }
    free(terminal->clipboard_write);
    terminal->clipboard_write = decoded;
    terminal->clipboard_write_length = written;
    terminal->clipboard_targets = targets;
}

static int hex_value(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

/* OSC 7 ; file://host/path. A remote host, such as a shell over ssh,
   names a directory that doesn't exist here, so it clears the value. */
static void osc7_directory(Terminal *terminal, const char *url)
{
    terminal->directory[0] = 0;
    if (strncmp(url, "file://", 7) != 0) return;
    const char *host = url + 7, *path = strchr(host, '/');
    if (path == NULL) return;
    size_t host_length = (size_t)(path - host);
    char name[256];
    bool local = host_length == 0 || (host_length == 9 && strncasecmp(host, "localhost", 9) == 0);
    if (!local && gethostname(name, sizeof name) == 0) {
        name[sizeof name - 1] = 0;
        local = strlen(name) == host_length && strncasecmp(host, name, host_length) == 0;
    }
    if (!local) return;
    size_t written = 0;
    for (const char *p = path; *p != 0; ++p) {
        unsigned char byte = (unsigned char)*p;
        if (byte == '%') {
            int high = hex_value((unsigned char)p[1]), low = high >= 0 ? hex_value((unsigned char)p[2]) : -1;
            if (low < 0) {
                terminal->directory[0] = 0;
                return;
            }
            byte = (unsigned char)(high * 16 + low);
            p += 2;
        }
        if (byte == 0 || written + 1 >= sizeof terminal->directory) {
            terminal->directory[0] = 0;
            return;
        }
        terminal->directory[written++] = (char)byte;
    }
    terminal->directory[written] = 0;
}

static void parser_finish_string(Terminal *terminal)
{
    Parser *parser = &terminal->parser;
    parser->state = PARSE_TEXT;
    if (!parser->osc) return;
    char *text = parser->string;
    size_t length = parser->string_length;
    text[length] = 0;
    if (parser->osc52) {
        if (!parser->unsupported) osc52_finish(terminal);
        terminal->osc52_length = 0;
        return;
    }
    if (length >= 2 && text[0] == '7' && text[1] == ';') {
        if (!parser->unsupported) osc7_directory(terminal, text + 2);
        return;
    }
    if (length >= 2 && text[0] == '8' && text[1] == ';') {
        TerminalBuffer *buffer = terminal_buffer(terminal);
        buffer->active_link = 0;
        terminal->links_dirty |= terminal->links != NULL;
        if (!terminal->allow_links || parser->unsupported) return;
        const char *url = memchr(text + 2, ';', length - 2);
        if (url == NULL) return;
        ++url;
        if (link_valid(url, length - (size_t)(url - text))) buffer->active_link = link_add(terminal, url);
        return;
    }
    if (parser->unsupported) return;
    if (length >= 2 && (text[0] == '0' || text[0] == '2') && text[1] == ';') {
        if (terminal->allow_title && length - 2 <= TITLE_LIMIT &&
            osc_text_valid(text + 2, length - 2, false) &&
            strcmp(terminal->dynamic_title, text + 2) != 0) {
            memcpy(terminal->dynamic_title, text + 2, length - 1);
            terminal->title_dirty = true;
        }
        return;
    }
    if (length >= 2 && text[0] == '4' && text[1] == ';') {
        /* OSC 4 ; index ; ? [; index ; ? ...] reports palette entries. Sets are ignored. */
        char *cursor = text + 2;
        while (*cursor != 0) {
            char *end;
            unsigned long index = strtoul(cursor, &end, 10);
            if (end == cursor || *end != ';' || index > 255) return;
            cursor = end + 1;
            char *next = strchr(cursor, ';');
            size_t spec = next != NULL ? (size_t)(next - cursor) : strlen(cursor);
            if (spec == 1 && cursor[0] == '?') {
                TerminalColor color = index < 16 ? terminal->palette[index] : palette_color((unsigned int)index);
                char response[64];
                snprintf(response, sizeof response, "\033]4;%lu;rgb:%04x/%04x/%04x\033\\", index,
                         257u * color.red, 257u * color.green, 257u * color.blue);
                terminal_reply(terminal, response);
            }
            if (next == NULL) break;
            cursor = next + 1;
        }
        return;
    }
    if (length != 4 || text[0] != '1' || text[2] != ';' || text[3] != '?') return;
    TerminalColor color;
    switch (parser->string[1]) {
    case '0': color = terminal->defaults.foreground; break;
    case '1': color = terminal->defaults.background; break;
    case '2': color = terminal->cursor_color; break;
    default: return;
    }
    char response[48];
    snprintf(response, sizeof response, "\033]1%c;rgb:%04x/%04x/%04x\033\\", parser->string[1],
             257u * color.red, 257u * color.green, 257u * color.blue);
    terminal_reply(terminal, response);
}

static bool parser_string_byte(Terminal *terminal, unsigned char byte)
{
    Parser *parser = &terminal->parser;
    if ((parser->osc && byte == '\a') || (parser->state == PARSE_STRING_ESCAPE && byte == '\\')) {
        parser_finish_string(terminal);
        return false;
    }
    if (parser->state == PARSE_STRING_ESCAPE) {
        /* An escape other than ST starts a new sequence after a broken string. */
        parser->state = byte == 0x1b ? PARSE_ESCAPE : PARSE_TEXT;
        if (byte < 0x20) return terminal_control(terminal, byte);
        if (byte <= 0x7e) return parser_escape_byte(terminal, byte);
        return false;
    }
    if (byte == 0x1b) {
        parser->state = PARSE_STRING_ESCAPE;
    } else {
        parser->state = PARSE_STRING;
        if (parser->osc52) {
            if (!parser->unsupported && !osc52_append(terminal, byte)) parser->unsupported = true;
        } else if (parser->osc && parser->string_length < OSC_LIMIT) {
            parser->string[parser->string_length++] = (char)byte;
            parser->osc52 = parser->string_length == 3 && memcmp(parser->string, "52;", 3) == 0;
        } else parser->unsupported = true;
    }
    return false;
}

static uint32_t line_drawing(unsigned char byte)
{
    static const uint32_t special_graphics[] = {
        ' ',    0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
        0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
        0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534,
        0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7
    };
    return byte >= '_' && byte <= '~' ? special_graphics[byte - '_'] : byte;
}

static bool terminal_text_byte(Terminal *terminal, unsigned char byte)
{
    if (byte < 0x80) {
        const TerminalCursor *cursor = &terminal_buffer(terminal)->cursor;
        return terminal_print(terminal, cursor->line_drawing[cursor->charset] ? line_drawing(byte) : byte);
    }
    Utf8Decoder *utf8 = &terminal->utf8;
    if (byte >= 0xc2 && byte <= 0xdf) *utf8 = (Utf8Decoder){byte & 0x1f, 0x80, 1};
    else if (byte >= 0xe0 && byte <= 0xef) *utf8 = (Utf8Decoder){byte & 0x0f, 0x800, 2};
    else if (byte >= 0xf0 && byte <= 0xf4) *utf8 = (Utf8Decoder){byte & 0x07, 0x10000, 3};
    else return terminal_print(terminal, 0xfffd);
    return false;
}

static bool terminal_parse_byte(Terminal *terminal, unsigned char byte)
{
    Parser *parser = &terminal->parser;
    if (byte == 0x18 || byte == 0x1a) { /* CAN or SUB */
        parser->state = PARSE_TEXT;
        return false;
    }
    if (parser->state == PARSE_STRING || parser->state == PARSE_STRING_ESCAPE) {
        return parser_string_byte(terminal, byte);
    }
    if (byte == 0x1b) {
        parser->state = PARSE_ESCAPE;
        return false;
    }
    if (byte < FIRST_GLYPH) return terminal_control(terminal, byte);
    if (byte == 0x7f) return false;
    if (byte > 0x7f && parser->state != PARSE_TEXT) {
        if (parser->state == PARSE_CSI) parser->unsupported = true;
        return false;
    }
    switch (parser->state) {
    case PARSE_CSI: return parser_csi_byte(terminal, byte);
    case PARSE_ESCAPE: return parser_escape_byte(terminal, byte);
    case PARSE_ESCAPE_INTERMEDIATE: parser_escape_intermediate(terminal, byte); return false;
    default: return terminal_text_byte(terminal, byte);
    }
}

static bool terminal_byte(Terminal *terminal, unsigned char byte)
{
    Utf8Decoder *utf8 = &terminal->utf8;
    if (utf8->remaining == 0) return terminal_parse_byte(terminal, byte);
    if ((byte & 0xc0) != 0x80) {
        utf8->remaining = 0;
        bool changed = terminal_print(terminal, 0xfffd);
        return terminal_parse_byte(terminal, byte) | changed;
    }
    utf8->codepoint = (utf8->codepoint << 6) | (byte & 0x3f);
    if (--utf8->remaining != 0) return false;
    uint32_t codepoint = utf8->codepoint;
    if (codepoint < utf8->minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        codepoint = 0xfffd;
    }
    return terminal_print(terminal, codepoint);
}

static bool terminal_feed(Terminal *terminal, const unsigned char *bytes, size_t length)
{
    bool changed = false;
    for (size_t i = 0; i < length; ++i) changed |= terminal_byte(terminal, bytes[i]);
    terminal->links_dirty |= changed && terminal->links != NULL;
    return changed;
}

/* Text selection and serialization */

enum { TEXT_LIMIT = 32 * 1024 * 1024 };

typedef struct {
    unsigned char *bytes;
    size_t length, capacity;
} TextBuffer;

static void text_destroy(TextBuffer *text)
{
    free(text->bytes);
    *text = (TextBuffer){0};
}

static bool text_append(TextBuffer *text, const void *data, size_t count)
{
    if (count == 0) return true;
    if (count > TEXT_LIMIT - text->length) return false;
    size_t needed = text->length + count;
    if (needed > text->capacity) {
        size_t capacity = text->capacity != 0 ? text->capacity : 256;
        while (capacity < needed) capacity *= 2;
        unsigned char *bytes = realloc(text->bytes, capacity);
        if (bytes == NULL) return false;
        text->bytes = bytes;
        text->capacity = capacity;
    }
    memcpy(text->bytes + text->length, data, count);
    text->length = needed;
    return true;
}

static bool text_codepoint(TextBuffer *text, uint32_t cp)
{
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    unsigned char bytes[4];
    if (cp < 0x80) {
        bytes[0] = (unsigned char)cp;
        return text_append(text, bytes, 1);
    }
    size_t count = cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    for (size_t i = count - 1; i > 0; --i) {
        bytes[i] = (unsigned char)(0x80 | (cp & 0x3f));
        cp >>= 6;
    }
    static const unsigned char prefix[] = {0xc0, 0xe0, 0xf0};
    bytes[0] = (unsigned char)(prefix[count - 2] | cp);
    return text_append(text, bytes, count);
}

static uint64_t view_oldest(const TerminalView *view)
{
    return view->first_live - (view->history != NULL ? view->history->count : 0);
}

static uint64_t view_last(const TerminalView *view)
{
    return view->first_live + view->screen->rows - 1;
}

static bool history_scroll(Terminal *terminal, bool up, size_t count)
{
    if (terminal->using_alternate) return false;
    History *history = &terminal->history;
    size_t previous = history->offset;
    if (up) history->offset += minimum(count, history->count - history->offset);
    else history->offset -= minimum(count, history->offset);
    return history->offset != previous;
}

static SelectionPoint selection_point(const TerminalView *view, const FontMetrics *metrics,
                                      Padding padding, int x, int y)
{
    size_t row = y <= padding.vertical ? 0 : (size_t)((y - padding.vertical) / metrics->height);
    size_t column = x <= padding.horizontal ? 0 : (size_t)((x - padding.horizontal) / metrics->width);
    uint64_t line = view->first_visible + minimum(row, view->screen->rows - 1);
    LineView source = view_line(view, line);
    size_t columns = minimum(source.columns, view->screen->columns);
    column = columns == 0 ? 0 : minimum(column, columns - 1);
    if (column > 0 && view_cell(view, &source, column)->width == 0) --column;
    return (SelectionPoint){line, column};
}

static int selection_class(uint32_t character)
{
    if (character == ' ' || iswspace((wint_t)character)) return 0;
    if (character == '_' || iswalnum((wint_t)character)) return 1;
    return 2;
}

static TextRange selection_extent(const TerminalView *view, SelectionPoint point, SelectionMode mode)
{
    LineView source = view_line(view, point.line);
    if (mode == SELECT_LINE) {
        uint64_t first = point.line, end = point.line;
        uint64_t oldest = view_oldest(view), last = view_last(view);
        while (first > oldest && view_line(view, first - 1).wrapped) --first;
        while (end < last && view_line(view, end).wrapped) ++end;
        return (TextRange){{first, 0}, {end, view_line(view, end).columns}};
    }
    TextRange range = {point, point};
    if (point.column >= source.columns) return range;
    const TerminalCell *cell = view_cell(view, &source, point.column);
    range.end.column = minimum(point.column + cell->width, source.columns);
    if (mode != SELECT_WORD) return range;

    int type = selection_class(cell->character);
    uint64_t oldest = view_oldest(view), last = view_last(view);
    for (;;) {
        SelectionPoint previous = range.first;
        LineView line = view_line(view, previous.line);
        if (previous.column == 0) {
            if (previous.line == oldest) break;
            line = view_line(view, --previous.line);
            if (!line.wrapped || line.columns == 0) break;
            previous.column = line.columns;
        }
        --previous.column;
        if (previous.column > 0 && view_cell(view, &line, previous.column)->width == 0)
            --previous.column;
        if (selection_class(view_cell(view, &line, previous.column)->character) != type) break;
        range.first = previous;
    }
    for (;;) {
        SelectionPoint next = range.end;
        LineView line = view_line(view, next.line);
        if (next.column == line.columns) {
            if (!line.wrapped || next.line == last) break;
            line = view_line(view, ++next.line);
            next.column = 0;
        }
        if (next.column >= line.columns) break;
        cell = view_cell(view, &line, next.column);
        if (cell->width == 0 || selection_class(cell->character) != type) break;
        next.column = minimum(next.column + cell->width, line.columns);
        range.end = next;
    }
    return range;
}

static void selection_start(Terminal *terminal, SelectionPoint point, SelectionMode mode)
{
    TerminalView view = terminal_view(terminal);
    TextRange range = selection_extent(&view, point, mode);
    terminal->selection = (Selection){.anchor = range, .range = range, .origin = point,
        .mode = mode, .dragging = true, .active = mode != SELECT_CHARACTER,
        .alternate = terminal->using_alternate};
}

static bool selection_extend(Terminal *terminal, SelectionPoint point)
{
    Selection *selection = &terminal->selection;
    if (!selection->dragging || selection->alternate != terminal->using_alternate) return false;
    if (!selection->active && point.line == selection->origin.line &&
        point.column == selection->origin.column) return false;
    TerminalView view = terminal_view(terminal);
    TextRange next = selection_extent(&view, point, selection->mode);
    TextRange range = {
        point_before(next.first, selection->anchor.first) ? next.first : selection->anchor.first,
        point_before(next.end, selection->anchor.end) ? selection->anchor.end : next.end
    };
    bool changed = !selection->active || point_before(range.first, selection->range.first) ||
                   point_before(selection->range.first, range.first) ||
                   point_before(range.end, selection->range.end) ||
                   point_before(selection->range.end, range.end);
    selection->range = range;
    selection->active = true;
    return changed;
}

static bool selection_text(Terminal *terminal, TextBuffer *text)
{
    TerminalView view = terminal_view(terminal);
    if (view.selection == NULL || !view.selection->active) return false;
    TextRange range = view.selection->range;
    uint64_t oldest = view_oldest(&view), last = view_last(&view);
    if (range.end.line < oldest || range.first.line > last) return false;
    if (range.first.line < oldest) range.first = (SelectionPoint){oldest, 0};
    if (range.end.line > last) range.end = (SelectionPoint){last, view_line(&view, last).columns};
    for (uint64_t row = range.first.line; row <= range.end.line; ++row) {
        LineView line = view_line(&view, row);
        size_t first = row == range.first.line ? minimum(range.first.column, line.columns) : 0;
        size_t end = row == range.end.line ? minimum(range.end.column, line.columns) : line.columns;
        if (end == line.columns) {
            end = minimum(end, line.stored);
            while (end > first) {
                const TerminalCell *cell = &line.cells[end - 1];
                if (cell->written || cell->character != ' ' || cell->width == 0) break;
                --end;
            }
        }
        for (size_t i = first; i < end; ++i) {
            const TerminalCell *cell = view_cell(&view, &line, i);
            if (cell->width == 0) continue;
            if (!text_codepoint(text, cell->character)) return false;
            for (size_t j = 0; j < sizeof cell->combining / sizeof cell->combining[0] && cell->combining[j]; ++j) {
                if (!text_codepoint(text, cell->combining[j])) return false;
            }
        }
        if (!line.wrapped && (row < range.end.line || view.selection->mode == SELECT_LINE) &&
            !text_append(text, "\n", 1)) return false;
    }
    return text->length != 0;
}

/* PTY and shell session */

#define INPUT_CAPACITY 8192
enum { PTY_READ_CAPACITY = 8192, PTY_PARSE_SLICE = 128, PTY_PARSE_BUDGET = 5 };

typedef struct {
    int master;
    int signal_fd;
    int startup, launch_gate;
    pid_t child;
    int child_status;
    bool child_status_valid;
    unsigned char input[INPUT_CAPACITY];
    unsigned char *large_input;
    size_t large_capacity;
    size_t input_start;
    size_t input_length;
    unsigned char output[PTY_READ_CAPACITY];
    size_t output_start, output_end;
    sigset_t original_signal_mask;
    struct sigaction original_sighup;
} Session;

typedef enum { PTY_OPEN, PTY_CLOSED, PTY_FAILED } PtyResult;

static struct winsize session_size(const TerminalScreen *screen, int width, int height, Padding padding)
{
    return (struct winsize){
        .ws_col = (unsigned short)screen->columns,
        .ws_row = (unsigned short)screen->rows,
        .ws_xpixel = (unsigned short)minimum(grid_extent(width, padding.horizontal), USHRT_MAX),
        .ws_ypixel = (unsigned short)minimum(grid_extent(height, padding.vertical), USHRT_MAX)
    };
}

static void exec_shell(const char *path)
{
    if (path == NULL || *path == '\0') return;
    const char *basename = strrchr(path, '/');
    execl(path, basename != NULL ? basename + 1 : path, "-i", (char *)NULL);
}

static bool terminfo_at(const char *directory, size_t length)
{
    if (length == 0 || length > PATH_MAX - 12) return false;
    char path[PATH_MAX];
    const char *const suffixes[] = {"/d/deal", "/64/deal"};
    for (size_t i = 0; i < 2; ++i) {
        snprintf(path, sizeof path, "%.*s%s", (int)length, directory, suffixes[i]);
        struct stat info;
        if (stat(path, &info) == 0 && S_ISREG(info.st_mode) && access(path, R_OK) == 0) return true;
    }
    return false;
}

static bool packaged_terminfo(char path[PATH_MAX])
{
    ssize_t length = readlink("/proc/self/exe", path, PATH_MAX - 1);
    if (length < 0 || length >= PATH_MAX - 1) return false;
    path[length] = 0;
    char *program = strrchr(path, '/');
    if (program == NULL || program - path < 4 || strncmp(program - 4, "/bin", 4) != 0) return false;
    size_t prefix = (size_t)(program - 4 - path);
    static const char suffix[] = "/share/terminfo";
    if (prefix + sizeof suffix >= PATH_MAX) return false;
    memcpy(path + prefix, suffix, sizeof suffix);
    return terminfo_at(path, prefix + sizeof suffix - 1);
}

static const char *config_term(const Config *config, char packaged[PATH_MAX])
{
    packaged[0] = 0;
    if (!config->custom_term) return compatibility_term;
    const char *single = getenv("TERMINFO");
    if (single != NULL && terminfo_at(single, strlen(single))) return "deal";
    const char *home = getenv("HOME");
    if (home != NULL) {
        char path[PATH_MAX];
        int length = snprintf(path, sizeof path, "%s/.terminfo", home);
        if (length > 0 && (size_t)length < sizeof path && terminfo_at(path, (size_t)length)) return "deal";
    }
    const char *list = getenv("TERMINFO_DIRS");
    while (list != NULL && *list != 0) {
        const char *end = strchr(list, ':');
        if (terminfo_at(list, end != NULL ? (size_t)(end - list) : strlen(list))) return "deal";
        list = end != NULL ? end + 1 : NULL;
    }
    const char *const dirs[] = {"/usr/share/terminfo", "/usr/lib/terminfo", "/lib/terminfo",
                               "/etc/terminfo", "/usr/local/share/terminfo"};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i)
        if (terminfo_at(dirs[i], strlen(dirs[i]))) return "deal";
    if (packaged_terminfo(packaged)) return "deal";
    packaged[0] = 0;
    return compatibility_term;
}

static _Noreturn void launch_child(const sigset_t *original_mask, int failure_fd,
                                    const Config *config, const char *shell)
{
    const Options *options = config->options;
    unsigned char failure = 's';
    char packaged[PATH_MAX];
    const char *term = config_term(config, packaged);
    if (sigprocmask(SIG_SETMASK, original_mask, NULL) == 0 &&
        setenv("TERM", term, 1) == 0 && (!*packaged || setenv("TERMINFO", packaged, 1) == 0)) {
        if (options != NULL && options->directory != NULL && chdir(options->directory) < 0) {
            failure = 'd';
        } else if (options != NULL && options->command != NULL) {
            /* Search PATH without execvp's shell fallback for files with no interpreter. */
            char **args = options->command;
            if (strchr(args[0], '/') != NULL) execv(args[0], args);
            else {
                const char *search = getenv("PATH");
                if (search == NULL) search = "/bin:/usr/bin";
                while (search != NULL) {
                    const char *end = strchr(search, ':');
                    size_t length = end != NULL ? (size_t)(end - search) : strlen(search);
                    char program[PATH_MAX];
                    int n = length < sizeof program
                        ? snprintf(program, sizeof program, "%.*s%s%s", (int)length, search,
                                   length ? "/" : "", args[0]) : -1;
                    if (n > 0 && (size_t)n < sizeof program) execv(program, args);
                    if (n > 0 && (size_t)n < sizeof program &&
                        errno != ENOENT && errno != ENOTDIR && errno != EACCES) break;
                    search = end != NULL ? end + 1 : NULL;
                }
            }
            failure = 'e';
        } else {
            exec_shell(config->shell);
            exec_shell(shell);
            exec_shell("/bin/sh");
        }
    }

    ssize_t written;
    do {
        written = write(failure_fd, &failure, 1);
    } while (written < 0 && errno == EINTR);
    _exit(127);
}

static const char *session_prepare_signals(Session *session)
{
    /* Block the signals before any graphics driver can create worker threads. */
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGCHLD);
    sigaddset(&signals, SIGHUP);
    /* A clipboard reader can close its pipe while an outgoing write is in flight. */
    sigaddset(&signals, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &signals, &session->original_signal_mask) < 0)
        return "could not block shell exit and reload signals";
    sigdelset(&signals, SIGPIPE);
    struct sigaction action = {.sa_handler = SIG_DFL, .sa_flags = SA_NOCLDSTOP};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, NULL) < 0)
        return "could not configure shell exit notifications";
    action.sa_flags = 0;
    if (sigaction(SIGHUP, &action, &session->original_sighup) < 0)
        return "could not configure reload notifications";
    session->signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    return session->signal_fd < 0 ? "could not create the signal notification descriptor" : NULL;
}

static const char *session_open(Session *session, struct winsize size, int display_fd,
                               const Config *config)
{
    const char *shell = getenv("SHELL");
    sigset_t shell_mask = session->original_signal_mask;
    sigdelset(&shell_mask, SIGCHLD);
    int startup[2], gate[2];
    if (pipe2(startup, O_CLOEXEC | O_NONBLOCK) < 0) {
        return "could not create a shell startup pipe";
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, gate) < 0) {
        close(startup[0]);
        close(startup[1]);
        return "could not create the shell launch gate";
    }
    session->startup = startup[0];
    int master = -1;
    pid_t child = forkpty(&master, NULL, NULL, &size);
    if (child == 0) {
        close(startup[0]);
        close(gate[1]);
        close(session->signal_fd);
        if (display_fd > STDERR_FILENO) close(display_fd);
        unsigned char go;
        ssize_t count;
        do {
            count = read(gate[0], &go, 1);
        } while (count < 0 && errno == EINTR);
        close(gate[0]);
        if (count != 1) _exit(127);
        sigaction(SIGHUP, &session->original_sighup, NULL);
        launch_child(&shell_mask, startup[1], config, shell);
    }
    close(startup[1]);
    close(gate[0]);
    session->launch_gate = gate[1];
    if (child < 0) {
        return "could not create a pseudoterminal and fork the shell";
    }
    session->master = master;
    session->child = child;
    int flags = fcntl(master, F_GETFL);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(master, F_SETFD, FD_CLOEXEC) < 0) {
        return "could not configure the pseudoterminal descriptor";
    }
    return NULL;
}

static const char *session_launch(Session *session)
{
    const unsigned char go = 1;
    ssize_t count;
    do {
        count = send(session->launch_gate, &go, 1, MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    close(session->launch_gate);
    session->launch_gate = -1;
    return count == 1 ? NULL : "could not release the shell for launch";
}

static const char *session_check_startup(Session *session)
{
    if (session->startup < 0) {
        return NULL;
    }
    unsigned char failure;
    ssize_t count;
    do {
        count = read(session->startup, &failure, 1);
    } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return NULL;
    }
    close(session->startup);
    session->startup = -1;
    if (count < 0) {
        return "could not read the shell startup result";
    }
    if (count == 0) return NULL;
    if (failure == 'd') return "could not enter the specified working directory";
    if (failure == 'e') return "could not execute the specified command";
    return "could not start an interactive shell";
}

static void session_reap(Session *session, int options)
{
    if (session->child > 0) {
        pid_t result;
        int status;
        do {
            result = waitpid(session->child, &status, options);
        } while (result < 0 && errno == EINTR);
        if (result > 0) {
            session->child_status_valid = true;
            session->child_status = status;
        }
        if (result > 0 || (result < 0 && errno == ECHILD)) {
            session->child = 0;
        }
    }
}

/* End the shell and release its descriptors, keeping the signal descriptor
   that every tab shares. */
static void session_end(Session *session)
{
    if (session->launch_gate >= 0) {
        close(session->launch_gate);
        session->launch_gate = -1;
    }
    free(session->large_input);
    session->large_input = NULL;
    if (session->master >= 0) {
        close(session->master);
        session->master = -1;
    }
    if (session->startup >= 0) {
        close(session->startup);
        session->startup = -1;
    }
    session_reap(session, WNOHANG);
    if (session->child > 0) {
        /* Give the PTY hangup and SIGHUP time to end the shell before forcing exit. */
        kill(session->child, SIGHUP);
        struct pollfd signal_event = {session->signal_fd, POLLIN, 0};
        while (poll(&signal_event, 1, 250) < 0 && errno == EINTR) {
        }
        session_reap(session, WNOHANG);
        if (session->child > 0) {
            kill(session->child, SIGKILL);
            session_reap(session, 0);
        }
    }
}

static void session_close(Session *session)
{
    session_end(session);
    if (session->signal_fd >= 0) {
        close(session->signal_fd);
        session->signal_fd = -1;
    }

}

static PtyResult session_resize(const Session *session, struct winsize size)
{
    int status;
    do {
        status = ioctl(session->master, TIOCSWINSZ, &size);
    } while (status < 0 && errno == EINTR);
    if (status == 0) {
        return PTY_OPEN;
    }
    return errno == EIO ? PTY_CLOSED : PTY_FAILED;
}

static bool session_append(Session *session, const unsigned char *bytes, size_t count, bool paste)
{
    if (count == 0) return true;
    bool inline_input = session->large_input == NULL;
    size_t capacity = inline_input ? sizeof session->input : session->large_capacity;
    unsigned char *input = inline_input ? session->input : session->large_input;
    if (count > capacity - session->input_length) {
        const size_t limit = TEXT_LIMIT + INPUT_CAPACITY;
        if (!paste || count > limit - session->input_length) return false;
        while (capacity < session->input_length + count) capacity = minimum(2 * capacity, limit);
        unsigned char *grown = realloc(session->large_input, capacity);
        if (grown == NULL) return false;
        if (inline_input) memcpy(grown, input, session->input_start + session->input_length);
        input = session->large_input = grown;
        session->large_capacity = capacity;
    }
    if (session->input_start + session->input_length + count > capacity) {
        memmove(input, input + session->input_start, session->input_length);
        session->input_start = 0;
    }
    memcpy(input + session->input_start + session->input_length, bytes, count);
    session->input_length += count;
    return true;
}

static bool session_queue(Session *session, const unsigned char *bytes, size_t count)
{
    return session_append(session, bytes, count, false);
}

enum { PASTE_MARKER_BYTES = 6 };

static bool paste_chunk(TextBuffer *text, unsigned char *bytes, size_t count, bool *after_cr)
{
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        unsigned char byte = bytes[i];
        if (byte == '\n') {
            if (!*after_cr) bytes[used++] = '\r';
        } else if (byte == '\r' || byte == '\t' || (byte >= 0x20 && byte != 0x7f)) {
            bytes[used++] = byte;
        }
        *after_cr = byte == '\r';
    }
    return text_append(text, bytes, used);
}

/* Receipt reserves space for the opening marker. Choose the mode at enqueue time. */
static bool session_paste(Session *session, TextBuffer *text, bool bracketed)
{
    if (text->length == PASTE_MARKER_BYTES) return true;
    if (bracketed && !text_append(text, "\033[201~", PASTE_MARKER_BYTES)) return false;
    size_t start = bracketed ? 0 : PASTE_MARKER_BYTES;
    size_t length = text->length - start;
    if (session->input_length == 0 && length > sizeof session->input) {
        free(session->large_input);
        session->large_input = text->bytes;
        session->large_capacity = text->capacity;
        session->input_start = start;
        session->input_length = length;
        *text = (TextBuffer){0};
        return true;
    }
    return session_append(session, text->bytes + start, length, true);
}

static void session_reply(void *context, const char *bytes, size_t length)
{
    session_queue(context, (const unsigned char *)bytes, length);
}

static PtyResult session_write(Session *session)
{
    ssize_t written;
    const unsigned char *input = session->large_input != NULL ? session->large_input : session->input;
    do {
        written = write(session->master, input + session->input_start,
                        session->input_length);
    } while (written < 0 && errno == EINTR);
    if (written >= 0) {
        session->input_length -= (size_t)written;
        session->input_start = session->input_length != 0
            ? session->input_start + (size_t)written : 0;
        if (session->large_input != NULL && session->input_length == 0) {
            free(session->large_input);
            session->large_input = NULL;
            session->large_capacity = 0;
        }
        return PTY_OPEN;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return PTY_OPEN;
    }
    return errno == EIO || errno == EPIPE ? PTY_CLOSED : PTY_FAILED;
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static PtyResult session_read(Session *session, Terminal *terminal, bool *redraw,
                              bool hangup)
{
    int64_t started = monotonic_milliseconds();
    unsigned int reads = 0;
    /* Escape sequences can cost much more than their byte counts suggest. */
    for (;;) {
        if (session->output_start < session->output_end) {
            size_t count = minimum(PTY_PARSE_SLICE, session->output_end - session->output_start);
            *redraw |= terminal_feed(terminal, session->output + session->output_start, count);
            session->output_start += count;
            if (started >= 0 && monotonic_milliseconds() - started >= PTY_PARSE_BUDGET) return PTY_OPEN;
            continue;
        }
        if (reads++ == 8) return PTY_OPEN;
        ssize_t count;
        do {
            count = read(session->master, session->output, sizeof session->output);
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
            session->output_start = 0;
            session->output_end = (size_t)count;
        } else if (count == 0 || errno == EIO) {
            return PTY_CLOSED;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return hangup ? PTY_CLOSED : PTY_OPEN;
        } else {
            return PTY_FAILED;
        }
    }
}

static int deadline_timeout(int64_t now, int64_t deadline)
{
    if (deadline == 0 || now < 0) return -1;
    return deadline > now ? (int)(deadline - now) : 0;
}

static int earlier_timeout(int a, int b)
{
    return b < 0 || (a >= 0 && a < b) ? a : b;
}

/* Wayland clipboard and primary selection */

enum { CLIPBOARD_SENDS = 8, CLIPBOARD_SOURCES = 4, CLIPBOARD_CHUNK = 32768, CLIPBOARD_TIMEOUT = 10000 };
static const char *const clipboard_types[] = {
    "text/plain;charset=utf-8", "text/plain;charset=UTF-8",
    "text/plain; charset=utf-8", "text/plain; charset=UTF-8", "UTF8_STRING", "text/plain"
};

typedef struct Clipboard Clipboard;
typedef struct ClipboardChannel ClipboardChannel;
typedef struct ClipboardSource ClipboardSource;

struct ClipboardSource {
    TextBuffer text;
    unsigned int refs;
    ClipboardChannel *channel;
    ClipboardSource *next;
    void *proxy;
};

typedef struct {
    void *proxy;
    const char *mime;
} ClipboardOffer;

struct ClipboardChannel {
    Clipboard *clipboard;
    /* The protocol on this channel determines the types of its proxies. */
    void *device;
    ClipboardOffer offer, pending;
    ClipboardSource *sources, *current;
    uint32_t seat_name, manager_name;
    bool primary;
};

typedef struct {
    int fd;
    ClipboardSource *source;
    size_t offset;
    int64_t deadline;
} ClipboardSend;

typedef struct {
    int fd;
    TextBuffer text;
    size_t received;
    int64_t deadline;
    bool after_cr, primary;
} ClipboardReceive;

struct Clipboard {
    ClipboardChannel regular, primary;
    ClipboardReceive receive;
    ClipboardSend send[CLIPBOARD_SENDS];
};

static void clipboard_init(Clipboard *clipboard)
{
    clipboard->regular.clipboard = clipboard;
    clipboard->primary = (ClipboardChannel){.clipboard = clipboard, .primary = true};
    clipboard->receive.fd = -1;
    for (size_t i = 0; i < CLIPBOARD_SENDS; ++i) clipboard->send[i].fd = -1;
}

static void clipboard_unref(ClipboardSource *source)
{
    if (source != NULL && --source->refs == 0) {
        text_destroy(&source->text);
        free(source);
    }
}

static int64_t clipboard_deadline(void)
{
    int64_t now = monotonic_milliseconds();
    return now < 0 ? 0 : now + CLIPBOARD_TIMEOUT;
}

static bool clipboard_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

static void clipboard_drop_send(ClipboardSend *send)
{
    if (send->fd < 0) return;
    close(send->fd);
    clipboard_unref(send->source);
    *send = (ClipboardSend){.fd = -1};
}

static void clipboard_drop_receive(ClipboardReceive *receive)
{
    if (receive->fd >= 0) close(receive->fd);
    text_destroy(&receive->text);
    *receive = (ClipboardReceive){.fd = -1};
}

static void clipboard_destroy_offer(ClipboardChannel *channel, void *offer)
{
    if (offer == NULL) return;
    if (channel->primary) zwp_primary_selection_offer_v1_destroy(offer);
    else wl_data_offer_destroy(offer);
}

static void clipboard_destroy_source_proxy(ClipboardSource *source)
{
    if (source->proxy == NULL) return;
    if (source->channel->primary) zwp_primary_selection_source_v1_destroy(source->proxy);
    else wl_data_source_destroy(source->proxy);
    source->proxy = NULL;
}

static void clipboard_drop_source(ClipboardSource *source)
{
    ClipboardSource **link = &source->channel->sources;
    while (*link != source) link = &(*link)->next;
    *link = source->next;
    if (source->channel->current == source) source->channel->current = NULL;
    clipboard_destroy_source_proxy(source);
    clipboard_unref(source);
}

static void clipboard_drop_channel(ClipboardChannel *channel)
{
    while (channel->sources != NULL) clipboard_drop_source(channel->sources);
    clipboard_destroy_offer(channel, channel->pending.proxy);
    clipboard_destroy_offer(channel, channel->offer.proxy);
    channel->pending = channel->offer = (ClipboardOffer){0};
    if (channel->device == NULL) return;
    if (channel->primary) zwp_primary_selection_device_v1_destroy(channel->device);
    else if (wl_data_device_get_version(channel->device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
        wl_data_device_release(channel->device);
    else wl_data_device_destroy(channel->device);
    channel->device = NULL;
}

static void clipboard_close(Clipboard *clipboard)
{
    clipboard_drop_receive(&clipboard->receive);
    for (size_t i = 0; i < CLIPBOARD_SENDS; ++i) clipboard_drop_send(&clipboard->send[i]);
    clipboard_drop_channel(&clipboard->primary);
    clipboard_drop_channel(&clipboard->regular);
}

static void clipboard_mime(ClipboardChannel *channel, void *offer, const char *mime)
{
    if (channel->pending.proxy != offer) return;
    for (size_t i = 0; i < sizeof clipboard_types / sizeof *clipboard_types; ++i) {
        if (channel->pending.mime == clipboard_types[i]) return;
        if (strcmp(mime, clipboard_types[i]) == 0) {
            channel->pending.mime = clipboard_types[i];
            return;
        }
    }
}

static void regular_offer_mime(void *data, struct wl_data_offer *offer, const char *mime)
{
    clipboard_mime(data, offer, mime);
}

static void primary_offer_mime(void *data, struct zwp_primary_selection_offer_v1 *offer, const char *mime)
{
    clipboard_mime(data, offer, mime);
}

static const struct wl_data_offer_listener regular_offer_listener = {.offer = regular_offer_mime};
static const struct zwp_primary_selection_offer_v1_listener primary_offer_listener = {.offer = primary_offer_mime};

static void regular_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
    (void)device;
    ClipboardChannel *channel = data;
    clipboard_destroy_offer(channel, channel->pending.proxy);
    channel->pending = (ClipboardOffer){.proxy = offer};
    if (wl_data_offer_add_listener(offer, &regular_offer_listener, channel) < 0) {
        clipboard_destroy_offer(channel, offer);
        channel->pending = (ClipboardOffer){0};
    }
}

static void primary_data_offer(void *data, struct zwp_primary_selection_device_v1 *device,
                               struct zwp_primary_selection_offer_v1 *offer)
{
    (void)device;
    ClipboardChannel *channel = data;
    clipboard_destroy_offer(channel, channel->pending.proxy);
    channel->pending = (ClipboardOffer){.proxy = offer};
    if (zwp_primary_selection_offer_v1_add_listener(offer, &primary_offer_listener, channel) < 0) {
        clipboard_destroy_offer(channel, offer);
        channel->pending = (ClipboardOffer){0};
    }
}

static void clipboard_set_offer(ClipboardChannel *channel, void *offer)
{
    if (offer != NULL && offer == channel->offer.proxy) return;
    clipboard_destroy_offer(channel, channel->offer.proxy);
    channel->offer = (ClipboardOffer){0};
    if (offer != NULL && offer == channel->pending.proxy) {
        channel->offer = channel->pending;
        channel->pending = (ClipboardOffer){0};
    }
}

static void regular_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
    (void)device;
    clipboard_set_offer(data, offer);
}

static void primary_selection(void *data, struct zwp_primary_selection_device_v1 *device,
                              struct zwp_primary_selection_offer_v1 *offer)
{
    (void)device;
    clipboard_set_offer(data, offer);
}

static void regular_drag_enter(void *data, struct wl_data_device *device, uint32_t serial,
                               struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                               struct wl_data_offer *offer)
{
    (void)device;
    (void)surface;
    (void)x;
    (void)y;
    ClipboardChannel *channel = data;
    if (offer != NULL && offer == channel->pending.proxy) {
        wl_data_offer_accept(offer, serial, NULL);
        clipboard_destroy_offer(channel, offer);
        channel->pending = (ClipboardOffer){0};
    }
}

static void regular_drag_noop(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
}

static void regular_drag_motion(void *data, struct wl_data_device *device, uint32_t time,
                                wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)device;
    (void)time;
    (void)x;
    (void)y;
}

static const struct wl_data_device_listener regular_device_listener = {
    .data_offer = regular_data_offer, .enter = regular_drag_enter, .leave = regular_drag_noop,
    .motion = regular_drag_motion, .drop = regular_drag_noop, .selection = regular_selection
};

static const struct zwp_primary_selection_device_v1_listener primary_device_listener = {
    .data_offer = primary_data_offer, .selection = primary_selection
};

static bool clipboard_sync_channel(ClipboardChannel *channel, Platform *platform, bool available, uint32_t manager)
{
    available = available && platform->seat != NULL;
    if (channel->device != NULL &&
        (!available || channel->seat_name != platform->seat_name || channel->manager_name != manager)) {
        ClipboardReceive *receive = &channel->clipboard->receive;
        if (receive->fd >= 0 && receive->primary == channel->primary) clipboard_drop_receive(receive);
        clipboard_drop_channel(channel);
    }
    if (!available || channel->device != NULL) return true;
    channel->seat_name = platform->seat_name;
    channel->manager_name = manager;
    if (channel->primary) {
        channel->device = zwp_primary_selection_device_manager_v1_get_device(platform->primary_manager,
                                                                             platform->seat);
        return channel->device != NULL &&
               zwp_primary_selection_device_v1_add_listener(channel->device, &primary_device_listener, channel) == 0;
    }
    channel->device = wl_data_device_manager_get_data_device(platform->clipboard_manager, platform->seat);
    return channel->device != NULL && wl_data_device_add_listener(channel->device, &regular_device_listener, channel) == 0;
}

static bool clipboard_sync(Clipboard *clipboard, Platform *platform, const Config *config)
{
    return clipboard_sync_channel(&clipboard->regular, platform, platform->clipboard_manager != NULL,
                                  platform->clipboard_name) &&
           clipboard_sync_channel(&clipboard->primary, platform,
                                  config->primary_selection && platform->primary_manager != NULL,
                                  platform->primary_name);
}

static void clipboard_send(ClipboardSource *source, const char *mime, int fd)
{
    bool supported = false;
    for (size_t i = 0; i < sizeof clipboard_types / sizeof *clipboard_types; ++i)
        if (strcmp(mime, clipboard_types[i]) == 0) supported = true;
    if (supported && clipboard_nonblocking(fd)) {
        for (size_t i = 0; i < CLIPBOARD_SENDS; ++i) {
            ClipboardSend *send = &source->channel->clipboard->send[i];
            if (send->fd >= 0) continue;
            ++source->refs;
            *send = (ClipboardSend){.fd = fd, .source = source, .deadline = clipboard_deadline()};
            return;
        }
    }
    close(fd);
}

static void regular_source_target(void *data, struct wl_data_source *source, const char *mime)
{
    (void)data;
    (void)source;
    (void)mime;
}

static void regular_source_send(void *data, struct wl_data_source *source, const char *mime, int32_t fd)
{
    (void)source;
    clipboard_send(data, mime, fd);
}

static void primary_source_send(void *data, struct zwp_primary_selection_source_v1 *source,
                                const char *mime, int32_t fd)
{
    (void)source;
    clipboard_send(data, mime, fd);
}

static void regular_source_cancel(void *data, struct wl_data_source *source)
{
    (void)source;
    clipboard_drop_source(data);
}

static void primary_source_cancel(void *data, struct zwp_primary_selection_source_v1 *source)
{
    (void)source;
    clipboard_drop_source(data);
}

static const struct wl_data_source_listener regular_source_listener = {
    .target = regular_source_target, .send = regular_source_send, .cancelled = regular_source_cancel
};

static const struct zwp_primary_selection_source_v1_listener primary_source_listener = {
    .send = primary_source_send, .cancelled = primary_source_cancel
};

static bool clipboard_channel_ready(const ClipboardChannel *channel, const Platform *platform)
{
    if (channel->device == NULL || platform->seat == NULL || channel->seat_name != platform->seat_name)
        return false;
    return channel->primary ? platform->primary_manager != NULL && channel->manager_name == platform->primary_name
                            : platform->clipboard_manager != NULL && channel->manager_name == platform->clipboard_name;
}

/* Publish text, taking ownership of it. */
static void clipboard_offer(ClipboardChannel *channel, Platform *platform, TextBuffer *text, uint32_t serial)
{
    ClipboardSource *source = clipboard_channel_ready(channel, platform) ? calloc(1, sizeof *source) : NULL;
    if (source == NULL) {
        text_destroy(text);
        return;
    }
    source->refs = 1;
    source->channel = channel;
    source->text = *text;
    *text = (TextBuffer){0};

    bool listening;
    if (channel->primary) {
        source->proxy = zwp_primary_selection_device_manager_v1_create_source(platform->primary_manager);
        listening = source->proxy != NULL &&
                    zwp_primary_selection_source_v1_add_listener(source->proxy, &primary_source_listener, source) == 0;
    } else {
        source->proxy = wl_data_device_manager_create_data_source(platform->clipboard_manager);
        listening = source->proxy != NULL &&
                    wl_data_source_add_listener(source->proxy, &regular_source_listener, source) == 0;
    }
    if (!listening) {
        clipboard_destroy_source_proxy(source);
        clipboard_unref(source);
        return;
    }
    source->next = channel->sources;
    channel->sources = source;
    channel->current = source;
    for (size_t i = 0; i < sizeof clipboard_types / sizeof *clipboard_types; ++i) {
        if (channel->primary) zwp_primary_selection_source_v1_offer(source->proxy, clipboard_types[i]);
        else wl_data_source_offer(source->proxy, clipboard_types[i]);
    }
    if (channel->primary) zwp_primary_selection_device_v1_set_selection(channel->device, source->proxy, serial);
    else wl_data_device_set_selection(channel->device, source->proxy, serial);

    /* Keep previous owners until cancellation, with a limit for unacknowledged requests. */
    ClipboardSource *last = source;
    for (size_t i = 1; i < CLIPBOARD_SOURCES && last->next != NULL; ++i) last = last->next;
    if (last->next != NULL) clipboard_drop_source(last->next);
}

static void clipboard_copy(ClipboardChannel *channel, Platform *platform, Terminal *terminal, uint32_t serial)
{
    if (!clipboard_channel_ready(channel, platform)) return;
    TextBuffer text = {0};
    if (selection_text(terminal, &text)) clipboard_offer(channel, platform, &text, serial);
    else text_destroy(&text);
}

/* Hand a decoded OSC 52 write to the requested selections. */
static void clipboard_remote_write(Clipboard *clipboard, Platform *platform, Terminal *terminal)
{
    if (terminal->clipboard_write == NULL) return;
    TextBuffer text = {terminal->clipboard_write, terminal->clipboard_write_length, terminal->clipboard_write_length};
    terminal->clipboard_write = NULL;
    unsigned int targets = terminal->clipboard_targets;
    if ((targets & OSC52_PRIMARY) && (targets & OSC52_CLIPBOARD)) {
        TextBuffer copy = {0};
        if (text_append(&copy, text.bytes, text.length)) {
            clipboard_offer(&clipboard->primary, platform, &copy, platform->input_serial);
        } else text_destroy(&copy);
    } else if (targets & OSC52_PRIMARY) {
        clipboard_offer(&clipboard->primary, platform, &text, platform->input_serial);
        return;
    }
    clipboard_offer(&clipboard->regular, platform, &text, platform->input_serial);
}

static void clipboard_clear_primary(ClipboardChannel *channel, const Platform *platform, uint32_t serial)
{
    if (channel->current == NULL || !clipboard_channel_ready(channel, platform)) return;
    zwp_primary_selection_device_v1_set_selection(channel->device, NULL, serial);
    channel->current = NULL;
}

static void clipboard_paste(ClipboardChannel *channel, const Platform *platform, Terminal *terminal)
{
    ClipboardReceive *receive = &channel->clipboard->receive;
    if (receive->fd >= 0 || channel->offer.mime == NULL || !clipboard_channel_ready(channel, platform)) return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) {
        terminal->bell_pending = true;
        return;
    }
    if (!clipboard_nonblocking(fds[0]) || !text_append(&receive->text, "\033[200~", PASTE_MARKER_BYTES)) {
        close(fds[0]);
        close(fds[1]);
        clipboard_drop_receive(receive);
        terminal->bell_pending = true;
        return;
    }
    if (channel->primary)
        zwp_primary_selection_offer_v1_receive(channel->offer.proxy, channel->offer.mime, fds[1]);
    else wl_data_offer_receive(channel->offer.proxy, channel->offer.mime, fds[1]);
    close(fds[1]);
    receive->fd = fds[0];
    receive->primary = channel->primary;
    receive->deadline = clipboard_deadline();
}

static void clipboard_read(ClipboardReceive *receive, Session *session, Terminal *terminal,
                           const Config *config, bool *redraw, short events)
{
    if (!events) return;
    unsigned char bytes[CLIPBOARD_CHUNK];
    if (events & (POLLERR | POLLNVAL)) {
        clipboard_drop_receive(receive);
        terminal->bell_pending = true;
        return;
    }
    ssize_t count = read(receive->fd, bytes, sizeof bytes);
    if (count > 0 && (size_t)count <= TEXT_LIMIT - 2 * PASTE_MARKER_BYTES - receive->received &&
        paste_chunk(&receive->text, bytes, (size_t)count, &receive->after_cr)) {
        receive->received += (size_t)count;
        receive->deadline = clipboard_deadline();
        return;
    }
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (count == 0 && session_paste(session, &receive->text, terminal->bracketed_paste)) {
        if (receive->received != 0 && config->scroll_on_input)
            *redraw |= history_scroll(terminal, false, SIZE_MAX);
    } else terminal->bell_pending = true;
    clipboard_drop_receive(receive);
}

static void clipboard_write(ClipboardSend *send, short events)
{
    if (!events) return;
    if (!(events & (POLLERR | POLLHUP | POLLNVAL))) {
        TextBuffer *text = &send->source->text;
        ssize_t count = write(send->fd, text->bytes + send->offset,
                              minimum(text->length - send->offset, CLIPBOARD_CHUNK));
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) return;
        if (count > 0) {
            send->offset += (size_t)count;
            if (send->offset < text->length) {
                send->deadline = clipboard_deadline();
                return;
            }
        }
    }
    clipboard_drop_send(send);
}

static int clipboard_timeout(Clipboard *clipboard, Terminal *terminal, int64_t now)
{
    int timeout = -1;
    ClipboardReceive *receive = &clipboard->receive;
    if (receive->fd >= 0) {
        timeout = deadline_timeout(now, receive->deadline);
        if (timeout == 0) {
            clipboard_drop_receive(receive);
            terminal->bell_pending = true;
            timeout = -1;
        }
    }
    for (size_t i = 0; i < CLIPBOARD_SENDS; ++i) {
        ClipboardSend *send = &clipboard->send[i];
        if (send->fd < 0) continue;
        int deadline = deadline_timeout(now, send->deadline);
        if (deadline == 0) clipboard_drop_send(send);
        else timeout = earlier_timeout(timeout, deadline);
    }
    return timeout;
}

/* Keyboard translation */

typedef struct {
    unsigned char bytes[16];
    size_t length;
} KeyInput;

static KeyInput keyboard_translate(xkb_keysym_t key, bool control, bool alt, bool application_arrows,
                                   bool application_keypad, const char *text, int count)
{
    const char *sequence = NULL;
    static const char *const function_keys[] = {
        "\033[11~", "\033[12~", "\033[13~", "\033[14~", "\033[15~", "\033[17~",
        "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~"
    };
    if (key >= XKB_KEY_F1 && key <= XKB_KEY_F12) sequence = function_keys[key - XKB_KEY_F1];
    if (application_keypad && key >= XKB_KEY_KP_0 && key <= XKB_KEY_KP_9) {
        return (KeyInput){{0x1b, 'O', (unsigned char)('p' + key - XKB_KEY_KP_0)}, 3};
    }
    if (application_keypad) {
        char code = 0;
        switch (key) {
        case XKB_KEY_KP_Insert: code = 'p'; break;
        case XKB_KEY_KP_End: code = 'q'; break;
        case XKB_KEY_KP_Down: code = 'r'; break;
        case XKB_KEY_KP_Page_Down: code = 's'; break;
        case XKB_KEY_KP_Left: code = 't'; break;
        case XKB_KEY_KP_Begin: code = 'u'; break;
        case XKB_KEY_KP_Right: code = 'v'; break;
        case XKB_KEY_KP_Home: code = 'w'; break;
        case XKB_KEY_KP_Up: code = 'x'; break;
        case XKB_KEY_KP_Page_Up: code = 'y'; break;
        case XKB_KEY_KP_Delete: code = 'n'; break;
        }
        if (code) return (KeyInput){{0x1b, 'O', (unsigned char)code}, 3};
    }
    switch (key) {
    case XKB_KEY_Return: case XKB_KEY_KP_Enter:
        if (key == XKB_KEY_KP_Enter && application_keypad) return (KeyInput){{0x1b, 'O', 'M'}, 3};
        return (KeyInput){{'\r'}, 1};
    case XKB_KEY_BackSpace: return (KeyInput){{0x7f}, 1};
    case XKB_KEY_Tab: case XKB_KEY_KP_Tab: return (KeyInput){{'\t'}, 1};
    case XKB_KEY_ISO_Left_Tab: sequence = "\033[Z"; break;
    case XKB_KEY_Escape: return (KeyInput){{0x1b}, 1};
    case XKB_KEY_Up: case XKB_KEY_KP_Up: sequence = application_arrows ? "\033OA" : "\033[A"; break;
    case XKB_KEY_Down: case XKB_KEY_KP_Down: sequence = application_arrows ? "\033OB" : "\033[B"; break;
    case XKB_KEY_Right: case XKB_KEY_KP_Right: sequence = application_arrows ? "\033OC" : "\033[C"; break;
    case XKB_KEY_Left: case XKB_KEY_KP_Left: sequence = application_arrows ? "\033OD" : "\033[D"; break;
    case XKB_KEY_Home: case XKB_KEY_KP_Home: sequence = "\033[1~"; break;
    case XKB_KEY_End: case XKB_KEY_KP_End: sequence = "\033[4~"; break;
    case XKB_KEY_Insert: case XKB_KEY_KP_Insert: sequence = "\033[2~"; break;
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete: sequence = "\033[3~"; break;
    case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up: sequence = "\033[5~"; break;
    case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down: sequence = "\033[6~"; break;
    case XKB_KEY_KP_Multiply: if (application_keypad) sequence = "\033Oj"; break;
    case XKB_KEY_KP_Add: if (application_keypad) sequence = "\033Ok"; break;
    case XKB_KEY_KP_Separator: if (application_keypad) sequence = "\033Ol"; break;
    case XKB_KEY_KP_Subtract: if (application_keypad) sequence = "\033Om"; break;
    case XKB_KEY_KP_Decimal: if (application_keypad) sequence = "\033On"; break;
    case XKB_KEY_KP_Divide: if (application_keypad) sequence = "\033Oo"; break;
    }

    KeyInput input = {0};
    if (sequence != NULL) {
        input.length = strlen(sequence);
        memcpy(input.bytes, sequence, input.length);
    } else if (control && key >= XKB_KEY_a && key <= XKB_KEY_z) {
        input.bytes[input.length++] = (unsigned char)(key - XKB_KEY_a + 1);
    } else if (control && key >= XKB_KEY_A && key <= XKB_KEY_Z) {
        input.bytes[input.length++] = (unsigned char)(key - XKB_KEY_A + 1);
    } else {
        /* XKB supplies UTF-8 and applies the standard Control transformations. */
        if (count > 0 && (control || (unsigned char)text[0] >= 0x20)) {
            if (alt && (unsigned char)text[0] >= 0x20 && (unsigned char)text[0] != 0x7f)
                input.bytes[input.length++] = 0x1b;
            memcpy(input.bytes + input.length, text, (size_t)count);
            input.length += (size_t)count;
        }
    }
    return input;
}

static KeyInput keyboard_encode(xkb_keysym_t key, unsigned int modifiers, bool application_arrows,
                               bool application_keypad, const char *text, int count)
{
    bool control = (modifiers & MOD_CTRL) != 0, alt = (modifiers & MOD_ALT) != 0;
    unsigned int parameter = 1 + !!(modifiers & MOD_SHIFT) + 2u * alt + 4u * control;
    char final = 0;
    unsigned int number = 0;
    if (parameter != 1) {
        static const unsigned int function[] = {15, 17, 18, 19, 20, 21, 23, 24};
        if (key >= XKB_KEY_F1 && key <= XKB_KEY_F4) final = (char)('P' + key - XKB_KEY_F1);
        if (key >= XKB_KEY_F5 && key <= XKB_KEY_F12) number = function[key - XKB_KEY_F5];
        switch (key) {
        case XKB_KEY_Up: case XKB_KEY_KP_Up: final = 'A'; break;
        case XKB_KEY_Down: case XKB_KEY_KP_Down: final = 'B'; break;
        case XKB_KEY_Right: case XKB_KEY_KP_Right: final = 'C'; break;
        case XKB_KEY_Left: case XKB_KEY_KP_Left: final = 'D'; break;
        case XKB_KEY_Home: case XKB_KEY_KP_Home: final = 'H'; break;
        case XKB_KEY_End: case XKB_KEY_KP_End: final = 'F'; break;
        case XKB_KEY_Insert: case XKB_KEY_KP_Insert: number = 2; break;
        case XKB_KEY_Delete: case XKB_KEY_KP_Delete: number = 3; break;
        case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up: number = 5; break;
        case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down: number = 6; break;
        case XKB_KEY_Tab: case XKB_KEY_KP_Tab:
            if (modifiers & MOD_SHIFT) key = XKB_KEY_ISO_Left_Tab;
            break;
        }
    }
    if (final != 0 || number != 0) {
        KeyInput input = {0};
        input.length = (size_t)snprintf((char *)input.bytes, sizeof input.bytes, "\033[%u;%u%c",
                                       number ? number : 1, parameter, final ? final : '~');
        return input;
    }
    KeyInput input = keyboard_translate(key, control, alt, application_arrows, application_keypad, text, count);
    bool single_control = input.length == 1 && (input.bytes[0] <= 0x1f || input.bytes[0] == 0x7f);
    if (alt && (single_control || key == XKB_KEY_ISO_Left_Tab)) {
        memmove(input.bytes + 1, input.bytes, input.length);
        ++input.length;
        input.bytes[0] = 0x1b;
    }
    return input;
}

typedef struct {
    SelectionPoint last_point;
    uint32_t last_click;
    unsigned int clicks, buttons, generation;
    unsigned int column, row, code;
    int64_t deadline;
    bool last_alternate, motion_reported;
} Mouse;

typedef struct {
    Platform *platform;
    Terminal *terminal;
    Session *session;
    const Config *config;
    const FontRenderer *renderer;
    Clipboard *clipboard;
    Mouse mouse;
    pid_t link_children[8];
    int font_size;
    bool reload;
    bool *redraw;
    int tab_action; /* TabAction requested by a shortcut or click, handled by the run loop. */
    size_t tab_target, tab_count;
    int bar; /* Height of the tab bar above the grid, zero when hidden. */
} Interaction;

typedef enum { TAB_NONE, TAB_NEW, TAB_CLOSE, TAB_NEXT, TAB_PREVIOUS, TAB_GOTO, TAB_CLOSE_AT } TabAction;

static bool shortcut_matches(Shortcut shortcut, unsigned int modifiers, xkb_keysym_t key,
                             xkb_keysym_t unshifted)
{
    return shortcut.key != XKB_KEY_NoSymbol && modifiers == shortcut.modifiers &&
           (key == shortcut.key || unshifted == shortcut.key);
}

static void input_zoom(Interaction *input, int64_t steps)
{
    input->font_size = config_zoom_size(input->config, input->font_size + steps * input->config->zoom_step);
}

static void window_open(Interaction *input);

static bool keyboard_event(void *data, xkb_keycode_t code, KeyEvent event, uint32_t serial)
{
    Interaction *input = data;
    struct xkb_state *keys = input->platform->keys;
    Terminal *terminal = input->terminal;
    const Config *config = input->config;
    const char *composed = input->platform->composed;
    xkb_keysym_t key = composed != NULL ? input->platform->composed_key : xkb_state_key_get_one_sym(keys, code);
    unsigned int modifiers = keyboard_modifiers(keys);
    const xkb_keysym_t *symbols;
    xkb_keysym_t normalized = shortcut_key(key), unshifted = normalized;
    if ((modifiers & MOD_SHIFT) &&
        xkb_keymap_key_get_syms_by_level(xkb_state_get_keymap(keys), code,
                                       xkb_state_key_get_layout(keys, code), 0, &symbols) == 1)
        unshifted = shortcut_key(symbols[0]);
    if (shortcut_matches(config->reload, modifiers, normalized, unshifted)) {
        if (event == KEY_PRESS) input->reload = true;
        return true;
    }
    if (shortcut_matches(config->zoom_in, modifiers, normalized, unshifted)) {
        input_zoom(input, 1);
        return true;
    }
    if (shortcut_matches(config->zoom_out, modifiers, normalized, unshifted)) {
        input_zoom(input, -1);
        return true;
    }
    if (shortcut_matches(config->zoom_reset, modifiers, normalized, unshifted)) {
        input->font_size = config->font_size;
        return true;
    }
    if (shortcut_matches(config->fullscreen, modifiers, normalized, unshifted)) {
        if (event == KEY_PRESS) platform_fullscreen(input->platform);
        return true;
    }
    if (shortcut_matches(config->new_window, modifiers, normalized, unshifted)) {
        if (event == KEY_PRESS) window_open(input);
        return true;
    }
    if (shortcut_matches(config->new_tab, modifiers, normalized, unshifted)) {
        if (event == KEY_PRESS) input->tab_action = TAB_NEW;
        return true;
    }
    if (shortcut_matches(config->close_tab, modifiers, normalized, unshifted)) {
        if (event == KEY_PRESS) input->tab_action = TAB_CLOSE;
        return true;
    }
    /* Switching shortcuts reach the application while there is only one tab. */
    if (input->tab_count > 1) {
        if (shortcut_matches(config->next_tab, modifiers, normalized, unshifted)) {
            input->tab_action = TAB_NEXT;
            return true;
        }
        if (shortcut_matches(config->previous_tab, modifiers, normalized, unshifted)) {
            input->tab_action = TAB_PREVIOUS;
            return true;
        }
        if (modifiers == MOD_ALT && unshifted >= XKB_KEY_1 && unshifted <= XKB_KEY_9) {
            if (event == KEY_PRESS) {
                input->tab_action = TAB_GOTO;
                input->tab_target = (size_t)(unshifted - XKB_KEY_1);
            }
            return true;
        }
    }
    if (modifiers == (MOD_CTRL | MOD_SHIFT) && (normalized == XKB_KEY_c || normalized == XKB_KEY_v)) {
        if (event == KEY_PRESS) {
            ClipboardChannel *clipboard = &input->clipboard->regular;
            if (normalized == XKB_KEY_c) clipboard_copy(clipboard, input->platform, terminal, serial);
            else clipboard_paste(clipboard, input->platform, terminal);
        }
        return true;
    }

    if (modifiers == MOD_SHIFT && !terminal->using_alternate) {
        size_t page = terminal->primary.screen.rows;
        switch (key) {
        case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up: *input->redraw |= history_scroll(terminal, true, page); return true;
        case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down: *input->redraw |= history_scroll(terminal, false, page); return true;
        case XKB_KEY_Home: case XKB_KEY_KP_Home: *input->redraw |= history_scroll(terminal, true, SIZE_MAX); return true;
        case XKB_KEY_End: case XKB_KEY_KP_End: *input->redraw |= history_scroll(terminal, false, SIZE_MAX); return true;
        }
    }
    /* A held local shortcut must not leak into the PTY when its modifiers are released. */
    if (event == KEY_REPEAT_SHORTCUT) return true;

    char text[sizeof ((KeyInput){0}).bytes];
    int count = 0;
    if (composed != NULL) {
        size_t length = strlen(composed);
        if (length < sizeof text) {
            memcpy(text, composed, length + 1);
            count = (int)length;
        }
    } else count = xkb_state_key_get_utf8(keys, code, text, sizeof text);
    if (count < 0 || (size_t)count >= sizeof text) count = 0;
    KeyInput encoded = keyboard_encode(key, modifiers, terminal->application_arrows,
                                       terminal->application_keypad, text, count);
    if (!session_queue(input->session, encoded.bytes, encoded.length)) {
        terminal->bell_pending = true;
    } else if (encoded.length != 0 && config->scroll_on_input) {
        *input->redraw |= history_scroll(terminal, false, SIZE_MAX);
    }
    return false;
}

/* Local pointer interaction and terminal mouse reporting */

static Padding input_padding(const Interaction *input)
{
    return grid_padding(input->config->padding, input->platform->width, input->platform->height - input->bar,
                        &input->renderer->metrics, &terminal_buffer(input->terminal)->screen);
}

/* Tabs split the whole bar equally. Tab i spans [tab_edge(i), tab_edge(i + 1)). */
static int tab_edge(int width, size_t count, size_t index)
{
    return count != 0 ? (int)((int64_t)width * (int64_t)index / (int64_t)count) : 0;
}

/* The close button fills the last two cells of a tab at least four cells wide. */
static bool tab_close_hit(int left, int right, int cell_width, double x)
{
    return right - left >= 4 * cell_width && x >= right - 2 * cell_width;
}

static size_t tab_at(const Interaction *input, double x, bool *close)
{
    int width = input->platform->width;
    size_t count = input->tab_count;
    if (x < 0 || x >= width || count == 0) return SIZE_MAX;
    size_t index = (size_t)(x * (double)count / width);
    if (index >= count) index = count - 1;
    *close = tab_close_hit(tab_edge(width, count, index), tab_edge(width, count, index + 1),
                           input->renderer->metrics.width, x);
    return index;
}

static bool mouse_cell(const Interaction *input, unsigned int *column, unsigned int *row)
{
    const FontMetrics *metrics = &input->renderer->metrics;
    const TerminalScreen *screen = &terminal_buffer(input->terminal)->screen;
    Padding padding = input_padding(input);
    double x = input->platform->pointer_x - padding.horizontal;
    double y = input->platform->pointer_y - input->bar - padding.vertical;
    if (x < 0 || y < 0 || x / metrics->width >= (double)screen->columns ||
        y / metrics->height >= (double)screen->rows ||
        x >= grid_extent(input->platform->width, padding.horizontal) ||
        y >= grid_extent(input->platform->height - input->bar, padding.vertical)) return false;
    *column = (unsigned int)(x / metrics->width) + 1;
    *row = (unsigned int)(y / metrics->height) + 1;
    return true;
}

static uint16_t mouse_link(const Interaction *input)
{
    if (!input->config->hyperlinks || input->terminal->links == NULL) return 0;
    unsigned int column, row;
    if (!mouse_cell(input, &column, &row)) return 0;
    TerminalView view = terminal_view(input->terminal);
    LineView line = view_line(&view, view.first_visible + row - 1);
    return view_cell(&view, &line, column - 1)->hyperlink;
}

static void link_reap(Interaction *input)
{
    for (size_t i = 0; i < sizeof input->link_children / sizeof *input->link_children; ++i) {
        pid_t *child = &input->link_children[i];
        if (*child == 0) continue;
        pid_t result;
        do { result = waitpid(*child, NULL, WNOHANG); } while (result < 0 && errno == EINTR);
        if (result == *child || (result < 0 && errno == ECHILD)) *child = 0;
    }
}

static void link_open(Interaction *input, const char *url)
{
    extern char **environ;
    if (!link_valid(url, strlen(url))) return;
    link_reap(input);
    for (size_t i = 0; i < sizeof input->link_children / sizeof *input->link_children; ++i) {
        if (input->link_children[i] != 0) continue;
        posix_spawnattr_t attributes;
        int error = posix_spawnattr_init(&attributes);
        if (error == 0) {
            error = posix_spawnattr_setsigmask(&attributes, &input->session->original_signal_mask);
            if (error == 0) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
            if (error == 0) {
                char *const arguments[] = {"xdg-open", (char *)url, NULL};
                error = posix_spawnp(&input->link_children[i], arguments[0], NULL, &attributes, arguments, environ);
                if (error != 0) input->link_children[i] = 0;
            }
            posix_spawnattr_destroy(&attributes);
        }
        if (error != 0) fprintf(stderr, "deal: could not start xdg-open: %s\n", strerror(error));
        return;
    }
}

/* The shell's directory: OSC 7 when the shell reports it, otherwise the
   directory of the shell process itself. NULL when neither is known. */
static const char *shell_directory(const Interaction *input, char directory[PATH_MAX])
{
    struct stat info;
    if (input->terminal->directory[0] != 0 && stat(input->terminal->directory, &info) == 0 && S_ISDIR(info.st_mode))
        return input->terminal->directory;
    if (input->session->child > 0) {
        char link[64];
        snprintf(link, sizeof link, "/proc/%d/cwd", (int)input->session->child);
        ssize_t count = readlink(link, directory, PATH_MAX - 1);
        if (count > 0) {
            directory[count] = 0;
            return directory;
        }
    }
    return NULL;
}

/* Start another deal in the shell's directory. */
static void window_open(Interaction *input)
{
    extern char **environ;
    char executable[PATH_MAX], directory[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (length <= 0) return;
    executable[length] = 0;
    /* A rebuilt binary leaves the running one marked deleted at the same path. */
    static const char deleted[] = " (deleted)";
    if ((size_t)length > sizeof deleted - 1 && strcmp(executable + length - (sizeof deleted - 1), deleted) == 0)
        executable[length - (sizeof deleted - 1)] = 0;
    const char *chosen = shell_directory(input, directory);
    char *arguments[6];
    size_t count = 0;
    arguments[count++] = executable;
    if (chosen != NULL) {
        arguments[count++] = "--working-directory";
        arguments[count++] = (char *)chosen;
    }
    if (input->config->options->config_path != NULL) {
        arguments[count++] = "--config";
        arguments[count++] = (char *)input->config->options->config_path;
    }
    arguments[count] = NULL;
    link_reap(input);
    for (size_t i = 0; i < sizeof input->link_children / sizeof *input->link_children; ++i) {
        if (input->link_children[i] != 0) continue;
        posix_spawnattr_t attributes;
        int error = posix_spawnattr_init(&attributes);
        if (error == 0) {
            error = posix_spawnattr_setsigmask(&attributes, &input->session->original_signal_mask);
            if (error == 0) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSID);
            if (error == 0) error = posix_spawn(&input->link_children[i], executable, NULL, &attributes, arguments, environ);
            if (error != 0) input->link_children[i] = 0;
            posix_spawnattr_destroy(&attributes);
        }
        if (error != 0) fprintf(stderr, "deal: could not open a new window: %s\n", strerror(error));
        return;
    }
    fprintf(stderr, "deal: too many windows are still starting\n");
}

static bool mouse_reporting(const Interaction *input, unsigned int modifiers)
{
    const Terminal *terminal = input->terminal;
    return input->config->mouse_reporting && terminal->mouse_mode != 0 &&
           (terminal->using_alternate || terminal->history.offset == 0) &&
           !(modifiers & input->config->mouse_override) && !terminal->selection.dragging;
}

static void mouse_sync(Interaction *input)
{
    Mouse *mouse = &input->mouse;
    if (mouse->generation == input->terminal->mouse_generation) return;
    mouse->generation = input->terminal->mouse_generation;
    mouse->buttons = 0;
    mouse->motion_reported = false;
}

static unsigned int mouse_report_code(unsigned int code, unsigned int modifiers)
{
    return code | (modifiers & MOD_SHIFT ? 4u : 0) | (modifiers & MOD_ALT ? 8u : 0) |
                  (modifiers & MOD_CTRL ? 16u : 0);
}

static bool mouse_send(Interaction *input, unsigned int code, unsigned int column,
                        unsigned int row, bool release)
{
    unsigned char bytes[32];
    size_t length;
    if (input->terminal->mouse_sgr) {
        length = (size_t)snprintf((char *)bytes, sizeof bytes, "\033[<%u;%u;%u%c",
                                 code, column, row, release ? 'm' : 'M');
    } else {
        if (column > 223 || row > 223) return false;
        bytes[0] = 0x1b;
        bytes[1] = '[';
        bytes[2] = 'M';
        bytes[3] = (unsigned char)(32 + code);
        bytes[4] = (unsigned char)(32 + column);
        bytes[5] = (unsigned char)(32 + row);
        length = 6;
    }
    return session_queue(input->session, bytes, length);
}

static SelectionPoint mouse_selection_point(const Interaction *input)
{
    TerminalView view = terminal_view(input->terminal);
    return selection_point(&view, &input->renderer->metrics, input_padding(input),
                           (int)input->platform->pointer_x, (int)input->platform->pointer_y - input->bar);
}

static void mouse_extend(Interaction *input)
{
    SelectionPoint point = mouse_selection_point(input);
    const SelectionPoint origin = input->terminal->selection.origin;
    if (point.line != origin.line || point.column != origin.column) input->mouse.clicks = 0;
    *input->redraw |= selection_extend(input->terminal, point);
}

static void pointer_left(void *data)
{
    Interaction *input = data;
    mouse_sync(input);
    unsigned int modifiers = keyboard_modifiers(input->platform->keys);
    for (unsigned int button = 0; button < 3; ++button) {
        if (input->mouse.buttons & (1u << button)) {
            unsigned int code = input->terminal->mouse_sgr ? button : 3;
            mouse_send(input, mouse_report_code(code, modifiers),
                       input->mouse.column, input->mouse.row, true);
        }
    }
    input->mouse = (Mouse){0};
    input->terminal->selection.dragging = false;
}

static void pointer_moved(void *data)
{
    Interaction *input = data;
    Mouse *mouse = &input->mouse;
    mouse_sync(input);
    if (input->terminal->selection.dragging) {
        mouse_extend(input);
        return;
    }
    unsigned int modifiers = keyboard_modifiers(input->platform->keys);
    unsigned int mode = input->terminal->mouse_mode;
    unsigned int column, row;
    if (!mouse_reporting(input, modifiers) || mode == 1000 || (mode == 1002 && mouse->buttons == 0) ||
        !mouse_cell(input, &column, &row)) {
        mouse->motion_reported = false;
        return;
    }
    unsigned int button = mouse->buttons & 1 ? 0 : mouse->buttons & 2 ? 1 : mouse->buttons & 4 ? 2 : 3;
    unsigned int code = mouse_report_code(32 + button, modifiers);
    if (mouse->motion_reported && column == mouse->column && row == mouse->row && code == mouse->code) return;
    if (mouse_send(input, code, column, row, false)) {
        mouse->column = column;
        mouse->row = row;
        mouse->code = code;
        mouse->motion_reported = true;
    }
}

static void pointer_button_event(void *data, uint32_t button, bool pressed, uint32_t serial, uint32_t time)
{
    Interaction *input = data;
    Mouse *mouse = &input->mouse;
    Terminal *terminal = input->terminal;
    const Config *config = input->config;
    unsigned int modifiers = keyboard_modifiers(input->platform->keys);
    unsigned int code;
    switch (button) {
    case BTN_LEFT: code = 0; break;
    case BTN_MIDDLE: code = 1; break;
    case BTN_RIGHT: code = 2; break;
    default: return;
    }
    /* Left click switches to a tab or presses its close button, and middle
       click closes it. */
    if (input->bar > 0 && input->platform->pointer_y < input->bar && !terminal->selection.dragging) {
        bool close = false;
        size_t index = tab_at(input, input->platform->pointer_x, &close);
        if (pressed && index < input->tab_count && code != 2) {
            input->tab_action = code == 1 || close ? TAB_CLOSE_AT : TAB_GOTO;
            input->tab_target = index;
        }
        return;
    }
    unsigned int bit = 1u << code;
    mouse_sync(input);
    mouse->motion_reported = false;
    if (!pressed) {
        if (mouse->buttons & bit) {
            mouse->buttons &= ~bit;
            unsigned int column, row;
            if (!mouse_cell(input, &column, &row) ||
                (!terminal->mouse_sgr && (column > 223 || row > 223))) {
                column = mouse->column;
                row = mouse->row;
            }
            mouse_send(input, mouse_report_code(terminal->mouse_sgr ? code : 3, modifiers), column, row, true);
        } else if (button == BTN_LEFT && terminal->selection.dragging) {
            mouse_extend(input);
            terminal->selection.dragging = false;
            mouse->deadline = 0;
            if (config->primary_selection) {
                ClipboardChannel *primary = &input->clipboard->primary;
                if (terminal->selection.active) clipboard_copy(primary, input->platform, terminal, serial);
                else clipboard_clear_primary(primary, input->platform, serial);
            }
        }
        return;
    }
    if (button == BTN_LEFT && modifiers == config->link_modifier) {
        const char *url = link_url(terminal, mouse_link(input));
        if (url != NULL) {
            mouse->clicks = 0;
            link_open(input, url);
            return;
        }
    }
    if (mouse_reporting(input, modifiers)) {
        unsigned int column, row;
        if (mouse_cell(input, &column, &row) &&
            mouse_send(input, mouse_report_code(code, modifiers), column, row, false)) {
            mouse->buttons |= bit;
            mouse->column = column;
            mouse->row = row;
        }
        mouse->clicks = 0;
        return;
    }
    if (button == BTN_MIDDLE && config->primary_selection) {
        clipboard_paste(&input->clipboard->primary, input->platform, terminal);
    } else if (button == BTN_LEFT) {
        SelectionPoint point = mouse_selection_point(input);
        if (mouse->last_alternate == terminal->using_alternate && point.line == mouse->last_point.line &&
            point.column == mouse->last_point.column &&
            (uint32_t)(time - mouse->last_click) <= (uint32_t)config->click_interval)
            mouse->clicks = mouse->clicks % 3 + 1;
        else mouse->clicks = 1;
        mouse->last_point = point;
        mouse->last_alternate = terminal->using_alternate;
        mouse->last_click = time;
        mouse->deadline = 0;
        SelectionMode mode = mouse->clicks == 3 ? SELECT_LINE : mouse->clicks == 2 ? SELECT_WORD : SELECT_CHARACTER;
        selection_start(terminal, point, mode);
        *input->redraw = true;
    }
}

static void pointer_scroll(void *data, uint32_t axis, int steps, unsigned int modifiers)
{
    Interaction *input = data;
    bool vertical = axis == WL_POINTER_AXIS_VERTICAL_SCROLL;
    if (modifiers & MOD_CTRL) {
        if (vertical) input_zoom(input, -(int64_t)steps);
        return;
    }
    bool up = steps < 0;
    size_t count = (size_t)(up ? -steps : steps);
    if (mouse_reporting(input, modifiers)) {
        unsigned int column, row;
        if (!mouse_cell(input, &column, &row)) return;
        unsigned int code = mouse_report_code((vertical ? 64u : 66u) + !up, modifiers);
        for (size_t i = 0; i < count; ++i) {
            if (!mouse_send(input, code, column, row, false)) break;
        }
    } else if (vertical) {
        size_t lines = (size_t)input->config->scroll_lines;
        if (history_scroll(input->terminal, up, minimum(count, SIZE_MAX / lines) * lines)) {
            if (input->terminal->selection.dragging) mouse_extend(input);
            *input->redraw = true;
        }
    }
}

static int mouse_tick(Interaction *input, int64_t now)
{
    Mouse *mouse = &input->mouse;
    Terminal *terminal = input->terminal;
    const FontMetrics *metrics = &input->renderer->metrics;
    int top = input_padding(input).vertical;
    int bottom = top + (int)terminal->primary.screen.rows * metrics->height;
    double y = input->platform->pointer_y - input->bar;
    bool up = y < top && terminal->history.offset < terminal->history.count;
    bool down = y >= bottom && terminal->history.offset > 0;
    if (!terminal->selection.dragging || terminal->using_alternate || (!up && !down) || now < 0) {
        mouse->deadline = 0;
        return -1;
    }
    if (mouse->deadline == 0) mouse->deadline = now + 55;
    if (now >= mouse->deadline) {
        double distance = up ? top - y : y - bottom;
        size_t lines = 1 + minimum((size_t)(distance / metrics->height), 7);
        if (history_scroll(terminal, up, lines)) {
            mouse_extend(input);
            *input->redraw = true;
        }
        mouse->deadline = now + 55;
    }
    return deadline_timeout(now, mouse->deadline);
}

typedef struct {
    int64_t next, flash_end, urgent_next;
} Bell;

static int bell_tick(Bell *bell, Terminal *terminal, const Config *config, Platform *platform,
                     int64_t now, bool *redraw)
{
    /* Limit requests so a flood of BEL characters makes one request a second. */
    if (terminal->bell_pending && config->bell_urgency && !platform->focused && now >= 0 &&
        now >= bell->urgent_next) {
        bell->urgent_next = now + 1000;
        platform_request_attention(platform);
    }
    if (bell->flash_end && (now < 0 || now >= bell->flash_end || !config->visual_bell)) {
        bell->flash_end = 0;
        *redraw = true;
    }
    if (terminal->bell_pending && config->visual_bell && now >= 0 && now >= bell->next) {
        bell->next = now + 250;
        bell->flash_end = now + 100;
        *redraw = true;
    }
    terminal->bell_pending = false;
    return deadline_timeout(now, bell->flash_end);
}

static void render_bell(int width, int height, TerminalColor color)
{
    render_color(color);
    glBegin(GL_QUADS);
    render_rectangle(0, 0, width, 1);
    render_rectangle(0, height - 1, width, 1);
    render_rectangle(0, 0, 1, height);
    render_rectangle(width - 1, 0, 1, height);
    glEnd();
}

typedef struct {
    int64_t deadline;
    bool visible;
} CursorBlink;

static int cursor_timeout(CursorBlink *blink, bool active, int interval, int64_t now, bool *redraw)
{
    if (!active || now < 0) {
        blink->deadline = 0;
        blink->visible = true;
        return -1;
    }
    if (*redraw || blink->deadline == 0) {
        blink->visible = true;
        blink->deadline = now + interval;
    } else if (now >= blink->deadline) {
        blink->visible = !blink->visible;
        blink->deadline = now + interval;
        *redraw = true;
    }
    return (int)(blink->deadline - now);
}

static bool terminal_resize_window(Terminal *terminal, int width, int height, Padding padding,
                                   const FontMetrics *metrics)
{
    size_t columns = (size_t)(grid_extent(width, padding.horizontal) / metrics->width);
    size_t rows = (size_t)(grid_extent(height, padding.vertical) / metrics->height);
    return terminal_resize(terminal, columns > 0 ? columns : 1, rows > 0 ? rows : 1);
}

static bool font_zoom(FontRenderer *renderer, Terminal *terminal, const Config *config,
                      Platform *platform, int size)
{
    Config zoomed = *config;
    zoomed.font_size = size;
    FontRenderer next = {0};
    const char *error = font_open(&next, &zoomed, renderer->config);
    if (error == NULL) error = font_make_atlas(&next);
    if (error == NULL && !terminal_resize_window(terminal, platform->width, platform->height,
                                                 config->padding, &next.metrics)) {
        error = "could not resize the screen buffers";
    }
    if (error != NULL) {
        font_close(&next);
        fprintf(stderr, "deal: could not change font size to %d: %s\n", size, error);
        return false;
    }
    font_close(renderer);
    *renderer = next;
    return true;
}

static void history_replace(Terminal *terminal, HistoryLine *lines, size_t capacity)
{
    History *history = &terminal->history;
    size_t kept = minimum(history->count, capacity);
    size_t discarded = history->count - kept;
    history->stored_cells = 0;
    for (size_t i = 0; i < history->count; ++i) {
        HistoryLine *line = &history->lines[(history->head + i) % history->capacity];
        if (i < discarded) free(line->cells);
        else {
            lines[i - discarded] = *line;
            history->stored_cells += line->stored;
        }
    }
    free(history->lines);
    history->lines = lines;
    history->capacity = capacity;
    history->head = 0;
    history->count = kept;
    history->offset = minimum(history->offset, kept);
    if (!terminal->selection.alternate &&
        terminal->selection.range.first.line < history->first_live - kept) terminal->selection = (Selection){0};
    terminal->links_dirty |= terminal->links != NULL;
}

static const char *font_family(const Config *config)
{
    return config->font_family != NULL ? config->font_family : default_font_family;
}

static bool config_reload(Config *config, Terminal *terminal, FontRenderer *renderer,
                          const Platform *platform)
{
    Config next = config_defaults();
    next.options = config->options;
    FontRenderer font = {0};
    HistoryLine *lines = NULL;
    const char *error = NULL;
    if (!config_read(&next, false)) {
        error = "invalid or unreadable configuration";
        goto failed;
    }

    config_overrides(&next);
    bool themed = config_theme(&next);
    if (!themed && next.theme_wallpaper && config->theme_loaded) {
        config_keep_theme(&next, config);
        themed = true;
    }
    next.theme_loaded = next.theme_wallpaper && themed;
    size_t capacity = (size_t)next.history_capacity;
    bool change_history = capacity != terminal->history.capacity;
    if (change_history && capacity != 0) {
        lines = calloc(capacity, sizeof(*lines));
        if (lines == NULL) {
            error = "could not allocate the scrollback ring";
            goto failed;
        }
    }
    int size = next.font_size == config->font_size ? config_zoom_size(&next, renderer->size)
                                                  : next.font_size;
    bool change_font = size != renderer->size || strcmp(font_family(&next), font_family(config)) != 0 ||
                       next.letter_spacing != config->letter_spacing || next.line_spacing != config->line_spacing;
    if (change_font) {
        Config sized = next;
        sized.font_size = size;
        error = font_open(&font, &sized, NULL);
        if (error == NULL) error = font_make_atlas(&font);
        if (error != NULL) goto failed;
    }
    const FontMetrics *metrics = change_font ? &font.metrics : &renderer->metrics;
    if (!terminal_resize_window(terminal, platform->width, platform->height, next.padding, metrics)) {
        error = "could not resize the screen buffers";
        goto failed;
    }

    if (change_history) history_replace(terminal, lines, capacity);
    if (change_font) {
        font_close(renderer);
        *renderer = font;
    }
    terminal_configure(terminal, &next);
    config_destroy(config);
    *config = next;
    return true;

failed:
    config_destroy(&next);
    font_close(&font);
    free(lines);
    fprintf(stderr, "deal: configuration reload failed: %s\n", error);
    return false;
}

static const char *platform_poll(Platform *platform, struct pollfd *events, nfds_t count, int timeout)
{
    struct wl_display *display = platform->display;
    /* EGL can queue Wayland events while swapping. Dispatch them before polling again. */
    if (wl_display_prepare_read(display) < 0)
        return errno == EAGAIN ? NULL : "could not prepare to read Wayland events";

    if (wl_display_flush(display) < 0) {
        if (errno != EAGAIN) {
            wl_display_cancel_read(display);
            return "could not write to the Wayland display";
        }
        events[0].events |= POLLOUT;
    }

    int ready = poll(events, count, timeout);
    int poll_error = errno;
    if (ready > 0 && (events[0].revents & POLLIN)) {
        if (wl_display_read_events(display) < 0) return "could not read from the Wayland display";
    } else {
        wl_display_cancel_read(display);
    }
    if (ready < 0 && poll_error != EINTR) return "could not wait for Wayland and PTY activity";
    if (events[0].revents & (POLLHUP | POLLERR | POLLNVAL)) return "lost the Wayland connection";
    return NULL;
}

/* Tabs. Each owns a terminal and its shell. The first tab uses main's
   terminal and session, which main releases. Others are allocated here. */
enum { TAB_LIMIT = 16 };

typedef struct {
    Terminal *terminal;
    Session *session;
    bool owned, hangup, closed, bell;
    int64_t drain_deadline;
} Tab;

typedef struct {
    Tab tabs[TAB_LIMIT];
    size_t count, active;
} Tabs;

/* Release a terminal's memory. Safe to repeat. */
static void terminal_free(Terminal *terminal)
{
    links_clear(terminal);
    free(terminal->osc52);
    terminal->osc52 = NULL;
    terminal->osc52_length = terminal->osc52_capacity = 0;
    free(terminal->clipboard_write);
    terminal->clipboard_write = NULL;
    history_clear(&terminal->history);
    free(terminal->history.lines);
    terminal->history.lines = NULL;
    terminal->history.capacity = 0;
    terminal_screen_destroy(&terminal->primary.screen);
    terminal_screen_destroy(&terminal->alternate.screen);
}

static void tab_remove(Tabs *tabs, size_t index)
{
    Tab *tab = &tabs->tabs[index];
    session_end(tab->session);
    terminal_free(tab->terminal);
    if (tab->owned) {
        free(tab->terminal);
        free(tab->session);
    }
    memmove(tab, tab + 1, (tabs->count - index - 1) * sizeof *tab);
    --tabs->count;
    if (tabs->active > index || (tabs->active == tabs->count && tabs->active > 0)) --tabs->active;
}

/* Open a shell in the active tab's directory, after the active tab. */
static const char *tab_open(Tabs *tabs, const Interaction *input, const Config *config,
                            const FontRenderer *renderer, int display_fd, int grid_height)
{
    if (tabs->count == TAB_LIMIT) return "the window already has the maximum number of tabs";
    char directory[PATH_MAX];
    const char *cwd = shell_directory(input, directory);
    Terminal *terminal = malloc(sizeof *terminal);
    Session *session = malloc(sizeof *session);
    if (terminal == NULL || session == NULL) {
        free(terminal);
        free(session);
        return "could not allocate a tab";
    }
    const Platform *platform = input->platform;
    terminal_init(terminal, config->foreground, config->background, config->cursor,
                  config->cursor_style, config->blink);
    terminal_configure(terminal, config);
    *session = (Session){.master = -1, .signal_fd = input->session->signal_fd, .startup = -1,
                         .launch_gate = -1, .original_signal_mask = input->session->original_signal_mask,
                         .original_sighup = input->session->original_sighup};
    const char *error = NULL;
    if (config->history_capacity > 0) {
        terminal->history.lines = calloc((size_t)config->history_capacity, sizeof *terminal->history.lines);
        if (terminal->history.lines == NULL) error = "could not allocate the scrollback ring";
        else terminal->history.capacity = (size_t)config->history_capacity;
    }
    if (error == NULL && !terminal_resize_window(terminal, platform->width, grid_height,
                                                 config->padding, &renderer->metrics))
        error = "could not allocate the screen buffers";
    /* A tab runs the shell, not the -e command, in the current directory. */
    Options options = *config->options;
    options.command = NULL;
    options.directory = cwd;
    Config tab_config = *config;
    tab_config.options = &options;
    if (error == NULL) {
        error = session_open(session, session_size(&terminal->primary.screen, platform->width,
                                                   grid_height, config->padding),
                             display_fd, &tab_config);
    }
    if (error == NULL) error = session_launch(session);
    if (error != NULL) {
        session_end(session);
        terminal_free(terminal);
        free(terminal);
        free(session);
        return error;
    }
    terminal->reply = session_reply;
    terminal->reply_context = session;
    size_t position = tabs->active + 1;
    memmove(&tabs->tabs[position + 1], &tabs->tabs[position], (tabs->count - position) * sizeof *tabs->tabs);
    tabs->tabs[position] = (Tab){.terminal = terminal, .session = session, .owned = true};
    ++tabs->count;
    tabs->active = position;
    return NULL;
}

/* Apply a reloaded configuration to a tab other than the active one, which
   config_reload already updated. */
static void tab_reconfigure(Terminal *terminal, const Config *config)
{
    size_t capacity = (size_t)config->history_capacity;
    if (capacity != terminal->history.capacity) {
        HistoryLine *lines = capacity != 0 ? calloc(capacity, sizeof *lines) : NULL;
        if (capacity == 0 || lines != NULL) history_replace(terminal, lines, capacity);
    }
    terminal_configure(terminal, config);
}

static int tab_bar_height(const FontRenderer *renderer, size_t count)
{
    return count > 1 ? renderer->metrics.height + 4 : 0;
}

/* Draw the tab bar in the top bar pixels: the active tab in the selection
   colors, a tab that rang its bell in the cursor color, others in the
   foreground color. */
static void render_tabs(FontRenderer *renderer, const Tabs *tabs, const Config *config,
                        int width, int height, int bar)
{
    glViewport(0, height - bar, width, bar);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, bar, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    const FontMetrics *metrics = &renderer->metrics;
    glBegin(GL_QUADS);
    for (size_t i = 0; i < tabs->count; ++i) {
        int left = tab_edge(width, tabs->count, i), right = tab_edge(width, tabs->count, i + 1);
        if (i == tabs->active) {
            render_color(config->selection_background);
            render_rectangle(left, 0, right - left, bar);
        } else if (i + 1 != tabs->active && i + 1 != tabs->count) {
            render_color(config->palette[8]);
            render_rectangle(right - 1, bar / 4, 1, bar / 2);
        }
    }
    render_color(config->palette[8]);
    render_rectangle(0, bar - 1, width, 1);
    glEnd();

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, renderer->atlas);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    int text_y = (bar - metrics->height) / 2;
    for (size_t i = 0; i < tabs->count; ++i) {
        const Tab *tab = &tabs->tabs[i];
        const char *title = tab->terminal->dynamic_title[0] != 0 ? tab->terminal->dynamic_title
                            : config->title != NULL ? config->title : default_title;
        char label[TITLE_LIMIT + 8];
        snprintf(label, sizeof label, "%zu %s", i + 1, title);
        TerminalColor color = i == tabs->active ? config->selection_foreground
                              : tab->bell ? config->cursor : config->foreground;
        int left = tab_edge(width, tabs->count, i), right = tab_edge(width, tabs->count, i + 1);
        /* The title stops a cell before the close button. */
        int x = left + metrics->width, limit = right - 3 * metrics->width;
        if (right - left >= 4 * metrics->width) {
            TerminalColor button = i == tabs->active ? config->selection_foreground : config->palette[8];
            render_glyph(renderer, 0x00d7, 1, 0, false, button, right - 2 * metrics->width, text_y);
        }
        /* Decode the UTF-8 label and stop with an ellipsis when it doesn't fit. */
        for (const unsigned char *p = (const unsigned char *)label; *p != 0;) {
            uint32_t cp = *p++;
            int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
            if (extra) cp &= 0x3fu >> extra;
            while (extra-- > 0 && (*p & 0xc0) == 0x80) cp = (cp << 6) | (*p++ & 0x3f);
            if (x + 2 * metrics->width > limit && *p != 0) {
                render_glyph(renderer, 0x2026, 1, 0, false, color, x, text_y);
                break;
            }
            if (x + metrics->width > limit) break;
            if (cp != ' ') render_glyph(renderer, cp, 1, 0, false, color, x, text_y);
            x += metrics->width;
        }
    }
    glyph_flush(renderer);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
}

/* Point input at the active tab. Return true when it changed. */
static bool tab_select(Tabs *tabs, Interaction *input, Clipboard *clipboard, bool focused)
{
    Tab *tab = &tabs->tabs[tabs->active];
    input->tab_count = tabs->count;
    if (input->terminal == tab->terminal) return false;
    input->terminal = tab->terminal;
    input->session = tab->session;
    tab->bell = false;
    tab->terminal->title_dirty = true;
    ++tab->terminal->mouse_generation;
    /* A paste in progress belongs to the tab it was started in. */
    clipboard_drop_receive(&clipboard->receive);
    if (focused && tab->terminal->focus_reporting && !tab->closed) terminal_reply(tab->terminal, "\033[I");
    return true;
}

static const char *run_loop(Platform *platform, Tabs *tabs, Config *config, FontRenderer *renderer,
                            Clipboard *clipboard, char *zoom_path, bool *save_zoom, DeskWatch *theme)
{
    int display_fd = wl_display_get_fd(platform->display);
    int width = platform->width, height = platform->height;
    Terminal *terminal = tabs->tabs[0].terminal;
    Session *session = tabs->tabs[0].session;
    terminal->reply = session_reply;
    terminal->reply_context = session;

    bool redraw = true;
    bool focused = false;
    int64_t link_deadline = 0, zoom_deadline = 0, sync_since = 0;
    int bar = 0;
    CursorBlink blink = {0};
    Bell bell = {0};
    Interaction input = {.platform = platform, .terminal = terminal, .session = session,
                         .config = config, .renderer = renderer, .clipboard = clipboard,
                         .font_size = renderer->size, .redraw = &redraw, .tab_count = tabs->count};
    platform->key_pressed = keyboard_event;
    platform->scrolled = pointer_scroll;
    platform->pointer_button_event = pointer_button_event;
    platform->pointer_moved = pointer_moved;
    platform->pointer_left = pointer_left;
    platform->input_data = &input;

    if (!clipboard_sync(clipboard, platform, config)) return "could not initialize Wayland selections";
    for (;;) {
        if (wl_display_dispatch_pending(platform->display) < 0) return "lost the Wayland connection";
        if (platform->error != NULL || platform->closed) return platform->error;
        redraw |= platform->needs_redraw;
        platform->needs_redraw = false;
        Tab *tab = &tabs->tabs[tabs->active];
        if (focused != platform->focused) {
            focused = platform->focused;
            redraw = true;
            if (terminal->focus_reporting && !tab->closed)
                terminal_reply(terminal, focused ? "\033[I" : "\033[O");
        }

        if (input.tab_action != TAB_NONE) {
            int action = input.tab_action;
            input.tab_action = TAB_NONE;
            /* Tell the program in a tab it loses focus only when another tab
               becomes active. tab_select reports focus to the new one. */
            bool switching = action == TAB_NEW || ((action == TAB_NEXT || action == TAB_PREVIOUS) && tabs->count > 1) ||
                             (action == TAB_GOTO && input.tab_target < tabs->count && input.tab_target != tabs->active);
            if (switching && focused && terminal->focus_reporting && !tab->closed)
                terminal_reply(terminal, "\033[O");
            if (action == TAB_NEW) {
                const char *error = tab_open(tabs, &input, config, renderer, display_fd,
                                             height - tab_bar_height(renderer, tabs->count + 1));
                if (error != NULL) fprintf(stderr, "deal: could not open a tab: %s\n", error);
            } else if (action == TAB_CLOSE) {
                tab_remove(tabs, tabs->active);
                if (tabs->count == 0) return NULL;
            } else if (action == TAB_NEXT) {
                tabs->active = (tabs->active + 1) % tabs->count;
            } else if (action == TAB_PREVIOUS) {
                tabs->active = (tabs->active + tabs->count - 1) % tabs->count;
            } else if (action == TAB_GOTO && input.tab_target < tabs->count) {
                tabs->active = input.tab_target;
            } else if (action == TAB_CLOSE_AT && input.tab_target < tabs->count) {
                tab_remove(tabs, input.tab_target);
                if (tabs->count == 0) return NULL;
            }
        }
        if (tab_select(tabs, &input, clipboard, focused)) {
            redraw = true;
            sync_since = 0;
        }
        terminal = input.terminal;
        session = input.session;
        tab = &tabs->tabs[tabs->active];

        bool reloaded = false, zoomed = false;
        if (input.reload) {
            input.reload = false;
            int previous_default = config->font_size;
            reloaded = config_reload(config, terminal, renderer, platform);
            if (reloaded) {
                input.font_size = renderer->size;
                if (previous_default != config->font_size) *save_zoom = false;
                xdg_toplevel_set_app_id(platform->toplevel, platform_app_id(config));
            }
        }
        if (!clipboard_sync(clipboard, platform, config)) return "could not initialize Wayland selections";
        if (!tab->closed) platform_repeat_key(platform, monotonic_milliseconds());
        if (input.font_size != renderer->size) {
            zoomed = font_zoom(renderer, terminal, config, platform, input.font_size);
            input.font_size = renderer->size;
            if (zoomed && zoom_path != NULL) {
                *save_zoom = true;
                zoom_deadline = monotonic_milliseconds() + 250;
            } else if (!zoomed) {
                platform->repeat.key = 0;
            }
        }
        if (reloaded) platform_size_hints(platform, config->padding, &renderer->metrics);
        bool resized = width != platform->width || height != platform->height;
        if (resized) {
            width = platform->width;
            height = platform->height;
            wl_egl_window_resize(platform->window, width, height, 0, 0);
        }
        /* The bar appears with the second tab and takes rows from every grid. */
        int previous_bar = bar;
        bar = tab_bar_height(renderer, tabs->count);
        input.bar = bar;
        if (resized || reloaded || zoomed || bar != previous_bar) {
            redraw = true;
            for (size_t i = 0; i < tabs->count; ++i) {
                Tab *other = &tabs->tabs[i];
                if (reloaded && i != tabs->active) tab_reconfigure(other->terminal, config);
                if (!terminal_resize_window(other->terminal, width, height - bar, config->padding, &renderer->metrics))
                    return "could not resize the screen buffers";
                if (other->closed) continue;
                PtyResult result = session_resize(other->session, session_size(&other->terminal->primary.screen,
                                                                              width, height - bar, config->padding));
                if (result == PTY_FAILED) return "could not resize the pseudoterminal";
                if (result == PTY_CLOSED) other->hangup = true;
            }
        }

        if (terminal->title_dirty) {
            platform_title(platform, config, terminal->dynamic_title);
            terminal->title_dirty = false;
        }
        for (size_t i = 0; i < tabs->count; ++i) {
            Tab *other = &tabs->tabs[i];
            clipboard_remote_write(clipboard, platform, other->terminal);
            if (i == tabs->active) continue;
            if (other->terminal->bell_pending) {
                redraw |= !other->bell;
                other->bell = true;
                other->terminal->bell_pending = false;
            }
            if (other->terminal->title_dirty) {
                other->terminal->title_dirty = false;
                redraw = true;
            }
        }
        int64_t now = monotonic_milliseconds();
        int timeout = clipboard_timeout(clipboard, terminal, now);
        if (!terminal->synchronized) sync_since = 0;
        else if (sync_since == 0) sync_since = now > 0 ? now : 1;
        bool hold = sync_since != 0 && now >= 0 && now - sync_since < SYNC_TIMEOUT;
        if (hold && redraw) timeout = earlier_timeout(timeout, deadline_timeout(now, sync_since + SYNC_TIMEOUT));
        timeout = earlier_timeout(timeout, bell_tick(&bell, terminal, config, platform, now, &redraw));
        timeout = earlier_timeout(timeout, mouse_tick(&input, now));
        if (*save_zoom) {
            if (now < 0 || now >= zoom_deadline) {
                config_save_zoom(zoom_path, config->font_size, renderer->size);
                *save_zoom = false;
            } else timeout = earlier_timeout(timeout, deadline_timeout(now, zoom_deadline));
        }
        if (terminal->links_dirty) {
            if (link_deadline == 0) link_deadline = now + 500;
            if (now < 0 || now >= link_deadline) {
                links_collect(terminal);
                link_deadline = 0;
            } else timeout = earlier_timeout(timeout, deadline_timeout(now, link_deadline));
        }
        uint16_t hover = platform->pointer_inside && !terminal->selection.dragging ? mouse_link(&input) : 0;
        if (hover != terminal->hovered_link) {
            terminal->hovered_link = hover;
            redraw = true;
        }
        const TerminalBuffer *buffer = terminal_buffer(terminal);
        TerminalView view = terminal_view(terminal);
        timeout = earlier_timeout(timeout, cursor_timeout(&blink, focused && buffer->cursor_visible &&
                                    buffer->cursor_blink && view.first_visible == view.first_live,
                                    config->blink_interval, now, &redraw));
        /* Answer a new size at once, so the compositor's border doesn't run
           ahead of the content during an interactive resize. */
        if (redraw && !hold && (platform->frame == NULL || resized)) {
            render_screen(renderer, &view, width, height - bar, buffer->cursor.column, buffer->cursor.row,
                          buffer->cursor_visible && blink.visible, buffer->cursor_style, config,
                          config->opacity);
            if (bell.flash_end) render_bell(width, height - bar, config->cursor);
            if (bar > 0) render_tabs(renderer, tabs, config, width, height, bar);
            const char *error = platform_swap(platform);
            if (error != NULL) return error;
            redraw = false;
        }

        /* A tab closes once its shell has exited and its output is drained. */
        for (size_t i = 0; i < tabs->count;) {
            Tab *other = &tabs->tabs[i];
            bool finished = other->closed && other->session->child == 0;
            if (!finished && (other->closed || (other->session->child == 0 && !other->hangup))) {
                if (other->drain_deadline == 0) other->drain_deadline = now + 250;
                if (now < 0 || now >= other->drain_deadline) finished = true;
                else timeout = earlier_timeout(timeout, deadline_timeout(now, other->drain_deadline));
            }
            if (!finished) {
                ++i;
                continue;
            }
            const char *error = session_check_startup(other->session);
            if (tabs->count == 1) return error;
            if (error != NULL) fprintf(stderr, "deal: %s\n", error);
            tab_remove(tabs, i);
            timeout = 0;
        }
        if (tab_select(tabs, &input, clipboard, focused)) {
            redraw = true;
            sync_since = 0;
        }
        terminal = input.terminal;
        session = input.session;
        tab = &tabs->tabs[tabs->active];

        if (config->theme_wallpaper && theme->fd < 0) {
            char *path = desk_palette_path();
            desk_watch_open(theme, path);
            free(path);
        } else if (!config->theme_wallpaper && theme->fd >= 0) desk_watch_close(theme);

        enum { WAYLAND, SIGNAL, PASTE, THEME, SENDS };
        enum { PTYS = SENDS + CLIPBOARD_SENDS, STARTUPS = PTYS + TAB_LIMIT, EVENTS = STARTUPS + TAB_LIMIT };
        struct pollfd events[EVENTS] = {
            [WAYLAND] = {display_fd, POLLIN, 0},
            [SIGNAL] = {session->signal_fd, POLLIN, 0},
            [PASTE] = {clipboard->receive.fd, POLLIN, 0},
            [THEME] = {theme->fd, POLLIN, 0}
        };
        for (size_t i = 0; i < CLIPBOARD_SENDS; ++i)
            events[SENDS + i] = (struct pollfd){clipboard->send[i].fd, POLLOUT, 0};
        for (size_t i = 0; i < TAB_LIMIT; ++i) {
            const Tab *other = i < tabs->count ? &tabs->tabs[i] : NULL;
            events[PTYS + i] = (struct pollfd){other == NULL || other->closed ? -1 : other->session->master,
                (short)(POLLIN | (other != NULL && other->session->input_length ? POLLOUT : 0)), 0};
            events[STARTUPS + i] = (struct pollfd){other != NULL ? other->session->startup : -1, POLLIN, 0};
            if (other != NULL && !other->closed &&
                (other->session->output_start < other->session->output_end || other->hangup)) timeout = 0;
        }
        if (!tab->closed && platform->repeat.key != 0)
            timeout = earlier_timeout(timeout, deadline_timeout(monotonic_milliseconds(), platform->repeat.deadline));
        const char *error = platform_poll(platform, events, sizeof events / sizeof events[0], timeout);
        if (error != NULL) return error;

        for (size_t i = 0; i < tabs->count; ++i) {
            if (!events[STARTUPS + i].revents) continue;
            error = session_check_startup(tabs->tabs[i].session);
            if (error == NULL) continue;
            if (tabs->count == 1) return error;
            fprintf(stderr, "deal: %s\n", error);
            tabs->tabs[i].closed = true;
        }
        /* A new palette goes through the reload path, which recolors live. */
        if (desk_watch_changed(theme, events[THEME].revents)) input.reload = true;
        if (events[SIGNAL].revents & (POLLHUP | POLLERR | POLLNVAL))
            return "lost the signal notification descriptor";
        if (events[SIGNAL].revents & POLLIN) {
            struct signalfd_siginfo signals[8];
            ssize_t count = read(session->signal_fd, signals, sizeof signals);
            if (count < 0 && errno != EINTR && errno != EAGAIN)
                return "could not read signal notifications";
            for (ssize_t i = 0; i < count / (ssize_t)sizeof *signals; ++i) {
                if (signals[i].ssi_signo == SIGHUP) input.reload = true;
                if (signals[i].ssi_signo == SIGCHLD) {
                    for (size_t j = 0; j < tabs->count; ++j) session_reap(tabs->tabs[j].session, WNOHANG);
                    link_reap(&input);
                }
            }
        }
        for (size_t i = 0; i < tabs->count; ++i) {
            Tab *other = &tabs->tabs[i];
            short revents = events[PTYS + i].revents;
            if (revents & POLLNVAL) return "lost the pseudoterminal descriptor";
            other->hangup |= (revents & (POLLHUP | POLLERR)) != 0;
            bool pending = other->session->output_start < other->session->output_end;
            if (!other->closed && ((revents & POLLIN) || pending || other->hangup)) {
                bool background = false;
                PtyResult result = session_read(other->session, other->terminal,
                                                i == tabs->active ? &redraw : &background, other->hangup);
                if (result == PTY_FAILED) return "could not read from the pseudoterminal";
                other->closed = result == PTY_CLOSED;
            }
            if (!other->closed && (revents & POLLOUT)) {
                PtyResult result = session_write(other->session);
                if (result == PTY_FAILED) return "could not write to the pseudoterminal";
                if (result == PTY_CLOSED) {
                    other->session->input_length = 0;
                    other->hangup = true;
                }
            }
        }
        if (tab->closed) clipboard_drop_receive(&clipboard->receive);
        else clipboard_read(&clipboard->receive, session, terminal, config, &redraw, events[PASTE].revents);
        for (size_t i = 0; i < CLIPBOARD_SENDS; ++i)
            clipboard_write(&clipboard->send[i], events[SENDS + i].revents);
    }
}

static void print_help(void)
{
    puts("Usage: deal [OPTIONS] [-e COMMAND [ARGS...]]\n"
         "  --help                    Show this help\n"
         "  --version                 Show the version\n"
         "  --config PATH             Read a different INI configuration\n"
         "  --working-directory PATH  Start the child in PATH\n"
         "  --title TITLE             Set the initial window title\n"
         "  --app-id ID               Set the Wayland application ID\n"
         "  --class ID                Alias for --app-id\n"
         "  --columns N               Set the initial grid width\n"
         "  --rows N                  Set the initial grid height\n"
         "  --diagnostics             Report Wayland, EGL, OpenGL and configuration\n"
         "  -e COMMAND [ARGS...]      Execute a program with the remaining arguments");
}

static int option_dimension(const char *text, int limit)
{
    int value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return 0;
        value = value * 10 + (*text - '0');
        if (value > limit) return 0;
    }
    return value;
}

/* A negative result means help or version was printed. */
static int options_parse(Options *options, int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (strcmp(key, "--help") == 0) { print_help(); return -1; }
        if (strcmp(key, "--version") == 0) { printf("deal %s\n", deal_version); return -1; }
        if (strcmp(key, "--diagnostics") == 0) { options->diagnostics = true; continue; }
        if (strcmp(key, "-e") == 0 && i + 1 < argc && *argv[i + 1]) {
            options->command = argv + i + 1;
            return 0;
        }
        const char *value = i + 1 < argc ? argv[i + 1] : "";
        if (!*value) {
            fprintf(stderr, "deal: invalid option or missing value for %s\n", key);
            return 2;
        }
        if (strcmp(key, "--config") == 0) options->config_path = value;
        else if (strcmp(key, "--working-directory") == 0) options->directory = value;
        else if (strcmp(key, "--title") == 0 || strcmp(key, "--app-id") == 0 || strcmp(key, "--class") == 0) {
            size_t length = strlen(value);
            if (length > TITLE_LIMIT || !osc_text_valid(value, length, false)) {
                fprintf(stderr, "deal: invalid value for %s\n", key);
                return 2;
            }
            if (strcmp(key, "--title") == 0) options->title = value;
            else options->app_id = value;
        } else if (strcmp(key, "--columns") == 0 || strcmp(key, "--rows") == 0) {
            bool columns = strcmp(key, "--columns") == 0;
            int limit = columns ? 512 : 256;
            int size = option_dimension(value, limit);
            if (size == 0) {
                fprintf(stderr, "deal: %s expects an integer from 1 to %d\n", key, limit);
                return 2;
            }
            if (columns) options->columns = size;
            else options->rows = size;
        } else {
            fprintf(stderr, "deal: unknown option %s (see --help)\n", key);
            return 2;
        }
        ++i;
    }
    return 0;
}

static const char *run(Platform *platform, Terminal *terminal, Session *session,
                       Config *config, FontRenderer *renderer, Clipboard *clipboard,
                       char *zoom_path, bool *save_zoom)
{
    DeskWatch theme = {.fd = -1};
    Tabs *tabs = calloc(1, sizeof *tabs);
    if (tabs == NULL) return "could not allocate the tab list";
    tabs->tabs[0] = (Tab){.terminal = terminal, .session = session};
    tabs->count = 1;
    const char *error = run_loop(platform, tabs, config, renderer, clipboard, zoom_path, save_zoom, &theme);
    /* main releases the first tab's terminal and session. */
    for (size_t i = tabs->count; i-- > 0;) {
        if (tabs->tabs[i].owned) tab_remove(tabs, i);
    }
    free(tabs);
    desk_watch_close(&theme);
    return error;
}

static void print_diagnostics(const Config *config, FontRenderer *renderer,
                              const Platform *platform, const char *graphical_error)
{
    char *default_path = config->options->config_path == NULL ? config_path("deal.conf", true) : NULL;
    const char *path = config->options->config_path != NULL ? config->options->config_path : default_path;
    char packaged[PATH_MAX];
    const char *term = config_term(config, packaged);
    printf("deal %s\nBuild platform: Linux Wayland (C11, %zu-bit)\nConfiguration (INI): %s\nTERM: %s%s\n",
           deal_version, sizeof(void *) * CHAR_BIT, path != NULL ? path : "unavailable", term,
           config->custom_term && strcmp(term, "deal") != 0 ? " (deal terminfo unavailable)" : "");
    free(default_path);
    printf("Wayland: %s\nxdg-shell: %s\nApplication ID: %s\n",
           platform->display != NULL ? "connected" : "unavailable",
           platform->configured ? "configured" : "unavailable", platform_app_id(config));
    if (config->theme_wallpaper) {
        char *palette_path = desk_palette_path();
        DeskPalette palette;
        int palette_error = desk_palette_load(palette_path, &palette);
        printf("Theme: wallpaper (%s%s%s)\n", palette_path != NULL ? palette_path : "no cache directory",
               palette_error == 0 ? "" : ", unavailable: ",
               palette_error == 0 ? "" : palette_error == EINVAL ? "incomplete palette" : strerror(palette_error));
        free(palette_path);
    } else puts("Theme: manual");
    printf("Compose: %s\nUrgency (xdg-activation): %s\n",
           platform->compose_locale != NULL ? platform->compose_locale : "unavailable",
           platform->activation == NULL ? "unavailable" : config->bell_urgency ? "enabled" : "disabled");
    printf("Clipboard protocol: %s\nPrimary selection: %s\n",
           platform->clipboard_manager != NULL ? "available" : "unavailable",
           platform->primary_manager == NULL ? "unavailable" : config->primary_selection ? "enabled" : "disabled");
    if (platform->egl_initialized) {
        const char *vendor = eglQueryString(platform->egl, EGL_VENDOR);
        printf("EGL: %d.%d (%s)\n", platform->egl_major, platform->egl_minor, vendor ? vendor : "unknown");
    } else puts("EGL: unavailable");
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        const char *vendor = (const char *)glGetString(GL_VENDOR);
        const char *name = (const char *)glGetString(GL_RENDERER);
        const char *version = (const char *)glGetString(GL_VERSION);
        bool software = name != NULL && (strcasestr(name, "llvmpipe") || strcasestr(name, "softpipe") ||
                                          strcasestr(name, "swrast") || strcasestr(name, "software"));
        printf("OpenGL vendor: %s\nOpenGL renderer: %s\nOpenGL version: %s\nRendering: %s\n",
               vendor ? vendor : "unknown", name ? name : "unknown", version ? version : "unknown",
               software ? "software" : name ? "no known software renderer detected" : "unknown");
    } else puts("OpenGL: unavailable");
    if (renderer->faces != NULL && renderer->metrics.width > 0) {
        FT_Face face = renderer->faces[renderer->primary].face;
        printf("Font: %s, %d px\nCell: %d × %d px\n",
               face != NULL && face->family_name != NULL ? face->family_name : font_family(config),
               renderer->size, renderer->metrics.width, renderer->metrics.height);
        static const char *const names[] = {NULL, "Bold", "Italic", "Bold italic"};
        for (unsigned int style = 1; style < 4; ++style) {
            const StyledFace *styled = font_styled(renderer, style);
            FT_Face styled_face = styled->font.face;
            const char *synthesis = styled->synthesis == 0 ? ""
                : styled->synthesis == TERMINAL_STYLE_BOLD ? " (synthetic bold)"
                : styled->synthesis == TERMINAL_STYLE_ITALIC ? " (synthetic italic)" : " (synthetic bold italic)";
            printf("%s face: %s%s%s%s\n", names[style],
                   styled_face != NULL && styled_face->family_name ? styled_face->family_name : "regular face",
                   styled_face != NULL && styled_face->style_name ? " " : "",
                   styled_face != NULL && styled_face->style_name ? styled_face->style_name : "", synthesis);
        }
    } else puts("Font: unavailable");
    if (graphical_error != NULL) printf("Startup probe: %s\n", graphical_error);
    puts("Features: desktop OpenGL, UTF-8, ANSI 256/RGB, alternate screen, scrollback reflow, "
         "Wayland keyboard repeat, keyboard and wheel zoom, selection, clipboard, mouse reports, "
         "focus reporting, OSC titles and links, bold and italic faces, underline styles and colors, "
         "strikethrough, overline, compose and dead keys, visual and urgency bells, OSC 52 copy, "
         "synchronized output, OSC 7 and new windows, wallpaper theme, INI reload");
}

int main(int argc, char **argv)
{
    Options options = {0};
    int parsed = options_parse(&options, argc, argv);
    if (parsed != 0) return parsed < 0 ? 0 : parsed;
    if (!options.diagnostics && options.directory != NULL) {
        struct stat directory;
        if (stat(options.directory, &directory) < 0 || !S_ISDIR(directory.st_mode)) {
            fprintf(stderr, "deal: --working-directory must name an accessible directory\n");
            return 2;
        }
    }
    if (setlocale(LC_CTYPE, "C.UTF-8") == NULL) setlocale(LC_CTYPE, "");
    Config config;
    if (!config_load(&config, &options)) return EXIT_FAILURE;
    char *zoom_path = options.config_path == NULL ? config_path("font-zoom", true) : NULL;
    bool save_zoom = false;
    Platform platform = {0};
    FontRenderer renderer = {0};
    Terminal terminal;
    terminal_init(&terminal, config.foreground, config.background, config.cursor,
                  config.cursor_style, config.blink);
    terminal_configure(&terminal, &config);
    terminal.title_dirty = false;
    Session session = {.master = -1, .signal_fd = -1, .startup = -1, .launch_gate = -1};
    Clipboard clipboard = {0};
    clipboard_init(&clipboard);
    Config initial_font = config;
    initial_font.font_size = config_read_zoom(zoom_path, &config);
    const char *error = options.diagnostics ? NULL : session_prepare_signals(&session);
    if (error == NULL) {
        error = font_open(&renderer, &initial_font, NULL);
        if (error != NULL && initial_font.font_size != config.font_size) {
            font_close(&renderer);
            renderer = (FontRenderer){0};
            error = font_open(&renderer, &config, NULL);
        }
    }
    if (error == font_spacing_error && options.config_path == NULL) {
        fprintf(stderr, "deal: %s; using default spacing\n", error);
        font_close(&renderer);
        renderer = (FontRenderer){0};
        config.letter_spacing = config.line_spacing = 0;
        error = font_open(&renderer, &config, NULL);
    }
    if (error == NULL) {
        error = platform_open(&platform, &config, &renderer.metrics);
    }
    if (options.diagnostics) {
        if (error == NULL && !platform.closed) error = platform_open_egl(&platform);
        print_diagnostics(&config, &renderer, &platform, error);
    } else if (error == NULL && !platform.closed) {
        if (config.history_capacity > 0) {
            terminal.history.lines = calloc((size_t)config.history_capacity, sizeof(*terminal.history.lines));
            if (terminal.history.lines == NULL) error = "could not allocate the scrollback ring";
            else terminal.history.capacity = (size_t)config.history_capacity;
        }
        if (error == NULL && !terminal_resize_window(&terminal, platform.width, platform.height,
                                                    config.padding, &renderer.metrics))
            error = "could not allocate the screen buffers";
        if (error == NULL) {
            struct winsize size = session_size(&terminal.primary.screen, platform.width, platform.height,
                                                config.padding);
            /* Fork before EGL starts driver threads and wait until the renderer is ready to exec. */
            error = session_open(&session, size, wl_display_get_fd(platform.display), &config);
        }
        if (error == NULL) error = platform_open_egl(&platform);
        if (error == NULL) error = font_make_atlas(&renderer);
        if (error == NULL) error = session_launch(&session);
        if (error == NULL) {
            error = run(&platform, &terminal, &session, &config, &renderer, &clipboard, zoom_path, &save_zoom);
            platform.key_pressed = NULL;
            platform.scrolled = NULL;
            platform.pointer_button_event = NULL;
            platform.pointer_moved = NULL;
            platform.pointer_left = NULL;
            platform.input_data = NULL;
        }
    }

    if (save_zoom) config_save_zoom(zoom_path, config.font_size, renderer.size);
    clipboard_close(&clipboard);
    session_close(&session);
    terminal_free(&terminal);
    font_close(&renderer);
    platform_close(&platform);
    config_destroy(&config);
    free(zoom_path);
    if (options.diagnostics) return EXIT_SUCCESS;
    if (error != NULL) {
        fprintf(stderr, "deal: %s\n", error);
        return EXIT_FAILURE;
    }
    if (options.command != NULL && session.child_status_valid) {
        if (WIFEXITED(session.child_status)) return WEXITSTATUS(session.child_status);
        if (WIFSIGNALED(session.child_status)) return 128 + WTERMSIG(session.child_status);
    }
    return EXIT_SUCCESS;
}
