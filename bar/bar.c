#define _GNU_SOURCE

#include "bar.h"
#include "dwl-ipc-client-protocol.h"
#include "layer-shell-client-protocol.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include <ft2build.h>
#include FT_FREETYPE_H

static const unsigned char bundled_config[] = {
#include "default-config.h"
    0
};

struct buffer {
    struct wl_list link;
    struct wl_buffer *proxy;
};

struct text_bounds {
    int left, width;
};

struct workspace_state {
    uint32_t active, occupied, urgent;
};

struct workspace_view {
    uint32_t visible, underlined, boxed, colors[BAR_MAX_WORKSPACES];
};

struct monitor {
    struct monitor *next;
    char name[128];
    struct workspace_state workspaces;
};

struct output {
    struct wl_list link;
    struct wl_output *proxy;
    uint32_t name;
    bool entered;
};

struct hit_span {
    int start, end;
};

struct hit_map {
    int height;
    struct hit_span tags[BAR_MAX_WORKSPACES];
    struct hit_span network, volume;
};

struct command {
    char text[BAR_NETWORK_COMMAND_SIZE];
    size_t offsets[BAR_NETWORK_COMMAND_SIZE / 2];
    size_t count;
};

enum {
    NM_PATH_SIZE = 256, NM_CONNECTIONS = 32, NM_SSID_SIZE = 33,
    NETWORK_LABEL_SIZE = BAR_NETWORK_TEXT_SIZE + NM_SSID_SIZE + 6,
};
enum network_kind { NETWORK_OFFLINE, NETWORK_ETHERNET, NETWORK_WIFI, NETWORK_OTHER };

struct network {
    sd_bus *bus;
    sd_bus_slot *request;
    int64_t retry;
    bool refresh, refresh_ap, changed, other;
    char paths[NM_CONNECTIONS][NM_PATH_SIZE];
    size_t count, index;
    char device[NM_PATH_SIZE], ap[NM_PATH_SIZE];
    enum network_kind kind;
    int strength;
    char ssid[NM_SSID_SIZE];
};

enum { VOLUME_LABEL_SIZE = BAR_VOLUME_TEXT_SIZE + 16 };

/* The [volume] command's output: the last line it printed. */
struct volume_feed {
    pid_t pid;
    int fd;
    int64_t started, retry, delay;
    char partial[256];
    size_t used;
    bool overflow, have, changed, muted;
    int percent;
    char raw[VOLUME_LABEL_SIZE];  /* A line that isn't wpctl's format, shown as it is. */
};

struct appearance {
    struct bar_config config;
    FT_Face font;
    struct text_bounds labels[BAR_MAX_WORKSPACES], right_bounds, network_bounds, volume_bounds;
    /* The right text is volume, network and clock; these place the first two in it. */
    size_t network_length, network_offset, volume_length;
    int network_x, volume_advance;
    bool volume_muted;
    struct command network_command;
    struct command volume_command, volume_click, volume_middle_click;
    struct command volume_scroll_up, volume_scroll_down;
    char right_text[BAR_CLOCK_TEXT_SIZE + 2 * BAR_NETWORK_SEPARATOR_SIZE + NETWORK_LABEL_SIZE +
                    VOLUME_LABEL_SIZE];
    uint32_t background;
};

enum theme_role {
    THEME_BACKGROUND, THEME_FOREGROUND, THEME_ACCENT, THEME_HIGHLIGHT, THEME_MUTED,
    THEME_SELECTION_BACKGROUND, THEME_SELECTION_FOREGROUND, THEME_BORDER_ACTIVE,
    THEME_BORDER_INACTIVE, THEME_URGENT, THEME_ROLE_COUNT,
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

struct bar {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_callback *sync;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wl_list outputs;
    struct output *ipc_target;
    struct zdwl_ipc_manager_v2 *ipc_manager;
    struct zdwl_ipc_output_v2 *ipc_output;
    uint32_t ipc_name, ipc_tags;
    struct workspace_state ipc_current, ipc_pending;
    bool ipc_ready;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    uint32_t seat_name;
    bool pointer_inside;
    double pointer_x, pointer_y;
    double axis_delta, axis_remainder;
    int64_t axis_value120, wheel_remainder;
    bool axis_stop;
    int scroll_target;  /* SCROLL_NONE, SCROLL_TAGS or SCROLL_VOLUME */
    struct hit_map hits;
    struct wl_list buffers;
    FT_Library freetype;
    struct appearance appearance, pending;
    struct network network;
    pid_t network_child;
    struct volume_feed volume;
    sigset_t child_mask;
    FcConfig *font_config;
    char *config_path;
    struct theme_watch theme_watch;
    uint32_t theme_palette[THEME_ROLE_COUNT];
    bool have_palette;
    int64_t clock_next;
    struct monitor *monitors, *selected, *first_monitor;
    struct workspace_view workspaces;
    char status_line[256];
    size_t status_length;
    bool status_invalid;
    uint32_t width;
    bool redraw, layout_dirty;
    bool closed;
    const char *error;
};

static char *trim(char *text)
{
    while (isspace((unsigned char)*text))
        ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

static const char *const theme_names[THEME_ROLE_COUNT] = {
    "background", "foreground", "accent", "highlight", "muted",
    "selection_background", "selection_foreground", "border_active",
    "border_inactive", "urgent",
};

static char *theme_palette_path(void)
{
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    char *path = NULL;
    if (cache && cache[0] == '/') {
        if (asprintf(&path, "%s/wall/colors.conf", cache) < 0)
            return NULL;
    } else if (home && home[0] == '/') {
        if (asprintf(&path, "%s/.cache/wall/colors.conf", home) < 0)
            return NULL;
    }
    return path;
}

static bool parse_theme_palette(FILE *file, uint32_t rgb[THEME_ROLE_COUNT])
{
    char text[1025];
    size_t length = fread(text, 1, sizeof text, file);
    if (length == sizeof text || !feof(file) || ferror(file) || memchr(text, '\0', length))
        return false;
    text[length] = '\0';

    uint32_t colors[THEME_ROLE_COUNT] = {0};
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
        while (i < THEME_ROLE_COUNT && strcmp(key, theme_names[i]))
            ++i;
        if (i == THEME_ROLE_COUNT || (seen & (1u << i)))
            return false;
        if (value[0] != '#' || strlen(value) != 7 ||
            strspn(value + 1, "0123456789abcdefABCDEF") != 6)
            return false;
        colors[i] = (uint32_t)strtoul(value + 1, NULL, 16);
        seen |= 1u << i;
    }
    if (seen != (1u << THEME_ROLE_COUNT) - 1)
        return false;
    memcpy(rgb, colors, sizeof colors);
    return true;
}

static bool load_theme_palette(const char *path, uint32_t rgb[THEME_ROLE_COUNT])
{
    if (!path)
        return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st;
    FILE *file = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? fdopen(fd, "rb") : NULL;
    if (!file) {
        close(fd);
        return false;
    }
    bool ok = parse_theme_palette(file, rgb);
    fclose(file);
    return ok;
}

enum section { BAR_SECTION, FONT_SECTION, WORKSPACE_SECTION, CLOCK_SECTION, NETWORK_SECTION, VOLUME_SECTION, THEME_SECTION };
static const char *const sections[] = {"bar", "font", "workspaces", "clock", "network", "volume", "theme"};

enum value_type { PIXELS, COLOR, OPACITY, SCALE, POSITION, BOOLEAN, STRING, LABELS, QUOTED_STRING, THEME_SOURCE, ACTIVE_STYLE };

struct config_key {
    enum section section;
    const char *name;
    enum value_type type;
    size_t offset;
    int min, max;
};

#define KEY(section, name, type, field, min, max) \
    {section, name, type, offsetof(struct bar_config, field), min, max}
static const struct config_key keys[] = {
    KEY(BAR_SECTION, "height", PIXELS, height, 1, 4096),
    KEY(BAR_SECTION, "position", POSITION, bottom, 0, 0),
    KEY(BAR_SECTION, "background", COLOR, background, 0, 0),
    KEY(BAR_SECTION, "opacity", OPACITY, opacity, 0, 0),
    KEY(BAR_SECTION, "scale", SCALE, scale, 0, 0),
    KEY(BAR_SECTION, "foreground", COLOR, foreground, 0, 0),
    KEY(BAR_SECTION, "padding", PIXELS, padding, 0, 4096),
    KEY(FONT_SECTION, "family", STRING, family, 1, BAR_FONT_FAMILY_SIZE - 1),
    KEY(FONT_SECTION, "size", PIXELS, font_size, 1, 256),
    KEY(WORKSPACE_SECTION, "labels", LABELS, labels, 0, 0),
    KEY(WORKSPACE_SECTION, "hide_empty", BOOLEAN, hide_empty, 0, 0),
    KEY(WORKSPACE_SECTION, "active_style", ACTIVE_STYLE, active_style, 0, 0),
    KEY(WORKSPACE_SECTION, "active_color", COLOR, active_color, 0, 0),
    KEY(WORKSPACE_SECTION, "occupied_color", COLOR, occupied_color, 0, 0),
    KEY(WORKSPACE_SECTION, "inactive_color", COLOR, inactive_color, 0, 0),
    KEY(WORKSPACE_SECTION, "urgent_color", COLOR, urgent_color, 0, 0),
    KEY(WORKSPACE_SECTION, "spacing", PIXELS, spacing, 0, 4096),
    KEY(CLOCK_SECTION, "format", STRING, clock_format, 1, BAR_CLOCK_FORMAT_SIZE - 1),
    KEY(NETWORK_SECTION, "enabled", BOOLEAN, network_enabled, 0, 0),
    KEY(NETWORK_SECTION, "show_ssid", BOOLEAN, network_show_ssid, 0, 0),
    KEY(NETWORK_SECTION, "show_strength", BOOLEAN, network_show_strength, 0, 0),
    KEY(NETWORK_SECTION, "separator", QUOTED_STRING, network_separator, 0, BAR_NETWORK_SEPARATOR_SIZE - 1),
    KEY(NETWORK_SECTION, "custom_text", QUOTED_STRING, network_custom_text, 0, BAR_NETWORK_TEXT_SIZE - 1),
    KEY(NETWORK_SECTION, "click_enabled", BOOLEAN, network_click_enabled, 0, 0),
    KEY(NETWORK_SECTION, "command", STRING, network_command, 1, BAR_NETWORK_COMMAND_SIZE - 1),
    KEY(VOLUME_SECTION, "enabled", BOOLEAN, volume_enabled, 0, 0),
    KEY(VOLUME_SECTION, "custom_text", QUOTED_STRING, volume_custom_text, 0, BAR_VOLUME_TEXT_SIZE - 1),
    KEY(VOLUME_SECTION, "muted_color", COLOR, volume_muted_color, 0, 0),
    KEY(VOLUME_SECTION, "command", STRING, volume_command, 0, BAR_NETWORK_COMMAND_SIZE - 1),
    KEY(VOLUME_SECTION, "click", STRING, volume_click, 0, BAR_NETWORK_COMMAND_SIZE - 1),
    KEY(VOLUME_SECTION, "middle_click", STRING, volume_middle_click, 0, BAR_NETWORK_COMMAND_SIZE - 1),
    KEY(VOLUME_SECTION, "scroll_up", STRING, volume_scroll_up, 0, BAR_NETWORK_COMMAND_SIZE - 1),
    KEY(VOLUME_SECTION, "scroll_down", STRING, volume_scroll_down, 0, BAR_NETWORK_COMMAND_SIZE - 1),
    KEY(THEME_SECTION, "source", THEME_SOURCE, wallpaper_theme, 0, 0),
};
#undef KEY

static const char *parse_value(struct bar_config *config, const struct config_key *key,
                               char *value)
{
    void *field = (char *)config + key->offset;
    char *end;
    errno = 0;
    switch (key->type) {
    case PIXELS: {
        long number = strtol(value, &end, 10);
        if (errno || end == value || *end || number < key->min || number > key->max)
            return "integer outside the allowed range";
        *(int *)field = (int)number;
        break;
    }
    case COLOR:
        if (value[0] != '#' || strlen(value) != 7 ||
            strspn(value + 1, "0123456789abcdefABCDEF") != 6)
            return "expected an RGB color in #RRGGBB form";
        *(uint32_t *)field = (uint32_t)strtoul(value + 1, NULL, 16);
        break;
    case OPACITY:
    case SCALE: {
        double number = strtod(value, &end);
        if (errno || end == value || *end ||
            value[strspn(value, "0123456789.+-eE")] || !isfinite(number))
            return "expected a decimal number";
        if (key->type == OPACITY && (number < 0 || number > 1))
            return "expected an opacity between 0 and 1";
        if (key->type == SCALE && number <= 0)
            return "expected a scale greater than zero";
        *(double *)field = number;
        break;
    }
    case POSITION:
        if (strcmp(value, "top") && strcmp(value, "bottom"))
            return "expected top or bottom";
        *(bool *)field = !strcmp(value, "bottom");
        break;
    case BOOLEAN:
        if (strcmp(value, "true") && strcmp(value, "false"))
            return "expected true or false";
        *(bool *)field = !strcmp(value, "true");
        break;
    case THEME_SOURCE:
        if (strcmp(value, "manual") && strcmp(value, "wallpaper"))
            return "expected manual or wallpaper";
        *(bool *)field = !strcmp(value, "wallpaper");
        break;
    case ACTIVE_STYLE:
        if (!strcmp(value, "box"))
            *(enum bar_active_style *)field = BAR_ACTIVE_BOX;
        else if (!strcmp(value, "underline"))
            *(enum bar_active_style *)field = BAR_ACTIVE_UNDERLINE;
        else if (!strcmp(value, "none"))
            *(enum bar_active_style *)field = BAR_ACTIVE_NONE;
        else
            return "expected box, underline or none";
        break;
    case QUOTED_STRING:
        if (*value == '"') {
            char *quote = strchr(value + 1, '"');
            if (!quote || quote[1])
                return "expected a matching closing quote";
            *quote = '\0';
            ++value;
        }
        /* fall through */
    case STRING: {
        size_t length = strlen(value);
        if (length < (size_t)key->min || length > (size_t)key->max)
            return "string is empty or too long";
        memcpy(field, value, length + 1);
        break;
    }
    case LABELS: {
        unsigned count = 0;
        for (;;) {
            char *comma = strchr(value, ',');
            if (comma)
                *comma = '\0';
            char *label = trim(value);
            if (!*label || strlen(label) >= BAR_LABEL_SIZE || count == BAR_MAX_WORKSPACES)
                return "expected 1 to 32 nonempty labels of at most 63 bytes";
            strcpy(config->labels[count++], label);
            if (!comma)
                break;
            value = comma + 1;
        }
        config->label_count = count;
        break;
    }
    }
    return NULL;
}

static bool read_config(const char *path, struct bar_config *config)
{
    *config = bar_defaults;
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK) : -1;
    if (fd < 0 && path && errno != ENOENT) {
        fprintf(stderr, "bar: %s: %s\n", path, strerror(errno));
        return false;
    }
    struct stat info;
    FILE *file = NULL;
    if (fd < 0) {
        path = "bundled bar.conf";
        file = fmemopen((void *)bundled_config, sizeof bundled_config - 1, "r");
    } else if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
        file = fdopen(fd, "r");
    }
    if (!file) {
        fprintf(stderr, "bar: %s: cannot open a regular configuration file\n", path);
        if (fd >= 0)
            close(fd);
        return false;
    }

    char *line = NULL;
    size_t capacity = 0, number = 0;
    int section = -1;
    ssize_t length;
    const char *error = NULL, *option = NULL;
    const size_t section_count = sizeof sections / sizeof sections[0];
    while ((length = getline(&line, &capacity, file)) >= 0) {
        ++number;
        option = NULL;
        if (length > 4096 || memchr(line, '\0', (size_t)length)) {
            error = "line is too long or contains a NUL byte";
            break;
        }
        char *text = trim(line);
        if (!*text || *text == '#' || *text == ';')
            continue;
        if (*text == '[') {
            char *end = strchr(++text, ']');
            if (!end || end[1]) {
                error = "invalid section header";
                break;
            }
            *end = '\0';
            option = trim(text);
            for (section = 0; (size_t)section < section_count; ++section)
                if (!strcmp(option, sections[section]))
                    break;
            if ((size_t)section == section_count) {
                error = "unknown section";
                break;
            }
            continue;
        }

        char *equals = strchr(text, '=');
        if (!equals || section < 0) {
            error = "expected key = value inside a section";
            break;
        }
        *equals = '\0';
        option = trim(text);
        const struct config_key *key = NULL;
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i)
            if ((int)keys[i].section == section && !strcmp(keys[i].name, option)) {
                key = &keys[i];
                break;
            }
        error = key ? parse_value(config, key, trim(equals + 1)) : "unknown option";
        if (error)
            break;
    }
    if (!error && !feof(file)) {
        option = NULL;
        error = "could not read configuration";
    }
    if (error)
        fprintf(stderr, "bar: %s:%zu: %s%s%s\n", path, number,
                option ? option : "", option ? ": " : "", error);
    free(line);
    fclose(file);
    return !error;
}

static bool clock_text(const struct bar_config *config, time_t now, char *text)
{
    struct tm local;
    return now != (time_t)-1 && localtime_r(&now, &local) &&
           strftime(text, BAR_CLOCK_TEXT_SIZE, config->clock_format, &local) > 0;
}

static bool valid_clock_format(const struct bar_config *config)
{
    const char *text = config->clock_format;
    while ((text = strchr(text, '%'))) {
        ++text;
        text += strspn(text, "_-0^#123456789");
        if (*text == 'E' || *text == 'O')
            ++text;
        if (!*text || !strchr("aAbBcCdDeFgGhHIjklmMnNpPrRsStTuUVwWxXyYzZ%", *text))
            return false;
        ++text;
    }
    char sample[BAR_CLOCK_TEXT_SIZE];
    return clock_text(config, time(NULL), sample);
}

static FT_Face load_font(struct bar *bar, const struct bar_config *config)
{
    if (!bar->font_config)
        bar->font_config = FcInitLoadConfigAndFonts();
    if (!bar->font_config)
        return NULL;

    FcPattern *pattern = FcPatternCreate();
    if (!pattern)
        return NULL;
    const FcChar8 *family = (const FcChar8 *)config->family;
    FcResult result;
    FcPattern *match = NULL;
    if (FcPatternAddString(pattern, FC_FAMILY, family) &&
        FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_REGULAR) &&
        FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN) &&
        FcPatternAddBool(pattern, FC_SCALABLE, FcTrue) &&
        FcConfigSubstitute(bar->font_config, pattern, FcMatchPattern)) {
        FcDefaultSubstitute(pattern);
        match = FcFontMatch(bar->font_config, pattern, &result);
    }
    FcPatternDestroy(pattern);
    if (!match)
        return NULL;

    bool family_matches = !strcasecmp(config->family, "monospace") ||
                          !strcasecmp(config->family, "sans-serif") ||
                          !strcasecmp(config->family, "serif") ||
                          !strcasecmp(config->family, "system-ui");
    for (int i = 0; !family_matches; ++i) {
        FcChar8 *name;
        if (FcPatternGetString(match, FC_FAMILY, i, &name) != FcResultMatch)
            break;
        family_matches = FcStrCmpIgnoreCase(family, name) == 0;
    }

    FcChar8 *file;
    int index;
    FT_Face face = NULL;
    if (family_matches &&
        FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
        FcPatternGetInteger(match, FC_INDEX, 0, &index) == FcResultMatch &&
        !FT_New_Face(bar->freetype, (const char *)file, index, &face) &&
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)config->font_size)) {
        FT_Done_Face(face);
        face = NULL;
    }
    FcPatternDestroy(match);
    return face;
}

/* Convert a straight-alpha RGB color into the wl_shm ARGB8888 representation. */
static uint32_t premultiply(uint32_t rgb, unsigned alpha)
{
    uint32_t pixel = alpha << 24;
    for (unsigned shift = 0; shift < 24; shift += 8)
        pixel |= ((((rgb >> shift) & 255) * alpha + 127) / 255) << shift;
    return pixel;
}

static uint32_t blend(unsigned coverage, uint32_t color, uint32_t pixel)
{
    unsigned inverse = 255 - coverage;
    uint32_t result = (coverage + ((pixel >> 24) * inverse + 127) / 255) << 24;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned fg = (color >> shift) & 255;
        unsigned bg = (pixel >> shift) & 255;
        result |= ((fg * coverage + bg * inverse + 127) / 255) << shift;
    }
    return result;
}

static uint32_t utf8_next(const unsigned char **cursor)
{
    const unsigned char *c = *cursor;
    uint32_t code = *c++;
    *cursor = c;
    if (code < 0x80)
        return code;

    unsigned tail;
    uint32_t minimum;
    if (code >= 0xc2 && code <= 0xdf) {
        code &= 0x1f;
        tail = 1;
        minimum = 0x80;
    } else if (code >= 0xe0 && code <= 0xef) {
        code &= 0x0f;
        tail = 2;
        minimum = 0x800;
    } else if (code >= 0xf0 && code <= 0xf4) {
        code &= 0x07;
        tail = 3;
        minimum = 0x10000;
    } else {
        return 0xfffd;
    }
    while (tail--) {
        if ((*c & 0xc0) != 0x80)
            return 0xfffd;
        code = (code << 6) | (*c++ & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return 0xfffd;
    *cursor = c;
    return code;
}

static struct text_bounds draw_text(FT_Face font, const char *text, uint32_t color,
                                    uint32_t *pixels, int width, int height,
                                    int pen, int clip_left)
{
    FT_Size_Metrics metrics = font->size->metrics;
    int ascent = (int)(metrics.ascender / 64);
    int descent = (int)(-metrics.descender / 64);
    int baseline = (height - ascent - descent) / 2 + ascent;
    int start = pen;
    int min_x = pen, max_x = pen;

    const unsigned char *c = (const unsigned char *)text;
    while (*c) {
        uint32_t code = utf8_next(&c);
        if (code < 0x20 || code == 0x7f)
            code = ' ';
        if (FT_Load_Char(font, code, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP))
            continue;

        FT_GlyphSlot glyph = font->glyph;
        const FT_Bitmap *bitmap = &glyph->bitmap;
        int left = pen + glyph->bitmap_left;
        int top = baseline - glyph->bitmap_top;
        pen += (int)(glyph->advance.x / 64);

        if (pen > max_x)
            max_x = pen;
        if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY || !bitmap->width)
            continue;
        if (left < min_x)
            min_x = left;
        if (left + (int)bitmap->width > max_x)
            max_x = left + (int)bitmap->width;
        if (!pixels)
            continue;

        for (int row = 0; row < (int)bitmap->rows; ++row) {
            int y = top + row;
            if (y < 0 || y >= height)
                continue;
            const unsigned char *mask = bitmap->buffer + (ptrdiff_t)row * bitmap->pitch;
            for (int col = 0; col < (int)bitmap->width; ++col) {
                int x = left + col;
                if (x >= clip_left && x < width && mask[col]) {
                    uint32_t *pixel = &pixels[(size_t)y * width + x];
                    *pixel = blend(mask[col], color, *pixel);
                }
            }
        }
    }
    return (struct text_bounds){min_x - start, max_x - min_x};
}

/* How far draw_text moves the pen over the first LENGTH bytes of TEXT. */
static int text_advance(FT_Face font, const char *text, size_t length)
{
    int pen = 0;
    const unsigned char *c = (const unsigned char *)text, *end = c + length;
    while (c < end && *c) {
        uint32_t code = utf8_next(&c);
        if (code < 0x20 || code == 0x7f)
            code = ' ';
        if (!FT_Load_Char(font, code, FT_LOAD_NO_BITMAP))
            pen += (int)(font->glyph->advance.x / 64);
    }
    return pen;
}

#define NM_SERVICE "org.freedesktop.NetworkManager"
#define NM_ROOT "/org/freedesktop/NetworkManager"
#define NM_ACTIVE NM_SERVICE ".Connection.Active"
#define NM_WIRELESS NM_SERVICE ".Device.Wireless"
#define NM_AP NM_SERVICE ".AccessPoint"
#define DBUS_PROPERTIES "org.freedesktop.DBus.Properties"

static int network_get(struct network *network, const char *path, const char *interface,
                       const char *property, sd_bus_message_handler_t reply)
{
    sd_bus_message *message = NULL;
    int result = sd_bus_message_new_method_call(network->bus, &message, NM_SERVICE,
                                                path, DBUS_PROPERTIES, property ? "Get" : "GetAll");
    if (result >= 0)
        result = sd_bus_message_set_auto_start(message, 0);
    if (result >= 0)
        result = property ? sd_bus_message_append(message, "ss", interface, property) :
                            sd_bus_message_append(message, "s", interface);
    if (result >= 0)
        result = sd_bus_call_async(network->bus, &network->request, message, reply, network, 0);
    sd_bus_message_unref(message);
    return result;
}

static int next_property(sd_bus_message *message, const char **name, const char **type)
{
    int result = sd_bus_message_enter_container(message, 'e', "sv");
    if (result <= 0)
        return result;
    if (sd_bus_message_read(message, "s", name) <= 0 ||
        sd_bus_message_peek_type(message, NULL, type) <= 0 ||
        sd_bus_message_enter_container(message, 'v', *type) <= 0)
        return -1;
    return 1;
}

static bool finish_property(sd_bus_message *message, int read, const char *type)
{
    return read >= 0 && (read || sd_bus_message_skip(message, type) >= 0) &&
           sd_bus_message_exit_container(message) >= 0 &&
           sd_bus_message_exit_container(message) >= 0;
}

static bool copy_path(char *dest, const char *path)
{
    if (!path || !strcmp(path, "/") || strlen(path) >= NM_PATH_SIZE)
        return false;
    strcpy(dest, path);
    return true;
}

static void network_result(struct network *network, enum network_kind kind,
                           int strength, const char *ssid)
{
    if (network->kind != kind || network->strength != strength || strcmp(network->ssid, ssid))
        network->changed = true;
    network->kind = kind;
    network->strength = strength;
    strcpy(network->ssid, ssid);
    if (kind != NETWORK_WIFI) {
        network->device[0] = network->ap[0] = '\0';
        network->refresh_ap = false;
    }
}

static int access_point_reply(sd_bus_message *message, void *data, sd_bus_error *error)
{
    (void)error;
    struct network *network = data;
    network->request = sd_bus_slot_unref(network->request);
    int strength = -1;
    char ssid[NM_SSID_SIZE] = "";
    if (sd_bus_message_enter_container(message, 'a', "{sv}") > 0) {
        const char *name, *type;
        while (next_property(message, &name, &type) > 0) {
            int result = 0;
            if (!strcmp(name, "Strength")) {
                uint8_t value;
                result = sd_bus_message_read(message, "y", &value);
                if (result > 0 && value <= 100)
                    strength = value;
            } else if (!strcmp(name, "Ssid")) {
                const void *bytes;
                size_t length;
                result = sd_bus_message_read_array(message, 'y', &bytes, &length);
                if (result > 0 && length < sizeof ssid) {
                    const unsigned char *raw = bytes;
                    for (size_t i = 0; i < length; ++i)
                        ssid[i] = raw[i] < 0x20 || raw[i] == 0x7f ? ' ' : (char)raw[i];
                }
            }
            if (!finish_property(message, result, type))
                break;
        }
    }
    network_result(network, NETWORK_WIFI, strength, ssid);
    return 0;
}

static int wireless_reply(sd_bus_message *message, void *data, sd_bus_error *error)
{
    (void)error;
    struct network *network = data;
    network->request = sd_bus_slot_unref(network->request);
    network->ap[0] = '\0';
    const char *path;
    if (sd_bus_message_read(message, "v", "o", &path) > 0 && copy_path(network->ap, path) &&
        network_get(network, network->ap, NM_AP, NULL, access_point_reply) >= 0)
        return 0;

    network_result(network, NETWORK_WIFI, -1, "");
    return 0;
}

static void next_connection(struct network *network);

static int connection_reply(sd_bus_message *message, void *data, sd_bus_error *error)
{
    (void)error;
    struct network *network = data;
    network->request = sd_bus_slot_unref(network->request);
    const char *connection_type = "", *device = NULL;
    uint32_t state = 0;
    if (sd_bus_message_enter_container(message, 'a', "{sv}") > 0) {
        const char *name, *type;
        while (next_property(message, &name, &type) > 0) {
            int result = 0;
            if (!strcmp(name, "State"))
                result = sd_bus_message_read(message, "u", &state);
            else if (!strcmp(name, "Type"))
                result = sd_bus_message_read(message, "s", &connection_type);
            else if (!strcmp(name, "Devices")) {
                result = sd_bus_message_enter_container(message, 'a', "o");
                if (result > 0) {
                    sd_bus_message_read(message, "o", &device);
                    while (sd_bus_message_skip(message, "o") > 0)
                        ;
                    result = sd_bus_message_exit_container(message) < 0 ? -1 : 1;
                }
            }
            if (!finish_property(message, result, type))
                break;
        }
    }

    enum { NM_ACTIVATED = 2 };
    if (state == NM_ACTIVATED && !strcmp(connection_type, "802-3-ethernet")) {
        network_result(network, NETWORK_ETHERNET, -1, "");
    } else if (state == NM_ACTIVATED && !strcmp(connection_type, "802-11-wireless")) {
        network->device[0] = network->ap[0] = '\0';
        if (!copy_path(network->device, device) ||
            network_get(network, network->device, NM_WIRELESS, "ActiveAccessPoint", wireless_reply) < 0)
            network_result(network, NETWORK_WIFI, -1, "");
    } else {
        if (state == NM_ACTIVATED && *connection_type && strcmp(connection_type, "loopback"))
            network->other = true;
        next_connection(network);
    }
    return 0;
}

static void next_connection(struct network *network)
{
    while (network->index < network->count) {
        const char *path = network->paths[network->index++];
        if (network_get(network, path, NM_ACTIVE, NULL, connection_reply) >= 0)
            return;
    }
    network_result(network, network->other ? NETWORK_OTHER : NETWORK_OFFLINE, -1, "");
}

static int network_root_reply(sd_bus_message *message, void *data, sd_bus_error *error)
{
    (void)error;
    struct network *network = data;
    network->request = sd_bus_slot_unref(network->request);
    network->index = network->count = 0;
    network->other = false;
    const char *primary = NULL;
    if (sd_bus_message_enter_container(message, 'a', "{sv}") > 0) {
        const char *name, *type;
        while (next_property(message, &name, &type) > 0) {
            int result = 0;
            if (!strcmp(name, "PrimaryConnection")) {
                result = sd_bus_message_read(message, "o", &primary);
            } else if (!strcmp(name, "ActiveConnections")) {
                result = sd_bus_message_enter_container(message, 'a', "o");
                if (result > 0) {
                    const char *path;
                    while (sd_bus_message_read(message, "o", &path) > 0)
                        if (network->count < NM_CONNECTIONS &&
                            copy_path(network->paths[network->count], path))
                            ++network->count;
                    result = sd_bus_message_exit_container(message) < 0 ? -1 : 1;
                }
            }
            if (!finish_property(message, result, type))
                break;
        }
    }

    char path[NM_PATH_SIZE];
    if (copy_path(path, primary)) {
        size_t i = 0;
        while (i < network->count && strcmp(network->paths[i], path))
            ++i;
        if (i == network->count && network->count < NM_CONNECTIONS)
            ++network->count;
        if (i == NM_CONNECTIONS)
            --i;
        memmove(network->paths + 1, network->paths, i * sizeof network->paths[0]);
        strcpy(network->paths[0], path);
    }
    next_connection(network);
    return 0;
}

static int network_properties_changed(sd_bus_message *message, void *data, sd_bus_error *error)
{
    (void)error;
    struct network *network = data;
    const char *interface, *path = sd_bus_message_get_path(message);
    if (!path || sd_bus_message_read(message, "s", &interface) <= 0)
        return 0;
    if ((!strcmp(interface, NM_SERVICE) && !strcmp(path, NM_ROOT)) ||
        (!strcmp(interface, NM_WIRELESS) && !strcmp(path, network->device))) {
        network->refresh = true;
    } else if (!strcmp(interface, NM_AP) && !strcmp(path, network->ap)) {
        network->refresh_ap = true;
    } else if (!strcmp(interface, NM_ACTIVE)) {
        for (size_t i = 0; i < network->count; ++i)
            if (!strcmp(path, network->paths[i])) {
                network->refresh = true;
                break;
            }
    }
    return 0;
}

static int network_owner_changed(sd_bus_message *message, void *data, sd_bus_error *error)
{
    (void)error;
    struct network *network = data;
    const char *new_owner;
    if (sd_bus_message_read(message, "sss", (const char **)NULL,
                            (const char **)NULL, &new_owner) > 0) {
        network->request = sd_bus_slot_unref(network->request);
        network->count = 0;
        network_result(network, NETWORK_OFFLINE, -1, "");
        network->refresh = *new_owner != '\0';
    }
    return 0;
}

static void network_disconnect(struct network *network)
{
    network->request = sd_bus_slot_unref(network->request);
    network->bus = sd_bus_close_unref(network->bus);
    network->count = 0;
    network->refresh = false;
    network_result(network, NETWORK_OFFLINE, -1, "");
}

static bool network_connect(struct network *network)
{
    if (sd_bus_open_system(&network->bus) < 0)
        return false;
    sd_bus_set_method_call_timeout(network->bus, 3000000);
    if (sd_bus_add_match_async(network->bus, NULL,
            "type='signal',sender='" NM_SERVICE "',interface='" DBUS_PROPERTIES
            "',member='PropertiesChanged'", network_properties_changed, NULL, network) < 0 ||
        sd_bus_add_match_async(network->bus, NULL,
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
            "member='NameOwnerChanged',arg0='" NM_SERVICE "'", network_owner_changed, NULL, network) < 0)
        return false;

    network->refresh = true;
    return true;
}

static void service_network(struct network *network, bool enabled, struct pollfd *fd,
                            int64_t now, int64_t *deadline)
{
    *fd = (struct pollfd){.fd = -1};
    if (!enabled) {
        if (network->bus || network->retry) {
            network_disconnect(network);
            network->retry = 0;
        }
        return;
    }

    if (!network->bus && now >= network->retry && !network_connect(network)) {
        network_disconnect(network);
        network->retry = now + 3000;
    }
    if (network->bus) {
        int result = 0;
        for (int i = 0; i < 32; ++i) {
            result = sd_bus_process(network->bus, NULL);
            if (result <= 0)
                break;
        }
        if (result > 0)
            *deadline = now;  /* Give Wayland a turn before processing more queued signals. */
        if (result >= 0 && network->refresh && !network->request) {
            network->refresh = network->refresh_ap = false;
            result = network_get(network, NM_ROOT, NM_SERVICE, NULL, network_root_reply);
        } else if (result >= 0 && network->refresh_ap && !network->request) {
            network->refresh_ap = false;
            if (network->ap[0])
                result = network_get(network, network->ap, NM_AP, NULL, access_point_reply);
        }

        int bus_fd = sd_bus_get_fd(network->bus);
        int events = sd_bus_get_events(network->bus);
        uint64_t timeout;
        if (result >= 0 && bus_fd >= 0 && events >= 0 &&
            sd_bus_get_timeout(network->bus, &timeout) >= 0) {
            *fd = (struct pollfd){.fd = bus_fd, .events = (short)events};
            if (timeout != UINT64_MAX) {
                int64_t wake = (int64_t)(timeout / 1000 + (timeout % 1000 != 0));
                if (wake < *deadline)
                    *deadline = wake;
            }
            return;
        }
        network_disconnect(network);
        network->retry = now + 3000;
    }
    if (network->retry < *deadline)
        *deadline = network->retry;
}

static void volume_label(const struct bar_config *config, const struct volume_feed *volume,
                         char label[VOLUME_LABEL_SIZE])
{
    label[0] = '\0';
    if (!config->volume_enabled || !volume->have)
        return;
    const char *prefix = config->volume_custom_text, *space = *prefix ? " " : "";
    if (volume->raw[0])
        snprintf(label, VOLUME_LABEL_SIZE, "%s", volume->raw);
    else if (volume->muted)
        snprintf(label, VOLUME_LABEL_SIZE, "%s%smuted", prefix, space);
    else
        snprintf(label, VOLUME_LABEL_SIZE, "%s%s%d%%", prefix, space, volume->percent);
}

static bool update_right_text(struct appearance *appearance, const struct network *network,
                              const struct volume_feed *volume)
{
    const struct bar_config *config = &appearance->config;
    char clock[BAR_CLOCK_TEXT_SIZE] = "";
    if (!clock_text(config, time(NULL), clock))
        clock[0] = '\0';

    char label[NETWORK_LABEL_SIZE] = "";
    if (config->network_enabled) {
        switch (network->kind) {
        case NETWORK_WIFI: {
            const char *ssid = config->network_show_ssid ? network->ssid : "";
            const char *prefix = config->network_custom_text;
            snprintf(label, sizeof label, "%s%s%s", prefix, *prefix && *ssid ? " " : "", ssid);
            if (config->network_show_strength && network->strength >= 0) {
                size_t length = strlen(label);
                snprintf(label + length, sizeof label - length, "%s%d%%", length ? " " : "",
                         network->strength);
            }
            break;
        }
        case NETWORK_ETHERNET:
            strcpy(label, "Ethernet");
            break;
        case NETWORK_OTHER:
            strcpy(label, "Connected");
            break;
        case NETWORK_OFFLINE:
            strcpy(label, "Offline");
            break;
        }
    }
    char sound[VOLUME_LABEL_SIZE];
    volume_label(config, volume, sound);
    bool muted = sound[0] && !volume->raw[0] && volume->muted;

    /* Volume, network and clock, with the separator between those present. */
    char text[sizeof appearance->right_text];
    const char *parts[] = {sound, label, clock};
    size_t starts[3] = {0}, length = 0;
    text[0] = '\0';
    for (size_t i = 0; i < 3; ++i) {
        if (!*parts[i]) {
            starts[i] = length;
            continue;
        }
        if (length)
            length += (size_t)snprintf(text + length, sizeof text - length, "%s",
                                       config->network_separator);
        if (length >= sizeof text)
            length = sizeof text - 1;
        starts[i] = length;
        length += (size_t)snprintf(text + length, sizeof text - length, "%s", parts[i]);
        if (length >= sizeof text)
            length = sizeof text - 1;
    }
    size_t network_length = strlen(label), volume_length = strlen(sound);
    if (!strcmp(appearance->right_text, text) && appearance->network_length == network_length &&
        appearance->volume_length == volume_length && appearance->volume_muted == muted)
        return false;
    strcpy(appearance->right_text, text);
    appearance->right_bounds = draw_text(appearance->font, text, 0, NULL, 0, 0, 0, 0);
    appearance->network_bounds = draw_text(appearance->font, label, 0, NULL, 0, 0, 0, 0);
    appearance->volume_bounds = draw_text(appearance->font, sound, 0, NULL, 0, 0, 0, 0);
    appearance->network_length = network_length;
    appearance->network_offset = starts[1];
    appearance->network_x = text_advance(appearance->font, text, starts[1]);
    appearance->volume_length = volume_length;
    appearance->volume_advance = text_advance(appearance->font, text, volume_length);
    appearance->volume_muted = muted;
    return true;
}

static bool scale_dimensions(struct bar_config *config, const char *path)
{
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        const struct config_key *key = &keys[i];
        if (key->type != PIXELS)
            continue;
        int *pixels = (int *)((char *)config + key->offset);
        double rounded = *pixels * config->scale + 0.5;
        if (rounded < key->min || rounded >= key->max + 1) {
            fprintf(stderr, "bar: %s: [%s] %s after scaling must be between %d and %d\n",
                    path, sections[key->section], key->name, key->min, key->max);
            return false;
        }
        *pixels = (int)rounded;
    }
    return true;
}

/* Store offsets so the parsed command can move with a staged appearance. */
static bool parse_command(struct command *command, const char *input)
{
    char *out = command->text;
    while (*input) {
        while (isspace((unsigned char)*input))
            input++;
        if (!*input)
            break;
        if (command->count == sizeof command->offsets / sizeof command->offsets[0])
            return false;
        command->offsets[command->count++] = (size_t)(out - command->text);

        char quote = '\0';
        while (*input && (quote || !isspace((unsigned char)*input))) {
            char byte = *input++;
            if (byte == '\\' && quote != '\'') {
                if (!*input)
                    return false;
                byte = *input++;
            } else if (quote && byte == quote) {
                quote = '\0';
                continue;
            } else if (!quote && (byte == '\'' || byte == '"')) {
                quote = byte;
                continue;
            }
            *out++ = byte;
        }
        if (quote)
            return false;
        *out++ = '\0';
    }
    return command->count && command->text[0] != '\0';
}

/* An empty command is allowed and does nothing. */
static bool parse_optional_command(struct command *command, const char *input)
{
    if (!input[strspn(input, " \t")]) {
        command->count = 0;
        return true;
    }
    return parse_command(command, input);
}

static void read_bar_palette(struct bar *bar)
{
    if (load_theme_palette(bar->theme_watch.path, bar->theme_palette))
        bar->have_palette = true;
}

static bool apply_bar_palette(struct appearance *appearance, const uint32_t palette[THEME_ROLE_COUNT])
{
    struct bar_config *config = &appearance->config;
    if (!config->wallpaper_theme)
        return false;
    const struct {
        uint32_t *color;
        enum theme_role role;
    } mapping[] = {
        {&config->background, THEME_BACKGROUND}, {&config->foreground, THEME_FOREGROUND},
        {&config->active_color, THEME_HIGHLIGHT}, {&config->occupied_color, THEME_ACCENT},
        {&config->inactive_color, THEME_MUTED}, {&config->urgent_color, THEME_URGENT},
        {&config->volume_muted_color, THEME_MUTED},
    };
    bool changed = false;
    for (size_t i = 0; i < sizeof mapping / sizeof mapping[0]; ++i) {
        uint32_t color = palette[mapping[i].role];
        if (*mapping[i].color != color) {
            *mapping[i].color = color;
            changed = true;
        }
    }
    if (changed)
        appearance->background = premultiply(config->background, (unsigned)(config->opacity * 255 + 0.5));
    return changed;
}

static void refresh_bar_palette(struct bar *bar)
{
    read_bar_palette(bar);
    if (!bar->have_palette)
        return;
    const struct bar_config *config = &bar->appearance.config;
    uint32_t foreground = config->foreground, background = config->background;
    if (apply_bar_palette(&bar->appearance, bar->theme_palette)) {
        bar->layout_dirty = true;
        if (foreground != config->foreground || background != config->background)
            bar->redraw = true;
    }
    if (bar->pending.font)
        apply_bar_palette(&bar->pending, bar->theme_palette);
}

static void close_unused_theme_watch(struct bar *bar)
{
    if (!bar->appearance.config.wallpaper_theme &&
        (!bar->pending.font || !bar->pending.config.wallpaper_theme)) {
        close_theme_watch(&bar->theme_watch);
        bar->have_palette = false;
    }
}

static bool load_appearance(struct bar *bar, struct appearance *appearance)
{
    struct appearance candidate = {0};
    struct bar_config *config = &candidate.config;
    if (!read_config(bar->config_path, config))
        return false;
    if (!parse_command(&candidate.network_command, config->network_command)) {
        fprintf(stderr, "bar: %s: invalid [network] command\n",
                bar->config_path ? bar->config_path : "defaults");
        return false;
    }
    const struct {
        struct command *command;
        const char *text, *name;
    } volume_commands[] = {
        {&candidate.volume_command, config->volume_command, "command"},
        {&candidate.volume_click, config->volume_click, "click"},
        {&candidate.volume_middle_click, config->volume_middle_click, "middle_click"},
        {&candidate.volume_scroll_up, config->volume_scroll_up, "scroll_up"},
        {&candidate.volume_scroll_down, config->volume_scroll_down, "scroll_down"},
    };
    for (size_t i = 0; i < sizeof volume_commands / sizeof volume_commands[0]; ++i) {
        if (!parse_optional_command(volume_commands[i].command, volume_commands[i].text)) {
            fprintf(stderr, "bar: %s: invalid [volume] %s\n",
                    bar->config_path ? bar->config_path : "defaults", volume_commands[i].name);
            return false;
        }
    }
    if (!scale_dimensions(config, bar->config_path ? bar->config_path : "defaults"))
        return false;
    if (!valid_clock_format(config)) {
        fprintf(stderr, "bar: %s: invalid [clock] format\n",
                bar->config_path ? bar->config_path : "defaults");
        return false;
    }
    candidate.font = load_font(bar, config);
    if (!candidate.font) {
        fprintf(stderr, "bar: %s: cannot load [font] family '%s' at size %d\n",
                bar->config_path ? bar->config_path : "defaults", config->family, config->font_size);
        return false;
    }
    candidate.background = premultiply(config->background, (unsigned)(config->opacity * 255 + 0.5));
    if (config->wallpaper_theme) {
        if (!bar->theme_watch.path)
            bar->theme_watch.path = theme_palette_path();
        if (bar->theme_watch.fd < 0)
            arm_theme_watch(&bar->theme_watch);
        read_bar_palette(bar);
        if (bar->have_palette)
            apply_bar_palette(&candidate, bar->theme_palette);
    }
    for (size_t i = 0; i < config->label_count; ++i)
        candidate.labels[i] = draw_text(candidate.font, config->labels[i], 0, NULL, 0, 0, 0, 0);
    update_right_text(&candidate, &bar->network, &bar->volume);
    *appearance = candidate;
    return true;
}

static struct monitor *find_monitor(struct bar *bar, const char *name)
{
    for (struct monitor *m = bar->monitors; m; m = m->next)
        if (!strcmp(m->name, name))
            return m;

    struct monitor *m = calloc(1, sizeof *m);
    if (m) {
        strcpy(m->name, name);
        m->next = bar->monitors;
        bar->monitors = m;
        if (!bar->first_monitor) {
            bar->first_monitor = m;
            bar->layout_dirty = true;
        }
    }
    return m;
}

static struct workspace_state displayed_state(const struct bar *bar)
{
    if (bar->ipc_manager)
        return bar->ipc_ready ? bar->ipc_current : (struct workspace_state){0};
    const struct monitor *m = bar->selected ? bar->selected : bar->first_monitor;
    return m ? m->workspaces : (struct workspace_state){0};
}

static bool workspaces_equal(struct workspace_state first, struct workspace_state second)
{
    return first.active == second.active && first.occupied == second.occupied &&
           first.urgent == second.urgent;
}

static bool parse_tags(const char *text, uint32_t tags[4])
{
    for (size_t i = 0; i < 4; ++i) {
        text += strspn(text, " \t");
        if (*text < '0' || *text > '9')
            return false;
        char *end;
        errno = 0;
        uintmax_t mask = strtoumax(text, &end, 10);
        if (errno || mask > UINT32_MAX || (*end && *end != ' ' && *end != '\t'))
            return false;
        tags[i] = (uint32_t)mask;
        text = end;
    }
    return text[strspn(text, " \t")] == '\0';
}

static void parse_status(struct bar *bar, char *line)
{
    char *field = strchr(line, ' ');
    if (!field || field == line || (size_t)(field - line) >= sizeof bar->monitors->name)
        return;
    *field++ = '\0';
    if (strpbrk(line, "\t\r"))
        return;
    char *value = strchr(field, ' ');
    if (!value)
        return;
    *value++ = '\0';

    uint32_t tags[4];
    bool selection = !strcmp(field, "selmon");
    if (selection) {
        if (strcmp(value, "0") && strcmp(value, "1"))
            return;
    } else if (strcmp(field, "tags") || !parse_tags(value, tags)) {
        return;
    }

    struct workspace_state old = displayed_state(bar);
    struct monitor *m = find_monitor(bar, line);
    if (!m)
        return;
    if (!selection) {
        m->workspaces = (struct workspace_state){.occupied = tags[0], .active = tags[1],
                                                .urgent = tags[3]};
    } else if (*value == '1') {
        bar->selected = m;
    } else if (bar->selected == m) {
        bar->selected = NULL;
    }
    if (!workspaces_equal(old, displayed_state(bar)))
        bar->layout_dirty = true;
}

static void consume_status(struct bar *bar, const char *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == '\n') {
            if (!bar->status_invalid) {
                bar->status_line[bar->status_length] = '\0';
                parse_status(bar, bar->status_line);
            }
            bar->status_length = 0;
            bar->status_invalid = false;
        } else if (!data[i] || bar->status_length == sizeof bar->status_line - 1) {
            bar->status_invalid = true;
        } else if (!bar->status_invalid) {
            bar->status_line[bar->status_length++] = data[i];
        }
    }
}

static bool drain_status(struct bar *bar, int fd)
{
    char data[4096];
    /* Limit each pass so a busy status pipe cannot starve Wayland or timers. */
    for (int reads = 0; reads < 16; ++reads) {
        ssize_t count = read(fd, data, sizeof data);
        if (count > 0)
            consume_status(bar, data, (size_t)count);
        else if (count < 0 && errno == EINTR)
            continue;
        else
            return count < 0 && errno == EAGAIN;
    }
    return true;
}

static int right_start(const struct bar *bar, const struct appearance *appearance)
{
    return (int)bar->width - appearance->config.padding - appearance->right_bounds.width;
}

static struct workspace_view layout_workspaces(const struct bar *bar,
                                                const struct appearance *appearance)
{
    const struct bar_config *config = &appearance->config;
    struct workspace_state state = displayed_state(bar);
    struct workspace_view view = {0};
    unsigned count = config->label_count;
    if (bar->ipc_ready && count > bar->ipc_tags)
        count = bar->ipc_tags;

    int limit = right_start(bar, appearance);
    if (appearance->right_text[0])
        limit -= config->spacing;
    int x = config->padding;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t bit = UINT32_C(1) << i;
        if (config->hide_empty && !((state.active | state.occupied | state.urgent) & bit))
            continue;
        int width = appearance->labels[i].width;
        if (x + width > limit)
            break;
        view.visible |= bit;
        if (state.active & bit) {
            if (config->active_style == BAR_ACTIVE_BOX)
                view.boxed |= bit;
            else if (config->active_style == BAR_ACTIVE_UNDERLINE && config->height >= 3)
                view.underlined |= bit;
        }
        if (state.urgent & bit)
            view.colors[i] = config->urgent_color;
        else if (state.active & bit)
            view.colors[i] = config->active_color;
        else if (state.occupied & bit)
            view.colors[i] = config->occupied_color;
        else
            view.colors[i] = config->inactive_color;
        x += width + config->spacing;
    }
    return view;
}

static void update_layout(struct bar *bar)
{
    if (!bar->layout_dirty)
        return;
    bar->layout_dirty = false;
    struct workspace_view view = layout_workspaces(bar, &bar->appearance);
    if (view.visible != bar->workspaces.visible ||
        view.underlined != bar->workspaces.underlined || view.boxed != bar->workspaces.boxed ||
        memcmp(view.colors, bar->workspaces.colors, sizeof view.colors))
        bar->redraw = true;
    bar->workspaces = view;
}

static void paint(const struct bar *bar, const struct appearance *appearance,
                  const struct workspace_view *view, struct hit_map *hits,
                  uint32_t *pixels, int width, int height)
{
    const struct bar_config *config = &appearance->config;
    size_t count = (size_t)width * height;
    for (size_t i = 0; i < count; ++i)
        pixels[i] = appearance->background;

    int x = config->padding;
    for (unsigned i = 0; i < config->label_count; ++i) {
        if (!(view->visible & (UINT32_C(1) << i)))
            continue;
        struct text_bounds bounds = appearance->labels[i];
        hits->tags[i] = (struct hit_span){x, x + bounds.width};
        uint32_t text_color = view->colors[i];
        if (view->boxed & (UINT32_C(1) << i)) {
            /* Half the spacing on each side, so neighboring boxes just meet. */
            int margin = config->spacing / 2;
            int left = x - margin, right = x + bounds.width + margin;
            if (left < 0)
                left = 0;
            if (right > width)
                right = width;
            for (int row = 0; row < height; ++row)
                for (int col = left; col < right; ++col)
                    pixels[(size_t)row * width + col] = 0xff000000u | view->colors[i];
            text_color = config->background;
        }
        draw_text(appearance->font, config->labels[i], text_color, pixels,
                  width, height, x - bounds.left, config->padding);
        if (view->underlined & (UINT32_C(1) << i)) {
            uint32_t *row = pixels + (size_t)(height - 2) * width;
            for (int col = x; col < x + bounds.width; ++col)
                row[col] = 0xff000000u | view->colors[i];
        }
        x += bounds.width + config->spacing;
    }

    int pen = right_start(bar, appearance) - appearance->right_bounds.left;
    if (appearance->volume_length) {
        char sound[VOLUME_LABEL_SIZE];
        memcpy(sound, appearance->right_text, appearance->volume_length);
        sound[appearance->volume_length] = '\0';
        draw_text(appearance->font, sound,
                  appearance->volume_muted ? config->volume_muted_color : config->foreground,
                  pixels, width, height, pen, config->padding);
        draw_text(appearance->font, appearance->right_text + appearance->volume_length,
                  config->foreground, pixels, width, height, pen + appearance->volume_advance,
                  config->padding);
        int start = pen + appearance->volume_bounds.left;
        int end = start + appearance->volume_bounds.width;
        if (start < config->padding)
            start = config->padding;
        if (end > width)
            end = width;
        if (start < end)
            hits->volume = (struct hit_span){start, end};
    } else {
        draw_text(appearance->font, appearance->right_text, config->foreground, pixels, width,
                  height, pen, config->padding);
    }
    if (config->network_click_enabled && appearance->network_bounds.width > 0) {
        int start = pen + appearance->network_x + appearance->network_bounds.left;
        int end = start + appearance->network_bounds.width;
        if (start < 0)
            start = 0;
        if (end > width)
            end = width;
        if (start < end)
            hits->network = (struct hit_span){start, end};
    }
}

static void destroy_buffer(struct buffer *buffer)
{
    wl_buffer_destroy(buffer->proxy);
    wl_list_remove(&buffer->link);
    free(buffer);
}

static void buffer_release(void *data, struct wl_buffer *proxy)
{
    (void)proxy;
    destroy_buffer(data);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static struct buffer *make_buffer(struct bar *bar, const struct appearance *appearance,
                                  const struct workspace_view *view, struct hit_map *hits)
{
    uint32_t width = bar->width, height = (uint32_t)appearance->config.height;
    /* wl_shm takes signed 32-bit byte counts and strides. */
    if (!width || !height || width > INT32_MAX / 4 ||
        height > INT32_MAX / (width * 4))
        return NULL;

    int stride = (int)(width * 4);
    int size = stride * (int)height;
    int fd = memfd_create("bar", MFD_CLOEXEC);
    if (fd < 0)
        return NULL;

    struct wl_shm_pool *pool = NULL;
    if (ftruncate(fd, size) == 0) {
        uint32_t *pixels = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
        if (pixels != MAP_FAILED) {
            paint(bar, appearance, view, hits, pixels, (int)width, (int)height);
            munmap(pixels, size);
            pool = wl_shm_create_pool(bar->shm, fd, size);
        }
    }
    close(fd);
    if (!pool)
        return NULL;

    struct buffer *buffer = calloc(1, sizeof *buffer);
    if (buffer) {
        buffer->proxy = wl_shm_pool_create_buffer(pool, 0, (int)width,
                                                 (int)height, stride,
                                                 WL_SHM_FORMAT_ARGB8888);
    }
    wl_shm_pool_destroy(pool);
    if (!buffer || !buffer->proxy) {
        free(buffer);
        return NULL;
    }

    /* The compositor retains the backing storage until it releases the buffer. */
    wl_list_insert(&bar->buffers, &buffer->link);
    wl_buffer_add_listener(buffer->proxy, &buffer_listener, buffer);
    return buffer;
}

static void configure(void *data, struct zwlr_layer_surface_v1 *layer,
                      uint32_t serial, uint32_t width, uint32_t height)
{
    (void)height;  /* Keep the requested logical height, even if the hint is stale. */
    struct bar *bar = data;
    if (bar->closed || bar->error)
        return;

    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (!width)
        width = bar->width ? bar->width : 640;
    if (width > INT32_MAX / 4) {
        bar->error = "configured surface is too wide for shared memory";
        return;
    }
    if (width == bar->width) {
        if (!bar->redraw && !bar->pending.font)
            wl_surface_commit(bar->surface);
        return;
    }
    bar->width = width;
    bar->layout_dirty = true;
    bar->redraw = true;
}

static bool set_input_region(struct bar *bar, const struct hit_map *hits)
{
    struct wl_region *region = wl_compositor_create_region(bar->compositor);
    if (!region)
        return false;
    if (bar->ipc_ready) {
        for (unsigned i = 0; i < BAR_MAX_WORKSPACES; ++i) {
            struct hit_span span = hits->tags[i];
            if (span.end > span.start)
                wl_region_add(region, span.start, 0, span.end - span.start, hits->height);
        }
    }
    if (hits->network.end > hits->network.start)
        wl_region_add(region, hits->network.start, 0, hits->network.end - hits->network.start, hits->height);
    if (hits->volume.end > hits->volume.start)
        wl_region_add(region, hits->volume.start, 0, hits->volume.end - hits->volume.start, hits->height);
    wl_surface_set_input_region(bar->surface, region);
    wl_region_destroy(region);
    return true;
}

static void set_geometry(struct bar *bar, const struct bar_config *config)
{
    uint32_t edge = config->bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM :
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
    zwlr_layer_surface_v1_set_anchor(bar->layer,
        edge | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(bar->layer, 0, (uint32_t)config->height);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer, config->height);
}

static const char *render(struct bar *bar, const struct appearance *appearance,
                          const struct workspace_view *view, bool geometry_changed)
{
    int height = appearance->config.height;
    struct hit_map hits = {.height = height};
    struct buffer *buffer = make_buffer(bar, appearance, view, &hits);
    if (!buffer)
        return "could not create the shared-memory buffer";
    if (!set_input_region(bar, &hits)) {
        destroy_buffer(buffer);
        return "could not create the bar input region";
    }

    if (geometry_changed)
        set_geometry(bar, &appearance->config);
    wl_surface_attach(bar->surface, buffer->proxy, 0, 0);
    wl_surface_damage(bar->surface, 0, 0, (int)bar->width, height);
    wl_surface_commit(bar->surface);
    bar->hits = hits;
    bar->redraw = false;
    return NULL;
}

static void queue_reload(struct bar *bar)
{
    /* The latest reload supersedes any candidate waiting for a free buffer. */
    if (bar->pending.font) {
        FT_Done_Face(bar->pending.font);
        bar->pending.font = NULL;
    }
    load_appearance(bar, &bar->pending);
    close_unused_theme_watch(bar);
}

static void apply_reload(struct bar *bar)
{
    update_right_text(&bar->pending, &bar->network, &bar->volume);
    struct workspace_view view = layout_workspaces(bar, &bar->pending);
    const struct bar_config *old = &bar->appearance.config;
    const struct bar_config *next = &bar->pending.config;
    bool geometry_changed = next->height != old->height || next->bottom != old->bottom;
    const char *error = render(bar, &bar->pending, &view, geometry_changed);
    if (error) {
        fprintf(stderr, "bar: keeping current configuration: %s\n", error);
        FT_Done_Face(bar->pending.font);
        if (!bar->redraw)
            wl_surface_commit(bar->surface);
    } else {
        /* A different volume command starts over; the text waits for its first line. */
        if (next->volume_enabled != old->volume_enabled ||
            strcmp(next->volume_command, old->volume_command))
            bar->volume.retry = -1;
        FT_Done_Face(bar->appearance.font);
        bar->appearance = bar->pending;
        bar->workspaces = view;
        bar->layout_dirty = false;
    }
    bar->pending.font = NULL;
    close_unused_theme_watch(bar);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
    (void)layer;
    struct bar *bar = data;
    bar->closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = configure,
    .closed = layer_closed,
};

static void ipc_tag_count(void *data, struct zdwl_ipc_manager_v2 *manager, uint32_t count)
{
    (void)manager;
    struct bar *bar = data;
    bar->ipc_tags = count > 32 ? 32 : count;
    bar->layout_dirty = true;
}

static void ipc_manager_string(void *data, struct zdwl_ipc_manager_v2 *manager,
                               const char *value)
{
    (void)data;
    (void)manager;
    (void)value;
}

static const struct zdwl_ipc_manager_v2_listener ipc_manager_listener = {
    .tags = ipc_tag_count,
    .layout = ipc_manager_string,
};

static void set_mask_bit(uint32_t *mask, uint32_t bit, bool set)
{
    *mask = (*mask & ~bit) | (set ? bit : 0);
}

static void ipc_tag(void *data, struct zdwl_ipc_output_v2 *output, uint32_t tag,
                    uint32_t state, uint32_t clients, uint32_t focused)
{
    (void)output;
    (void)focused;
    if (tag >= BAR_MAX_WORKSPACES)
        return;
    struct bar *bar = data;
    struct workspace_state *pending = &bar->ipc_pending;
    uint32_t bit = UINT32_C(1) << tag;
    set_mask_bit(&pending->active, bit, state & ZDWL_IPC_OUTPUT_V2_TAG_STATE_ACTIVE);
    set_mask_bit(&pending->urgent, bit, state & ZDWL_IPC_OUTPUT_V2_TAG_STATE_URGENT);
    set_mask_bit(&pending->occupied, bit, clients != 0);
}

static void ipc_frame(void *data, struct zdwl_ipc_output_v2 *output)
{
    (void)output;
    struct bar *bar = data;
    if (!bar->ipc_ready || !workspaces_equal(bar->ipc_current, bar->ipc_pending))
        bar->layout_dirty = true;
    if (!bar->ipc_ready)
        bar->redraw = true;  /* Install the input region with the first IPC frame. */
    bar->ipc_current = bar->ipc_pending;
    bar->ipc_ready = true;
}

/* The existing IPC version still sends these events even though bar uses only tags. */
static void ipc_ignore(void *data, struct zdwl_ipc_output_v2 *output)
{
    (void)data;
    (void)output;
}

static void ipc_ignore_uint(void *data, struct zdwl_ipc_output_v2 *output, uint32_t value)
{
    (void)value;
    ipc_ignore(data, output);
}

static void ipc_ignore_string(void *data, struct zdwl_ipc_output_v2 *output, const char *value)
{
    (void)value;
    ipc_ignore(data, output);
}

static void ipc_geometry(void *data, struct zdwl_ipc_output_v2 *output,
                         int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    ipc_ignore(data, output);
}

static const struct zdwl_ipc_output_v2_listener ipc_output_listener = {
    .toggle_visibility = ipc_ignore,
    .active = ipc_ignore_uint,
    .tag = ipc_tag,
    .layout = ipc_ignore_uint,
    .title = ipc_ignore_string,
    .appid = ipc_ignore_string,
    .layout_symbol = ipc_ignore_string,
    .frame = ipc_frame,
    .fullscreen = ipc_ignore_uint,
    .floating = ipc_ignore_uint,
    .focused_geometry = ipc_geometry,
};

static void update_ipc_output(struct bar *bar)
{
    struct output *target = NULL;
    if (bar->ipc_manager) {
        if (bar->ipc_target && bar->ipc_target->entered)
            return;
        struct output *output;
        wl_list_for_each(output, &bar->outputs, link) {
            if (output->entered) {
                target = output;
                break;
            }
        }
    }
    if (target == bar->ipc_target)
        return;

    if (bar->ipc_output)
        zdwl_ipc_output_v2_release(bar->ipc_output);
    bar->ipc_output = NULL;
    bar->ipc_target = target;
    bar->ipc_ready = false;
    bar->ipc_pending = (struct workspace_state){0};
    memset(bar->hits.tags, 0, sizeof bar->hits.tags);
    bar->axis_remainder = 0;
    bar->wheel_remainder = 0;
    bar->layout_dirty = true;
    bar->redraw = true;

    if (target) {
        bar->ipc_output = zdwl_ipc_manager_v2_get_output(bar->ipc_manager, target->proxy);
        if (bar->ipc_output)
            zdwl_ipc_output_v2_add_listener(bar->ipc_output, &ipc_output_listener, bar);
    }
}

static void surface_output(struct bar *bar, struct wl_output *proxy, bool entered)
{
    struct output *output;
    wl_list_for_each(output, &bar->outputs, link) {
        if (output->proxy == proxy) {
            output->entered = entered;
            update_ipc_output(bar);
            return;
        }
    }
}

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)surface;
    surface_output(data, output, true);
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)surface;
    surface_output(data, output, false);
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

static void reap_children(struct bar *bar)
{
    for (;;) {
        pid_t pid = waitpid(-1, NULL, WNOHANG);
        if (pid < 0 && errno == EINTR)
            continue;
        if (pid < 0 && errno == ECHILD)
            bar->network_child = bar->volume.pid = 0;
        if (pid <= 0)
            return;
        if (pid == bar->network_child)
            bar->network_child = 0;
        if (pid == bar->volume.pid)
            bar->volume.pid = 0;
    }
}

/* OUTPUT >= 0 becomes the child's stdout, and the child gets its own process group. */
static int spawn_command(struct bar *bar, const struct command *command, int output, pid_t *pid)
{
    char *argv[BAR_NETWORK_COMMAND_SIZE / 2 + 1];
    for (size_t i = 0; i < command->count; i++)
        argv[i] = (char *)command->text + command->offsets[i];
    argv[command->count] = NULL;

    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (error)
        return error;
    error = posix_spawnattr_setsigmask(&attributes, &bar->child_mask);
    if (!error)
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK |
                                         (output >= 0 ? POSIX_SPAWN_SETPGROUP : 0));
    if (!error && output >= 0)
        error = posix_spawnattr_setpgroup(&attributes, 0);

    posix_spawn_file_actions_t files;
    if (!error)
        error = posix_spawn_file_actions_init(&files);
    if (!error) {
        error = posix_spawn_file_actions_addopen(&files, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (!error && output >= 0)
            error = posix_spawn_file_actions_adddup2(&files, output, STDOUT_FILENO);
        if (!error)
            error = posix_spawnp(pid, argv[0], &files, &attributes, argv, environ);
        posix_spawn_file_actions_destroy(&files);
    }
    posix_spawnattr_destroy(&attributes);
    return error;
}

static void launch_network(struct bar *bar)
{
    reap_children(bar);
    if (bar->network_child)
        return;

    int error = spawn_command(bar, &bar->appearance.network_command, -1, &bar->network_child);
    if (error)
        fprintf(stderr, "bar: could not launch [network] command: %s\n", strerror(error));
}

/* One-shot commands for clicks and scrolling; SIGCHLD reaps them. */
static void run_volume_command(struct bar *bar, const struct command *command, const char *name)
{
    if (!command->count)
        return;
    pid_t pid;
    int error = spawn_command(bar, command, -1, &pid);
    if (error)
        fprintf(stderr, "bar: could not run [volume] %s: %s\n", name, strerror(error));
}

static void stop_volume(struct volume_feed *volume)
{
    if (volume->fd >= 0)
        close(volume->fd);
    volume->fd = -1;
    if (volume->pid > 0)
        kill(-volume->pid, SIGTERM);  /* The whole group, so a pipeline doesn't linger. */
    volume->pid = 0;
    volume->used = 0;
    volume->overflow = false;
    if (volume->have)
        volume->changed = true;
    volume->have = false;
}

static void start_volume(struct bar *bar, int64_t now)
{
    struct volume_feed *volume = &bar->volume;
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
        volume->retry = now + 30000;
        return;
    }
    pid_t pid;
    int error = spawn_command(bar, &bar->appearance.volume_command, pipe_fds[1], &pid);
    close(pipe_fds[1]);
    if (error) {
        fprintf(stderr, "bar: could not start [volume] command: %s\n", strerror(error));
        close(pipe_fds[0]);
        volume->retry = now + 30000;
        return;
    }
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    volume->fd = pipe_fds[0];
    volume->pid = pid;
    volume->started = now;
}

/* "Volume: 0.30" or "Volume: 0.30 [MUTED]" from wpctl or vol; anything else as it is. */
static void volume_line(struct volume_feed *volume, const char *line)
{
    double level;
    int used = 0;
    const char *rest = NULL;
    if (sscanf(line, "Volume: %lf%n", &level, &used) == 1 && used > 0 && isfinite(level) &&
        level >= 0 && level <= 10)
        rest = line + used;
    if (rest && (!*rest || !strcmp(rest, " [MUTED]"))) {
        volume->percent = (int)(level * 100 + 0.5);
        volume->muted = *rest != '\0';
        volume->raw[0] = '\0';
    } else {
        size_t length = 0;
        for (const unsigned char *c = (const unsigned char *)line;
             *c && length + 1 < sizeof volume->raw; ++c)
            if (*c >= 0x20 && *c != 0x7f)
                volume->raw[length++] = (char)*c;
        volume->raw[length] = '\0';
        if (!length)
            return;
    }
    volume->have = volume->changed = true;
}

static void read_volume(struct volume_feed *volume, int64_t now)
{
    char bytes[1024];
    for (int round = 0; round < 16; ++round) {
        ssize_t count = read(volume->fd, bytes, sizeof bytes);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && errno == EAGAIN)
            return;
        if (count <= 0) {
            /* Restart after a second, backing off to 30 s while it keeps exiting early. */
            stop_volume(volume);
            if (now - volume->started > 10000 || !volume->delay)
                volume->delay = 1000;
            else if (volume->delay < 30000)
                volume->delay *= 2;
            volume->retry = now + volume->delay;
            return;
        }
        for (ssize_t i = 0; i < count; ++i) {
            if (bytes[i] == '\n') {
                volume->partial[volume->used] = '\0';
                if (!volume->overflow)
                    volume_line(volume, volume->partial);
                volume->used = 0;
                volume->overflow = false;
            } else if (volume->used + 1 < sizeof volume->partial) {
                volume->partial[volume->used++] = bytes[i];
            } else {
                volume->overflow = true;
            }
        }
    }
}

static void service_volume(struct bar *bar, struct pollfd *fd, int64_t now, int64_t *deadline)
{
    struct volume_feed *volume = &bar->volume;
    *fd = (struct pollfd){.fd = -1};
    if (volume->retry < 0) {
        stop_volume(volume);
        volume->retry = 0;
        volume->delay = 0;
    }
    if (!bar->appearance.config.volume_enabled || !bar->appearance.volume_command.count) {
        stop_volume(volume);
        return;
    }
    if (volume->fd < 0 && now >= volume->retry)
        start_volume(bar, now);
    if (volume->fd >= 0)
        *fd = (struct pollfd){.fd = volume->fd, .events = POLLIN};
    else if (volume->retry < *deadline)
        *deadline = volume->retry;
}

static bool pointer_hits(const struct bar *bar, struct hit_span span)
{
    return bar->pointer_inside &&
           bar->pointer_y >= 0 && bar->pointer_y < bar->hits.height &&
           bar->pointer_x >= span.start && bar->pointer_x < span.end;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)serial;
    (void)time;
    struct bar *bar = data;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED ||
        (button != BTN_LEFT && button != BTN_RIGHT && button != BTN_MIDDLE))
        return;

    if (pointer_hits(bar, bar->hits.volume)) {
        if (button == BTN_LEFT)
            run_volume_command(bar, &bar->appearance.volume_click, "click");
        else if (button == BTN_MIDDLE)
            run_volume_command(bar, &bar->appearance.volume_middle_click, "middle_click");
        return;
    }
    if (button == BTN_MIDDLE)
        return;

    if (button == BTN_LEFT && pointer_hits(bar, bar->hits.network)) {
        launch_network(bar);
        return;
    }

    for (size_t i = 0; bar->ipc_ready && i < bar->appearance.config.label_count && i < bar->ipc_tags; ++i) {
        if (!pointer_hits(bar, bar->hits.tags[i]))
            continue;
        uint32_t bit = UINT32_C(1) << i;
        if (button == BTN_LEFT)
            zdwl_ipc_output_v2_set_tags(bar->ipc_output, bit, 1);
        else if (button == BTN_RIGHT && (bar->ipc_current.active ^ bit))
            zdwl_ipc_output_v2_set_tags(bar->ipc_output, bar->ipc_current.active ^ bit, 0);
        return;
    }
}

static void scroll_tags(struct bar *bar, int steps)
{
    int count = (int)bar->ipc_tags;
    if (count > (int)bar->appearance.config.label_count)
        count = (int)bar->appearance.config.label_count;
    if (!count)
        return;
    steps = -(steps % count);
    if (!steps)
        return;
    int base = steps > 0 ? -1 : 0;
    /* Advance from the last active tag, or back up from the first. */
    for (int i = 0; i < count; ++i) {
        if (!(bar->ipc_current.active & (UINT32_C(1) << i)))
            continue;
        base = i;
        if (steps < 0)
            break;
    }
    int next = (base + steps + count) % count;
    zdwl_ipc_output_v2_set_tags(bar->ipc_output, UINT32_C(1) << next, 1);
}

static bool pointer_over_tags(const struct bar *bar)
{
    for (unsigned i = 0; bar->ipc_ready && i < bar->appearance.config.label_count &&
         i < bar->ipc_tags; ++i)
        if (pointer_hits(bar, bar->hits.tags[i]))
            return true;
    return false;
}

enum { SCROLL_NONE, SCROLL_TAGS, SCROLL_VOLUME };

/* Positive steps scroll down. A fast wheel is capped so it can't start dozens of commands. */
static void scroll_volume(struct bar *bar, int steps)
{
    const struct command *command = steps < 0 ? &bar->appearance.volume_scroll_up :
                                                &bar->appearance.volume_scroll_down;
    int count = steps < 0 ? -steps : steps;
    for (int i = 0; i < count && i < 5; ++i)
        run_volume_command(bar, command, steps < 0 ? "scroll_up" : "scroll_down");
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)pointer;
    struct bar *bar = data;
    int target = pointer_over_tags(bar) ? SCROLL_TAGS :
                 pointer_hits(bar, bar->hits.volume) ? SCROLL_VOLUME : SCROLL_NONE;
    if (target != bar->scroll_target) {
        bar->axis_remainder = 0;
        bar->wheel_remainder = 0;
        bar->scroll_target = target;
    }

    int steps = 0;
    if (target && bar->axis_value120) {
        int64_t total = bar->wheel_remainder + bar->axis_value120;
        int64_t whole = total / 120;
        steps = whole > INT_MAX ? INT_MAX : whole < INT_MIN ? INT_MIN : (int)whole;
        bar->wheel_remainder = total % 120;
        bar->axis_remainder = 0;
    } else if (target && bar->axis_delta) {
        /* Continuous scroll devices move by one step per ten logical pixels. */
        double total = bar->axis_remainder + bar->axis_delta;
        double whole = total / 10;
        steps = whole > INT_MAX ? INT_MAX : whole < INT_MIN ? INT_MIN : (int)whole;
        bar->axis_remainder = total - steps * 10.0;
        bar->wheel_remainder = 0;
    }
    if (steps && target == SCROLL_TAGS)
        scroll_tags(bar, steps);
    else if (steps && target == SCROLL_VOLUME)
        scroll_volume(bar, steps);
    if (bar->axis_stop) {
        bar->axis_remainder = 0;
        bar->wheel_remainder = 0;
    }
    bar->axis_stop = false;
    bar->axis_delta = 0;
    bar->axis_value120 = 0;
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    (void)time;
    struct bar *bar = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        bar->axis_delta += wl_fixed_to_double(value);
        if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION)
            pointer_frame(data, pointer);
    }
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                              uint32_t axis)
{
    (void)pointer;
    (void)time;
    struct bar *bar = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        bar->axis_stop = true;
}

static void pointer_value120(void *data, struct wl_pointer *pointer, uint32_t axis,
                             int32_t value120)
{
    (void)pointer;
    struct bar *bar = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        bar->axis_value120 += value120;
}

static void pointer_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                             int32_t discrete)
{
    (void)pointer;
    struct bar *bar = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        bar->axis_value120 += (int64_t)discrete * 120;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)data;
    (void)pointer;
    (void)source;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)time;
    struct bar *bar = data;
    bar->pointer_x = wl_fixed_to_double(x);
    bar->pointer_y = wl_fixed_to_double(y);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    struct bar *bar = data;
    bar->pointer_inside = surface == bar->surface;
    pointer_motion(data, pointer, serial, x, y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    (void)serial;
    (void)surface;
    struct bar *bar = data;
    bar->pointer_inside = false;
    pointer_frame(data, pointer);
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
    .axis_discrete = pointer_discrete,
    .axis_value120 = pointer_value120,
};

static void release_pointer(struct bar *bar)
{
    if (!bar->pointer)
        return;
    if (wl_pointer_get_version(bar->pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
        wl_pointer_release(bar->pointer);
    else
        wl_pointer_destroy(bar->pointer);
    bar->pointer = NULL;
    bar->pointer_inside = false;
    pointer_frame(bar, NULL);
}

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct bar *bar = data;
    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER)) {
        release_pointer(bar);
    } else if (!bar->pointer) {
        bar->pointer = wl_seat_get_pointer(seat);
        if (bar->pointer)
            wl_pointer_add_listener(bar->pointer, &pointer_listener, bar);
    }
}

static void seat_label(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_label,
};

static void release_seat(struct bar *bar)
{
    release_pointer(bar);
    if (!bar->seat)
        return;
    if (wl_seat_get_version(bar->seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
        wl_seat_release(bar->seat);
    else
        wl_seat_destroy(bar->seat);
    bar->seat = NULL;
}

static void registry_ready(void *data, struct wl_callback *sync, uint32_t serial)
{
    (void)serial;
    struct bar *bar = data;
    wl_callback_destroy(sync);
    bar->sync = NULL;

    if (!bar->compositor || !bar->shm || !bar->shell) {
        bar->error = "compositor must provide wl_compositor, wl_shm and wlr-layer-shell";
        return;
    }

    bar->surface = wl_compositor_create_surface(bar->compositor);
    if (!bar->surface) {
        bar->error = "could not create the Wayland surface";
        return;
    }
    wl_surface_add_listener(bar->surface, &surface_listener, bar);
    bar->layer = zwlr_layer_shell_v1_get_layer_surface(
        bar->shell, bar->surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "bar");
    if (!bar->layer || !set_input_region(bar, &bar->hits)) {
        bar->error = "could not create the layer surface or input region";
        return;
    }

    zwlr_layer_surface_v1_add_listener(bar->layer, &layer_listener, bar);
    set_geometry(bar, &bar->appearance.config);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        bar->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(bar->surface);
}

static const struct wl_callback_listener sync_listener = {
    .done = registry_ready,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
    struct bar *bar = data;
    if (!bar->compositor && strcmp(interface, wl_compositor_interface.name) == 0)
        bar->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    else if (!bar->shm && strcmp(interface, wl_shm_interface.name) == 0)
        bar->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!bar->shell && strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
        bar->shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    else if (!bar->ipc_manager && !strcmp(interface, zdwl_ipc_manager_v2_interface.name)) {
        bar->ipc_manager = wl_registry_bind(registry, name, &zdwl_ipc_manager_v2_interface,
                                            version < 2 ? version : 2);
        if (bar->ipc_manager) {
            bar->ipc_name = name;
            zdwl_ipc_manager_v2_add_listener(bar->ipc_manager, &ipc_manager_listener, bar);
            update_ipc_output(bar);
            bar->layout_dirty = true;
        }
    } else if (!strcmp(interface, wl_output_interface.name)) {
        struct output *output = calloc(1, sizeof *output);
        if (!output)
            return;
        output->proxy = wl_registry_bind(registry, name, &wl_output_interface, 1);
        if (output->proxy) {
            output->name = name;
            wl_list_insert(&bar->outputs, &output->link);
        } else {
            free(output);
        }
    } else if (!bar->seat && !strcmp(interface, wl_seat_interface.name)) {
        bar->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                     version < 8 ? version : 8);
        if (bar->seat) {
            bar->seat_name = name;
            wl_seat_add_listener(bar->seat, &seat_listener, bar);
        }
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)registry;
    struct bar *bar = data;
    if (bar->ipc_manager && name == bar->ipc_name) {
        zdwl_ipc_manager_v2_release(bar->ipc_manager);
        bar->ipc_manager = NULL;
        bar->ipc_tags = 0;
        update_ipc_output(bar);
        bar->layout_dirty = true;
    } else if (bar->seat && name == bar->seat_name) {
        release_seat(bar);
    } else {
        struct output *output;
        wl_list_for_each(output, &bar->outputs, link) {
            if (output->name != name)
                continue;
            wl_list_remove(&output->link);
            output->entered = false;
            update_ipc_output(bar);
            wl_output_destroy(output->proxy);
            free(output);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static int64_t monotonic_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0)
        return -1;
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int status_input_flags(void)
{
    struct stat info;
    if (fstat(STDIN_FILENO, &info) < 0 || !S_ISFIFO(info.st_mode))
        return -1;
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) == O_WRONLY ||
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return flags;
}

enum { POLL_WAYLAND, POLL_SIGNALS, POLL_STATUS, POLL_NETWORK, POLL_VOLUME, POLL_THEME, POLL_COUNT };

static bool handle_signals(struct bar *bar, int fd)
{
    struct signalfd_siginfo info;
    bool reload = false;
    while (read(fd, &info, sizeof info) == sizeof info) {
        if (info.ssi_signo == SIGCHLD)
            reap_children(bar);
        else if (info.ssi_signo == SIGHUP)
            reload = true;
        else
            return false;
    }
    if (reload)
        queue_reload(bar);
    return true;
}

static int run(struct bar *bar, int signals, int status_fd)
{
    struct pollfd fds[POLL_COUNT] = {
        [POLL_WAYLAND] = {.fd = wl_display_get_fd(bar->display)},
        [POLL_SIGNALS] = {.fd = signals, .events = POLLIN},
        [POLL_STATUS] = {.fd = status_fd, .events = POLLIN},
    };

    for (;;) {
        if (wl_display_dispatch_pending(bar->display) < 0)
            return -1;
        if (bar->closed || bar->error)
            return 0;

        int64_t now = monotonic_ms();
        if (now < 0) {
            bar->error = "could not read the monotonic clock";
            return 0;
        }
        bool enabled = bar->appearance.config.network_enabled;
        int64_t deadline = INT64_MAX;
        service_network(&bar->network, enabled, &fds[POLL_NETWORK], now, &deadline);
        service_volume(bar, &fds[POLL_VOLUME], now, &deadline);
        if (now >= bar->clock_next || bar->network.changed || bar->volume.changed) {
            if (update_right_text(&bar->appearance, &bar->network, &bar->volume)) {
                bar->redraw = true;
                bar->layout_dirty = true;
            }
            bar->network.changed = bar->volume.changed = false;
            if (now >= bar->clock_next)
                bar->clock_next = now + 1000;
        }
        if (bar->width) {
            /* Keep reading events even when both buffers belong to the compositor. */
            if (bar->pending.font && wl_list_length(&bar->buffers) < 2)
                apply_reload(bar);
            update_layout(bar);
            if (bar->redraw && wl_list_length(&bar->buffers) < 2) {
                bar->error = render(bar, &bar->appearance, &bar->workspaces, false);
                if (bar->error)
                    return 0;
            }
        }

        fds[POLL_WAYLAND].events = POLLIN;
        fds[POLL_THEME] = (struct pollfd){.fd = bar->theme_watch.fd, .events = POLLIN};
        if (wl_display_flush(bar->display) < 0) {
            if (errno != EAGAIN)
                return -1;
            fds[POLL_WAYLAND].events |= POLLOUT;
        }

        now = monotonic_ms();
        if (now < 0) {
            bar->error = "could not read the monotonic clock";
            return 0;
        }
        if (enabled != bar->appearance.config.network_enabled)
            deadline = now;
        if (bar->clock_next < deadline)
            deadline = bar->clock_next;
        int timeout = deadline > now ? (int)(deadline - now) : 0;
        if (wl_display_prepare_read(bar->display) < 0)
            continue;
        if (poll(fds, POLL_COUNT, timeout) < 0) {
            int error = errno;
            wl_display_cancel_read(bar->display);
            if (error == EINTR)
                continue;
            return -1;
        }
        if (fds[POLL_WAYLAND].revents & POLLIN) {
            if (wl_display_read_events(bar->display) < 0)
                return -1;
        } else {
            wl_display_cancel_read(bar->display);
        }
        if (fds[POLL_WAYLAND].revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
        if (fds[POLL_VOLUME].revents && bar->volume.fd == fds[POLL_VOLUME].fd)
            read_volume(&bar->volume, monotonic_ms());
        if (theme_watch_changed(&bar->theme_watch, fds[POLL_THEME].revents))
            refresh_bar_palette(bar);
        if ((fds[POLL_SIGNALS].revents & POLLIN) && !handle_signals(bar, signals))
            return 0;
        if (fds[POLL_STATUS].revents && !drain_status(bar, fds[POLL_STATUS].fd)) {
            fds[POLL_STATUS].fd = -1;
            bar->selected = bar->first_monitor = NULL;
            bar->layout_dirty = true;
        }
    }
}

static void cleanup(struct bar *bar)
{
    close_theme_watch(&bar->theme_watch);
    stop_volume(&bar->volume);
    reap_children(bar);
    network_disconnect(&bar->network);
    release_seat(bar);
    if (bar->ipc_output)
        zdwl_ipc_output_v2_release(bar->ipc_output);
    if (bar->ipc_manager)
        zdwl_ipc_manager_v2_release(bar->ipc_manager);
    struct output *output, *next_output;
    wl_list_for_each_safe(output, next_output, &bar->outputs, link) {
        wl_output_destroy(output->proxy);
        wl_list_remove(&output->link);
        free(output);
    }
    while (bar->monitors) {
        struct monitor *next = bar->monitors->next;
        free(bar->monitors);
        bar->monitors = next;
    }
    if (bar->layer)
        zwlr_layer_surface_v1_destroy(bar->layer);
    if (bar->surface)
        wl_surface_destroy(bar->surface);

    struct buffer *buffer, *next;
    wl_list_for_each_safe(buffer, next, &bar->buffers, link)
        destroy_buffer(buffer);

    if (bar->sync)
        wl_callback_destroy(bar->sync);
    /* The layer-shell destroy request was added in version 3. */
    if (bar->shell)
        wl_proxy_destroy((struct wl_proxy *)bar->shell);
    if (bar->shm)
        wl_shm_destroy(bar->shm);
    if (bar->compositor)
        wl_compositor_destroy(bar->compositor);
    if (bar->registry)
        wl_registry_destroy(bar->registry);
    if (bar->display) {
        wl_display_flush(bar->display);
        wl_display_disconnect(bar->display);
    }

    if (bar->pending.font)
        FT_Done_Face(bar->pending.font);
    if (bar->appearance.font)
        FT_Done_Face(bar->appearance.font);
    if (bar->font_config)
        FcConfigDestroy(bar->font_config);
    free(bar->config_path);
    if (bar->freetype)
        FT_Done_FreeType(bar->freetype);
}

static bool config_path(char **path, const char *override)
{
    if (override) {
        *path = strdup(override);
        return *path != NULL;
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return asprintf(path, "%s/bar/bar.conf", xdg) >= 0;
    const char *home = getenv("HOME");
    return !home || !*home || asprintf(path, "%s/.config/bar/bar.conf", home) >= 0;
}

int main(int argc, char **argv)
{
    const char *override = NULL;
    int option;
    opterr = 0;
    while ((option = getopt(argc, argv, "c:")) != -1) {
        if (option != 'c' || !*optarg) {
            fprintf(stderr, "usage: bar [-c PATH]\n");
            return EXIT_FAILURE;
        }
        override = optarg;
    }
    if (optind != argc) {
        fprintf(stderr, "usage: bar [-c PATH]\n");
        return EXIT_FAILURE;
    }

    sigset_t mask, child_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &mask, &child_mask) < 0) {
        perror("bar: sigprocmask");
        return EXIT_FAILURE;
    }
    struct sigaction child_action = { .sa_handler = SIG_DFL, .sa_flags = SA_NOCLDSTOP };
    sigemptyset(&child_action.sa_mask);
    if (sigaction(SIGCHLD, &child_action, NULL) < 0) {
        perror("bar: sigaction");
        return EXIT_FAILURE;
    }
    int signals = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signals < 0) {
        perror("bar: signalfd");
        return EXIT_FAILURE;
    }

    struct bar bar = {.theme_watch = {.fd = -1}, .volume = {.fd = -1}};
    bar.child_mask = child_mask;
    wl_list_init(&bar.buffers);
    wl_list_init(&bar.outputs);
    setlocale(LC_TIME, "");
    int status = EXIT_FAILURE;
    if (!config_path(&bar.config_path, override)) {
        bar.error = "could not allocate the configuration path";
    } else if (FT_Init_FreeType(&bar.freetype)) {
        bar.error = "could not initialize FreeType";
    } else if (load_appearance(&bar, &bar.appearance)) {
        bar.display = wl_display_connect(NULL);
        if (!bar.display) {
            bar.error = "could not connect to the Wayland compositor";
        } else {
            bar.registry = wl_display_get_registry(bar.display);
            bar.sync = wl_display_sync(bar.display);
            if (!bar.registry || !bar.sync) {
                bar.error = "could not initialize the Wayland registry";
            } else {
                wl_registry_add_listener(bar.registry, &registry_listener, &bar);
                wl_callback_add_listener(bar.sync, &sync_listener, &bar);
                int flags = status_input_flags();
                int status_fd = flags < 0 ? -1 : STDIN_FILENO;
                if (run(&bar, signals, status_fd) < 0)
                    bar.error = "Wayland event loop failed";
                if (status_fd >= 0)
                    fcntl(status_fd, F_SETFL, flags);
                status = bar.error ? EXIT_FAILURE : EXIT_SUCCESS;
            }
        }
    }

    if (bar.error)
        fprintf(stderr, "bar: %s\n", bar.error);
    cleanup(&bar);
    close(signals);
    return status;
}
