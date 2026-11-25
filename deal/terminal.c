#define _GNU_SOURCE
#include "terminal.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#if LUA_VERSION_NUM != 504
#error deal requires Lua 5.4
#endif

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
static const char deal_version[] = "0.9.0";
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
enum { SOURCE_FG = 1, SOURCE_BG, SOURCE_ANSI, SOURCE_DIM = 128 };
enum { TITLE_LIMIT = 512, LINK_LIMIT = 2048, LINK_SLOTS = 512, OSC_LIMIT = 2304 };

/* Startup configuration */

typedef enum { CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BAR } CursorStyle;
static const char *const cursor_style_names[] = {"block", "underline", "bar"};
static const char default_zoom_in[] = "Ctrl+Shift+=";
static const char default_zoom_out[] = "Ctrl+Shift+-";
static const char default_reload[] = "Ctrl+Shift+R";
static const char default_fullscreen[] = "F11";

typedef struct {
    KeySym key;
    unsigned int modifiers;
} Shortcut;

static KeySym shortcut_key(KeySym key)
{
    if (key >= XK_A && key <= XK_Z) return XK_a + (key - XK_A);
    if (key == XK_plus) return XK_equal;
    if (key == XK_underscore) return XK_minus;
    return key;
}

static unsigned int modifier_parse(const char *text, size_t length)
{
    if (length == 4 && memcmp(text, "Ctrl", 4) == 0) return ControlMask;
    if (length == 5 && memcmp(text, "Shift", 5) == 0) return ShiftMask;
    if (length == 3 && memcmp(text, "Alt", 3) == 0) return Mod1Mask;
    return 0;
}

static bool shortcut_parse(const char *text, Shortcut *shortcut, bool allow_bare)
{
    unsigned int modifiers = 0;
    const char *part;
    while ((part = strchr(text, '+')) != NULL) {
        unsigned int modifier = modifier_parse(text, (size_t)(part - text));
        if (modifier == 0 || (modifiers & modifier)) return false;
        modifiers |= modifier;
        text = part + 1;
    }
    if ((!allow_bare && modifiers == 0) || *text == '\0') return false;
    unsigned char first = (unsigned char)*text;
    KeySym key = first > ' ' && first <= '~' && text[1] == '\0' ? first : XStringToKeysym(text);
    if (key == NoSymbol) return false;
    *shortcut = (Shortcut){shortcut_key(key), modifiers};
    return true;
}

typedef struct {
    int horizontal, vertical;
} Padding;

typedef struct {
    const char *config_path, *directory, *title, *window_class;
    char **command;
    int columns, rows;
    bool diagnostics;
} Options;

typedef struct {
    char *title;
    char *shell;
    char *font_family;
    const Options *options;
    bool custom_term, audible_bell, visual_bell, bell_urgency;
    int font_size, letter_spacing, line_spacing;
    Padding padding;
    int columns, rows;
    int history_capacity;
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
    bool scroll_on_input;
    Shortcut zoom_in;
    Shortcut zoom_out, reload, fullscreen;
    bool dynamic_title, mouse_reporting, hyperlinks, invalid;
} Config;

static Config config_defaults(void)
{
    Config defaults = {.padding = {DEFAULT_PADDING, DEFAULT_PADDING}, .font_size = DEFAULT_FONT_SIZE,
                    .columns = INITIAL_COLUMNS, .rows = INITIAL_ROWS, .decorations = true, .opacity = 1.0f,
                    .blink_interval = DEFAULT_BLINK_INTERVAL, .cursor_thickness = DEFAULT_CURSOR_THICKNESS,
                    .history_capacity = 10000, .scroll_on_input = true,
                    .foreground = blank_cell.foreground,
                    .background = blank_cell.background, .cursor = blank_cell.foreground,
                    .cursor_style = CURSOR_UNDERLINE, .selection_foreground = {16, 22, 30},
                    .selection_background = {138, 193, 222},
                    .dynamic_title = true, .mouse_reporting = true, .hyperlinks = true,
                    .mouse_override = ShiftMask, .link_modifier = ControlMask};
    memcpy(defaults.palette, ansi_colors, sizeof defaults.palette);
    shortcut_parse(default_zoom_in, &defaults.zoom_in, false);
    shortcut_parse(default_zoom_out, &defaults.zoom_out, false);
    shortcut_parse(default_reload, &defaults.reload, false);
    shortcut_parse(default_fullscreen, &defaults.fullscreen, true);
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

static unsigned int config_rgb(TerminalColor color)
{
    return ((unsigned int)color.red << 16) | ((unsigned int)color.green << 8) | color.blue;
}

static int config_write_template(FILE *file, const void *data)
{
    const Config *defaults = data;
    return fprintf(file,
        "-- deal configuration\n"
        "-- Shortcuts are active. Commented entries show the built-in defaults.\n"
        "-- Uncomment an entry to customize it.\n"
        "return {\n"
        "    shortcuts = {\n"
        "        zoom_in = \"%s\",\n"
        "        zoom_out = \"%s\",\n"
        "    },\n"
        "    keybinds = {\n"
        "        reload_config = \"%s\",\n"
        "        fullscreen = \"%s\", -- Set to false to disable.\n"
        "    },\n"
        "    window = {\n"
        "        -- dynamic_title = true,\n"
        "        -- title = \"%s\",\n"
        "        -- padding = %d, -- Or { horizontal = %d, vertical = %d }.\n"
        "        -- opacity = 1.0, -- Default background opacity with an X11 compositor.\n"
        "        -- decorations = true,\n"
        "        -- columns = %d, -- Initial size in cells.\n"
        "        -- rows = %d,\n"
        "    },\n"
        "    font = {\n"
        "        -- family = \"%s\",\n"
        "        -- size = %d, -- Pixels. Changing this overrides the last remembered zoom.\n"
        "        -- letter_spacing = 0, -- Pixels added to each column.\n"
        "        -- line_spacing = 0, -- Pixels added to each row.\n"
        "    },\n"
        "    colors = {\n"
        "        -- foreground = \"#%06x\",\n"
        "        -- background = \"#%06x\",\n"
        "        -- cursor = \"#%06x\",\n"
        "        -- palette = { [1] = \"#%06x\" }, -- Entries 1 through 16 override the ANSI palette.\n"
        "    },\n"
        "    mouse = {\n"
        "        -- reporting = true,\n"
        "        -- selection_override = \"Shift\",\n"
        "    },\n"
        "    hyperlinks = {\n"
        "        -- enabled = true,\n"
        "        -- modifier = \"Ctrl\",\n"
        "    },\n"
        "    cursor = {\n"
        "        -- style = \"%s\", -- block, underline, or bar\n"
        "        -- blink = %s,\n"
        "        -- blink_interval = %d, -- Milliseconds between visible and hidden.\n"
        "        -- thickness = %d, -- Pixels for the underline and bar.\n"
        "    },\n"
        "    terminal = {\n"
        "        -- shell = nil, -- Uses $SHELL, then /bin/sh. Set an absolute path to override it.\n"
        "        -- term = \"xterm-color\", -- Use deal after installing its terminfo.\n"
        "    },\n"
        "    bell = {\n"
        "        -- audible = false,\n"
        "        -- visual = false,\n"
        "        -- urgency = false,\n"
        "    },\n"
        "    scrollback = {\n"
        "        -- capacity = %d, -- Lines. Set to 0 to disable history.\n"
        "        -- scroll_on_input = %s, -- Return to the bottom when typing or pasting.\n"
        "    },\n"
        "    selection = {\n"
        "        -- foreground = \"#%06x\",\n"
        "        -- background = \"#%06x\",\n"
        "    },\n"
        "}\n",
        default_zoom_in, default_zoom_out, default_reload, default_fullscreen, default_title,
        defaults->padding.horizontal, defaults->padding.horizontal, defaults->padding.vertical,
        defaults->columns, defaults->rows, default_font_family, defaults->font_size,
        config_rgb(defaults->foreground), config_rgb(defaults->background), config_rgb(defaults->cursor), config_rgb(defaults->palette[0]),
        cursor_style_names[defaults->cursor_style], defaults->blink ? "true" : "false",
        defaults->blink_interval, defaults->cursor_thickness, defaults->history_capacity, defaults->scroll_on_input ? "true" : "false",
        config_rgb(defaults->selection_foreground), config_rgb(defaults->selection_background));
}

static int config_write_file(char *path, int (*write_contents)(FILE *, const void *),
                             const void *data, bool replace)
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
            if (error == 0 && replace && rename(temporary, path) < 0) error = errno;
            /* Publish the complete template only if another launch has not created it. */
            if (error == 0 && !replace && link(temporary, path) < 0 && errno != EEXIST) error = errno;
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

static int config_read_zoom(const char *path, int configured)
{
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
    return valid ? zoom.size : configured;
}

static void config_save_zoom(char *path, int configured, int size)
{
    SavedZoom zoom = {configured, size};
    if (path != NULL && config_write_file(path, config_write_zoom, &zoom, true) < 0) {
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

static void config_warning(lua_State *lua, const char *path, const char *key, const char *expected)
{
    Config *config = lua_touserdata(lua, 1);
    config->invalid = true;
    fprintf(stderr, "deal: %s: %s must be %s\n", path, key, expected);
}

static int config_get(lua_State *lua, const char *key)
{
    lua_pushstring(lua, key);
    return lua_rawget(lua, -2);
}

static void config_string(lua_State *lua, const char *path, const char *key,
                          const char *label, char **target, size_t limit, bool absolute)
{
    int type = config_get(lua, key);
    if (type != LUA_TNIL) {
        size_t length = 0;
        const char *value = type == LUA_TSTRING ? lua_tolstring(lua, -1, &length) : NULL;
        bool valid = value != NULL && length > 0 && length <= limit && memchr(value, 0, length) == NULL;
        if (valid && absolute) {
            struct stat attributes;
            valid = value[0] == '/' && stat(value, &attributes) == 0 &&
                    S_ISREG(attributes.st_mode) && access(value, X_OK) == 0;
        }
        if (!valid) {
            config_warning(lua, path, label, absolute ? "an absolute executable path" : "a nonempty string up to 512 bytes");
        } else {
            *target = strdup(value);
            if (*target == NULL) luaL_error(lua, "could not allocate %s", label);
        }
    }
    lua_pop(lua, 1);
}

static void config_color(lua_State *lua, const char *path, const char *key,
                         const char *label, TerminalColor *target)
{
    int type = config_get(lua, key);
    if (type != LUA_TNIL) {
        size_t length = 0;
        const char *value = type == LUA_TSTRING ? lua_tolstring(lua, -1, &length) : NULL;
        if (value == NULL || !config_parse_color(value, length, target)) {
            config_warning(lua, path, label, "a #RRGGBB color");
        }
    }
    lua_pop(lua, 1);
}

static bool config_section(lua_State *lua, const char *path, const char *key)
{
    int type = config_get(lua, key);
    if (type == LUA_TTABLE) return true;
    if (type != LUA_TNIL) config_warning(lua, path, key, "a table");
    lua_pop(lua, 1);
    return false;
}

static void config_integer(lua_State *lua, const char *path, const char *key, const char *label,
                           int *target, int low, int high)
{
    int type = config_get(lua, key);
    if (type != LUA_TNIL) {
        int is_integer;
        lua_Integer value = lua_tointegerx(lua, -1, &is_integer);
        if (type == LUA_TNUMBER && is_integer && value >= low && value <= high) {
            *target = (int)value;
        } else {
            char expected[64];
            snprintf(expected, sizeof expected, "an integer from %d to %d", low, high);
            config_warning(lua, path, label, expected);
        }
    }
    lua_pop(lua, 1);
}

static void config_padding(lua_State *lua, const char *path, Padding *padding)
{
    int type = config_get(lua, "padding");
    if (type == LUA_TTABLE) {
        config_integer(lua, path, "horizontal", "window.padding.horizontal", &padding->horizontal, 0, 128);
        config_integer(lua, path, "vertical", "window.padding.vertical", &padding->vertical, 0, 128);
    } else if (type != LUA_TNIL) {
        int integral;
        lua_Integer pixels = lua_tointegerx(lua, -1, &integral);
        if (type == LUA_TNUMBER && integral && pixels >= 0 && pixels <= 128) {
            *padding = (Padding){(int)pixels, (int)pixels};
        } else {
            config_warning(lua, path, "window.padding", "an integer from 0 to 128 or a table");
        }
    }
    lua_pop(lua, 1);
}

static void config_opacity(lua_State *lua, const char *path, float *opacity)
{
    int type = config_get(lua, "opacity");
    if (type != LUA_TNIL) {
        lua_Number value = lua_tonumber(lua, -1);
        if (type == LUA_TNUMBER && value >= 0.0 && value <= 1.0) *opacity = (float)value;
        else config_warning(lua, path, "window.opacity", "a number from 0.0 to 1.0");
    }
    lua_pop(lua, 1);
}

static void config_shortcut(lua_State *lua, const char *path, const char *key, const char *label,
                            Shortcut *shortcut, bool optional)
{
    int type = config_get(lua, key);
    if (optional && type == LUA_TBOOLEAN && !lua_toboolean(lua, -1)) {
        *shortcut = (Shortcut){0};
    } else if (type != LUA_TNIL) {
        size_t length = 0;
        const char *value = type == LUA_TSTRING ? lua_tolstring(lua, -1, &length) : NULL;
        if (value == NULL || length > 64 || memchr(value, 0, length) != NULL ||
            !shortcut_parse(value, shortcut, optional)) {
            config_warning(lua, path, label, optional ? "a shortcut such as F11, or false" : "a shortcut such as Ctrl+Shift+=");
        }
    }
    lua_pop(lua, 1);
}

static void config_boolean(lua_State *lua, const char *path, const char *key, const char *label, bool *target)
{
    int type = config_get(lua, key);
    if (type == LUA_TBOOLEAN) *target = lua_toboolean(lua, -1);
    else if (type != LUA_TNIL) config_warning(lua, path, label, "a boolean");
    lua_pop(lua, 1);
}

static void config_modifier(lua_State *lua, const char *path, const char *key, const char *label,
                            unsigned int *target)
{
    int type = config_get(lua, key);
    if (type != LUA_TNIL) {
        size_t length = 0;
        const char *text = type == LUA_TSTRING ? lua_tolstring(lua, -1, &length) : NULL;
        unsigned int modifier = text != NULL ? modifier_parse(text, length) : 0;
        if (modifier == 0) config_warning(lua, path, label, "Shift, Alt, or Ctrl");
        else *target = modifier;
    }
    lua_pop(lua, 1);
}

static void config_palette(lua_State *lua, const char *path, TerminalColor palette[16])
{
    int type = config_get(lua, "palette");
    if (type == LUA_TTABLE) {
        for (int i = 0; i < 16; ++i) {
            type = lua_rawgeti(lua, -1, i + 1);
            if (type != LUA_TNIL) {
                size_t length = 0;
                const char *text = type == LUA_TSTRING ? lua_tolstring(lua, -1, &length) : NULL;
                if (text == NULL || !config_parse_color(text, length, &palette[i])) {
                    char label[32];
                    snprintf(label, sizeof label, "colors.palette[%d]", i + 1);
                    config_warning(lua, path, label, "a #RRGGBB color");
                }
            }
            lua_pop(lua, 1);
        }
    } else if (type != LUA_TNIL) config_warning(lua, path, "colors.palette", "a table");
    lua_pop(lua, 1);
}

/* Run both evaluation and table reads under Lua's protected call. */
static int config_evaluate(lua_State *lua)
{
    Config *config = lua_touserdata(lua, 1);
    const char *path = lua_touserdata(lua, 2);
    luaL_openlibs(lua);
    if (luaL_loadfilex(lua, path, "t") != LUA_OK) return lua_error(lua);
    lua_call(lua, 0, 1);
    if (!lua_istable(lua, -1)) return luaL_error(lua, "expected a configuration table");

    if (config_section(lua, path, "shortcuts")) {
        config_shortcut(lua, path, "zoom_in", "shortcuts.zoom_in", &config->zoom_in, false);
        config_shortcut(lua, path, "zoom_out", "shortcuts.zoom_out", &config->zoom_out, false);
        lua_pop(lua, 1);
    }
    if (config_section(lua, path, "keybinds")) {
        config_shortcut(lua, path, "reload_config", "keybinds.reload_config", &config->reload, false);
        config_shortcut(lua, path, "fullscreen", "keybinds.fullscreen", &config->fullscreen, true);
        lua_pop(lua, 1);
    }
    if (config_section(lua, path, "window")) {
        config_string(lua, path, "title", "window.title", &config->title, TITLE_LIMIT, false);
        config_padding(lua, path, &config->padding);
        config_opacity(lua, path, &config->opacity);
        config_boolean(lua, path, "decorations", "window.decorations", &config->decorations);
        config_integer(lua, path, "columns", "window.columns", &config->columns, 1, 512);
        config_integer(lua, path, "rows", "window.rows", &config->rows, 1, 256);
        config_boolean(lua, path, "dynamic_title", "window.dynamic_title", &config->dynamic_title);
        lua_pop(lua, 1);
    }
    if (config_section(lua, path, "mouse")) {
        config_boolean(lua, path, "reporting", "mouse.reporting", &config->mouse_reporting);
        config_modifier(lua, path, "selection_override", "mouse.selection_override", &config->mouse_override);
        lua_pop(lua, 1);
    }
    if (config_section(lua, path, "hyperlinks")) {
        config_boolean(lua, path, "enabled", "hyperlinks.enabled", &config->hyperlinks);
        config_modifier(lua, path, "modifier", "hyperlinks.modifier", &config->link_modifier);
        lua_pop(lua, 1);
    }
    if (config_section(lua, path, "font")) {
        config_string(lua, path, "family", "font.family", &config->font_family, 512, false);
        config_integer(lua, path, "size", "font.size", &config->font_size, MIN_FONT_SIZE, MAX_FONT_SIZE);
        config_integer(lua, path, "letter_spacing", "font.letter_spacing", &config->letter_spacing, -16, 32);
        config_integer(lua, path, "line_spacing", "font.line_spacing", &config->line_spacing, -32, 64);
        lua_pop(lua, 1);
    }
    if (config_section(lua, path, "colors")) {
        config_color(lua, path, "foreground", "colors.foreground", &config->foreground);
        config_color(lua, path, "background", "colors.background", &config->background);
        config_color(lua, path, "cursor", "colors.cursor", &config->cursor);
        config_palette(lua, path, config->palette);
        lua_pop(lua, 1);
    }
    if (config_section(lua, path, "cursor")) {
        int type = config_get(lua, "style");
        if (type != LUA_TNIL) {
            size_t length = 0;
            const char *style = type == LUA_TSTRING ? lua_tolstring(lua, -1, &length) : "";
            if (length == 5 && memcmp(style, "block", 5) == 0) config->cursor_style = CURSOR_BLOCK;
            else if (length == 9 && memcmp(style, "underline", 9) == 0) config->cursor_style = CURSOR_UNDERLINE;
            else if (length == 3 && memcmp(style, "bar", 3) == 0) config->cursor_style = CURSOR_BAR;
            else config_warning(lua, path, "cursor.style", "block, underline, or bar");
        }
        lua_pop(lua, 1);
        config_boolean(lua, path, "blink", "cursor.blink", &config->blink);
        config_integer(lua, path, "blink_interval", "cursor.blink_interval", &config->blink_interval, 50, 5000);
        config_integer(lua, path, "thickness", "cursor.thickness", &config->cursor_thickness, 1, 32);
        lua_pop(lua, 1);
    }
    if (config_section(lua, path, "terminal")) {
        config_string(lua, path, "shell", "terminal.shell", &config->shell, PATH_MAX - 1, true);
        int type = config_get(lua, "term");
        if (type != LUA_TNIL) {
            size_t length = 0;
            const char *name = type == LUA_TSTRING ? lua_tolstring(lua, -1, &length) : "";
            if (length == 4 && memcmp(name, "deal", 4) == 0) config->custom_term = true;
            else if (length != sizeof compatibility_term - 1 ||
                     memcmp(name, compatibility_term, sizeof compatibility_term - 1) != 0)
                config_warning(lua, path, "terminal.term", "deal or xterm-color");
        }
        lua_pop(lua, 2);
    }
    if (config_section(lua, path, "bell")) {
        config_boolean(lua, path, "audible", "bell.audible", &config->audible_bell);
        config_boolean(lua, path, "visual", "bell.visual", &config->visual_bell);
        config_boolean(lua, path, "urgency", "bell.urgency", &config->bell_urgency);
        lua_pop(lua, 1);
    }
    if (config_section(lua, path, "scrollback")) {
        config_integer(lua, path, "capacity", "scrollback.capacity", &config->history_capacity, 0, 1000000);
        int type = config_get(lua, "scroll_on_input");
        if (type == LUA_TBOOLEAN) config->scroll_on_input = lua_toboolean(lua, -1);
        else if (type != LUA_TNIL) config_warning(lua, path, "scrollback.scroll_on_input", "a boolean");
        lua_pop(lua, 2);
    }
    if (config_section(lua, path, "selection")) {
        config_color(lua, path, "foreground", "selection.foreground", &config->selection_foreground);
        config_color(lua, path, "background", "selection.background", &config->selection_background);
        lua_pop(lua, 1);
    }
    return 0;
}

static bool config_read(Config *config, bool startup)
{
    const Options *options = config->options;
    const char *explicit_path = options != NULL ? options->config_path : NULL;
    char *default_path = explicit_path == NULL ? config_path("init.lua", false) : NULL;
    const char *path = explicit_path != NULL ? explicit_path : default_path;
    if (path == NULL) return false;
    struct stat attributes;
    int status = stat(path, &attributes);
    if (startup && status < 0 && errno == ENOENT && explicit_path == NULL) {
        if (options != NULL && options->diagnostics) {
            free(default_path);
            return true;
        }
        if (config_write_file(default_path, config_write_template, config, false) == 0)
            status = stat(path, &attributes);
    }
    bool okay = false;
    if (status < 0) {
        fprintf(stderr, "deal: %s: %s\n", path, strerror(errno));
    } else if (!S_ISREG(attributes.st_mode)) {
        fprintf(stderr, "deal: %s: configuration must be a regular file\n", path);
    } else {
        lua_State *lua = luaL_newstate();
        if (lua == NULL) {
            fprintf(stderr, "deal: %s: could not initialize Lua 5.4\n", path);
        } else {
            lua_pushcfunction(lua, config_evaluate);
            lua_pushlightuserdata(lua, config);
            lua_pushlightuserdata(lua, (void *)path);
            if (lua_pcall(lua, 2, 0, 0) == LUA_OK) {
                okay = startup || !config->invalid;
            } else {
                const char *message = lua_type(lua, -1) == LUA_TSTRING ? lua_tostring(lua, -1) : "Lua evaluation failed";
                size_t length = strlen(path);
                if (strncmp(message, path, length) == 0 && message[length] == ':') fprintf(stderr, "deal: %s\n", message);
                else fprintf(stderr, "deal: %s: %s\n", path, message);
            }
            lua_close(lua);
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

static bool config_load(Config *config, const Options *options)
{
    *config = config_defaults();
    config->options = options;
    if (!config_read(config, true)) {
        config_destroy(config);
        *config = config_defaults();
        config->options = options;
        if (options != NULL && options->config_path != NULL) return false;
    }
    config_overrides(config);
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
    unsigned char span;
    bool mark;
    int x, y, width, height;
} CachedGlyph;

typedef struct {
    FT_Library library;
    FcConfig *config;
    FcFontSet *matches;
    FontFace *faces;
    size_t primary;
    int size;
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

static bool font_load_glyph(FT_Face face, uint32_t codepoint)
{
    FT_UInt index = FT_Get_Char_Index(face, codepoint);
    if (index == 0 || FT_Load_Glyph(face, index, FT_LOAD_RENDER) != 0) return false;
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
    if (font_load_glyph(face, 0x2500)) {
        GlyphBounds line = font_ink_bounds(face->glyph);
        font->box.left = line.left;
        font->box.right = line.right;
    }
    if (font_load_glyph(face, 0x2502)) {
        GlyphBounds line = font_ink_bounds(face->glyph);
        font->box.top = line.top;
        font->box.bottom = line.bottom;
    }
    if (font_load_glyph(face, 0x2588)) font->block = font_ink_bounds(face->glyph);
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

static const char *font_open(FontRenderer *renderer, const Config *config)
{
    renderer->size = config->font_size;
    if (FT_Init_FreeType(&renderer->library) != 0) return "could not initialize FreeType";
    renderer->config = FcInitLoadConfigAndFonts();
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
    if (renderer->matches != NULL) FcFontSetDestroy(renderer->matches);
    if (renderer->config != NULL) FcConfigDestroy(renderer->config);
    if (renderer->library != NULL) FT_Done_FreeType(renderer->library);
}

static FontFace *font_find_glyph(FontRenderer *renderer, uint32_t codepoint)
{
    FontFace *primary = &renderer->faces[renderer->primary];
    if (font_load_glyph(primary->face, codepoint)) return primary;
    for (size_t i = 0; i < (size_t)renderer->matches->nfont; ++i) {
        if (i == renderer->primary) continue;
        FcCharSet *charset;
        if (FcPatternGetCharSet(renderer->matches->fonts[i], FC_CHARSET, 0, &charset) == FcResultMatch &&
            !FcCharSetHasChar(charset, codepoint)) continue;
        FontFace *font = font_open_face(renderer, i);
        if (font != NULL && font_load_glyph(font->face, codepoint)) return font;
    }
    return NULL;
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
    FontFace *font = font_find_glyph(renderer, codepoint);
    if (font == NULL && !glyph->mark) {
        codepoint = 0xfffd;
        font = font_find_glyph(renderer, codepoint);
        if (font == NULL) {
            codepoint = '?';
            font = font_find_glyph(renderer, codepoint);
        }
    }
    if (font == NULL) return;
    const FT_Bitmap *bitmap = &font->face->glyph->bitmap;
    if (bitmap->width == 0 || bitmap->rows == 0) return;
    GlyphBounds target = glyph_placement(renderer, font, codepoint, glyph->span, glyph->mark);
    int cell_width = renderer->metrics.width * glyph->span;
    int cell_height = renderer->metrics.height;
    int left = target.left > 0 ? target.left : 0;
    int top = target.top > 0 ? target.top : 0;
    int right = target.right < cell_width ? target.right : cell_width;
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

static size_t glyph_cached(FontRenderer *renderer, uint32_t codepoint, unsigned char span, bool mark)
{
    uint32_t hash = (codepoint << 2) | (span == 2 ? 2u : 0u) | mark;
    hash = (hash ^ (hash >> 16)) * UINT32_C(0x7feb352d);
    hash ^= hash >> 15;
    size_t bucket = (hash % (renderer->cache_size / GLYPH_CACHE_WAYS)) * GLYPH_CACHE_WAYS;
    size_t target = bucket;
    for (size_t slot = bucket; slot < bucket + GLYPH_CACHE_WAYS; ++slot) {
        CachedGlyph *glyph = &renderer->cache[slot];
        if (glyph->used != 0 && glyph->codepoint == codepoint && glyph->span == span && glyph->mark == mark) {
            glyph->used = ++renderer->access;
            return slot;
        }
        if (glyph->used < renderer->cache[target].used) target = slot;
    }
    /* Previously submitted quads must finish using this tile before it changes. */
    glyph_flush(renderer);
    CachedGlyph *entry = &renderer->cache[target];
    *entry = (CachedGlyph){.used = ++renderer->access, .codepoint = codepoint, .span = span, .mark = mark};
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

static void render_glyph(FontRenderer *renderer, uint32_t codepoint, unsigned char span, bool mark,
                         TerminalColor foreground, int x, int y)
{
    size_t slot = glyph_cached(renderer, codepoint, span, mark);
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
    /* An ARGB X11 compositor consumes premultiplied color channels. */
    float background_scale = opacity / 255.0f;
    glClearColor(background.red * background_scale, background.green * background_scale,
                 background.blue * background_scale, opacity);
    glClear(GL_COLOR_BUFFER_BIT);

    int pad_x = config->padding.horizontal, pad_y = config->padding.vertical;
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
        bool has_underline = false;
        for (size_t column = 0; column < screen->columns; ++column) {
            const TerminalCell *cell = view_cell(view, &source, column);
            bool under_block = row == block_row && column >= block_first && column < block_end;
            bool selected = column >= selected_first && column < selected_end;
            TerminalColor bg = selected ? config->selection_background : cell_color(config, cell->background_source, cell->background);
            if (under_block) bg = config->cursor;
            bool solid = opacity < 1.0f
                ? selected || under_block || cell->background_source != SOURCE_BG
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
            has_underline |= cell->underline || (view->hovered_link && cell->hyperlink == view->hovered_link);
        }
        if (solid_run) {
            render_color(run);
            render_rectangle(pad_x + (int)start * cell_width, y, (int)(screen->columns - start) * cell_width, cell_height);
        }
        if (!has_underline) continue;
        for (size_t column = 0; column < screen->columns && column < source.stored; ++column) {
            const TerminalCell *cell = &source.cells[column];
            bool hover = view->hovered_link && cell->hyperlink == view->hovered_link;
            if (!cell->underline && !hover) continue;
            bool selected = column >= selected_first && column < selected_end;
            bool under_block = row == block_row && column >= block_first && column < block_end;
            TerminalColor fg = selected ? config->selection_foreground : cell_color(config, cell->foreground_source, cell->foreground);
            TerminalColor bg = selected ? config->selection_background : cell_color(config, cell->background_source, cell->background);
            render_color(under_block ? bg : fg);
            render_rectangle(pad_x + (int)column * cell_width, y + (cell_height > 1 ? cell_height - 2 : 0),
                             hover && !cell->underline && cell_width > 1 ? cell_width - 1 : cell_width, 1);
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
            if (!glyph && !cell->combining[0]) continue;
            int x = pad_x + (int)column * cell_width;
            bool selected = column >= selected_first && column < selected_end;
            TerminalColor foreground = selected ? config->selection_foreground : cell_color(config, cell->foreground_source, cell->foreground);
            if (row == block_row && column == block_first) foreground = selected ? config->selection_background : cell_color(config, cell->background_source, cell->background);
            if (glyph) {
                render_glyph(renderer, cell->character, cell->width, false, foreground, x, y);
            }
            for (size_t i = 0; i < sizeof cell->combining / sizeof cell->combining[0] && cell->combining[i]; ++i) {
                render_glyph(renderer, cell->combining[i], cell->width, true, foreground, x, y);
            }
        }
    }
    glyph_flush(renderer);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);

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

typedef struct {
    Display *display;
    Window window, root;
    Colormap colormap;
    GLXContext context;
    GLXWindow glx_window;
    Atom wm_protocols;
    Atom wm_delete_window, net_wm_name, utf8_string;
    Atom net_wm_state, net_wm_fullscreen, motif_hints, compositor_selection;
    bool argb, composited;
    int64_t compositor_check;
    unsigned int alt_mask;
    bool focused, urgent;
    int width;
    int height;
    bool mapped;
} Platform;

static void platform_title(Platform *platform, const Config *config, const char *dynamic)
{
    const char *title = config->title != NULL ? config->title : default_title;
    if (config->options != NULL && config->options->title != NULL) title = config->options->title;
    if (*dynamic) title = dynamic;
    XStoreName(platform->display, platform->window, title);
    XChangeProperty(platform->display, platform->window, platform->net_wm_name,
                    platform->utf8_string, 8, PropModeReplace,
                    (const unsigned char *)title, (int)strlen(title));
}

static void platform_modifiers(Platform *platform)
{
    Display *display = platform->display;
    XModifierKeymap *map = XGetModifierMapping(display);
    unsigned int alt = 0;
    if (map != NULL) {
        KeyCode left = XKeysymToKeycode(display, XK_Alt_L), right = XKeysymToKeycode(display, XK_Alt_R);
        for (int slot = 3; slot < 8; ++slot) {
            for (int i = 0; i < map->max_keypermod; ++i) {
                KeyCode code = map->modifiermap[slot * map->max_keypermod + i];
                if (code != 0 && (code == left || code == right)) alt |= 1u << slot;
            }
        }
        XFreeModifiermap(map);
    }
    platform->alt_mask = alt ? alt : Mod1Mask;
}

static unsigned int event_modifiers(const Platform *platform, unsigned int state)
{
    unsigned int alt = platform->alt_mask ? platform->alt_mask : Mod1Mask;
    return (state & (ShiftMask | ControlMask)) | ((state & alt) ? Mod1Mask : 0);
}

static void platform_size_hints(const Platform *platform, Padding padding, const FontMetrics *metrics)
{
    XSizeHints hints = {
        .flags = PBaseSize | PMinSize,
        .base_width = 2 * padding.horizontal, .base_height = 2 * padding.vertical,
        .min_width = 2 * padding.horizontal + metrics->width,
        .min_height = 2 * padding.vertical + metrics->height
    };
    XSetWMNormalHints(platform->display, platform->window, &hints);
}

static void platform_decorations(const Platform *platform, bool enabled)
{
    /* Motif flags: decorations are specified, with all or no decorations. */
    unsigned long hints[] = {2, 0, enabled ? 1u : 0u, 0, 0};
    XChangeProperty(platform->display, platform->window, platform->motif_hints,
                    platform->motif_hints, 32, PropModeReplace, (const unsigned char *)hints, 5);
}

static void platform_fullscreen(const Platform *platform)
{
    XEvent request = {.xclient = {
        .type = ClientMessage, .display = platform->display, .window = platform->window,
        .message_type = platform->net_wm_state, .format = 32,
        .data.l = {2, (long)platform->net_wm_fullscreen, 0, 1, 0}
    }};
    XSendEvent(platform->display, platform->root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &request);
}

static int initial_extent(int limit, int cells, int cell_size, int padding)
{
    if (limit > 4096) limit = 4096;
    int fit = grid_extent(limit, padding) / cell_size;
    if (fit == 0) return limit > 0 ? limit : 1;
    return 2 * padding + (fit < cells ? fit : cells) * cell_size;
}

static XVisualInfo *platform_argb_visual(Display *display, int screen, GLXFBConfig *chosen)
{
    int major, minor, opcode, event, error;
    if (!glXQueryVersion(display, &major, &minor) || (major < 1 || (major == 1 && minor < 3)) ||
        !XQueryExtension(display, "RENDER", &opcode, &event, &error)) return NULL;

    const int attributes[] = {
        GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR, GLX_DOUBLEBUFFER, True, GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None
    };
    int count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(display, screen, attributes, &count);
    XVisualInfo *visual = NULL;
    int best = INT_MAX;
    for (int i = 0; i < count; ++i) {
        XVisualInfo *candidate = glXGetVisualFromFBConfig(display, configs[i]);
        /* Select the usual X11 ARGB8888 visual, with the high byte for alpha. */
        if (candidate == NULL) continue;
        if (candidate->depth == 32 && candidate->red_mask == 0x00ff0000 &&
            candidate->green_mask == 0x0000ff00 && candidate->blue_mask == 0x000000ff) {
            int depth = 0, stencil = 0, multisample = 0;
            glXGetFBConfigAttrib(display, configs[i], GLX_DEPTH_SIZE, &depth);
            glXGetFBConfigAttrib(display, configs[i], GLX_STENCIL_SIZE, &stencil);
            glXGetFBConfigAttrib(display, configs[i], GLX_SAMPLE_BUFFERS, &multisample);
            int score = depth + stencil + 256 * multisample;
            if (score < best) {
                if (visual != NULL) XFree(visual);
                *chosen = configs[i];
                visual = candidate;
                best = score;
                if (score == 0) break;
                continue;
            }
        }
        XFree(candidate);
    }
    if (configs != NULL) XFree(configs);
    return visual;
}

static bool platform_drawable(Platform *platform, XVisualInfo *visual, GLXFBConfig framebuffer)
{
    Display *display = platform->display;
    platform->colormap = XCreateColormap(display, platform->root, visual->visual, AllocNone);
    XSetWindowAttributes attributes = {
        .colormap = platform->colormap,
        .border_pixel = 0,
        .event_mask = ExposureMask | StructureNotifyMask | KeyPressMask |
                      ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                      EnterWindowMask | LeaveWindowMask | FocusChangeMask
    };
    platform->window = XCreateWindow(display, platform->root, 0, 0, platform->width,
                                     platform->height, 0, visual->depth, InputOutput,
                                     visual->visual, CWColormap | CWBorderPixel | CWEventMask, &attributes);
    GLXDrawable drawable = platform->window;
    if (framebuffer != NULL) {
        platform->glx_window = glXCreateWindow(display, framebuffer, platform->window, NULL);
        if (platform->glx_window == None) return false;
        drawable = platform->glx_window;
        platform->context = glXCreateNewContext(display, framebuffer, GLX_RGBA_TYPE, NULL, True);
    } else {
        platform->context = glXCreateContext(display, visual, NULL, True);
    }
    return platform->context != NULL && glXMakeCurrent(display, drawable, platform->context);
}

static void platform_drop_drawable(Platform *platform)
{
    Display *display = platform->display;
    if (platform->context != NULL) {
        glXMakeCurrent(display, None, NULL);
        glXDestroyContext(display, platform->context);
        platform->context = NULL;
    }
    if (platform->glx_window != None) {
        glXDestroyWindow(display, platform->glx_window);
        platform->glx_window = None;
    }
    if (platform->window != None) {
        XDestroyWindow(display, platform->window);
        platform->window = None;
    }
    if (platform->colormap != None) {
        XFreeColormap(display, platform->colormap);
        platform->colormap = None;
    }
}

/* GLX and window creation can report errors asynchronously through Xlib. */
static bool drawable_x_error;

static int platform_drawable_error(Display *display, XErrorEvent *event)
{
    (void)display;
    (void)event;
    drawable_x_error = true;
    return 0;
}

static bool platform_try_argb(Platform *platform, XVisualInfo *visual, GLXFBConfig framebuffer)
{
    XSync(platform->display, False);
    XErrorHandler previous = XSetErrorHandler(platform_drawable_error);
    drawable_x_error = false;
    bool ready = platform_drawable(platform, visual, framebuffer);
    XSync(platform->display, False);
    ready &= !drawable_x_error;
    if (!ready) {
        platform_drop_drawable(platform);
        XSync(platform->display, False);
    }
    XSetErrorHandler(previous);
    return ready;
}

static const char *platform_open(Platform *platform, const Config *config, const FontMetrics *metrics)
{
    Display *display = platform->display = XOpenDisplay(NULL);
    if (display == NULL) return "could not open the X11 display";

    int screen = DefaultScreen(display);
    platform->root = RootWindow(display, screen);
    platform->width = initial_extent(DisplayWidth(display, screen), config->columns,
                                     metrics->width, config->padding.horizontal);
    platform->height = initial_extent(DisplayHeight(display, screen), config->rows,
                                      metrics->height, config->padding.vertical);

    GLXFBConfig framebuffer = NULL;
    XVisualInfo *visual = platform_argb_visual(display, screen, &framebuffer);
    if (visual != NULL) {
        platform->argb = platform_try_argb(platform, visual, framebuffer);
        XFree(visual);
    }
    if (!platform->argb) {
        int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                            GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
        visual = glXChooseVisual(display, screen, attributes);
        if (visual == NULL) return "could not find a double-buffered GLX visual";
        bool ready = platform_drawable(platform, visual, NULL);
        XFree(visual);
        if (!ready) return "could not initialize the GLX context";
    }

    char compositor[40];
    snprintf(compositor, sizeof compositor, "_NET_WM_CM_S%d", screen);
    platform->compositor_selection = XInternAtom(display, compositor, False);
    platform->net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    platform->net_wm_fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    platform->motif_hints = XInternAtom(display, "_MOTIF_WM_HINTS", False);
    const char *window_class = config->options != NULL ? config->options->window_class : NULL;
    XClassHint identity = {.res_name = "deal", .res_class = (char *)(window_class != NULL ? window_class : "deal")};
    XSetClassHint(display, platform->window, &identity);
    platform_decorations(platform, config->decorations);
    platform_size_hints(platform, config->padding, metrics);
    platform->net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    platform->utf8_string = XInternAtom(display, "UTF8_STRING", False);
    platform_title(platform, config, "");
    platform_modifiers(platform);
    platform->wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    platform->wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    if (!XSetWMProtocols(display, platform->window, &platform->wm_delete_window, 1)) {
        return "could not register the window close protocol";
    }

    if (config->options == NULL || !config->options->diagnostics) XMapWindow(display, platform->window);
    return NULL;
}

static void platform_close(Platform *platform)
{
    if (platform->display == NULL) return;
    platform_drop_drawable(platform);
    XCloseDisplay(platform->display);
    platform->display = NULL;
}

/* Terminal screen state */


typedef enum { COLOR_DEFAULT, COLOR_INDEXED, COLOR_RGB } ColorKind;

typedef struct {
    ColorKind kind;
    unsigned char index;
    TerminalColor rgb;
} ColorSpec;

typedef struct {
    ColorSpec foreground;
    ColorSpec background;
    bool bold;
    bool dim;
    bool underline;
    bool inverse;
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
    uint8_t fs = color_source(rendition->foreground, SOURCE_FG, rendition->bold);
    uint8_t bs = color_source(rendition->background, SOURCE_BG, false);
    TerminalColor fg = resolve_color(terminal, rendition->foreground, fs);
    TerminalColor bg = resolve_color(terminal, rendition->background, bs);
    if (rendition->dim) {
        if (fs) fs |= SOURCE_DIM;
        fg.red /= 2;
        fg.green /= 2;
        fg.blue /= 2;
    }
    return (TerminalCell){.character = character, .foreground = rendition->inverse ? bg : fg,
                          .background = rendition->inverse ? fg : bg,
                          .foreground_source = rendition->inverse ? bs : fs,
                          .background_source = rendition->inverse ? fs : bs,
                          .underline = rendition->underline, .width = width};
}

static TerminalCell terminal_blank(const Terminal *terminal, const TerminalBuffer *buffer)
{
    TerminalCell cell = terminal_cell(terminal, buffer, ' ', 1);
    cell.underline = false;
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
            ColorSpec discard;
            ColorSpec *target = code == 38 ? &rendition->foreground
                               : code == 48 ? &rendition->background : &discard;
            size_t limit = end > i + 1 ? end : parser->count;
            i = sgr_extended(target, parser, i, limit);
            continue;
        }
        if (end > i + 1) {
            i = end;
            continue;
        }
        switch (code) {
        case 0: *rendition = (Rendition){0}; break;
        case 1: rendition->bold = true; break;
        case 2: rendition->dim = true; break;
        case 4: rendition->underline = true; break;
        case 7: rendition->inverse = true; break;
        case 22: rendition->bold = rendition->dim = false; break;
        case 24: rendition->underline = false; break;
        case 27: rendition->inverse = false; break;
        case 39: rendition->foreground = (ColorSpec){0}; break;
        case 49: rendition->background = (ColorSpec){0}; break;
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
        parser->unsupported = false;
        parser->string_length = 0;
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

/* Reject invalid UTF-8 and controls before handing text to X11 or a URL handler. */
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

static void parser_finish_string(Terminal *terminal)
{
    Parser *parser = &terminal->parser;
    parser->state = PARSE_TEXT;
    if (!parser->osc) return;
    char *text = parser->string;
    size_t length = parser->string_length;
    text[length] = 0;
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
        if (parser->osc && parser->string_length < OSC_LIMIT) {
            parser->string[parser->string_length++] = (char)byte;
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
    size_t first = point.column, end = first;
    if (first < source.columns) {
        const TerminalCell *cell = view_cell(view, &source, first);
        end += cell->width;
        if (mode == SELECT_WORD) {
            int type = selection_class(cell->character);
            while (first > 0) {
                size_t previous = first - 1;
                if (previous > 0 && view_cell(view, &source, previous)->width == 0) --previous;
                if (selection_class(view_cell(view, &source, previous)->character) != type) break;
                first = previous;
            }
            while (end < source.columns) {
                cell = view_cell(view, &source, end);
                if (cell->width == 0 || selection_class(cell->character) != type) break;
                end += cell->width;
            }
        }
    }
    return (TextRange){{point.line, first}, {point.line, minimum(end, source.columns)}};
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
    selection->range.first = point_before(next.first, selection->anchor.first) ? next.first : selection->anchor.first;
    selection->range.end = point_before(next.end, selection->anchor.end) ? selection->anchor.end : next.end;
    selection->active = true;
    return true;
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
    int child_signal;
    int startup;
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
    struct sigaction original_sigchld;
    bool restore_signals;
} Session;

typedef enum { PTY_OPEN, PTY_CLOSED, PTY_FAILED } PtyResult;

static struct winsize session_size(const TerminalScreen *screen, int width, int height, Padding padding)
{
    return (struct winsize){
        .ws_col = (unsigned short)screen->columns,
        .ws_row = (unsigned short)screen->rows,
        .ws_xpixel = (unsigned short)grid_extent(width, padding.horizontal),
        .ws_ypixel = (unsigned short)grid_extent(height, padding.vertical)
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

static const char *session_open(Session *session, struct winsize size, int x11_fd,
                               const Config *config)
{
    const char *shell = getenv("SHELL");

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGCHLD);
    struct sigaction action = {.sa_handler = SIG_DFL, .sa_flags = SA_NOCLDSTOP};
    sigemptyset(&action.sa_mask);
    if (sigprocmask(SIG_BLOCK, &signals, &session->original_signal_mask) < 0) {
        return "could not configure shell exit notifications";
    }
    if (sigaction(SIGCHLD, &action, &session->original_sigchld) < 0) {
        sigprocmask(SIG_SETMASK, &session->original_signal_mask, NULL);
        return "could not configure shell exit notifications";
    }
    session->restore_signals = true;
    sigset_t shell_mask = session->original_signal_mask;
    sigdelset(&shell_mask, SIGCHLD);
    session->child_signal = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (session->child_signal < 0) {
        return "could not create a shell exit notification descriptor";
    }

    int startup[2];
    if (pipe2(startup, O_CLOEXEC | O_NONBLOCK) < 0) {
        return "could not create a shell startup pipe";
    }
    session->startup = startup[0];
    int master = -1;
    pid_t child = forkpty(&master, NULL, NULL, &size);
    if (child == 0) {
        close(startup[0]);
        close(session->child_signal);
        if (x11_fd > STDERR_FILENO) {
            close(x11_fd);
        }
        launch_child(&shell_mask, startup[1], config, shell);
    }
    close(startup[1]);
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

static void session_close(Session *session)
{
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
        struct pollfd signal_event = {session->child_signal, POLLIN, 0};
        while (poll(&signal_event, 1, 250) < 0 && errno == EINTR) {
        }
        session_reap(session, WNOHANG);
        if (session->child > 0) {
            kill(session->child, SIGKILL);
            session_reap(session, 0);
        }
    }
    if (session->child_signal >= 0) {
        close(session->child_signal);
        session->child_signal = -1;
    }
    if (session->restore_signals) {
        sigaction(SIGCHLD, &session->original_sigchld, NULL);
        sigprocmask(SIG_SETMASK, &session->original_signal_mask, NULL);
        session->restore_signals = false;
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

static bool session_paste(Session *session, const TextBuffer *text, bool latin1, bool bracketed)
{
    TextBuffer input = {0};
    bool okay = !bracketed || text_append(&input, "\033[200~", 6);
    size_t start = 0;
    for (size_t i = 0; okay && i < text->length; ++i) {
        unsigned char byte = text->bytes[i];
        bool copy = byte == '\r' || byte == '\t' ||
                    (byte >= 0x20 && byte != 0x7f && (!latin1 || byte < 0x80));
        if (copy) continue;
        okay = text_append(&input, text->bytes + start, i - start);
        start = i + 1;
        if (!okay) break;
        if (byte == '\n' && (i == 0 || text->bytes[i - 1] != '\r')) okay = text_append(&input, "\r", 1);
        else if (latin1 && byte >= 0xa0) okay = text_codepoint(&input, byte);
    }
    if (okay && text->length > start) okay = text_append(&input, text->bytes + start, text->length - start);
    if (okay && bracketed) okay = text_append(&input, "\033[201~", 6);
    if (okay) okay = session_append(session, input.bytes, input.length, true);
    text_destroy(&input);
    return okay;
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

/* Native X11 selections */

enum { TRANSFER_TIMEOUT = 10000, CLIPBOARD_CHUNK = 32768, OUTBOUND_TRANSFERS = 4 };

typedef struct {
    TextBuffer text;
    Time timestamp;
} OwnedSelection;

typedef struct {
    Window requestor;
    Atom property, target;
    long previous_mask;
    TextBuffer text;
    size_t sent;
    int64_t deadline;
} SelectionSend;

typedef struct {
    Window window;
    Atom selection, target;
    Time timestamp;
    TextBuffer text;
    int64_t deadline;
    bool incremental;
} SelectionReceive;

typedef struct {
    Atom clipboard, utf8, targets, timestamp, incr, property;
    size_t chunk;
    OwnedSelection primary, copied;
    SelectionSend outbound[OUTBOUND_TRANSFERS];
    SelectionReceive inbound;
} Clipboard;

static int deadline_timeout(int64_t now, int64_t deadline)
{
    if (deadline == 0 || now < 0) return -1;
    return deadline > now ? (int)(deadline - now) : 0;
}

static int earlier_timeout(int a, int b)
{
    return b < 0 || (a >= 0 && a < b) ? a : b;
}

static bool clipboard_open(Clipboard *clipboard, Display *display)
{
    char *names[] = {"CLIPBOARD", "UTF8_STRING", "TARGETS", "TIMESTAMP", "INCR", "DEAL_SELECTION"};
    Atom atoms[sizeof names / sizeof names[0]];
    if (!XInternAtoms(display, names, sizeof atoms / sizeof atoms[0], False, atoms)) return false;
    for (size_t i = 0; i < sizeof atoms / sizeof atoms[0]; ++i) {
        if (atoms[i] == None) return false;
    }
    clipboard->clipboard = atoms[0];
    clipboard->utf8 = atoms[1];
    clipboard->targets = atoms[2];
    clipboard->timestamp = atoms[3];
    clipboard->incr = atoms[4];
    clipboard->property = atoms[5];
    long units = XMaxRequestSize(display);
    clipboard->chunk = units > 32 ? minimum((size_t)(units - 32), CLIPBOARD_CHUNK / 4) * 4 : 0;
    return clipboard->chunk != 0;
}

static OwnedSelection *clipboard_owner(Clipboard *clipboard, Atom selection)
{
    if (selection == XA_PRIMARY) return &clipboard->primary;
    if (selection == clipboard->clipboard) return &clipboard->copied;
    return NULL;
}

static bool timestamp_before(Time a, Time b)
{
    return ((uint32_t)a - (uint32_t)b) > UINT32_C(0x7fffffff);
}

static bool clipboard_copy(Clipboard *clipboard, Platform *platform, Terminal *terminal,
                           Atom selection, Time timestamp)
{
    TextBuffer text = {0};
    if (selection_text(terminal, &text)) {
        XSetSelectionOwner(platform->display, selection, platform->window, timestamp);
        if (XGetSelectionOwner(platform->display, selection) == platform->window) {
            OwnedSelection *owned = clipboard_owner(clipboard, selection);
            text_destroy(&owned->text);
            *owned = (OwnedSelection){text, timestamp};
            return true;
        }
    }
    text_destroy(&text);
    return false;
}

/* A requesting client can close its window at any point during a transfer. */
static bool clipboard_x_failed;

static int clipboard_x_error(Display *display, XErrorEvent *event)
{
    (void)display;
    (void)event;
    clipboard_x_failed = true;
    return 0;
}

static XErrorHandler clipboard_x_begin(Display *display)
{
    XSync(display, False);
    clipboard_x_failed = false;
    return XSetErrorHandler(clipboard_x_error);
}

static bool clipboard_x_end(Display *display, XErrorHandler previous)
{
    XSync(display, False);
    XSetErrorHandler(previous);
    return !clipboard_x_failed;
}

static void clipboard_drop_send(Display *display, SelectionSend *send, bool destroyed)
{
    if (send->requestor != None && !destroyed) {
        XErrorHandler previous = clipboard_x_begin(display);
        XSelectInput(display, send->requestor, send->previous_mask);
        clipboard_x_end(display, previous);
    }
    text_destroy(&send->text);
    *send = (SelectionSend){0};
}

static void clipboard_drop_receive(Display *display, SelectionReceive *receive)
{
    if (receive->window != None) XDestroyWindow(display, receive->window);
    text_destroy(&receive->text);
    *receive = (SelectionReceive){0};
}

static void clipboard_close(Clipboard *clipboard, Display *display)
{
    for (size_t i = 0; i < OUTBOUND_TRANSFERS; ++i) clipboard_drop_send(display, &clipboard->outbound[i], false);
    clipboard_drop_receive(display, &clipboard->inbound);
    text_destroy(&clipboard->primary.text);
    text_destroy(&clipboard->copied.text);
}

static bool text_latin1(const TextBuffer *utf8, TextBuffer *output)
{
    for (size_t i = 0; i < utf8->length;) {
        unsigned char lead = utf8->bytes[i++];
        unsigned char byte = lead < 0x80 ? lead : '?';
        if ((lead == 0xc2 || lead == 0xc3) && i < utf8->length) {
            byte = (unsigned char)(((lead & 0x1f) << 6) | (utf8->bytes[i] & 0x3f));
        }
        while (i < utf8->length && (utf8->bytes[i] & 0xc0) == 0x80) ++i;
        if (!text_append(output, &byte, 1)) return false;
    }
    return true;
}

static SelectionSend *clipboard_begin_send(Clipboard *clipboard, Display *display,
                                            const XSelectionRequestEvent *request, Atom property,
                                            const TextBuffer *text)
{
    SelectionSend *slot = NULL;
    for (size_t i = 0; i < OUTBOUND_TRANSFERS; ++i) {
        if (clipboard->outbound[i].requestor == request->requestor) return NULL;
        if (slot == NULL && clipboard->outbound[i].requestor == None) slot = &clipboard->outbound[i];
    }
    XWindowAttributes attributes;
    if (slot == NULL || !XGetWindowAttributes(display, request->requestor, &attributes) ||
        !text_append(&slot->text, text->bytes, text->length)) return NULL;
    slot->requestor = request->requestor;
    slot->property = property;
    slot->target = request->target;
    slot->previous_mask = attributes.your_event_mask;
    slot->deadline = monotonic_milliseconds() + TRANSFER_TIMEOUT;
    XSelectInput(display, slot->requestor, attributes.your_event_mask | PropertyChangeMask | StructureNotifyMask);
    unsigned long length = (unsigned long)text->length;
    XChangeProperty(display, slot->requestor, property, clipboard->incr, 32, PropModeReplace,
                    (const unsigned char *)&length, 1);
    return slot;
}

static void clipboard_request(Clipboard *clipboard, Platform *platform, const XSelectionRequestEvent *request)
{
    Display *display = platform->display;
    OwnedSelection *owned = clipboard_owner(clipboard, request->selection);
    Atom property = request->property != None ? request->property : request->target;
    Atom result = None;
    SelectionSend *send = NULL;
    TextBuffer legacy = {0};
    XErrorHandler previous = clipboard_x_begin(display);
    if (request->owner == platform->window && owned != NULL && owned->text.length != 0 &&
        (request->time == CurrentTime || !timestamp_before(request->time, owned->timestamp))) {
        if (request->target == clipboard->targets) {
            Atom targets[] = {clipboard->targets, clipboard->timestamp, clipboard->utf8, XA_STRING};
            XChangeProperty(display, request->requestor, property, XA_ATOM, 32, PropModeReplace,
                            (const unsigned char *)targets, sizeof targets / sizeof targets[0]);
            result = property;
        } else if (request->target == clipboard->timestamp) {
            unsigned long time = owned->timestamp;
            XChangeProperty(display, request->requestor, property, XA_INTEGER, 32, PropModeReplace,
                            (const unsigned char *)&time, 1);
            result = property;
        } else if (request->target == clipboard->utf8 || request->target == XA_STRING) {
            const TextBuffer *payload = &owned->text;
            if (request->target == XA_STRING) payload = text_latin1(payload, &legacy) ? &legacy : NULL;
            if (payload != NULL && payload->length <= clipboard->chunk) {
                XChangeProperty(display, request->requestor, property, request->target, 8, PropModeReplace,
                                payload->bytes, (int)payload->length);
                result = property;
            } else if (payload != NULL) {
                send = clipboard_begin_send(clipboard, display, request, property, payload);
                if (send != NULL) result = property;
            }
        }
    }
    if (!clipboard_x_end(display, previous)) {
        if (send != NULL) clipboard_drop_send(display, send, false);
        send = NULL;
        result = None;
    }
    XEvent notify = {.xselection = {.type = SelectionNotify, .display = display,
        .requestor = request->requestor, .selection = request->selection,
        .target = request->target, .property = result, .time = request->time}};
    previous = clipboard_x_begin(display);
    Status sent = XSendEvent(display, request->requestor, False, NoEventMask, &notify);
    bool delivered = clipboard_x_end(display, previous);
    if ((!sent || !delivered) && send != NULL) clipboard_drop_send(display, send, false);
    text_destroy(&legacy);
}

static void clipboard_send_chunk(Clipboard *clipboard, Display *display, const XPropertyEvent *event)
{
    for (size_t i = 0; i < OUTBOUND_TRANSFERS; ++i) {
        SelectionSend *send = &clipboard->outbound[i];
        if (send->requestor != event->window || send->property != event->atom) continue;
        size_t remaining = send->text.length - send->sent;
        size_t length = minimum(remaining, clipboard->chunk);
        XErrorHandler previous = clipboard_x_begin(display);
        XChangeProperty(display, send->requestor, send->property, send->target, 8, PropModeReplace,
                        send->text.bytes + send->sent, (int)length);
        if (!clipboard_x_end(display, previous) || remaining == 0) clipboard_drop_send(display, send, false);
        else {
            send->sent += length;
            send->deadline = monotonic_milliseconds() + TRANSFER_TIMEOUT;
        }
        return;
    }
}

static void clipboard_paste_text(Terminal *terminal, Session *session,
                                 const Config *config, const TextBuffer *text, bool latin1, bool *redraw)
{
    if (text->length == 0) return;
    if (!session_paste(session, text, latin1, terminal->bracketed_paste)) terminal->bell_pending = true;
    else if (config->scroll_on_input) *redraw |= history_scroll(terminal, false, SIZE_MAX);
}

static void clipboard_paste(Clipboard *clipboard, Platform *platform, Terminal *terminal,
                            Session *session, const Config *config, Atom selection, Time timestamp,
                            bool *redraw)
{
    Display *display = platform->display;
    OwnedSelection *owned = clipboard_owner(clipboard, selection);
    clipboard_drop_receive(display, &clipboard->inbound);
    Window owner = XGetSelectionOwner(display, selection);
    if (owner == None) return;
    if (owner == platform->window && owned != NULL && owned->text.length != 0) {
        clipboard_paste_text(terminal, session, config, &owned->text, false, redraw);
        return;
    }
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(display, window, PropertyChangeMask);
    clipboard->inbound = (SelectionReceive){.window = window, .selection = selection, .target = clipboard->utf8,
        .timestamp = timestamp, .deadline = monotonic_milliseconds() + TRANSFER_TIMEOUT};
    XConvertSelection(display, selection, clipboard->utf8, clipboard->property, window, timestamp);
}

typedef enum { RECEIVE_FAILED, RECEIVE_WAIT, RECEIVE_COMPLETE } ReceiveResult;

static ReceiveResult clipboard_read(Clipboard *clipboard, Display *display)
{
    SelectionReceive *receive = &clipboard->inbound;
    unsigned long offset = 0;
    for (;;) {
        Atom type;
        int format;
        unsigned long count, remaining;
        unsigned char *data = NULL;
        int status = XGetWindowProperty(display, receive->window, clipboard->property, (long)offset,
                                        CLIPBOARD_CHUNK / 4, False, AnyPropertyType, &type,
                                        &format, &count, &remaining, &data);
        if (status != Success) {
            if (data != NULL) XFree(data);
            return RECEIVE_FAILED;
        }
        if (type == clipboard->incr && !receive->incremental && offset == 0) {
            bool valid = format == 32 && count == 1 && remaining == 0 && data != NULL &&
                         ((const unsigned long *)data)[0] <= TEXT_LIMIT;
            if (data != NULL) XFree(data);
            if (!valid) return RECEIVE_FAILED;
            receive->incremental = true;
            XDeleteProperty(display, receive->window, clipboard->property);
            return RECEIVE_WAIT;
        }
        bool valid = type == receive->target && format == 8 && (count == 0 || data != NULL) &&
                     count <= TEXT_LIMIT - receive->text.length &&
                     remaining <= TEXT_LIMIT - receive->text.length - count &&
                     text_append(&receive->text, data, count);
        if (data != NULL) XFree(data);
        if (!valid) return RECEIVE_FAILED;
        if (remaining == 0) {
            XDeleteProperty(display, receive->window, clipboard->property);
            return receive->incremental && (count > 0 || offset > 0) ? RECEIVE_WAIT : RECEIVE_COMPLETE;
        }
        if (count == 0 || count % 4 != 0) return RECEIVE_FAILED;
        offset += count / 4;
    }
}

static void clipboard_finish_read(Clipboard *clipboard, Platform *platform, Terminal *terminal,
                                  Session *session, const Config *config, bool *redraw)
{
    SelectionReceive *receive = &clipboard->inbound;
    ReceiveResult result = clipboard_read(clipboard, platform->display);
    if (result == RECEIVE_WAIT) {
        receive->deadline = monotonic_milliseconds() + TRANSFER_TIMEOUT;
        return;
    }
    if (result == RECEIVE_COMPLETE) {
        clipboard_paste_text(terminal, session, config, &receive->text,
                             receive->target == XA_STRING, redraw);
    } else terminal->bell_pending = true;
    clipboard_drop_receive(platform->display, receive);
}

static void clipboard_notify(Clipboard *clipboard, Platform *platform, Terminal *terminal,
                             Session *session, const Config *config, const XSelectionEvent *event,
                             bool *redraw)
{
    SelectionReceive *receive = &clipboard->inbound;
    if (event->requestor != receive->window || event->selection != receive->selection ||
        event->target != receive->target) return;
    if (event->property == clipboard->property) {
        clipboard_finish_read(clipboard, platform, terminal, session, config, redraw);
    } else if (event->property == None && receive->target != XA_STRING) {
        receive->target = XA_STRING;
        receive->deadline = monotonic_milliseconds() + TRANSFER_TIMEOUT;
        XConvertSelection(platform->display, receive->selection, receive->target, clipboard->property,
                          receive->window, receive->timestamp);
    } else clipboard_drop_receive(platform->display, receive);
}

static int clipboard_tick(Clipboard *clipboard, Display *display, int64_t now)
{
    int timeout = deadline_timeout(now, clipboard->inbound.deadline);
    if (timeout == 0) {
        clipboard_drop_receive(display, &clipboard->inbound);
        timeout = -1;
    }
    for (size_t i = 0; i < OUTBOUND_TRANSFERS; ++i) {
        SelectionSend *send = &clipboard->outbound[i];
        int next = deadline_timeout(now, send->deadline);
        if (next == 0) clipboard_drop_send(display, send, false);
        else timeout = earlier_timeout(timeout, next);
    }
    return timeout;
}

/* Keyboard translation */

typedef struct {
    unsigned char bytes[16];
    size_t length;
} KeyInput;

static KeyInput keyboard_translate(KeySym key, bool control, bool alt, bool application_arrows,
                                   bool application_keypad, const char *text, int count)
{
    const char *sequence = NULL;
    static const char *const function_keys[] = {
        "\033[11~", "\033[12~", "\033[13~", "\033[14~", "\033[15~", "\033[17~",
        "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~"
    };
    if (key >= XK_F1 && key <= XK_F12) sequence = function_keys[key - XK_F1];
    if (application_keypad && key >= XK_KP_0 && key <= XK_KP_9) {
        return (KeyInput){{0x1b, 'O', (unsigned char)('p' + key - XK_KP_0)}, 3};
    }
    if (application_keypad) {
        char code = 0;
        switch (key) {
        case XK_KP_Insert: code = 'p'; break;
        case XK_KP_End: code = 'q'; break;
        case XK_KP_Down: code = 'r'; break;
        case XK_KP_Page_Down: code = 's'; break;
        case XK_KP_Left: code = 't'; break;
        case XK_KP_Begin: code = 'u'; break;
        case XK_KP_Right: code = 'v'; break;
        case XK_KP_Home: code = 'w'; break;
        case XK_KP_Up: code = 'x'; break;
        case XK_KP_Page_Up: code = 'y'; break;
        case XK_KP_Delete: code = 'n'; break;
        }
        if (code) return (KeyInput){{0x1b, 'O', (unsigned char)code}, 3};
    }
    switch (key) {
    case XK_Return: case XK_KP_Enter:
        if (key == XK_KP_Enter && application_keypad) return (KeyInput){{0x1b, 'O', 'M'}, 3};
        return (KeyInput){{'\r'}, 1};
    case XK_BackSpace: return (KeyInput){{0x7f}, 1};
    case XK_Tab: case XK_KP_Tab: return (KeyInput){{'\t'}, 1};
    case XK_ISO_Left_Tab: sequence = "\033[Z"; break;
    case XK_Escape: return (KeyInput){{0x1b}, 1};
    case XK_Up: case XK_KP_Up: sequence = application_arrows ? "\033OA" : "\033[A"; break;
    case XK_Down: case XK_KP_Down: sequence = application_arrows ? "\033OB" : "\033[B"; break;
    case XK_Right: case XK_KP_Right: sequence = application_arrows ? "\033OC" : "\033[C"; break;
    case XK_Left: case XK_KP_Left: sequence = application_arrows ? "\033OD" : "\033[D"; break;
    case XK_Home: case XK_KP_Home: sequence = "\033[1~"; break;
    case XK_End: case XK_KP_End: sequence = "\033[4~"; break;
    case XK_Insert: case XK_KP_Insert: sequence = "\033[2~"; break;
    case XK_Delete: case XK_KP_Delete: sequence = "\033[3~"; break;
    case XK_Page_Up: case XK_KP_Page_Up: sequence = "\033[5~"; break;
    case XK_Page_Down: case XK_KP_Page_Down: sequence = "\033[6~"; break;
    case XK_KP_Multiply: if (application_keypad) sequence = "\033Oj"; break;
    case XK_KP_Add: if (application_keypad) sequence = "\033Ok"; break;
    case XK_KP_Separator: if (application_keypad) sequence = "\033Ol"; break;
    case XK_KP_Subtract: if (application_keypad) sequence = "\033Om"; break;
    case XK_KP_Decimal: if (application_keypad) sequence = "\033On"; break;
    case XK_KP_Divide: if (application_keypad) sequence = "\033Oo"; break;
    }

    KeyInput input = {0};
    if (sequence != NULL) {
        input.length = strlen(sequence);
        memcpy(input.bytes, sequence, input.length);
    } else if (control && key >= XK_a && key <= XK_z) {
        input.bytes[input.length++] = (unsigned char)(key - XK_a + 1);
    } else if (control && key >= XK_A && key <= XK_Z) {
        input.bytes[input.length++] = (unsigned char)(key - XK_A + 1);
    } else {
        for (int i = 0; i < count && input.length < sizeof input.bytes; ++i) {
            unsigned char byte = (unsigned char)text[i];
            bool printable = (byte >= FIRST_GLYPH && byte <= LAST_GLYPH) || byte >= 0xa0;
            if (printable) {
                size_t length = byte >= 0xa0 ? 2 : 1;
                bool prefix = alt && input.length == 0;
                if (sizeof input.bytes - input.length < length + prefix) break;
                if (prefix) input.bytes[input.length++] = 0x1b;
                if (length == 2) input.bytes[input.length++] = (unsigned char)(0xc0 | (byte >> 6));
                input.bytes[input.length++] = length == 2 ? (unsigned char)(0x80 | (byte & 0x3f)) : byte;
            } else if (control && byte < 0x20) {
                input.bytes[input.length++] = byte;
            }
        }
    }
    return input;
}

static KeyInput keyboard_encode(KeySym key, unsigned int modifiers, bool application_arrows,
                               bool application_keypad, const char *text, int count)
{
    bool control = (modifiers & ControlMask) != 0, alt = (modifiers & Mod1Mask) != 0;
    unsigned int parameter = 1 + !!(modifiers & ShiftMask) + 2u * alt + 4u * control;
    char final = 0;
    unsigned int number = 0;
    if (parameter != 1) {
        static const unsigned int function[] = {15, 17, 18, 19, 20, 21, 23, 24};
        if (key >= XK_F1 && key <= XK_F4) final = (char)('P' + key - XK_F1);
        if (key >= XK_F5 && key <= XK_F12) number = function[key - XK_F5];
        switch (key) {
        case XK_Up: case XK_KP_Up: final = 'A'; break;
        case XK_Down: case XK_KP_Down: final = 'B'; break;
        case XK_Right: case XK_KP_Right: final = 'C'; break;
        case XK_Left: case XK_KP_Left: final = 'D'; break;
        case XK_Home: case XK_KP_Home: final = 'H'; break;
        case XK_End: case XK_KP_End: final = 'F'; break;
        case XK_Insert: case XK_KP_Insert: number = 2; break;
        case XK_Delete: case XK_KP_Delete: number = 3; break;
        case XK_Page_Up: case XK_KP_Page_Up: number = 5; break;
        case XK_Page_Down: case XK_KP_Page_Down: number = 6; break;
        case XK_Tab: case XK_KP_Tab:
            if (modifiers & ShiftMask) key = XK_ISO_Left_Tab;
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
    if (alt && (single_control || key == XK_ISO_Left_Tab)) {
        memmove(input.bytes + 1, input.bytes, input.length);
        ++input.length;
        input.bytes[0] = 0x1b;
    }
    return input;
}

static void keyboard_event(Platform *platform, Terminal *terminal, Session *session,
                           Clipboard *clipboard, const Config *config, XKeyEvent *event,
                           int *font_size, bool *reload, bool *redraw)
{
    char text[sizeof ((KeyInput){0}).bytes];
    KeySym key;
    int count = XLookupString(event, text, sizeof text, &key, NULL);
    unsigned int modifiers = event_modifiers(platform, event->state);
    bool control = (modifiers & ControlMask) != 0;
    bool shift = (modifiers & ShiftMask) != 0;
    bool alt = (modifiers & Mod1Mask) != 0;
    KeySym normalized = shortcut_key(key), unshifted = normalized;
    if (shift && (modifiers == config->reload.modifiers || modifiers == config->zoom_in.modifiers ||
                  modifiers == config->zoom_out.modifiers || modifiers == config->fullscreen.modifiers)) {
        XKeyEvent base = *event;
        char ignored[16];
        base.state &= ~(ShiftMask | LockMask);
        XLookupString(&base, ignored, sizeof ignored, &unshifted, NULL);
        unshifted = shortcut_key(unshifted);
    }
    if (modifiers == config->reload.modifiers &&
        (normalized == config->reload.key || unshifted == config->reload.key)) {
        *reload = true;
        return;
    }
    if (modifiers == config->zoom_in.modifiers &&
        (normalized == config->zoom_in.key || unshifted == config->zoom_in.key)) {
        if (*font_size < MAX_FONT_SIZE) ++*font_size;
        return;
    }
    if (modifiers == config->zoom_out.modifiers &&
        (normalized == config->zoom_out.key || unshifted == config->zoom_out.key)) {
        if (*font_size > MIN_FONT_SIZE) --*font_size;
        return;
    }
    if (config->fullscreen.key != NoSymbol && modifiers == config->fullscreen.modifiers &&
        (normalized == config->fullscreen.key || unshifted == config->fullscreen.key)) {
        platform_fullscreen(platform);
        return;
    }
    if (control && shift && !alt) {
        if (key == XK_c || key == XK_C) {
            clipboard_copy(clipboard, platform, terminal, clipboard->clipboard, event->time);
            return;
        }
        if (key == XK_v || key == XK_V) {
            clipboard_paste(clipboard, platform, terminal, session, config, clipboard->clipboard,
                            event->time, redraw);
            return;
        }
    }
    if (shift && !control && !alt && !terminal->using_alternate) {
        size_t page = terminal->primary.screen.rows;
        switch (key) {
        case XK_Page_Up: case XK_KP_Page_Up: *redraw |= history_scroll(terminal, true, page); return;
        case XK_Page_Down: case XK_KP_Page_Down: *redraw |= history_scroll(terminal, false, page); return;
        case XK_Home: case XK_KP_Home: *redraw |= history_scroll(terminal, true, SIZE_MAX); return;
        case XK_End: case XK_KP_End: *redraw |= history_scroll(terminal, false, SIZE_MAX); return;
        }
    }
    KeyInput input = keyboard_encode(key, modifiers, terminal->application_arrows,
                                     terminal->application_keypad, text, count);
    if (!session_queue(session, input.bytes, input.length)) {
        terminal->bell_pending = true;
    } else if (input.length != 0 && config->scroll_on_input) {
        *redraw |= history_scroll(terminal, false, SIZE_MAX);
    }
}

/* Native X11 pointer input */

typedef struct {
    int x, y;
    Time last_click;
    SelectionPoint last_point;
    unsigned int clicks;
    int64_t deadline;
    bool last_alternate, inside;
    unsigned int buttons, generation;
    size_t report_column, report_row;
    unsigned int report_code;
    bool report_position;
} Mouse;

typedef struct {
    Mouse mouse;
    Clipboard clipboard;
    int font_size;
    bool reload;
    pid_t link_children[8];
    int64_t link_deadline;
} Interaction;

static bool mouse_cell(const TerminalView *view, const FontMetrics *metrics, Padding padding, int x, int y,
                       size_t *column, size_t *row)
{
    if (x < padding.horizontal || y < padding.vertical) return false;
    *column = (size_t)((x - padding.horizontal) / metrics->width);
    *row = (size_t)((y - padding.vertical) / metrics->height);
    return *column < view->screen->columns && *row < view->screen->rows;
}

static uint16_t mouse_link(Terminal *terminal, const Config *config, const FontMetrics *metrics, int x, int y)
{
    if (!config->hyperlinks || terminal->links == NULL) return 0;
    TerminalView view = terminal_view(terminal);
    size_t column, row;
    if (!mouse_cell(&view, metrics, config->padding, x, y, &column, &row)) return 0;
    LineView line = view_line(&view, view.first_visible + row);
    return view_cell(&view, &line, column)->hyperlink;
}

static void link_reap(Interaction *ui)
{
    for (size_t i = 0; i < sizeof ui->link_children / sizeof ui->link_children[0]; ++i) {
        pid_t *child = &ui->link_children[i];
        if (*child == 0) continue;
        pid_t result;
        do { result = waitpid(*child, NULL, WNOHANG); } while (result < 0 && errno == EINTR);
        if (result == *child || (result < 0 && errno == ECHILD)) *child = 0;
    }
}

static void link_open(Interaction *ui, const Session *session, const char *url)
{
    extern char **environ;
    link_reap(ui);
    for (size_t i = 0; i < sizeof ui->link_children / sizeof ui->link_children[0]; ++i) {
        if (ui->link_children[i] != 0) continue;
        posix_spawnattr_t attributes;
        int error = posix_spawnattr_init(&attributes);
        if (error == 0) {
            error = posix_spawnattr_setsigmask(&attributes, &session->original_signal_mask);
            if (error == 0) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
            if (error == 0) {
                char *const arguments[] = {"xdg-open", (char *)url, NULL};
                pid_t child;
                error = posix_spawnp(&child, arguments[0], NULL, &attributes, arguments, environ);
                if (error == 0) ui->link_children[i] = child;
            }
            posix_spawnattr_destroy(&attributes);
        }
        if (error != 0) fprintf(stderr, "deal: could not start xdg-open: %s\n", strerror(error));
        return;
    }
}

typedef enum { MOUSE_PRESS, MOUSE_RELEASE, MOUSE_MOTION } MouseAction;

static bool mouse_report(Mouse *mouse, const Platform *platform, Terminal *terminal, Session *session,
                         const Config *config, const FontMetrics *metrics, MouseAction action,
                         unsigned int button, unsigned int state)
{
    if (mouse->generation != terminal->mouse_generation) {
        mouse->generation = terminal->mouse_generation;
        mouse->buttons = 0;
        mouse->report_position = false;
    }
    unsigned int modifiers = event_modifiers(platform, state);
    bool override = (modifiers & config->mouse_override) || terminal->selection.dragging;
    if (!config->mouse_reporting || terminal->mouse_mode == 0 ||
        (!terminal->using_alternate && terminal->history.offset != 0) || override) {
        if (action == MOUSE_RELEASE && button >= Button1 && button <= Button3) {
            mouse->buttons &= ~(1u << (button - Button1));
            mouse->report_position = false;
        }
        return false;
    }

    TerminalView view = terminal_view(terminal);
    size_t column = 0, row = 0;
    bool in_grid = mouse->x < platform->width - config->padding.horizontal &&
                   mouse->y < platform->height - config->padding.vertical &&
                   mouse_cell(&view, metrics, config->padding, mouse->x, mouse->y, &column, &row);
    bool in_encoding = in_grid && (terminal->mouse_sgr || (column < 223 && row < 223));
    unsigned int code;
    if (action == MOUSE_MOTION) {
        if (!in_encoding || terminal->mouse_mode == 1000 ||
            (terminal->mouse_mode == 1002 && mouse->buttons == 0)) return true;
        code = 32 + (mouse->buttons & 1 ? 0 : mouse->buttons & 2 ? 1 : mouse->buttons & 4 ? 2 : 3);
    } else if (button >= Button1 && button <= Button3) {
        code = button - Button1;
        unsigned int bit = 1u << code;
        if (action == MOUSE_RELEASE) {
            bool forwarded = (mouse->buttons & bit) != 0;
            mouse->buttons &= ~bit;
            if (!forwarded) return true;
            if (!terminal->mouse_sgr) code = 3;
        }
    } else if (action == MOUSE_PRESS && button >= Button4 && button <= 7) {
        code = 64 + button - Button4;
    } else return true;
    if (!in_encoding) {
        mouse->report_position = false;
        return true;
    }
    code |= (modifiers & ShiftMask ? 4 : 0) | (modifiers & Mod1Mask ? 8 : 0) |
            (modifiers & ControlMask ? 16 : 0);
    if (action == MOUSE_MOTION && mouse->report_position && mouse->report_code == code &&
        mouse->report_column == column && mouse->report_row == row) return true;
    mouse->report_position = false;

    unsigned char bytes[32];
    size_t count;
    if (terminal->mouse_sgr) {
        count = (size_t)snprintf((char *)bytes, sizeof bytes, "\033[<%u;%zu;%zu%c",
                                 code, column + 1, row + 1, action == MOUSE_RELEASE ? 'm' : 'M');
    } else {
        bytes[0] = 0x1b; bytes[1] = '['; bytes[2] = 'M';
        bytes[3] = (unsigned char)(32 + code);
        bytes[4] = (unsigned char)(33 + column);
        bytes[5] = (unsigned char)(33 + row);
        count = 6;
    }
    if (session_queue(session, bytes, count)) {
        if (action == MOUSE_PRESS && button >= Button1 && button <= Button3)
            mouse->buttons |= 1u << (button - Button1);
        mouse->report_position = action == MOUSE_MOTION;
        mouse->report_column = column;
        mouse->report_row = row;
        mouse->report_code = code;
    }
    return true;
}

static bool platform_focus(Platform *platform, Terminal *terminal, const XFocusChangeEvent *event)
{
    if (event->window != platform->window || event->mode == NotifyGrab ||
        event->mode == NotifyUngrab || event->detail == NotifyInferior) return false;
    bool focused = event->type == FocusIn;
    if (focused == platform->focused) return false;
    platform->focused = focused;
    if (terminal->focus_reporting) terminal_reply(terminal, focused ? "\033[I" : "\033[O");
    return true;
}

static bool mouse_extend(Mouse *mouse, Terminal *terminal, const FontMetrics *metrics, Padding padding)
{
    TerminalView view = terminal_view(terminal);
    SelectionPoint point = selection_point(&view, metrics, padding, mouse->x, mouse->y);
    if (point.line != terminal->selection.origin.line || point.column != terminal->selection.origin.column) {
        mouse->clicks = 0;
    }
    return selection_extend(terminal, point);
}

static void mouse_press(Interaction *ui, Platform *platform, Terminal *terminal, Session *session,
                        const Config *config, const FontMetrics *metrics, const XButtonEvent *event,
                        bool *redraw)
{
    Mouse *mouse = &ui->mouse;
    mouse->x = event->x;
    mouse->y = event->y;
    mouse->inside = true;
    unsigned int modifiers = event_modifiers(platform, event->state);
    if (event->button == Button1 && modifiers == config->link_modifier) {
        const char *url = link_url(terminal, mouse_link(terminal, config, metrics, mouse->x, mouse->y));
        if (url != NULL) {
            link_open(ui, session, url);
            return;
        }
    }
    if (mouse_report(mouse, platform, terminal, session, config, metrics,
                     MOUSE_PRESS, event->button, event->state)) return;
    switch (event->button) {
    case Button1: {
        TerminalView view = terminal_view(terminal);
        SelectionPoint point = selection_point(&view, metrics, config->padding, mouse->x, mouse->y);
        if (terminal->using_alternate == mouse->last_alternate && point.line == mouse->last_point.line &&
            point.column == mouse->last_point.column &&
            (uint32_t)(event->time - mouse->last_click) <= 350) mouse->clicks = mouse->clicks % 3 + 1;
        else mouse->clicks = 1;
        mouse->last_point = point;
        mouse->last_alternate = terminal->using_alternate;
        mouse->last_click = event->time;
        mouse->deadline = 0;
        SelectionMode mode = mouse->clicks == 3 ? SELECT_LINE : mouse->clicks == 2 ? SELECT_WORD : SELECT_CHARACTER;
        selection_start(terminal, point, mode);
        *redraw = true;
        break;
    }
    case Button2:
        clipboard_paste(&ui->clipboard, platform, terminal, session, config, XA_PRIMARY, event->time, redraw);
        break;
    case Button4: case Button5:
        if (history_scroll(terminal, event->button == Button4, 3)) {
            if (terminal->selection.dragging) mouse_extend(mouse, terminal, metrics, config->padding);
            *redraw = true;
        }
        break;
    }
}

static int mouse_tick(Mouse *mouse, Terminal *terminal, const FontMetrics *metrics,
                      Padding padding, int64_t now, bool *redraw)
{
    const History *history = &terminal->history;
    int bottom = padding.vertical + (int)terminal->primary.screen.rows * metrics->height;
    bool up = mouse->y < padding.vertical && history->offset < history->count;
    bool down = mouse->y >= bottom && history->offset > 0;
    if (!terminal->selection.dragging || terminal->using_alternate || (!up && !down) || now < 0) {
        mouse->deadline = 0;
        return -1;
    }
    if (mouse->deadline == 0) mouse->deadline = now + 55;
    if (now >= mouse->deadline) {
        int distance = up ? padding.vertical - mouse->y : mouse->y - bottom;
        size_t lines = 1 + minimum((size_t)(distance / metrics->height), 7);
        if (history_scroll(terminal, up, lines)) {
            mouse_extend(mouse, terminal, metrics, padding);
            *redraw = true;
        }
        mouse->deadline = now + 55;
    }
    return deadline_timeout(now, mouse->deadline);
}

/* Application lifecycle */

static bool platform_events(Platform *platform, Terminal *terminal, Session *session,
                            Interaction *ui, const Config *config, const FontMetrics *metrics,
                            bool *redraw)
{
    Display *display = platform->display;
    Clipboard *clipboard = &ui->clipboard;
    for (unsigned int i = 0; i < 128 && XPending(display) > 0; ++i) {
        XEvent event;
        XNextEvent(display, &event);
        switch (event.type) {
        case ClientMessage:
            if (event.xclient.window == platform->window &&
                event.xclient.message_type == platform->wm_protocols &&
                event.xclient.format == 32 &&
                (Atom)event.xclient.data.l[0] == platform->wm_delete_window) {
                return false;
            }
            break;
        case DestroyNotify:
            if (event.xdestroywindow.window == platform->window) {
                platform->window = None;
                return false;
            }
            for (size_t slot = 0; slot < OUTBOUND_TRANSFERS; ++slot) {
                SelectionSend *send = &clipboard->outbound[slot];
                if (send->requestor == event.xdestroywindow.window) clipboard_drop_send(display, send, true);
            }
            break;
        case ConfigureNotify:
            if (event.xconfigure.window == platform->window &&
                event.xconfigure.width > 0 && event.xconfigure.width <= USHRT_MAX &&
                event.xconfigure.height > 0 && event.xconfigure.height <= USHRT_MAX &&
                (platform->width != event.xconfigure.width ||
                 platform->height != event.xconfigure.height)) {
                platform->width = event.xconfigure.width;
                platform->height = event.xconfigure.height;
                *redraw = true;
            }
            break;
        case Expose:
            if (event.xexpose.window == platform->window) *redraw = true;
            break;
        case MapNotify:
            if (event.xmap.window == platform->window) {
                platform->mapped = true;
                *redraw = true;
            }
            break;
        case UnmapNotify:
            if (event.xunmap.window == platform->window) {
                platform->mapped = false;
                ui->mouse.inside = false;
                ui->mouse.buttons = 0;
                ui->mouse.report_position = false;
                terminal->selection.dragging = false;
            }
            break;
        case KeyPress:
            keyboard_event(platform, terminal, session, clipboard, config, &event.xkey,
                           &ui->font_size, &ui->reload, redraw);
            break;
        case ButtonPress:
            mouse_press(ui, platform, terminal, session, config, metrics, &event.xbutton, redraw);
            break;
        case MotionNotify:
            ui->mouse.x = event.xmotion.x;
            ui->mouse.y = event.xmotion.y;
            if (terminal->selection.dragging) {
                *redraw |= mouse_extend(&ui->mouse, terminal, metrics, config->padding);
            } else {
                mouse_report(&ui->mouse, platform, terminal, session, config, metrics,
                             MOUSE_MOTION, 0, event.xmotion.state);
            }
            break;
        case EnterNotify: case LeaveNotify:
            ui->mouse.inside = event.type == EnterNotify;
            ui->mouse.x = event.xcrossing.x;
            ui->mouse.y = event.xcrossing.y;
            ui->mouse.report_position = false;
            break;
        case ButtonRelease:
            ui->mouse.x = event.xbutton.x;
            ui->mouse.y = event.xbutton.y;
            if (event.xbutton.button == Button1 && terminal->selection.dragging) {
                *redraw |= mouse_extend(&ui->mouse, terminal, metrics, config->padding);
                terminal->selection.dragging = false;
                clipboard_copy(clipboard, platform, terminal, XA_PRIMARY, event.xbutton.time);
            } else {
                mouse_report(&ui->mouse, platform, terminal, session, config, metrics,
                             MOUSE_RELEASE, event.xbutton.button, event.xbutton.state);
            }
            break;
        case FocusIn: case FocusOut:
            *redraw |= platform_focus(platform, terminal, &event.xfocus);
            break;
        case SelectionRequest:
            clipboard_request(clipboard, platform, &event.xselectionrequest);
            break;
        case SelectionNotify:
            clipboard_notify(clipboard, platform, terminal, session, config, &event.xselection, redraw);
            break;
        case SelectionClear: {
            OwnedSelection *owned = clipboard_owner(clipboard, event.xselectionclear.selection);
            if (owned != NULL && !timestamp_before(event.xselectionclear.time, owned->timestamp)) {
                text_destroy(&owned->text);
                if (event.xselectionclear.selection == XA_PRIMARY && terminal->selection.active &&
                    !terminal->selection.dragging) {
                    terminal->selection.active = false;
                    *redraw = true;
                }
            }
            break;
        }
        case PropertyNotify:
            if (event.xproperty.state == PropertyDelete) {
                clipboard_send_chunk(clipboard, display, &event.xproperty);
            } else if (clipboard->inbound.incremental && event.xproperty.window == clipboard->inbound.window &&
                       event.xproperty.atom == clipboard->property) {
                clipboard_finish_read(clipboard, platform, terminal, session, config, redraw);
            }
            break;
        case MappingNotify:
            XRefreshKeyboardMapping(&event.xmapping);
            if (event.xmapping.request != MappingPointer) platform_modifiers(platform);
            break;
        }
    }
    return true;
}

typedef struct {
    int64_t next, flash_end;
} Bell;

static void platform_urgency(Platform *platform, bool enabled)
{
    XWMHints *existing = XGetWMHints(platform->display, platform->window);
    XWMHints hints = existing != NULL ? *existing : (XWMHints){0};
    if (existing != NULL) XFree(existing);
    if (enabled) hints.flags |= XUrgencyHint;
    else hints.flags &= ~XUrgencyHint;
    XSetWMHints(platform->display, platform->window, &hints);
    platform->urgent = enabled;
}

static int bell_tick(Bell *bell, Platform *platform, Terminal *terminal, const Config *config,
                    int64_t now, bool *redraw)
{
    if (platform->urgent && (platform->focused || !config->bell_urgency))
        platform_urgency(platform, false);
    if (bell->flash_end && (now < 0 || now >= bell->flash_end || !config->visual_bell)) {
        bell->flash_end = 0;
        *redraw = true;
    }
    bool pending = terminal->bell_pending;
    terminal->bell_pending = false;
    if (pending && now >= 0 && now >= bell->next &&
        (config->audible_bell || config->visual_bell || config->bell_urgency)) {
        bell->next = now + 250;
        if (config->audible_bell) XBell(platform->display, 0);
        if (config->visual_bell && platform->mapped) {
            bell->flash_end = now + 100;
            *redraw = true;
        }
        if (config->bell_urgency && !platform->focused && !platform->urgent)
            platform_urgency(platform, true);
    }
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

static int platform_compositor_tick(Platform *platform, const Config *config, int64_t now,
                                    bool *redraw)
{
    if (!platform->argb || config->opacity >= 1.0f || !platform->mapped) {
        platform->compositor_check = 0;
        platform->composited = false;
        return -1;
    }
    if (platform->compositor_check == 0 || (now >= 0 && now >= platform->compositor_check)) {
        bool active = XGetSelectionOwner(platform->display, platform->compositor_selection) != None;
        *redraw |= active != platform->composited;
        platform->composited = active;
        platform->compositor_check = now >= 0 ? now + 2000 : 1;
    }
    return deadline_timeout(now, platform->compositor_check);
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
    const char *error = font_open(&next, &zoomed);
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
    size_t capacity = (size_t)next.history_capacity;
    bool change_history = capacity != terminal->history.capacity;
    if (change_history && capacity != 0) {
        lines = calloc(capacity, sizeof(*lines));
        if (lines == NULL) {
            error = "could not allocate the scrollback ring";
            goto failed;
        }
    }
    int size = next.font_size == config->font_size ? renderer->size : next.font_size;
    bool change_font = size != renderer->size || strcmp(font_family(&next), font_family(config)) != 0 ||
                       next.letter_spacing != config->letter_spacing || next.line_spacing != config->line_spacing;
    if (change_font) {
        Config sized = next;
        sized.font_size = size;
        error = font_open(&font, &sized);
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

static const char *run(Platform *platform, Terminal *terminal, Session *session,
                       Config *config, FontRenderer *renderer, Interaction *ui, char *zoom_path)
{
    Display *display = platform->display;
    int x11_fd = ConnectionNumber(display);
    int width = platform->width, height = platform->height;
    if (!terminal_resize_window(terminal, width, height, config->padding, &renderer->metrics)) {
        return "could not allocate the screen buffer";
    }
    const char *error = session_open(session, session_size(&terminal->primary.screen, width, height,
                                                           config->padding), x11_fd, config);
    if (error != NULL) {
        return error;
    }
    terminal->reply = session_reply;
    terminal->reply_context = session;
    ui->font_size = renderer->size;

    bool redraw = true;
    bool output_dirty = false;
    bool pty_hangup = false;
    int64_t next_frame = 0, drain_deadline = 0;
    CursorBlink blink = {0};
    Bell bell = {0};
    for (;;) {
        if (!platform_events(platform, terminal, session, ui, config, &renderer->metrics, &redraw)) {
            return NULL;
        }
        bool reloaded = false;
        if (ui->reload) {
            ui->reload = false;
            bool decorated = config->decorations;
            reloaded = config_reload(config, terminal, renderer, platform);
            if (reloaded) {
                ui->font_size = renderer->size;
                if (decorated != config->decorations) platform_decorations(platform, config->decorations);
            }
            redraw |= reloaded;
        }
        bool zoomed = false;
        if (ui->font_size != renderer->size) {
            zoomed = font_zoom(renderer, terminal, config, platform, ui->font_size);
            ui->font_size = renderer->size;
            redraw |= zoomed;
            if (zoomed) config_save_zoom(zoom_path, config->font_size, renderer->size);
        }
        if (reloaded || zoomed) platform_size_hints(platform, config->padding, &renderer->metrics);
        if (reloaded || zoomed || width != platform->width || height != platform->height) {
            width = platform->width;
            height = platform->height;
            if (!reloaded && !zoomed && !terminal_resize_window(terminal, width, height, config->padding, &renderer->metrics)) {
                return "could not resize the screen buffer";
            }
            PtyResult status = session_resize(session, session_size(&terminal->primary.screen, width,
                                                                     height, config->padding));
            if (status != PTY_OPEN) {
                return status == PTY_FAILED ? "could not resize the pseudoterminal"
                                            : session_check_startup(session);
            }
        }
        int64_t now = monotonic_milliseconds();
        if (terminal->title_dirty) {
            platform_title(platform, config, terminal->dynamic_title);
            terminal->title_dirty = false;
        }
        int timeout = clipboard_tick(&ui->clipboard, display, now);
        timeout = earlier_timeout(timeout, bell_tick(&bell, platform, terminal, config, now, &redraw));
        timeout = earlier_timeout(timeout, platform_compositor_tick(platform, config, now, &redraw));
        if (terminal->links_dirty) {
            if (ui->link_deadline == 0) ui->link_deadline = now + 500;
            if (now < 0 || now >= ui->link_deadline) {
                links_collect(terminal);
                ui->link_deadline = 0;
            } else timeout = earlier_timeout(timeout, deadline_timeout(now, ui->link_deadline));
        }
        timeout = earlier_timeout(timeout, mouse_tick(&ui->mouse, terminal, &renderer->metrics,
                                                     config->padding, now, &redraw));
        uint16_t hover = ui->mouse.inside && !terminal->selection.dragging ?
            mouse_link(terminal, config, &renderer->metrics, ui->mouse.x, ui->mouse.y) : 0;
        if (hover != terminal->hovered_link) {
            terminal->hovered_link = hover;
            redraw = true;
        }
        const TerminalBuffer *buffer = terminal_buffer(terminal);
        TerminalView view = terminal_view(terminal);
        if (platform->mapped && output_dirty) {
            int frame_timeout = deadline_timeout(now, next_frame);
            if (frame_timeout <= 0) redraw = true;
            else timeout = earlier_timeout(timeout, frame_timeout);
        }
        timeout = earlier_timeout(timeout, cursor_timeout(&blink, platform->mapped && platform->focused &&
                                    buffer->cursor_visible && buffer->cursor_blink &&
                                    view.first_visible == view.first_live, config->blink_interval, now, &redraw));
        if (redraw && platform->mapped) {
            render_screen(renderer, &view, width, height, buffer->cursor.column, buffer->cursor.row,
                          buffer->cursor_visible && blink.visible, buffer->cursor_style, config,
                          platform->composited ? config->opacity : 1.0f);
            if (bell.flash_end) render_bell(width, height, config->cursor);
            glXSwapBuffers(display, platform->glx_window != None ? platform->glx_window : platform->window);
            redraw = false;
            output_dirty = false;
            next_frame = now >= 0 ? now + 16 : 0;
        }

        enum { X11, PTY, CHILD, STARTUP };
        struct pollfd events[] = {
            [X11] = {x11_fd, POLLIN, 0},
            [PTY] = {session->master, POLLIN | (session->input_length > 0 ? POLLOUT : 0), 0},
            [CHILD] = {session->child_signal, POLLIN, 0},
            [STARTUP] = {session->startup, POLLIN, 0}
        };
        bool parser_pending = session->output_start < session->output_end;
        if (parser_pending || pty_hangup || session->child == 0 || XPending(display) > 0) timeout = 0;
        int ready = poll(events, sizeof events / sizeof events[0], timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return "could not wait for X11 and PTY activity";
        }
        if (events[X11].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            return "lost the X11 connection";
        }
        if (events[STARTUP].revents != 0) {
            error = session_check_startup(session);
            if (error != NULL) {
                return error;
            }
        }
        if (events[CHILD].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            return "lost the shell exit notification descriptor";
        }
        if (events[CHILD].revents & POLLIN) {
            struct signalfd_siginfo signal_info;
            ssize_t count;
            do {
                count = read(session->child_signal, &signal_info, sizeof signal_info);
            } while (count < 0 && errno == EINTR);
            session_reap(session, WNOHANG);
            link_reap(ui);
            if (session->child == 0 && drain_deadline == 0) drain_deadline = monotonic_milliseconds() + 250;
        }
        if (events[PTY].revents & POLLNVAL) {
            return "lost the pseudoterminal descriptor";
        }
        pty_hangup |= (events[PTY].revents & (POLLHUP | POLLERR)) != 0;
        /* A hangup still allows queued bytes to be read. Only time out a slave that remains open. */
        if (!pty_hangup && drain_deadline != 0 && monotonic_milliseconds() >= drain_deadline) {
            return session_check_startup(session);
        }
        if ((events[PTY].revents & POLLIN) || parser_pending || pty_hangup || session->child == 0) {
            PtyResult status = session_read(session, terminal, &output_dirty, pty_hangup || session->child == 0);
            if (status == PTY_FAILED) {
                return "could not read from the pseudoterminal";
            }
            if (status == PTY_CLOSED) {
                return session_check_startup(session);
            }
        }
        if (events[PTY].revents & POLLOUT) {
            PtyResult status = session_write(session);
            if (status == PTY_FAILED) return "could not write to the pseudoterminal";
            if (status == PTY_CLOSED) {
                session->input_length = 0;
                pty_hangup = true;
            }
        }
    }
}

static void print_help(void)
{
    puts("Usage: deal [OPTIONS] [-e COMMAND [ARGS...]]\n"
         "  --help                    Show this help\n"
         "  --version                 Show the version\n"
         "  --config PATH             Read a different Lua configuration\n"
         "  --working-directory PATH  Start the child in PATH\n"
         "  --title TITLE             Set the initial window title\n"
         "  --class CLASS             Set the X11 window class\n"
         "  --columns N               Set the initial grid width\n"
         "  --rows N                  Set the initial grid height\n"
         "  --diagnostics             Print build, X11, rendering and terminal details\n"
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
        else if (strcmp(key, "--title") == 0 || strcmp(key, "--class") == 0) {
            size_t length = strlen(value);
            if (length > TITLE_LIMIT || !osc_text_valid(value, length, false)) {
                fprintf(stderr, "deal: invalid value for %s\n", key);
                return 2;
            }
            if (strcmp(key, "--title") == 0) options->title = value;
            else options->window_class = value;
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

static void print_diagnostics(const Config *config, const FontRenderer *renderer,
                              const Platform *platform, const char *graphical_error)
{
    char *default_path = config->options->config_path == NULL ? config_path("init.lua", true) : NULL;
    const char *path = config->options->config_path != NULL ? config->options->config_path : default_path;
    char packaged[PATH_MAX];
    const char *term = config_term(config, packaged);
    printf("deal %s\nBuild platform: Linux X11 (C11, %zu-bit)\nConfiguration: %s\nTERM: %s%s\n",
           deal_version, sizeof(void *) * CHAR_BIT, path != NULL ? path : "unavailable", term,
           config->custom_term && strcmp(term, "deal") != 0 ? " (deal terminfo unavailable)" : "");
    free(default_path);
    printf("X11: %s\n", platform->display != NULL ? "available" : "unavailable");
    if (platform->compositor_selection != None)
        printf("Compositor: %s\nARGB visual: %s\n",
               XGetSelectionOwner(platform->display, platform->compositor_selection) != None ? "available" : "unavailable",
               platform->argb ? "available" : "unavailable");
    else puts("Compositor: unavailable");
    if (glXGetCurrentContext() != NULL) {
        const char *vendor = (const char *)glGetString(GL_VENDOR);
        const char *name = (const char *)glGetString(GL_RENDERER);
        const char *version = (const char *)glGetString(GL_VERSION);
        bool software = name != NULL && (strcasestr(name, "llvmpipe") || strcasestr(name, "softpipe") ||
                                          strcasestr(name, "swrast") || strcasestr(name, "software"));
        printf("OpenGL vendor: %s\nOpenGL renderer: %s\nOpenGL version: %s\nRendering: %s\n",
               vendor ? vendor : "unknown", name ? name : "unknown", version ? version : "unknown",
               software ? "software" : glXIsDirect(platform->display, platform->context)
                   ? "direct GLX (no software renderer detected)" : "indirect GLX");
    } else puts("OpenGL: unavailable");
    if (renderer->faces != NULL && renderer->metrics.width > 0) {
        FT_Face face = renderer->faces[renderer->primary].face;
        printf("Font: %s, %d px\nCell: %d × %d px\n",
               face != NULL && face->family_name != NULL ? face->family_name : font_family(config),
               renderer->size, renderer->metrics.width, renderer->metrics.height);
    } else puts("Font: unavailable");
    if (graphical_error != NULL) printf("Startup probe: %s\n", graphical_error);
    puts("Features: UTF-8, ANSI 256/RGB, alternate screen, scrollback reflow, mouse and focus reporting, "
         "bracketed paste, OSC titles and links, X11 clipboard, fullscreen, Lua reload, configurable bell");
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
    Platform platform = {0};
    Interaction ui = {0};
    FontRenderer renderer = {0};
    Terminal terminal;
    terminal_init(&terminal, config.foreground, config.background, config.cursor,
                  config.cursor_style, config.blink);
    terminal_configure(&terminal, &config);
    terminal.title_dirty = false;
    Session session = {.master = -1, .child_signal = -1, .startup = -1};
    Config initial_font = config;
    initial_font.font_size = config_read_zoom(zoom_path, config.font_size);
    const char *error = font_open(&renderer, &initial_font);
    if (error != NULL && initial_font.font_size != config.font_size) {
        font_close(&renderer);
        renderer = (FontRenderer){0};
        error = font_open(&renderer, &config);
    }
    if (error == font_spacing_error) {
        fprintf(stderr, "deal: %s; using default spacing\n", error);
        font_close(&renderer);
        renderer = (FontRenderer){0};
        config.letter_spacing = config.line_spacing = 0;
        error = font_open(&renderer, &config);
    }
    if (error == NULL) {
        error = platform_open(&platform, &config, &renderer.metrics);
    }
    if (options.diagnostics) {
        print_diagnostics(&config, &renderer, &platform, error);
        font_close(&renderer);
        platform_close(&platform);
        config_destroy(&config);
        free(zoom_path);
        return EXIT_SUCCESS;
    }
    if (error == NULL) {
        error = font_make_atlas(&renderer);
    }
    if (error == NULL) {
        if (!clipboard_open(&ui.clipboard, platform.display)) error = "could not initialize X11 selections";
        else if (config.history_capacity > 0) {
            terminal.history.lines = calloc((size_t)config.history_capacity, sizeof(*terminal.history.lines));
            if (terminal.history.lines == NULL) error = "could not allocate the scrollback ring";
            else terminal.history.capacity = (size_t)config.history_capacity;
        }
    }
    if (error == NULL) {
        error = run(&platform, &terminal, &session, &config, &renderer, &ui, zoom_path);
    }

    link_reap(&ui);
    session_close(&session);
    clipboard_close(&ui.clipboard, platform.display);
    links_clear(&terminal);
    history_clear(&terminal.history);
    free(terminal.history.lines);
    terminal_screen_destroy(&terminal.primary.screen);
    terminal_screen_destroy(&terminal.alternate.screen);
    font_close(&renderer);
    platform_close(&platform);
    config_destroy(&config);
    free(zoom_path);
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
