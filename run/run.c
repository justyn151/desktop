#define _GNU_SOURCE
#include "run.h"
#include "layer-shell-client-protocol.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wctype.h>
#include <xkbcommon/xkbcommon-compose.h>

static const unsigned char bundled_config[] = {
#include "default-config.h"
    0
};

static const struct config defaults = {
    .background = 0x353535, .foreground = 0xc5ad81,
    .highlight = 0xead4aa, .muted = 0x887d69,
    .selected_background = 0xc5ad81, .selected_foreground = 0x353535,
    .opacity = 0.78, .height = 24, .padding = 8,
    .font_size = 14, .input_width = 240, .result_spacing = 12,
    .font_file = "/usr/share/fonts/TTF/DejaVuSansMono.ttf", .prompt = "run:",
    .desktop_entries = true, .path_commands = true, .prefer_desktop_entries = true,
    .case_sensitive = false, .fuzzy = true,
    .terminal = "foot",
};

static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

enum theme_role {
    THEME_BACKGROUND, THEME_FOREGROUND, THEME_ACCENT, THEME_HIGHLIGHT, THEME_MUTED,
    THEME_SELECTION_BACKGROUND, THEME_SELECTION_FOREGROUND, THEME_BORDER_ACTIVE,
    THEME_BORDER_INACTIVE, THEME_URGENT, THEME_ROLE_COUNT,
};

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

static bool parse_scale(const char *value, double *scale)
{
    char *end;
    errno = 0;
    double number = strtod(value, &end);
    if (errno || end == value || *end || !isfinite(number) || number <= 0 ||
        value[strspn(value, "0123456789.+-eE")])
        return false;
    *scale = number;
    return true;
}

static bool parse_boolean(const char *value, bool *flag)
{
    if (!strcmp(value, "true"))
        *flag = true;
    else if (!strcmp(value, "false"))
        *flag = false;
    else
        return false;
    return true;
}

static bool config_value(struct config *cfg, const char *section,
                         const char *key, const char *value)
{
    if (!strcmp(section, "theme")) {
        if (strcmp(key, "source") || (strcmp(value, "manual") && strcmp(value, "wallpaper")))
            return false;
        cfg->wallpaper_theme = !strcmp(value, "wallpaper");
        return true;
    }
    const struct {
        const char *section, *key;
        int *field;
        int minimum, maximum;
    } numbers[] = {
        {"appearance", "height", &cfg->height, 1, 512},
        {"appearance", "padding", &cfg->padding, 0, 512},
        {"font", "size", &cfg->font_size, 1, 256},
        {"layout", "input_width", &cfg->input_width, 1, 8192},
        {"layout", "result_spacing", &cfg->result_spacing, 0, 512},
    };
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
        if (strcmp(section, numbers[i].section) || strcmp(key, numbers[i].key))
            continue;
        char *end;
        errno = 0;
        long n = strtol(value, &end, 10);
        if (end == value || *end || errno ||
            n < numbers[i].minimum || n > numbers[i].maximum)
            return false;
        *numbers[i].field = (int)n;
        return true;
    }

    if (!strcmp(section, "appearance")) {
        if (!strcmp(key, "scale"))
            return parse_scale(value, &cfg->scale);
        if (!strcmp(key, "opacity")) {
            char *end;
            double alpha = strtod(value, &end);
            if (end == value || *end || !(alpha >= 0 && alpha <= 1))
                return false;
            cfg->opacity = alpha;
            return true;
        }

        const struct { const char *key; uint32_t *field; } colors[] = {
            {"background", &cfg->background}, {"foreground", &cfg->foreground},
            {"highlight", &cfg->highlight}, {"muted", &cfg->muted},
            {"selected_background", &cfg->selected_background},
            {"selected_foreground", &cfg->selected_foreground},
        };
        for (size_t i = 0; i < sizeof colors / sizeof colors[0]; ++i) {
            if (strcmp(key, colors[i].key))
                continue;
            if (*value != '#' || strlen(value + 1) != 6 ||
                strspn(value + 1, "0123456789abcdefABCDEF") != 6)
                return false;
            *colors[i].field = (uint32_t)strtoul(value + 1, NULL, 16);
            return true;
        }
    }

    if (!strcmp(section, "font") && !strcmp(key, "file") &&
        *value && strlen(value) < sizeof cfg->font_file) {
        strcpy(cfg->font_file, value);
        return true;
    }
    if (!strcmp(section, "layout") && !strcmp(key, "prompt") &&
        strlen(value) < sizeof cfg->prompt) {
        strcpy(cfg->prompt, value);
        return true;
    }
    if (!strcmp(section, "applications")) {
        if (!strcmp(key, "desktop_entries"))
            return parse_boolean(value, &cfg->desktop_entries);
        if (!strcmp(key, "path_commands"))
            return parse_boolean(value, &cfg->path_commands);
        if (!strcmp(key, "prefer_desktop_entries"))
            return parse_boolean(value, &cfg->prefer_desktop_entries);
        if (!strcmp(key, "terminal") && *value && strlen(value) < sizeof cfg->terminal) {
            strcpy(cfg->terminal, value);
            return true;
        }
    }
    if (!strcmp(section, "search")) {
        if (!strcmp(key, "case_sensitive"))
            return parse_boolean(value, &cfg->case_sensitive);
        if (!strcmp(key, "fuzzy"))
            return parse_boolean(value, &cfg->fuzzy);
    }
    return false;
}

static bool read_config(struct config *cfg, const char *path, bool optional,
                        bool *found)
{
    FILE *file;
    if (path) {
        file = fopen(path, "r");
    } else {
        path = "bundled run.conf";
        file = fmemopen((void *)bundled_config, sizeof bundled_config - 1, "r");
    }
    if (!file) {
        if (optional && errno == ENOENT)
            return true;
        fprintf(stderr, "run: %s: %s\n", path, strerror(errno));
        return false;
    }

    *found = true;
    char *line = NULL, section[32] = "";
    size_t capacity = 0, number = 0;
    bool ok = true;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        ++number;
        if (memchr(line, '\0', (size_t)length)) {
            ok = false;
            break;
        }
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        size_t len = strlen(s);
        if (*s == '[' && s[len - 1] == ']') {
            s[len - 1] = '\0';
            s = trim(s + 1);
            if (!strcmp(s, "appearance") || !strcmp(s, "font") ||
                !strcmp(s, "layout") || !strcmp(s, "applications") || !strcmp(s, "search") ||
                !strcmp(s, "theme")) {
                strcpy(section, s);
                continue;
            }
        } else {
            char *equals = strchr(s, '=');
            if (equals) {
                *equals = '\0';
                if (config_value(cfg, section, trim(s), trim(equals + 1)))
                    continue;
            }
        }
        ok = false;
        break;
    }
    if (!ok)
        fprintf(stderr, "run: %s:%zu: invalid configuration entry\n", path, number);
    else if (!feof(file)) {
        fprintf(stderr, "run: %s: %s\n", path, strerror(errno));
        ok = false;
    }
    free(line);
    fclose(file);
    return ok;
}

static bool load_config(struct config *cfg, const char *override)
{
    bool found = false;
    if (override)
        return read_config(cfg, override, false, &found);

    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    const char *roots[] = {xdg, home};
    const char *suffixes[] = {"/run/run.conf", "/.config/run/run.conf"};
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; ++i) {
        if (!roots[i] || roots[i][0] != '/')
            continue;
        char *path;
        if (asprintf(&path, "%s%s", roots[i], suffixes[i]) < 0) {
            fprintf(stderr, "run: cannot allocate configuration path\n");
            return false;
        }
        bool ok = read_config(cfg, path, true, &found);
        free(path);
        if (!ok || found)
            return ok;
    }
    return read_config(cfg, NULL, false, &found);
}

static double bar_scale(void)
{
    const char *root = getenv("XDG_CONFIG_HOME"), *suffix = "/bar/bar.conf";
    if (!root || !*root) {
        root = getenv("HOME");
        suffix = "/.config/bar/bar.conf";
    }
    char *path;
    if (!root || !*root || asprintf(&path, "%s%s", root, suffix) < 0)
        return 1.0;
    FILE *file = fopen(path, "r");
    free(path);
    if (!file)
        return 1.0;

    double scale = 1.0;
    char *line = NULL;
    size_t capacity = 0;
    bool in_bar_section = false;
    while (getline(&line, &capacity, file) >= 0) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        size_t len = strlen(s);
        if (*s == '[' && s[len - 1] == ']') {
            s[len - 1] = '\0';
            in_bar_section = !strcmp(trim(s + 1), "bar");
        } else if (in_bar_section) {
            char *equals = strchr(s, '=');
            if (!equals)
                continue;
            *equals = '\0';
            if (!strcmp(trim(s), "scale"))
                parse_scale(trim(equals + 1), &scale);
        }
    }
    free(line);
    fclose(file);
    return scale;
}

static void apply_wallpaper_theme(struct config *cfg)
{
    if (!cfg->wallpaper_theme)
        return;
    uint32_t colors[THEME_ROLE_COUNT];
    char *path = theme_palette_path();
    bool ok = load_theme_palette(path, colors);
    free(path);
    if (!ok)
        return;
    cfg->background = colors[THEME_BACKGROUND];
    cfg->foreground = colors[THEME_FOREGROUND];
    cfg->highlight = colors[THEME_HIGHLIGHT];
    cfg->muted = colors[THEME_MUTED];
    cfg->selected_background = colors[THEME_SELECTION_BACKGROUND];
    cfg->selected_foreground = colors[THEME_SELECTION_FOREGROUND];
}

static bool scale_config(struct config *cfg)
{
    double scale = cfg->scale ? cfg->scale : bar_scale();
    const struct {
        const char *name;
        int *field;
        int minimum, maximum;
    } pixels[] = {
        {"height", &cfg->height, 1, 512},
        {"padding", &cfg->padding, 0, 512},
        {"font size", &cfg->font_size, 1, 256},
        {"input width", &cfg->input_width, 1, 8192},
        {"result spacing", &cfg->result_spacing, 0, 512},
    };
    for (size_t i = 0; i < sizeof pixels / sizeof pixels[0]; ++i) {
        double rounded = *pixels[i].field * scale + 0.5;
        if (rounded < pixels[i].minimum || rounded >= pixels[i].maximum + 1) {
            fprintf(stderr, "run: %s after scaling must be between %d and %d\n",
                    pixels[i].name, pixels[i].minimum, pixels[i].maximum);
            return false;
        }
        *pixels[i].field = (int)rounded;
    }
    return true;
}

static uint32_t utf8_next(const char **text)
{
    const unsigned char *s = (const unsigned char *)*text;
    uint32_t code = *s++;
    ++*text;
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
        if ((*s & 0xc0) != 0x80)
            return 0xfffd;
        code = (code << 6) | (*s++ & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return 0xfffd;
    *text = (const char *)s;
    return code;
}

static wchar_t *search_text(wchar_t *out, const char *text, bool case_sensitive)
{
    while (*text) {
        wint_t code = (wint_t)utf8_next(&text);
        *out++ = (wchar_t)(case_sensitive ? code : towlower(code));
    }
    *out++ = L'\0';
    return out;
}

static bool add_command(struct launcher *run, const char *dir, const char *name,
                        size_t directory)
{
    if (run->command_count == run->command_capacity) {
        size_t capacity = run->command_capacity ? run->command_capacity * 2 : 256;
        struct command *commands = reallocarray(run->commands, capacity, sizeof *commands);
        if (!commands)
            return false;
        run->commands = commands;
        run->command_capacity = capacity;
    }
    struct command cmd = {.directory = directory};
    cmd.name = strdup(name);
    if (!cmd.name || asprintf(&cmd.path, "%s/%s", dir, name) < 0) {
        free(cmd.name);
        return false;
    }
    run->commands[run->command_count++] = cmd;
    return true;
}

static bool scan_directory(struct launcher *run, const char *name, size_t index)
{
    DIR *dir = opendir(name);
    if (!dir)
        return true;

    int fd = dirfd(dir);
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_type != DT_REG && entry->d_type != DT_LNK &&
            entry->d_type != DT_UNKNOWN)
            continue;

        struct stat st;
        if (fstatat(fd, entry->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode) ||
            faccessat(fd, entry->d_name, X_OK, AT_EACCESS) != 0)
            continue;
        if (!add_command(run, name, entry->d_name, index)) {
            ok = false;
            break;
        }
    }
    closedir(dir);
    return ok;
}

static int name_order(const char *a, const char *b)
{
    int order = strcoll(a, b);
    return order ? order : strcmp(a, b);
}

static int command_order(const void *a, const void *b)
{
    const struct command *left = a, *right = b;
    int order = name_order(left->name, right->name);
    if (order)
        return order;
    return (left->directory > right->directory) - (left->directory < right->directory);
}

static bool discover_commands(struct launcher *run)
{
    const char *path = getenv("PATH");
    if (!path)
        return true;
    char *list = strdup(path);
    if (!list)
        return false;
    char *rest = list, *dir;
    size_t index = 0;
    bool ok = true;
    while ((dir = strsep(&rest, ":"))) {
        if (!scan_directory(run, *dir ? dir : ".", index++)) {
            ok = false;
            break;
        }
    }
    free(list);
    if (!ok || !run->command_count)
        return ok;

    qsort(run->commands, run->command_count, sizeof *run->commands, command_order);
    size_t unique = 1;
    for (size_t i = 1; i < run->command_count; ++i) {
        struct command *cmd = &run->commands[i];
        if (!strcmp(cmd->name, run->commands[unique - 1].name)) {
            free(cmd->name);
            free(cmd->path);
        } else {
            run->commands[unique++] = *cmd;
        }
    }
    run->command_count = unique;
    return true;
}

static const char *command_path(const struct launcher *run, const char *name)
{
    if (strchr(name, '/'))
        return name;

    size_t first = 0, last = run->command_count;
    while (first < last) {
        size_t middle = first + (last - first) / 2;
        const struct command *cmd = &run->commands[middle];
        int order = name_order(name, cmd->name);
        if (!order)
            return cmd->path;
        if (order < 0)
            last = middle;
        else
            first = middle + 1;
    }
    return NULL;
}

/* Quotes may be joined within an argument. Backslashes quote one byte outside
 * single quotes, which also preserves the rest of a UTF-8 sequence verbatim. */
static const char *parse_arguments(const char *input, char *storage, char **argv)
{
    static const char spaces[] = " \t\r\n\v\f";
    const char *s = input + strspn(input, spaces);
    while (*s) {
        char quote = '\0';
        *argv++ = storage;
        while (*s && (quote || !strchr(spaces, *s))) {
            char c = *s++;
            if (c == '\\' && quote != '\'') {
                if (!*s)
                    return "Trailing backslash";
                *storage++ = *s++;
            } else if (c == quote) {
                quote = '\0';
            } else if (!quote && (c == '\'' || c == '"')) {
                quote = c;
            } else {
                *storage++ = c;
            }
        }
        if (quote)
            return "Unclosed quote";
        *storage++ = '\0';
        s += strspn(s, spaces);
    }
    *argv = NULL;
    return NULL;
}

enum desktop_status { DESKTOP_SKIP, DESKTOP_OK, DESKTOP_NOMEM };

enum desktop_key {
    ENTRY_NAME, ENTRY_GENERIC, ENTRY_KEYWORDS, ENTRY_ICON,
    ENTRY_EXEC, ENTRY_TRY_EXEC, ENTRY_TYPE, ENTRY_TERMINAL, ENTRY_HIDDEN,
    ENTRY_NO_DISPLAY, ENTRY_PATH, ENTRY_ONLY_SHOW, ENTRY_NOT_SHOW,
    ENTRY_DBUS, ENTRY_FLATPAK, ENTRY_KEY_COUNT,
};

enum { LOCALIZED_KEYS = ENTRY_ICON + 1, DESKTOP_FILE_LIMIT = 1024 * 1024,
       DESKTOP_EXEC_LIMIT = 128 * 1024 };

struct desktop_entry {
    char *value[ENTRY_KEY_COUNT];
    char *localized[LOCALIZED_KEYS];
    unsigned locale_rank[LOCALIZED_KEYS], locale_seen[LOCALIZED_KEYS];
    bool has_localized[LOCALIZED_KEYS];
};

struct desktop_locale {
    char *language, *country, *modifier;
};

struct desktop_file {
    char *id, *path;
    size_t directory;
};

struct desktop_files {
    struct desktop_file *items;
    size_t count, capacity;
};

static struct desktop_locale split_locale(char *locale)
{
    struct desktop_locale parts = {.language = locale};
    parts.modifier = strchr(locale, '@');
    if (parts.modifier)
        *parts.modifier++ = '\0';
    char *encoding = strchr(locale, '.');
    if (encoding)
        *encoding = '\0';
    parts.country = strchr(locale, '_');
    if (parts.country)
        *parts.country++ = '\0';
    return parts;
}

static unsigned locale_rank(const struct desktop_locale *current, char *locale)
{
    struct desktop_locale entry = split_locale(locale);
    if (strcmp(entry.language, current->language) ||
        (entry.country && (!current->country || strcmp(entry.country, current->country))) ||
        (entry.modifier && (!current->modifier || strcmp(entry.modifier, current->modifier))))
        return 0;
    return 1 + (entry.country ? 2 : 0) + (entry.modifier ? 1 : 0);
}

static bool desktop_field(struct desktop_entry *entry, const struct desktop_locale *locale,
                          char *key, char *value)
{
    static const char *const keys[ENTRY_KEY_COUNT] = {
        [ENTRY_NAME] = "Name", [ENTRY_GENERIC] = "GenericName", [ENTRY_KEYWORDS] = "Keywords",
        [ENTRY_EXEC] = "Exec", [ENTRY_TRY_EXEC] = "TryExec", [ENTRY_TYPE] = "Type",
        [ENTRY_TERMINAL] = "Terminal", [ENTRY_HIDDEN] = "Hidden",
        [ENTRY_NO_DISPLAY] = "NoDisplay", [ENTRY_PATH] = "Path",
        [ENTRY_ONLY_SHOW] = "OnlyShowIn", [ENTRY_NOT_SHOW] = "NotShowIn", [ENTRY_ICON] = "Icon",
        [ENTRY_DBUS] = "DBusActivatable", [ENTRY_FLATPAK] = "X-Flatpak",
    };

    char *localized = strchr(key, '[');
    if (localized) {
        char *end = strchr(localized + 1, ']');
        if (!end || end[1] || end == localized + 1)
            return false;
        *localized++ = '\0';
        *end = '\0';
    }
    if (!*key || key[strspn(key, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-")])
        return false;

    for (size_t i = 0; i < ENTRY_KEY_COUNT; ++i) {
        if (strcmp(keys[i], key))
            continue;
        if (!localized) {
            if (entry->value[i])
                return false;
            entry->value[i] = value;
        } else {
            if (i >= LOCALIZED_KEYS)
                return false;
            entry->has_localized[i] = true;
            unsigned rank = locale_rank(locale, localized);
            if (!rank)
                break;
            if (entry->locale_seen[i] & (1u << rank))
                return false;
            entry->locale_seen[i] |= 1u << rank;
            if (rank > entry->locale_rank[i]) {
                entry->locale_rank[i] = rank;
                entry->localized[i] = value;
            }
        }
        break;
    }
    return true;
}

static enum desktop_status read_desktop(const char *path, char **contents)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return DESKTOP_SKIP;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > DESKTOP_FILE_LIMIT) {
        close(fd);
        return DESKTOP_SKIP;
    }
    size_t size = (size_t)st.st_size;
    char *buffer = malloc(size + 1);
    if (!buffer) {
        close(fd);
        return DESKTOP_NOMEM;
    }
    size_t offset = 0;
    while (offset < size) {
        ssize_t length = read(fd, buffer + offset, size - offset);
        if (length < 0 && errno == EINTR)
            continue;
        if (length <= 0)
            break;
        offset += (size_t)length;
    }
    close(fd);
    if (offset != size || memchr(buffer, '\0', size)) {
        free(buffer);
        return DESKTOP_SKIP;
    }
    buffer[size] = '\0';
    for (const char *p = buffer; *p;) {
        const char *start = p;
        if (utf8_next(&p) == 0xfffd && p == start + 1) {
            free(buffer);
            return DESKTOP_SKIP;
        }
    }
    *contents = buffer;
    return DESKTOP_OK;
}

static bool parse_desktop(char *file, const struct desktop_locale *locale,
                          struct desktop_entry *entry)
{
    char *line;
    bool main_group = false, in_main = false;
    while ((line = strsep(&file, "\n"))) {
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;
        if (*s == '[') {
            size_t length = strlen(s);
            if (length < 3 || s[length - 1] != ']')
                return false;
            s[length - 1] = '\0';
            if (strpbrk(s + 1, "[]"))
                return false;
            if (!strcmp(s + 1, "Desktop Entry")) {
                if (main_group)
                    return false;
                main_group = in_main = true;
            } else {
                if (!main_group)
                    return false;
                in_main = false;
            }
            continue;
        }
        if (!main_group)
            return false;
        if (!in_main)
            continue;
        char *equals = strchr(s, '=');
        if (!equals)
            return false;
        *equals = '\0';
        if (!desktop_field(entry, locale, trim(s), trim(equals + 1)))
            return false;
    }

    for (size_t i = 0; i < LOCALIZED_KEYS; ++i) {
        if (entry->has_localized[i] && !entry->value[i])
            return false;
    }
    return entry->value[ENTRY_NAME] && *entry->value[ENTRY_NAME] &&
           entry->value[ENTRY_TYPE] && !strcmp(entry->value[ENTRY_TYPE], "Application") &&
           entry->value[ENTRY_EXEC] && *entry->value[ENTRY_EXEC];
}

/* Return the end of the decoded value. List separators become NUL bytes. */
static char *decode_desktop_value(char *text, bool list)
{
    if (!text)
        return NULL;
    char *out = text;
    while (*text) {
        char c = *text++;
        if (c == '\\') {
            switch (*text) {
            case 's': c = ' '; break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            case ';':
                if (!list)
                    return NULL;
                c = ';';
                break;
            default:
                return NULL;
            }
            ++text;
        } else if (list && c == ';') {
            c = '\0';
        }
        *out++ = c;
    }
    *out++ = '\0';
    return out;
}

static bool list_contains(const char *list, const char *end,
                          const char *name, size_t length)
{
    for (const char *p = list; p && p < end;) {
        size_t item_length = strlen(p);
        if (item_length == length && !memcmp(p, name, length))
            return true;
        p += item_length + 1;
    }
    return false;
}

static bool desktop_visible(char *only, char *except)
{
    char *only_end = decode_desktop_value(only, true);
    char *except_end = decode_desktop_value(except, true);
    if ((only && !only_end) || (except && !except_end))
        return false;
    const char *current = getenv("XDG_CURRENT_DESKTOP");
    while (current && *current) {
        size_t length = strcspn(current, ":");
        if (length && list_contains(except, except_end, current, length))
            return false;
        if (length && list_contains(only, only_end, current, length))
            return true;
        current += length;
        if (*current)
            ++current;
    }
    return only == NULL;
}

static const char *executable_path(const struct launcher *run, const char *name)
{
    if (!strchr(name, '/'))
        return command_path(run, name);

    struct stat st;
    if (stat(name, &st) || !S_ISREG(st.st_mode) ||
        faccessat(AT_FDCWD, name, X_OK, AT_EACCESS))
        return NULL;
    return name;
}

static bool try_exec_available(const struct launcher *run, const char *name)
{
    if (*name == '/' || !strchr(name, '/'))
        return executable_path(run, name) != NULL;

    const char *path = getenv("PATH");
    if (!path)
        return false;
    char *dirs = strdup(path);
    if (!dirs)
        return false;
    bool available = false;
    char *rest = dirs, *dir;
    while ((dir = strsep(&rest, ":"))) {
        char *candidate;
        if (asprintf(&candidate, "%s/%s", *dir ? dir : ".", name) < 0)
            break;
        available = executable_path(run, candidate) != NULL;
        free(candidate);
        if (available)
            break;
    }
    free(dirs);
    return available;
}

static char *absolute_path(const char *path, const char *cwd)
{
    if (*path == '/')
        return strdup(path);
    char *absolute;
    if (!cwd || asprintf(&absolute, "%s/%s", cwd, path) < 0)
        return NULL;
    return absolute;
}

struct exec_arguments {
    char **argv;
    size_t count, capacity, bytes;
};

static enum desktop_status append_argument(struct exec_arguments *args,
                                           const char *text, size_t length)
{
    if (length >= DESKTOP_EXEC_LIMIT - args->bytes)
        return DESKTOP_SKIP;
    if (args->count + 2 > args->capacity) {
        size_t capacity = args->capacity ? 2 * args->capacity : 8;
        char **argv = reallocarray(args->argv, capacity, sizeof *argv);
        if (!argv)
            return DESKTOP_NOMEM;
        args->argv = argv;
        args->capacity = capacity;
    }
    char *argument = strndup(text, length);
    if (!argument)
        return DESKTOP_NOMEM;
    args->argv[args->count++] = argument;
    args->argv[args->count] = NULL;
    args->bytes += length + 1;
    return DESKTOP_OK;
}

static const char exec_reserved[] = "\"'\\><~|&;$*?#()`";

/* General desktop value escapes have already been decoded at this point. */
static bool read_exec_word(char **rest, char **word, bool *quoted)
{
    char *p = *rest;
    *quoted = *p == '"';
    if (*quoted) {
        *word = ++p;
        char *out = p;
        while (*p && *p != '"') {
            char c = *p++;
            if (c == '\\') {
                if (!*p || !strchr("\"\\$`", *p))
                    return false;
                c = *p++;
            } else if (c == '$' || c == '`') {
                return false;
            }
            *out++ = c;
        }
        if (*p != '"' || (p[1] && p[1] != ' ' && p[1] != '\t'))
            return false;
        *rest = p + 1;
        *out = '\0';
    } else {
        *word = p;
        while (*p && *p != ' ' && *p != '\t') {
            unsigned char c = (unsigned char)*p++;
            if (c < 0x20 || c == 0x7f || strchr(exec_reserved, c))
                return false;
        }
        *rest = *p ? p + 1 : p;
        *p = '\0';
    }
    return true;
}

/* Flatpak exports arguments using GLib shell quoting, including quoted
 * fragments attached to options. Decode only the quoting into argv. */
static bool read_flatpak_word(char **rest, char **word, bool *quoted)
{
    char *p = *rest, *out = p;
    char quote = '\0';
    *word = p;
    *quoted = false;
    while (*p && (quote || (*p != ' ' && *p != '\t'))) {
        char c = *p++;
        if (c == quote) {
            quote = '\0';
        } else if (!quote && (c == '\'' || c == '"')) {
            quote = c;
            *quoted = true;
        } else if (c == '\\' && quote != '\'') {
            if (!*p)
                return false;
            if (!quote || strchr("\"\\$`", *p))
                c = *p++;
            *out++ = c;
        } else {
            if (!quote && ((unsigned char)c < 0x20 || c == 0x7f || strchr(exec_reserved, c)))
                return false;
            *out++ = c;
        }
    }
    if (quote)
        return false;
    *rest = *p ? p + 1 : p;
    *out = '\0';
    return true;
}

static bool flatpak_command(const struct exec_arguments *args, bool exported)
{
    const char *program = args->argv[0];
    const char *slash = strrchr(program, '/');
    return !strcmp(args->argv[1], "run") &&
           (exported || !strcmp(slash ? slash + 1 : program, "flatpak"));
}

static void remove_empty_forwarding_groups(struct exec_arguments *args)
{
    size_t first = 2;
    bool forwarding = false;
    /* Exported commands put the run options before the application ID. */
    while (first < args->count && args->argv[first][0] == '-') {
        const char *option = args->argv[first++];
        if (!strcmp(option, "--"))
            break;
        if (!strcmp(option, "--file-forwarding"))
            forwarding = true;
    }
    if (!forwarding || first == args->count)
        return;

    size_t out = ++first;
    bool inside_group = false;
    for (size_t i = first; i < args->count; ++i) {
        bool marker = !strcmp(args->argv[i], "@@") || !strcmp(args->argv[i], "@@u");
        if (marker && !inside_group && i + 1 < args->count &&
            !strcmp(args->argv[i + 1], "@@")) {
            free(args->argv[i]);
            free(args->argv[++i]);
            continue;
        }
        if (marker)
            inside_group = !inside_group;
        args->argv[out++] = args->argv[i];
    }
    args->argv[out] = NULL;
    args->count = out;
}

static enum desktop_status parse_exec(char *exec, const char *name, const char *desktop_path,
                                      const char *icon, bool exported, struct exec_arguments *args)
{
    if (strlen(exec) >= DESKTOP_EXEC_LIMIT)
        return DESKTOP_SKIP;
    char expanded[DESKTOP_EXEC_LIMIT];
    bool file_code_seen = false, flatpak_run = false;
    for (char *rest = exec + strspn(exec, " \t"); *rest; rest += strspn(rest, " \t")) {
        char *word;
        bool quoted;
        bool valid = flatpak_run ? read_flatpak_word(&rest, &word, &quoted)
                                 : read_exec_word(&rest, &word, &quoted);
        if (!valid)
            return DESKTOP_SKIP;
        bool first = args->count == 0;
        if (!quoted && !strcmp(word, "%i")) {
            if (first)
                return DESKTOP_SKIP;
            if (icon && *icon) {
                enum desktop_status status = append_argument(args, "--icon", sizeof "--icon" - 1);
                if (status == DESKTOP_OK)
                    status = append_argument(args, icon, strlen(icon));
                if (status != DESKTOP_OK)
                    return status;
            }
            continue;
        }

        bool steam_uri = !strncmp(word, "steam://", sizeof "steam://" - 1);
        size_t length = 0;
        for (const char *p = word; *p;) {
            const char *part = p;
            size_t count = 1;
            if (*p == '%' && steam_uri && isxdigit((unsigned char)p[1]) &&
                isxdigit((unsigned char)p[2])) {
                /* Preserve percent-encoded bytes in Steam launch links. */
                count = 3;
                p += count;
            } else if (*p++ == '%') {
                char code = *p;
                if (!code)
                    return DESKTOP_SKIP;
                ++p;
                if (code != '%') {
                    if (first || quoted)
                        return DESKTOP_SKIP;
                    if (code == 'c' || code == 'k') {
                        part = code == 'c' ? name : desktop_path;
                        count = strlen(part);
                    } else if (strchr("fFuU", code)) {
                        bool standalone = p == word + 2 && !*p;
                        if ((file_code_seen && !flatpak_run) ||
                            ((code == 'F' || code == 'U') && !standalone))
                            return DESKTOP_SKIP;
                        file_code_seen = true;
                        continue;
                    } else if (strchr("dDnNvm", code)) {
                        continue;
                    } else {
                        return DESKTOP_SKIP;
                    }
                }
            }
            if (count >= sizeof expanded - length)
                return DESKTOP_SKIP;
            memcpy(expanded + length, part, count);
            length += count;
        }
        if (first && !length)
            return DESKTOP_SKIP;
        if (length || !*word) {
            expanded[length] = '\0';
            enum desktop_status status = append_argument(args, expanded, length);
            if (status != DESKTOP_OK)
                return status;
            if (args->count == 2)
                flatpak_run = flatpak_command(args, exported);
        }
    }
    if (flatpak_run)
        remove_empty_forwarding_groups(args);
    return args->count ? DESKTOP_OK : DESKTOP_SKIP;
}

static wchar_t *desktop_search(const char *name, const char *program, const char *generic,
                               const char *keywords, const char *keywords_end,
                               bool case_sensitive)
{
    size_t size = strlen(name) + strlen(program) + 3;
    if (generic && *generic)
        size += strlen(generic) + 1;
    if (keywords)
        size += (size_t)(keywords_end - keywords);
    wchar_t *fields = reallocarray(NULL, size, sizeof *fields);
    if (!fields)
        return NULL;

    wchar_t *out = search_text(fields, name, case_sensitive);
    out = search_text(out, program, case_sensitive);
    if (generic && *generic)
        out = search_text(out, generic, case_sensitive);
    for (const char *p = keywords; p && p < keywords_end; p += strlen(p) + 1) {
        if (*p)
            out = search_text(out, p, case_sensitive);
    }
    *out = L'\0';
    return fields;
}

static void free_application(struct application *app)
{
    free(app->name);
    free(app->path);
    free(app->directory);
    for (size_t i = 0; i < app->argc; ++i)
        free(app->argv[i]);
    free(app->argv);
    free(app->search);
}

static enum desktop_status build_application(const struct launcher *run,
                                             struct desktop_entry *entry,
                                             const char *desktop_path, const char *cwd,
                                             struct application *app)
{
    bool hidden = false, no_display = false, dbus_activatable = false;
    char **value = entry->value;
    if ((value[ENTRY_HIDDEN] && !parse_boolean(value[ENTRY_HIDDEN], &hidden)) || hidden ||
        (value[ENTRY_NO_DISPLAY] && !parse_boolean(value[ENTRY_NO_DISPLAY], &no_display)) ||
        no_display ||
        (value[ENTRY_TERMINAL] && !parse_boolean(value[ENTRY_TERMINAL], &app->terminal)) ||
        (app->terminal && !run->terminal.path) ||
        (value[ENTRY_DBUS] && !parse_boolean(value[ENTRY_DBUS], &dbus_activatable)) ||
        !desktop_visible(value[ENTRY_ONLY_SHOW], value[ENTRY_NOT_SHOW]))
        return DESKTOP_SKIP;

    for (size_t i = 0; i < LOCALIZED_KEYS; ++i) {
        if (entry->localized[i])
            value[i] = entry->localized[i];
    }

    char *name = value[ENTRY_NAME], *generic = value[ENTRY_GENERIC];
    char *exec = value[ENTRY_EXEC], *try_exec = value[ENTRY_TRY_EXEC];
    char *directory = value[ENTRY_PATH], *icon = value[ENTRY_ICON];
    char *keywords = value[ENTRY_KEYWORDS];
    char *keywords_end = decode_desktop_value(keywords, true);
    if ((keywords && !keywords_end) ||
        !decode_desktop_value(name, false) || !*name ||
        (generic && !decode_desktop_value(generic, false)) ||
        !decode_desktop_value(exec, false) ||
        (icon && !decode_desktop_value(icon, false)) ||
        (try_exec && (!decode_desktop_value(try_exec, false) ||
                       !try_exec_available(run, try_exec))) ||
        (directory && !decode_desktop_value(directory, false)))
        return DESKTOP_SKIP;

    if (directory && *directory) {
        struct stat st;
        if (stat(directory, &st) || !S_ISDIR(st.st_mode) ||
            faccessat(AT_FDCWD, directory, X_OK, AT_EACCESS))
            return DESKTOP_SKIP;
    }

    struct exec_arguments args = {0};
    bool exported = value[ENTRY_FLATPAK] && *value[ENTRY_FLATPAK];
    enum desktop_status status = parse_exec(exec, name, desktop_path, icon, exported, &args);
    app->argv = args.argv;
    app->argc = args.count;
    if (status != DESKTOP_OK)
        return status;

    const char *program = app->argv[0], *basename = strrchr(program, '/');
    if (strchr(program, '=') || (basename && *program != '/'))
        return DESKTOP_SKIP;
    const char *path = executable_path(run, program);
    if (!path || (*path != '/' && !cwd))
        return DESKTOP_SKIP;

    app->name = strdup(name);
    app->path = absolute_path(path, cwd);
    app->search = desktop_search(name, basename ? basename + 1 : program,
                                 generic, keywords, keywords_end, run->config.case_sensitive);
    if (directory && *directory)
        app->directory = strdup(directory);
    return app->name && app->path && app->search &&
           (!directory || !*directory || app->directory) ? DESKTOP_OK : DESKTOP_NOMEM;
}

static enum desktop_status load_application(const struct launcher *run,
                                            const struct desktop_file *file,
                                            const struct desktop_locale *locale,
                                            const char *cwd, struct application *app)
{
    char *contents;
    enum desktop_status status = read_desktop(file->path, &contents);
    if (status == DESKTOP_OK) {
        struct desktop_entry entry = {0};
        status = parse_desktop(contents, locale, &entry)
            ? build_application(run, &entry, file->path, cwd, app) : DESKTOP_SKIP;
        free(contents);
    }
    return status;
}

static bool add_desktop_file(struct desktop_files *files, const char *path,
                             const char *relative, size_t directory)
{
    if (files->count == files->capacity) {
        size_t capacity = files->capacity ? 2 * files->capacity : 64;
        struct desktop_file *items = reallocarray(files->items, capacity, sizeof *items);
        if (!items)
            return false;
        files->items = items;
        files->capacity = capacity;
    }
    struct desktop_file file = {
        .id = strdup(relative), .path = strdup(path), .directory = directory,
    };
    if (!file.id || !file.path) {
        free(file.id);
        free(file.path);
        return false;
    }
    for (char *p = file.id; *p; ++p) {
        if (*p == '/')
            *p = '-';
    }
    files->items[files->count++] = file;
    return true;
}

static size_t data_root_length(const char *root)
{
    size_t length = strlen(root);
    while (length && root[length - 1] == '/')
        --length;
    return length;
}

static char *data_path(const char *root, const char *suffix)
{
    size_t length = data_root_length(root);
    char *path;
    return asprintf(&path, "%.*s%s", (int)length, root, suffix) < 0 ? NULL : path;
}

static bool same_data_root(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    size_t length = data_root_length(a);
    return length == data_root_length(b) && !strncmp(a, b, length);
}

static bool scan_desktops(struct desktop_files *files, const char *root,
                          const char *suffix, size_t priority)
{
    if (!root || *root != '/')
        return true;
    char *directory = data_path(root, suffix);
    if (!directory)
        return false;

    const size_t prefix = strlen(directory) + 1;
    char *roots[] = {directory, NULL};
    /* Flatpak exports can point into the installed application tree. */
    FTS *tree = fts_open(roots, FTS_LOGICAL | FTS_NOCHDIR, NULL);
    if (!tree) {
        bool ok = errno != ENOMEM;
        free(directory);
        return ok;
    }

    bool ok = true;
    while (ok) {
        errno = 0;
        FTSENT *entry = fts_read(tree);
        if (!entry) {
            ok = errno != ENOMEM;
            break;
        }
        static const char extension[] = ".desktop";
        size_t name_length = entry->fts_namelen;
        if (entry->fts_info == FTS_F && entry->fts_level > 0 && entry->fts_pathlen > prefix &&
            name_length >= sizeof extension &&
            !strcmp(entry->fts_name + name_length - (sizeof extension - 1), extension)) {
            const char *path = entry->fts_path;
            ok = add_desktop_file(files, path, path + prefix, priority);
        }
    }
    fts_close(tree);
    free(directory);
    return ok;
}

static bool collect_desktop_files(struct desktop_files *files)
{
    const char *home = getenv("HOME"), *data_home = getenv("XDG_DATA_HOME");
    if (data_home && *data_home != '/')
        data_home = NULL;
    if (!scan_desktops(files, data_home ? data_home : home,
                       data_home ? "/applications" : "/.local/share/applications", 0))
        return false;

    const char *system = getenv("XDG_DATA_DIRS");
    if (!system || !*system)
        system = "/usr/local/share:/usr/share";
    char *dirs = strdup(system);
    if (!dirs)
        return false;
    char *data_export = data_home ? data_path(data_home, "/flatpak/exports/share") : NULL;
    bool have_home = home && *home == '/';
    char *home_export = have_home ? data_path(home, "/.local/share/flatpak/exports/share") : NULL;
    const char *fallbacks[] = {data_export, home_export, "/var/lib/flatpak/exports/share"};
    bool scanned[] = {false, same_data_root(data_export, home_export), false};
    size_t priority = 1;
    bool ok = (!data_home || data_export) && (!have_home || home_export);
    char *rest = dirs, *dir;
    while (ok && (dir = strsep(&rest, ":"))) {
        if (*dir != '/')
            continue;
        for (size_t i = 0; i < sizeof fallbacks / sizeof fallbacks[0]; ++i)
            scanned[i] |= same_data_root(dir, fallbacks[i]);
        ok = scan_desktops(files, dir, "/applications", priority++);
    }
    for (size_t i = 0; ok && i < sizeof fallbacks / sizeof fallbacks[0]; ++i) {
        if (!scanned[i] && fallbacks[i])
            ok = scan_desktops(files, fallbacks[i], "/applications", priority++);
    }
    free(data_export);
    free(home_export);
    free(dirs);
    return ok;
}

static int desktop_file_order(const void *a, const void *b)
{
    const struct desktop_file *left = a, *right = b;
    int order = strcmp(left->id, right->id);
    if (order)
        return order;
    if (left->directory != right->directory)
        return (left->directory > right->directory) - (left->directory < right->directory);
    return strcmp(left->path, right->path);
}

static bool setup_terminal(struct launcher *run, const char *cwd)
{
    struct terminal *terminal = &run->terminal;
    const char *config = run->config.terminal;
    size_t length = strlen(config);
    terminal->options = malloc(length + 1);
    terminal->argv = reallocarray(NULL, length / 2 + 2, sizeof *terminal->argv);
    if (!terminal->options || !terminal->argv)
        return false;
    if (parse_arguments(config, terminal->options, terminal->argv) || !terminal->argv[0])
        return true;
    const char *path = executable_path(run, terminal->argv[0]);
    if (!path || (*path != '/' && !cwd))
        return true;

    while (terminal->argv[terminal->argc])
        ++terminal->argc;
    terminal->path = absolute_path(path, cwd);
    return terminal->path != NULL;
}

static bool add_application(struct launcher *run, struct application app)
{
    if (run->application_count == run->application_capacity) {
        size_t capacity = run->application_capacity ? 2 * run->application_capacity : 32;
        struct application *apps = reallocarray(run->applications, capacity, sizeof *apps);
        if (!apps)
            return false;
        run->applications = apps;
        run->application_capacity = capacity;
    }
    run->applications[run->application_count++] = app;
    return true;
}

static bool load_applications(struct launcher *run, struct desktop_files *files)
{
    if (!files->count)
        return true;
    qsort(files->items, files->count, sizeof *files->items, desktop_file_order);
    const char *messages = setlocale(LC_MESSAGES, "");
    char *locale_name = strdup(messages ? messages : "C");
    if (!locale_name)
        return false;
    struct desktop_locale locale = split_locale(locale_name);
    char *cwd = getcwd(NULL, 0);
    bool ok = setup_terminal(run, cwd);

    for (size_t i = 0; ok && i < files->count; ++i) {
        const struct desktop_file *file = &files->items[i];
        /* The first file claims its ID even if the entry is hidden or unusable. */
        if (i && !strcmp(file->id, files->items[i - 1].id))
            continue;
        struct application app = {0};
        enum desktop_status status = load_application(run, file, &locale, cwd, &app);
        if (status == DESKTOP_OK && add_application(run, app))
            continue;
        free_application(&app);
        ok = status == DESKTOP_SKIP;
    }
    free(cwd);
    free(locale_name);
    return ok;
}

static bool discover_applications(struct launcher *run)
{
    struct desktop_files files = {0};
    bool ok = collect_desktop_files(&files) && load_applications(run, &files);
    for (size_t i = 0; i < files.count; ++i) {
        free(files.items[i].id);
        free(files.items[i].path);
    }
    free(files.items);
    return ok;
}

static int result_order(const void *a, const void *b)
{
    const struct result *left = a, *right = b;
    int order = name_order(left->name, right->name);
    if (order)
        return order;
    if (left->preference != right->preference)
        return left->preference > right->preference ? -1 : 1;
    return (left->order > right->order) - (left->order < right->order);
}

static bool discover_results(struct launcher *run)
{
    if (!discover_commands(run) ||
        (run->config.desktop_entries && !discover_applications(run)))
        return false;
    size_t path_count = run->config.path_commands ? run->command_count : 0;
    size_t count = path_count + run->application_count;
    if (!count)
        return true;
    run->results = calloc(count, sizeof *run->results);
    run->matches = reallocarray(NULL, count, sizeof *run->matches);
    if (!run->results || !run->matches)
        return false;
    run->result_count = count;
    for (size_t i = 0; i < path_count; ++i) {
        const struct command *cmd = &run->commands[i];
        wchar_t *fields = reallocarray(NULL, strlen(cmd->name) + 2, sizeof *fields);
        if (!fields)
            return false;
        *search_text(fields, cmd->name, run->config.case_sensitive) = L'\0';
        run->results[i] = (struct result){
            .name = cmd->name, .search = fields, .command = cmd, .order = i,
        };
    }
    for (size_t i = 0; i < run->application_count; ++i) {
        const struct application *app = &run->applications[i];
        run->results[path_count + i] = (struct result){
            .name = app->name, .search = app->search, .application = app,
            .order = path_count + i,
            .preference = run->config.prefer_desktop_entries ? (app->terminal ? 1 : 2) : 0,
        };
    }
    qsort(run->results, count, sizeof *run->results, result_order);
    for (size_t i = 0; i < count; ++i)
        run->results[i].order = i;
    return true;
}

enum {
    FUZZY_ADJACENT = 24,
    FUZZY_WORD_START = 12,
    FUZZY_FIELD_START = 18,
    FUZZY_GAP_OPEN = 5,
    FUZZY_GAP_CHAR = 3,
    FUZZY_DP_QUERY = 64,
    FUZZY_DP_CELLS = 8192,
};

struct fuzzy_alignment {
    size_t start, end;
    int64_t score;
};

static int64_t higher_score(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

static int position_bonus(const wchar_t *field, size_t index)
{
    if (!index)
        return FUZZY_FIELD_START + FUZZY_WORD_START;
    return iswalnum((wint_t)field[index]) && !iswalnum((wint_t)field[index - 1])
        ? FUZZY_WORD_START : 0;
}

/* Walk from either edge. A failed left-to-right walk proves there is no match. */
static bool greedy_alignment(const wchar_t *field, size_t start, size_t end,
                             const wchar_t *query, size_t length, bool backwards,
                             struct fuzzy_alignment *alignment)
{
    size_t matched = 0, first = 0, previous = 0;
    int64_t score = 0;
    for (size_t step = 0; step < end - start; ++step) {
        size_t index = backwards ? end - 1 - step : start + step;
        size_t q = backwards ? length - 1 - matched : matched;
        if (field[index] != query[q])
            continue;
        score += position_bonus(field, index);
        if (matched) {
            size_t gap = step - previous - 1;
            score += gap ? -FUZZY_GAP_OPEN - FUZZY_GAP_CHAR * (int64_t)gap
                         : FUZZY_ADJACENT;
        } else {
            first = index;
        }
        if (++matched == length) {
            *alignment = (struct fuzzy_alignment){
                .start = backwards ? index : first,
                .end = (backwards ? first : index) + 1,
                .score = score,
            };
            return true;
        }
        previous = step;
    }
    return false;
}

/* Keep the best score for each query prefix that ends at the previous
 * character, and one that skips it. Each skipped character also adds to
 * the span penalty. Descending updates preserve the previous prefix states. */
static int64_t optimal_alignment(const wchar_t *field, size_t field_length,
                                 const wchar_t *query, size_t length, int64_t best)
{
    const int64_t unreachable = INT64_MIN / 2;
    int64_t hit[FUZZY_DP_QUERY], gap[FUZZY_DP_QUERY];
    for (size_t j = 0; j < length; ++j)
        hit[j] = gap[j] = unreachable;

    for (size_t i = 0; i < field_length; ++i) {
        size_t reachable = i + 1 < length ? i + 1 : length;
        int bonus = position_bonus(field, i);
        for (size_t count = reachable; count > 0; --count) {
            size_t j = count - 1;
            int64_t previous = hit[j];
            if (field[i] == query[j]) {
                int64_t predecessor = j ? higher_score(hit[j - 1] + FUZZY_ADJACENT,
                                                        gap[j - 1]) : 0;
                hit[j] = predecessor + bonus;
                if (count == length)
                    best = higher_score(best, hit[j]);
            } else {
                hit[j] = unreachable;
            }
            gap[j] = higher_score(gap[j], previous - FUZZY_GAP_OPEN) - FUZZY_GAP_CHAR;
        }
    }
    return best;
}

static bool fuzzy_score(const wchar_t *field, size_t field_length,
                        const wchar_t *query, size_t length, int64_t *score)
{
    struct fuzzy_alignment left;
    if (!greedy_alignment(field, 0, field_length, query, length, false, &left))
        return false;
    *score = left.score;
    if (length <= FUZZY_DP_QUERY && field_length <= FUZZY_DP_CELLS / length) {
        *score = optimal_alignment(field, field_length, query, length, *score);
        return true;
    }

    /* Large inputs still get a score with a bounded number of linear scans.
     * Tighten each greedy span from its opposite edge. */
    struct fuzzy_alignment right, compact;
    if (left.end < field_length &&
        greedy_alignment(field, 0, left.end, query, length, true, &compact))
        *score = higher_score(*score, compact.score);
    if (greedy_alignment(field, 0, field_length, query, length, true, &right)) {
        *score = higher_score(*score, right.score);
        if (right.start &&
            greedy_alignment(field, right.start, field_length, query, length, false, &compact))
            *score = higher_score(*score, compact.score);
    }
    return true;
}

static int quality_order(struct match_quality a, struct match_quality b)
{
    if (a.kind != b.kind)
        return (a.kind > b.kind) - (a.kind < b.kind);
    if (a.score != b.score)
        return (a.score < b.score) - (a.score > b.score);
    return (int)a.metadata - (int)b.metadata;
}

static struct match_quality score_result(const struct result *result, const wchar_t *query,
                                         size_t length, bool fuzzy)
{
    struct match_quality best = {.kind = MATCH_NONE};
    size_t primary_fields = result->application ? 2 : 1;
    size_t index = 0;
    const wchar_t *field = result->search;
    while (*field) {
        size_t field_length = wcslen(field);
        const wchar_t *match = field_length >= length ? wcsstr(field, query) : NULL;
        if (match) {
            struct match_quality quality = {
                .kind = match != field ? MATCH_SUBSTRING :
                        field_length == length ? MATCH_EXACT : MATCH_PREFIX,
                .metadata = index >= primary_fields,
            };
            if (quality.kind == MATCH_EXACT && !quality.metadata)
                return quality;
            if (quality_order(quality, best) < 0)
                best = quality;
        }
        field += field_length + 1;
        ++index;
    }
    if (best.kind != MATCH_NONE || !fuzzy || length < 2)
        return best;

    index = 0;
    field = result->search;
    while (*field) {
        size_t field_length = wcslen(field);
        struct match_quality quality = {.kind = MATCH_FUZZY, .metadata = index >= primary_fields};
        if (field_length >= length && fuzzy_score(field, field_length, query, length, &quality.score) &&
            quality_order(quality, best) < 0)
            best = quality;
        field += field_length + 1;
        ++index;
    }
    return best;
}

static int match_order(const struct result *a, const struct result *b)
{
    int order = quality_order(a->quality, b->quality);
    if (order)
        return order;
    if (a->preference != b->preference)
        return (a->preference < b->preference) - (a->preference > b->preference);
    return (a->order > b->order) - (a->order < b->order);
}

static void sift_matches(struct result **matches, size_t root, size_t count)
{
    struct result *item = matches[root];
    while (root < count / 2) {
        size_t child = 2 * root + 1;
        if (child + 1 < count && match_order(matches[child], matches[child + 1]) < 0)
            ++child;
        if (match_order(item, matches[child]) >= 0)
            break;
        matches[root] = matches[child];
        root = child;
    }
    matches[root] = item;
}

/* In-place heapsort keeps query-time sorting free of allocations. */
static void sort_matches(struct result **matches, size_t count)
{
    for (size_t root = count / 2; root > 0; --root)
        sift_matches(matches, root - 1, count);
    for (size_t end = count; end > 1; --end) {
        struct result *last = matches[end - 1];
        matches[end - 1] = matches[0];
        matches[0] = last;
        sift_matches(matches, 0, end - 1);
    }
}

static void filter(struct launcher *run)
{
    wchar_t query[INPUT_SIZE];
    wchar_t *end = search_text(query, run->input, run->config.case_sensitive);
    size_t length = (size_t)(end - query - 1);
    run->match_count = run->selected = run->page_start = 0;
    for (size_t i = 0; i < run->result_count; ++i) {
        struct result *result = &run->results[i];
        if (length) {
            result->quality = score_result(result, query, length, run->config.fuzzy);
            if (result->quality.kind == MATCH_NONE)
                continue;
        }
        run->matches[run->match_count++] = result;
    }
    if (length)
        sort_matches(run->matches, run->match_count);
    run->message[0] = '\0';
    run->dirty = true;
}

static uint32_t argb(uint32_t rgb, unsigned alpha)
{
    uint32_t pixel = alpha << 24;
    for (unsigned shift = 0; shift < 24; shift += 8)
        pixel |= ((((rgb >> shift) & 255) * alpha + 127) / 255) << shift;
    return pixel;
}

static uint32_t blend(uint32_t color, unsigned alpha, uint32_t below)
{
    unsigned inverse = 255 - alpha;
    uint32_t out = (alpha + ((below >> 24) * inverse + 127) / 255) << 24;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned fg = (color >> shift) & 255, bg = (below >> shift) & 255;
        out |= ((fg * alpha + bg * inverse + 127) / 255) << shift;
    }
    return out;
}

static void fill(struct buffer *buf, int left, int right, int top, int bottom,
                 uint32_t pixel)
{
    for (int y = top; y < bottom; ++y) {
        uint32_t *row = buf->pixels + (size_t)y * buf->width;
        for (int x = left; x < right; ++x)
            row[x] = pixel;
    }
}

static int glyph_advance(FT_Face font, uint32_t code)
{
    if (code < 0x20 || (code >= 0x7f && code <= 0x9f))
        code = ' ';
    if (FT_Load_Char(font, code, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP))
        return -1;
    return max(0, (int)(font->glyph->advance.x / 64));
}

static int text_width_up_to(FT_Face font, const char *text, int limit)
{
    int width = 0;
    while (*text) {
        int advance = max(0, glyph_advance(font, utf8_next(&text)));
        if (advance > limit - width)
            return limit + 1;
        width += advance;
    }
    return width;
}

static int text_width(FT_Face font, const char *text)
{
    return text_width_up_to(font, text, INT_MAX - 1);
}

static void draw_text(FT_Face font, struct buffer *buf, const char *text,
                      int pen, int left, int right, int baseline, uint32_t color)
{
    while (*text && pen < right) {
        int advance = glyph_advance(font, utf8_next(&text));
        if (advance < 0)
            continue;
        FT_GlyphSlot glyph = font->glyph;
        if (FT_Render_Glyph(glyph, FT_RENDER_MODE_NORMAL) || !glyph->bitmap.buffer ||
            glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
            pen += advance;
            continue;
        }

        const FT_Bitmap *bitmap = &glyph->bitmap;
        int x0 = pen + glyph->bitmap_left, y0 = baseline - glyph->bitmap_top;
        int start = max(left, x0), end = min(right, x0 + (int)bitmap->width);
        for (int y = max(0, y0); y < min(buf->height, y0 + (int)bitmap->rows); ++y) {
            const unsigned char *mask = bitmap->buffer + (ptrdiff_t)(y - y0) * bitmap->pitch;
            uint32_t *row = buf->pixels + (size_t)y * buf->width;
            for (int x = start; x < end; ++x)
                if (mask[x - x0])
                    row[x] = blend(color, mask[x - x0], row[x]);
        }
        pen += advance;
    }
}

static const char *previous_char(const char *start, const char *end)
{
    const char *previous = end - 1;
    while (previous > start && end - previous < 4 &&
           ((unsigned char)*previous & 0xc0) == 0x80)
        --previous;
    const char *next = previous;
    utf8_next(&next);
    return next == end ? previous : end - 1;
}

static void paint_input(struct launcher *run, struct buffer *buf, int left,
                        int right, int baseline)
{
    if (right <= left)
        return;
    int cursor = min(right - left, run->buffer_scale);
    int width = 0;
    const char *start = run->input + run->cursor;
    while (start > run->input) {
        const char *previous = previous_char(run->input, start);
        const char *text = previous;
        int advance = max(0, glyph_advance(run->font, utf8_next(&text)));
        if (advance > right - left - cursor - width)
            break;
        start = previous;
        width += advance;
    }
    draw_text(run->font, buf, start, left, left, right, baseline, run->config.foreground);

    int ascent = (int)(run->font->size->metrics.ascender / 64);
    int descent = (int)(-run->font->size->metrics.descender / 64);
    int top = min(buf->height - 1, max(0, baseline - ascent));
    int bottom = min(buf->height, max(top + 1, baseline + descent));
    fill(buf, left + width, left + width + cursor, top, bottom, argb(run->config.highlight, 255));
}

static int result_width(const struct launcher *run, size_t i, int padding, int limit)
{
    int inset = 2 * padding;
    return inset + text_width_up_to(run->font, run->matches[i]->name, max(0, limit - inset));
}

static void paint_results(struct launcher *run, struct buffer *buf, int left,
                          int right, int baseline)
{
    const struct config *cfg = &run->config;
    if (run->message[0]) {
        draw_text(run->font, buf, run->message, left, left, right, baseline, cfg->highlight);
        return;
    }
    if (!run->match_count || left >= right)
        return;

    int spacing = cfg->result_spacing * run->buffer_scale;
    int pad = 2 * run->buffer_scale;
    int available = right - left;
    size_t selected = run->selected, first = run->page_start;
    if (selected < first) {
        first = selected;
        int remaining = available - result_width(run, first, pad, available);
        while (first) {
            int previous = result_width(run, first - 1, pad, remaining) + spacing;
            if (previous > remaining)
                break;
            remaining -= previous;
            --first;
        }
    } else if (selected > first) {
        int remaining = available;
        for (size_t i = first; i <= selected; ++i) {
            int width = result_width(run, i, pad, remaining);
            if (width > remaining) {
                first = selected;
                break;
            }
            remaining -= width + spacing;
        }
    }
    run->page_start = first;

    for (size_t i = first; i < run->match_count && left < right; ++i) {
        bool active = i == selected;
        int width = result_width(run, i, pad, right - left);
        if (width > right - left && !active)
            break;

        int end = left + min(width, right - left);
        uint32_t color = active ? cfg->selected_foreground : cfg->foreground;
        if (active)
            fill(buf, left, end, 0, buf->height, argb(cfg->selected_background, 255));
        int text_left = min(end, left + pad), text_end = max(text_left, end - pad);
        if (width > right - left) {
            const char *ellipsis = "…";
            int ellipsis_x = max(text_left, text_end - text_width(run->font, ellipsis));
            draw_text(run->font, buf, run->matches[i]->name,
                      text_left, text_left, ellipsis_x, baseline, color);
            draw_text(run->font, buf, ellipsis, ellipsis_x, ellipsis_x, text_end,
                      baseline, color);
        } else {
            draw_text(run->font, buf, run->matches[i]->name,
                      text_left, text_left, text_end, baseline, color);
        }
        left = end + spacing;
    }
}

static void paint(struct launcher *run, struct buffer *buf)
{
    const struct config *cfg = &run->config;
    int padding = cfg->padding * run->buffer_scale;
    int gap = max(run->buffer_scale, padding);
    int right = buf->width - min(padding, buf->width / 4);
    int left = min(padding, buf->width / 4);
    int ascent = (int)(run->font->size->metrics.ascender / 64);
    int descent = (int)(-run->font->size->metrics.descender / 64);
    int baseline = (buf->height - ascent - descent) / 2 + ascent;

    fill(buf, 0, buf->width, 0, buf->height,
         argb(cfg->background, (unsigned)(255 * cfg->opacity + 0.5)));
    char count[48];
    snprintf(count, sizeof count, "%zu/%zu", run->match_count, run->result_count);
    int count_x = right - text_width(run->font, count);
    draw_text(run->font, buf, count, count_x, left, right, baseline, cfg->muted);
    right = max(left, count_x - gap);
    int prompt_end = min(left + text_width(run->font, cfg->prompt), left + (right - left) / 2);
    draw_text(run->font, buf, cfg->prompt, left, left, prompt_end, baseline, cfg->highlight);
    if (cfg->prompt[0])
        left = min(right, prompt_end + gap);
    int available = right - left;
    int input_limit = (run->match_count || run->message[0]) ? (available + 1) / 2 : available;
    int input_end = left + min(cfg->input_width * run->buffer_scale, input_limit);
    paint_input(run, buf, left, input_end, baseline);
    paint_results(run, buf, min(right, input_end + gap), right, baseline);
}

static void destroy_buffer(struct buffer *buf)
{
    if (!buf->proxy)
        return;
    wl_buffer_destroy(buf->proxy);
    munmap(buf->pixels, buf->size);
    *buf = (struct buffer){0};
}

static void buffer_release(void *data, struct wl_buffer *proxy)
{
    (void)proxy;
    struct buffer *buf = data;
    buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static bool create_buffer(struct launcher *run, struct buffer *buf, int width, int height)
{
    if (width > INT32_MAX / 4 || height > INT32_MAX / (width * 4))
        return false;
    int stride = width * 4, size = stride * height;
    int fd = memfd_create("run", MFD_CLOEXEC);
    if (fd < 0)
        return false;
    if (ftruncate(fd, size) == -1) {
        close(fd);
        return false;
    }
    void *pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return false;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(run->shm, fd, size);
    close(fd);
    struct wl_buffer *proxy = NULL;
    if (pool) {
        proxy = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
    }
    if (!proxy) {
        munmap(pixels, (size_t)size);
        return false;
    }

    *buf = (struct buffer){.proxy = proxy, .pixels = pixels, .size = (size_t)size,
                           .width = width, .height = height};
    wl_buffer_add_listener(proxy, &buffer_listener, buf);
    return true;
}

static void redraw(struct launcher *run)
{
    if (!run->configured)
        return;
    if (!run->dirty) {
        if (run->commit_pending)
            wl_surface_commit(run->surface);
        run->commit_pending = false;
        return;
    }

    int width = run->width * run->buffer_scale, height = run->height * run->buffer_scale;
    struct buffer *buf = NULL;
    for (size_t i = 0; i < sizeof run->buffers / sizeof run->buffers[0]; ++i) {
        struct buffer *candidate = &run->buffers[i];
        if (!candidate->busy) {
            buf = candidate;
            if (buf->width == width && buf->height == height)
                break;
        }
    }
    if (!buf)
        return;
    if (buf->width != width || buf->height != height) {
        destroy_buffer(buf);
        if (!create_buffer(run, buf, width, height)) {
            run->error = "cannot allocate a shared-memory buffer";
            return;
        }
    }
    if (run->font_scale != run->buffer_scale) {
        if (FT_Set_Pixel_Sizes(run->font, 0, (FT_UInt)(run->config.font_size * run->buffer_scale))) {
            run->error = "cannot set the font pixel size";
            return;
        }
        run->font_scale = run->buffer_scale;
    }

    paint(run, buf);
    if (wl_surface_get_version(run->surface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
        wl_surface_set_buffer_scale(run->surface, run->buffer_scale);
    wl_surface_attach(run->surface, buf->proxy, 0, 0);
    if (wl_surface_get_version(run->surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
        wl_surface_damage_buffer(run->surface, 0, 0, width, height);
    else
        wl_surface_damage(run->surface, 0, 0, run->width, run->height);
    wl_surface_commit(run->surface);
    buf->busy = true;
    run->dirty = run->commit_pending = false;
}

static void show_message(struct launcher *run, const char *text)
{
    if (strcmp(run->message, text)) {
        snprintf(run->message, sizeof run->message, "%s", text);
        run->dirty = true;
    }
}

static int spawn_command(const char *path, char *const argv[], const char *directory)
{
    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (error)
        return error;
    error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);

    posix_spawn_file_actions_t files;
    if (!error) {
        error = posix_spawn_file_actions_init(&files);
        if (!error) {
            /* Detached applications must not read or change the launcher's tty. */
            error = posix_spawn_file_actions_addopen(&files, STDIN_FILENO,
                                                    "/dev/null", O_RDONLY, 0);
            for (int fd = STDOUT_FILENO; !error && fd <= STDERR_FILENO; ++fd) {
                if (isatty(fd))
                    error = posix_spawn_file_actions_addopen(&files, fd, "/dev/null", O_WRONLY, 0);
            }
            if (!error && directory)
                error = posix_spawn_file_actions_addchdir_np(&files, directory);
            if (!error) {
                pid_t child;
                error = posix_spawn(&child, path, &files, &attributes, argv, environ);
            }
            posix_spawn_file_actions_destroy(&files);
        }
    }
    posix_spawnattr_destroy(&attributes);
    return error;
}

static void launch(struct launcher *run, const char *path, char *const argv[],
                   const char *directory)
{
    int error = path ? spawn_command(path, argv, directory) : ENOENT;
    if (!error) {
        run->closed = true;
    } else {
        char message[sizeof run->message];
        snprintf(message, sizeof message, "Cannot execute: %s", strerror(error));
        show_message(run, message);
    }
}

static void launch_in_terminal(struct launcher *run, const char *path, char *const command[],
                               size_t argc, const char *directory)
{
    const struct terminal *terminal = &run->terminal;
    if (!terminal->path) {
        show_message(run, "Configured terminal is unavailable");
        return;
    }
    char **argv = reallocarray(NULL, terminal->argc + argc + 1, sizeof *argv);
    if (!argv) {
        show_message(run, "Cannot allocate terminal arguments");
        return;
    }
    memcpy(argv, terminal->argv, terminal->argc * sizeof *argv);
    memcpy(argv + terminal->argc, command, (argc + 1) * sizeof *argv);
    argv[terminal->argc] = (char *)path;
    launch(run, terminal->path, argv, directory);
    free(argv);
}

static bool needs_terminal(const struct launcher *run, const char *path)
{
    struct stat executable;
    if (!path || stat(path, &executable) < 0)
        return false;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    for (size_t i = 0; i < run->application_count; ++i) {
        const struct application *app = &run->applications[i];
        if (!app->terminal)
            continue;
        if (!strcmp(path, app->path))
            return true;
        const char *program = strrchr(app->path, '/');
        program = program ? program + 1 : app->path;
        struct stat target;
        if (!strcmp(name, program) && stat(app->path, &target) == 0 &&
            executable.st_dev == target.st_dev && executable.st_ino == target.st_ino)
            return true;
    }
    return false;
}

static void launch_command(struct launcher *run, const char *path, char *const argv[])
{
    if (!needs_terminal(run, path)) {
        launch(run, path, argv, NULL);
        return;
    }
    size_t argc = 0;
    while (argv[argc])
        ++argc;
    launch_in_terminal(run, path, argv, argc, NULL);
}

static void launch_input(struct launcher *run)
{
    char storage[INPUT_SIZE];
    char *argv[INPUT_SIZE / 2 + 1];
    const char *error = parse_arguments(run->input, storage, argv);
    if (error)
        show_message(run, error);
    else if (argv[0])
        launch_command(run, command_path(run, argv[0]), argv);
}

static void launch_selection(struct launcher *run)
{
    const struct result *result = run->matches[run->selected];
    const struct application *app = result->application;
    if (!app) {
        const struct command *cmd = result->command;
        char *argv[] = {cmd->name, NULL};
        launch_command(run, cmd->path, argv);
        return;
    }
    if (app->terminal)
        launch_in_terminal(run, app->path, app->argv, app->argc, app->directory);
    else
        launch(run, app->path, app->argv, app->directory);
}

static void move_selection(struct launcher *run, int direction)
{
    size_t selected = run->selected;
    if (direction < 0 && selected > 0)
        --selected;
    else if (direction > 0 && selected + 1 < run->match_count)
        ++selected;
    if (selected != run->selected || run->message[0]) {
        run->selected = selected;
        run->message[0] = '\0';
        run->dirty = true;
    }
}

static void complete_selection(struct launcher *run)
{
    if (!run->match_count)
        return;
    const struct result *result = run->matches[run->selected];
    const char *name = result->name;
    size_t length = strlen(name);
    if (length >= sizeof run->input) {
        show_message(run, "Command name is too long");
    } else if (strcmp(run->input, name) || run->message[0]) {
        memcpy(run->input, name, length + 1);
        run->input_length = run->cursor = length;
        filter(run);
        /* Distinct desktop entries can share the same displayed name. */
        for (size_t i = 0; i < run->match_count; ++i) {
            if (run->matches[i] == result) {
                run->selected = i;
                break;
            }
        }
    } else if (run->cursor != length) {
        run->cursor = length;
        run->dirty = true;
    }
}

static void erase_input(struct launcher *run, size_t start, size_t end)
{
    memmove(run->input + start, run->input + end, run->input_length - end + 1);
    run->input_length -= end - start;
    run->cursor = start;
    filter(run);
}

static bool type_key(struct launcher *run, xkb_keycode_t key, xkb_keysym_t sym)
{
    char text[128];
    int length;
    bool composed = false;
    if (run->compose) {
        xkb_compose_state_feed(run->compose, sym);
        enum xkb_compose_status status = xkb_compose_state_get_status(run->compose);
        if (status == XKB_COMPOSE_COMPOSING)
            return false;
        if (status == XKB_COMPOSE_CANCELLED) {
            xkb_compose_state_reset(run->compose);
            return false;
        }
        if (status == XKB_COMPOSE_COMPOSED) {
            length = xkb_compose_state_get_utf8(run->compose, text, sizeof text);
            xkb_compose_state_reset(run->compose);
            composed = true;
        }
    }
    if (!composed)
        length = xkb_state_key_get_utf8(run->keys, key, text, sizeof text);
    if (length <= 0 || (size_t)length >= sizeof text)
        return false;
    const char *p = text;
    while (*p) {
        if (iswcntrl((wint_t)utf8_next(&p)))
            return false;
    }
    if ((size_t)length >= sizeof run->input - run->input_length) {
        show_message(run, "Input is too long");
        return false;
    }
    char *cursor = run->input + run->cursor;
    memmove(cursor + length, cursor, run->input_length - run->cursor + 1);
    memcpy(cursor, text, (size_t)length);
    run->cursor += (size_t)length;
    run->input_length += (size_t)length;
    filter(run);
    return !composed;
}

static bool handle_key(struct launcher *run, xkb_keycode_t key)
{
    struct xkb_state *state = run->keys;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(state, key);
    bool ctrl = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL,
                                             XKB_STATE_MODS_EFFECTIVE) > 0;
    bool shift = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT,
                                              XKB_STATE_MODS_EFFECTIVE) > 0;
    xkb_keysym_t lower = xkb_keysym_to_lower(sym);

    if (sym == XKB_KEY_Escape) {
        run->closed = true;
    } else if (sym == XKB_KEY_Left || sym == XKB_KEY_KP_Left) {
        move_selection(run, -1);
        return true;
    } else if (sym == XKB_KEY_Right || sym == XKB_KEY_KP_Right || (ctrl && lower == XKB_KEY_j)) {
        move_selection(run, 1);
        return true;
    } else if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
        complete_selection(run);
    } else if (sym == XKB_KEY_BackSpace) {
        if (run->compose && xkb_compose_state_get_status(run->compose) == XKB_COMPOSE_COMPOSING) {
            xkb_compose_state_reset(run->compose);
            return false;
        }
        if (run->cursor) {
            const char *start = previous_char(run->input, run->input + run->cursor);
            erase_input(run, (size_t)(start - run->input), run->cursor);
        }
        return true;
    } else if (ctrl && (lower == XKB_KEY_a || lower == XKB_KEY_e)) {
        if (run->compose)
            xkb_compose_state_reset(run->compose);
        size_t cursor = lower == XKB_KEY_a ? 0 : run->input_length;
        if (cursor != run->cursor) {
            run->cursor = cursor;
            run->dirty = true;
        }
    } else if (ctrl && (lower == XKB_KEY_k || lower == XKB_KEY_u)) {
        if (run->compose)
            xkb_compose_state_reset(run->compose);
        size_t start = lower == XKB_KEY_k ? run->cursor : 0;
        if (start != run->input_length || (lower == XKB_KEY_u && run->message[0]))
            erase_input(run, start, run->input_length);
    } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (shift) {
            launch_input(run);
        } else if (run->match_count)
            launch_selection(run);
    } else if (!ctrl) {
        return type_key(run, key, sym);
    }
    return false;
}

static void stop_repeat(struct launcher *run)
{
    if (!run->repeating)
        return;
    const struct itimerspec timeout = {0};
    run->repeating = false;
    if (timerfd_settime(run->timer, 0, &timeout, NULL) == -1)
        run->error = "cannot stop keyboard repeat";
}

static void start_repeat(struct launcher *run, uint32_t key)
{
    if (run->repeat_rate <= 0 || !run->keyboard ||
        !xkb_keymap_key_repeats(xkb_state_get_keymap(run->keys), key + 8))
        return;

    long interval = 1000000000L / min(run->repeat_rate, 1000);
    struct itimerspec timeout = {
        .it_interval = {.tv_sec = interval / 1000000000L, .tv_nsec = interval % 1000000000L},
        .it_value = {.tv_sec = run->repeat_delay / 1000,
                     .tv_nsec = (run->repeat_delay % 1000) * 1000000L},
    };
    if (!timeout.it_value.tv_sec && !timeout.it_value.tv_nsec)
        timeout.it_value.tv_nsec = 1;
    if (timerfd_settime(run->timer, 0, &timeout, NULL) == -1) {
        run->error = "cannot start keyboard repeat";
        return;
    }
    run->repeat_key = key;
    run->repeating = true;
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                            int32_t fd, uint32_t size)
{
    (void)keyboard;
    struct launcher *run = data;
    stop_repeat(run);
    if (run->compose)
        xkb_compose_state_reset(run->compose);

    struct xkb_state *state = NULL;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0) {
        const char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(
                run->xkb, map, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (keymap) {
                state = xkb_state_new(keymap);
                xkb_keymap_unref(keymap);
            }
            munmap((void *)map, size);
        }
    }
    close(fd);
    if (!state) {
        run->error = "compositor did not provide a usable XKB keymap";
        return;
    }
    xkb_state_unref(run->keys);
    run->keys = state;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    (void)keyboard;
    (void)serial;
    (void)keys;
    struct launcher *run = data;
    run->focused = surface == run->surface;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface)
{
    (void)keyboard;
    (void)serial;
    (void)surface;
    struct launcher *run = data;
    run->focused = false;
    stop_repeat(run);
    if (run->compose)
        xkb_compose_state_reset(run->compose);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    struct launcher *run = data;
    if (run->closed || run->error || !run->focused || !run->keys)
        return;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (run->repeating && key == run->repeat_key)
            stop_repeat(run);
    } else if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        bool repeat = handle_key(run, key + 8);
        if (xkb_keymap_key_repeats(xkb_state_get_keymap(run->keys), key + 8)) {
            stop_repeat(run);
            if (repeat)
                start_repeat(run, key);
        }
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)keyboard;
    (void)serial;
    struct launcher *run = data;
    if (run->keys)
        xkb_state_update_mask(run->keys, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)keyboard;
    struct launcher *run = data;
    run->repeat_rate = rate;
    run->repeat_delay = max(0, delay);
    if (run->repeating) {
        uint32_t key = run->repeat_key;
        stop_repeat(run);
        start_repeat(run, key);
    }
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat,
};

static void destroy_keyboard(struct launcher *run)
{
    if (!run->keyboard)
        return;
    if (wl_keyboard_get_version(run->keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
        wl_keyboard_release(run->keyboard);
    else
        wl_keyboard_destroy(run->keyboard);
    run->keyboard = NULL;
}

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct launcher *run = data;
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!run->keyboard) {
            run->keyboard = wl_seat_get_keyboard(seat);
            if (run->keyboard)
                wl_keyboard_add_listener(run->keyboard, &keyboard_listener, run);
            else
                run->error = "cannot create the Wayland keyboard";
        }
    } else if (run->keyboard) {
        stop_repeat(run);
        destroy_keyboard(run);
        run->error = "the Wayland seat lost its keyboard";
    }
}

static void seat_label(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_label,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    struct launcher *run = data;
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
    const uint32_t compositor_version = WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION;
#else
    const uint32_t compositor_version = WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION;
#endif
    if (!strcmp(interface, wl_compositor_interface.name) && !run->compositor)
        run->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                          version < compositor_version ? version : compositor_version);
    else if (!strcmp(interface, wl_shm_interface.name) && !run->shm)
        run->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name) && !run->shell)
        run->shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    else if (!strcmp(interface, wl_seat_interface.name) && !run->seat) {
        run->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                    version < WL_SEAT_RELEASE_SINCE_VERSION
                                        ? version : WL_SEAT_RELEASE_SINCE_VERSION);
        run->seat_name = name;
        if (run->seat)
            wl_seat_add_listener(run->seat, &seat_listener, run);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)registry;
    struct launcher *run = data;
    if (run->seat && run->seat_name == name)
        run->error = "the Wayland seat was removed";
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                            uint32_t serial, uint32_t width, uint32_t height)
{
    struct launcher *run = data;
    /* The compositor chooses the width. A zero height leaves the choice to us. */
    if (!height)
        height = (uint32_t)run->config.height;
    if (!width && run->configured)
        width = (uint32_t)run->width;
    if (!width || width > INT_MAX / MAX_SCALE || height > INT_MAX / MAX_SCALE) {
        run->error = "compositor provided invalid surface dimensions";
        return;
    }
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (run->width != (int)width || run->height != (int)height) {
        run->width = (int)width;
        run->height = (int)height;
        run->dirty = true;
    }
    run->configured = run->commit_pending = true;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
    (void)layer;
    struct launcher *run = data;
    run->closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure, .closed = layer_closed,
};

static void surface_output(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
static void surface_scale(void *data, struct wl_surface *surface, int32_t scale)
{
    (void)surface;
    struct launcher *run = data;
    scale = max(1, min(MAX_SCALE, scale));
    if (scale != run->buffer_scale) {
        run->buffer_scale = scale;
        run->dirty = true;
    }
}

static void surface_transform(void *data, struct wl_surface *surface, uint32_t transform)
{
    (void)data;
    (void)surface;
    (void)transform;
}
#endif

static const struct wl_surface_listener surface_listener = {
    .enter = surface_output, .leave = surface_output,
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
    .preferred_buffer_scale = surface_scale, .preferred_buffer_transform = surface_transform,
#endif
};

static void startup_done(void *data, struct wl_callback *callback, uint32_t serial)
{
    (void)serial;
    struct launcher *run = data;
    wl_callback_destroy(callback);
    run->startup = NULL;
    if (!run->keyboard && !run->error && !run->closed)
        run->error = "the Wayland seat has no keyboard";
}

static const struct wl_callback_listener startup_listener = {.done = startup_done};

static bool setup_wayland(struct launcher *run)
{
    run->display = wl_display_connect(NULL);
    if (!run->display) {
        run->error = "cannot connect to the Wayland compositor";
        return false;
    }
    run->registry = wl_display_get_registry(run->display);
    if (!run->registry) {
        run->error = "cannot get the Wayland registry";
        return false;
    }
    wl_registry_add_listener(run->registry, &registry_listener, run);
    if (wl_display_roundtrip(run->display) == -1) {
        run->error = "cannot read the Wayland registry";
        return false;
    }
    if (!run->compositor || !run->shm || !run->shell || !run->seat) {
        run->error = "Wayland compositor must provide wl_compositor, wl_shm, wlr-layer-shell and wl_seat";
        return false;
    }

    run->surface = wl_compositor_create_surface(run->compositor);
    if (!run->surface) {
        run->error = "cannot create the Wayland surface";
        return false;
    }
    wl_surface_add_listener(run->surface, &surface_listener, run);
    /* A NULL output lets dwl place the launcher on its selected monitor. */
    run->layer = zwlr_layer_shell_v1_get_layer_surface(
        run->shell, run->surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "run");
    if (!run->layer) {
        run->error = "cannot create the overlay layer surface";
        return false;
    }
    zwlr_layer_surface_v1_add_listener(run->layer, &layer_listener, run);
    zwlr_layer_surface_v1_set_size(run->layer, 0, (uint32_t)run->config.height);
    zwlr_layer_surface_v1_set_anchor(run->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(run->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        run->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);

    struct wl_region *region = wl_compositor_create_region(run->compositor);
    if (!region) {
        run->error = "cannot create the Wayland input region";
        return false;
    }
    wl_surface_set_input_region(run->surface, region);
    wl_region_destroy(region);
    wl_surface_commit(run->surface);

    run->startup = wl_display_sync(run->display);
    if (!run->startup) {
        run->error = "cannot synchronize with the Wayland compositor";
        return false;
    }
    wl_callback_add_listener(run->startup, &startup_listener, run);
    if (wl_display_flush(run->display) == -1 && errno != EAGAIN) {
        run->error = "cannot write to the Wayland compositor";
        return false;
    }
    return true;
}

static void dispatch_repeat(struct launcher *run)
{
    uint64_t ticks;
    ssize_t count = read(run->timer, &ticks, sizeof ticks);
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (count != (ssize_t)sizeof ticks) {
        run->error = "cannot read keyboard repeat timer";
        return;
    }

    /* Limit catch-up after a long event-loop stall. */
    if (ticks > 8)
        ticks = 8;
    while (ticks-- && run->repeating && !run->closed && !run->error) {
        if (!handle_key(run, run->repeat_key + 8))
            stop_repeat(run);
    }
}

static void event_loop(struct launcher *run)
{
    struct wl_display *display = run->display;
    struct pollfd fds[] = {
        {.fd = wl_display_get_fd(display), .events = POLLIN},
        {.fd = run->timer, .events = POLLIN},
    };
    bool repeat_ready = false;
    while (!run->closed && !run->error) {
        if (wl_display_dispatch_pending(display) == -1)
            break;
        if (repeat_ready && !run->closed && !run->error)
            dispatch_repeat(run);
        repeat_ready = false;
        if (run->closed || run->error)
            return;
        redraw(run);
        if (run->error)
            return;

        if (wl_display_prepare_read(display) == -1)
            continue;
        fds[0].events = POLLIN;
        if (wl_display_flush(display) == -1) {
            if (errno != EAGAIN) {
                wl_display_cancel_read(display);
                break;
            }
            fds[0].events |= POLLOUT;
        }

        int ready = poll(fds, sizeof fds / sizeof fds[0], -1);
        if (ready < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR)
                continue;
            run->error = "cannot poll launcher events";
            return;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(display) == -1)
                break;
        } else {
            wl_display_cancel_read(display);
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            run->error = "keyboard repeat timer is unavailable";
            return;
        }
        repeat_ready = (fds[1].revents & POLLIN) != 0;
    }
    if (!run->closed && !run->error)
        run->error = "Wayland compositor connection closed";
}

static bool setup_text(struct launcher *run)
{
    const char *locale = setlocale(LC_CTYPE, "");
    if (!locale || MB_CUR_MAX == 1)
        locale = setlocale(LC_CTYPE, "C.UTF-8");
    setlocale(LC_COLLATE, "");

    run->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!run->xkb) {
        run->error = "cannot initialize libxkbcommon";
        return false;
    }
    if (locale) {
        struct xkb_compose_table *table = xkb_compose_table_new_from_locale(
            run->xkb, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (table) {
            run->compose = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
            xkb_compose_table_unref(table);
        }
    }

    if (FT_Init_FreeType(&run->freetype) ||
        FT_New_Face(run->freetype, run->config.font_file, 0, &run->font) ||
        FT_Select_Charmap(run->font, FT_ENCODING_UNICODE) ||
        FT_Set_Pixel_Sizes(run->font, 0, (FT_UInt)run->config.font_size)) {
        fprintf(stderr, "run: cannot use font %s\n", run->config.font_file);
        return false;
    }
    run->font_scale = 1;
    return true;
}

static void cleanup(struct launcher *run)
{
    if (run->layer)
        zwlr_layer_surface_v1_destroy(run->layer);
    if (run->surface)
        wl_surface_destroy(run->surface);
    if (run->display)
        wl_display_flush(run->display);
    if (run->startup)
        wl_callback_destroy(run->startup);
    for (size_t i = 0; i < sizeof run->buffers / sizeof run->buffers[0]; ++i)
        destroy_buffer(&run->buffers[i]);
    destroy_keyboard(run);
    if (run->seat) {
        if (wl_seat_get_version(run->seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_seat_release(run->seat);
        else
            wl_seat_destroy(run->seat);
    }
    /* The shell global's destroy request starts at v3. We bind v1. */
    if (run->shell)
        wl_proxy_destroy((struct wl_proxy *)run->shell);
    if (run->shm)
        wl_shm_destroy(run->shm);
    if (run->compositor)
        wl_compositor_destroy(run->compositor);
    if (run->registry)
        wl_registry_destroy(run->registry);
    if (run->display)
        wl_display_disconnect(run->display);

    if (run->timer >= 0)
        close(run->timer);
    xkb_compose_state_unref(run->compose);
    xkb_state_unref(run->keys);
    xkb_context_unref(run->xkb);
    if (run->font)
        FT_Done_Face(run->font);
    if (run->freetype)
        FT_Done_FreeType(run->freetype);
    free(run->matches);
    for (size_t i = 0; i < run->result_count; ++i) {
        if (run->results[i].command)
            free(run->results[i].search);
    }
    free(run->results);
    for (size_t i = 0; i < run->application_count; ++i)
        free_application(&run->applications[i]);
    free(run->applications);
    free(run->terminal.path);
    free(run->terminal.options);
    free(run->terminal.argv);
    for (size_t i = 0; i < run->command_count; ++i) {
        free(run->commands[i].name);
        free(run->commands[i].path);
    }
    free(run->commands);
}

int main(int argc, char *argv[])
{
    static const char usage[] = "Usage: run [-c PATH]\n";
    const char *config_path = NULL;
    int option;
    while ((option = getopt(argc, argv, "c:h")) != -1) {
        if (option == 'c') {
            config_path = optarg;
        } else if (option == 'h') {
            fputs(usage, stdout);
            return EXIT_SUCCESS;
        } else {
            fputs(usage, stderr);
            return EXIT_FAILURE;
        }
    }
    if (optind != argc) {
        fputs(usage, stderr);
        return EXIT_FAILURE;
    }

    struct launcher run = {.config = defaults, .buffer_scale = 1, .timer = -1};
    if (!load_config(&run.config, config_path) || !scale_config(&run.config))
        return EXIT_FAILURE;
    apply_wallpaper_theme(&run.config);
    bool ok = setup_text(&run);
    if (ok) {
        run.timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (run.timer == -1)
            run.error = "cannot create keyboard repeat timer";
        else if (setup_wayland(&run)) {
            if (discover_results(&run)) {
                filter(&run);
                event_loop(&run);
            } else {
                run.error = "cannot allocate the application catalog";
            }
        }
    }
    cleanup(&run);
    if (run.error)
        fprintf(stderr, "run: %s\n", run.error);
    return ok && !run.error ? EXIT_SUCCESS : EXIT_FAILURE;
}
