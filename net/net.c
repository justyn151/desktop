#define _GNU_SOURCE
#include "net.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <netinet/ether.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

static const unsigned char bundled_config[] = {
#include "default-config.h"
    0
};

enum Interface { MANAGER, DEVICE, WIRELESS, ACCESS_POINT };

static const char *const interfaces[] = {
    [MANAGER] = NM, [DEVICE] = NM_DEVICE,
    [WIRELESS] = NM_WIRELESS, [ACCESS_POINT] = NM_AP,
};

struct App;
struct Object;
struct SavedProfile;

struct Properties {
    struct Object *object;
    enum Interface interface;
    sd_bus_slot *request;
    bool ready;
};

struct Ssid {
    unsigned char bytes[SSID_MAX];
    size_t length;
};

enum Security { OPEN, PSK, SAE, ENTERPRISE, OWE, WEP, UNKNOWN_SECURITY };

enum Action {
    IDLE, TOGGLE_AUTOCONNECT, CONFIRM_FORGET, FORGETTING,
    CONFIRM_RADIO_OFF, SET_RADIO, WAIT_RADIO, HIDDEN_SSID, HIDDEN_SECURITY,
    LOOKUP, ENTER_PASSWORD, REGISTER_AGENT, START_CONNECTION, ACTIVATING,
    CONFIRM_DISCONNECT, START_DISCONNECT, DISCONNECTING,
};

struct Operation {
    enum Action action;
    enum Security security, group;
    struct Ssid ssid;
    char *device, *ap, *profile, *active;
    char profile_uuid[UUID_SIZE];
    uint64_t profile_version;
    sd_bus_slot *request;
    sd_bus_message *profiles, *secret_request;
    uint64_t deadline;
    const char *password_error, *ssid_error;
    char password[PASSWORD_MAX + 1];
    size_t password_length;
    unsigned secret_attempts;
    bool canceled, password_submitted, password_overflow, forget_warning, forget_prompt_shown;
    bool hidden, ssid_overflow, radio_enable, radio_prompt_shown;
};

struct Scan {
    uint64_t deadline;
    unsigned completed, denied, throttled, timed_out, failed;
};

struct Object {
    struct App *app;
    char *path;
    struct Object *next;
    struct Object *children;
    struct Properties props;
    bool listed;
    union {
        struct {
            int enabled;
            int hardware_enabled;
        } manager;
        struct {
            struct Properties wireless;
            struct Object *active;
            char *name, *active_connection, *permanent_address;
            char *active_profile;
            uint32_t type, state, reason, mode, bitrate;
            int64_t last_scan, scan_baseline;
            sd_bus_slot *scan_request, *active_profile_request;
            bool scan_waiting;
        } device;
        struct {
            struct Ssid ssid;
            char *bssid;
            uint8_t strength;
            uint32_t flags, wpa, rsn, mode, frequency;
        } ap;
    };
};

struct Row {
    const struct Object *ap;
    const struct Object *device;
    bool connected, saved;
};

struct Line {
    char text[256];
};

struct Theme {
    char normal[48], highlight[48], muted[48], selection[48];
};

struct SavedView {
    struct SavedProfile *profiles, **rows;
    sd_bus_slot *request;
    const char *error;
    size_t count, capacity, selected, top, pending;
    char selected_uuid[UUID_SIZE];
    bool listed;
};

struct DetailLine {
    const char *label;
    char value[128];
};

struct IpDetails {
    struct App *app;
    char *path;
    sd_bus_slot *request;
    struct DetailLine *lines;
    size_t count, capacity;
    bool ipv6, refresh, failed;
};

struct Details {
    bool open;
    struct Ssid ssid;
    char *device, *ap, *profile;
    char profile_uuid[UUID_SIZE];
    struct DetailLine basic[12];
    size_t basic_count, top, last_top;
    struct IpDetails ip[2];
    char *active_path;
    sd_bus_slot *active_request;
    bool active_refresh, active_failed;
    char *active_settings;
    sd_bus_slot *settings_request;
    int active_autoconnect;
    bool active_autoconnect_known;
};

struct App {
    sd_bus *bus;
    struct Object manager;
    char *owner;
    char *profile_permission;
    sd_bus_slot *agent;
    bool agent_registered;
    struct Operation operation;
    struct Scan scan;
    struct Theme theme;
    struct SavedView saved;
    struct Details details;
    bool show_saved, radio_was_available;
    const char *notice, *scan_notice;
    unsigned matches_pending;
    bool match_failed;
    struct Row *rows;
    size_t count, capacity, selected, top;
    struct Ssid selected_ssid;
    char *selected_hidden;
    enum Security selected_security;
    bool have_selection;
    bool model_dirty, screen_dirty, clear_screen;
    struct Line *lines;
    unsigned width, height;
};

static struct termios saved_termios;
static bool terminal_active;
static volatile sig_atomic_t interrupted, resized;

static uint64_t monotonic_us(void);
static void reset_operations(struct App *app);
static void active_state_changed(struct App *app, uint32_t state, uint32_t reason);
static void request_active_profile(struct Object *device);
static void request_saved_list(struct App *app);
static void clear_saved_profiles(struct App *app);
static bool saved_signal(struct App *app, sd_bus_message *message, const char *path,
                         const char *interface, const char *member);
static void request_saved_profile(struct App *app, const char *path);
static void rebuild_saved_rows(struct App *app);
static void mark_saved_networks(struct App *app);
static void close_details(struct App *app);
static bool ip_details_changed(struct App *app, const char *path, const char *interface);

enum ConfigColor {
    BACKGROUND, FOREGROUND, HIGHLIGHT, MUTED, SELECTION_BACKGROUND, SELECTION_FOREGROUND,
    COLOR_COUNT
};

struct Config {
    uint32_t rgb[COLOR_COUNT];
    bool show_saved, wallpaper_theme;
};

static const struct Config config_defaults = {
    .rgb = { 0x353535, 0xc5ad81, 0xead4aa, 0x887d69, 0xc5ad81, 0x353535 },
};

static char *trim_config(char *text)
{
    while (isspace((unsigned char)*text))
        text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
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
        char *key = trim_config(line);
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
        key = trim_config(key);
        value = trim_config(value);
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

static const char *parse_config_color(struct Config *config, const char *key, const char *value)
{
    static const char *const names[COLOR_COUNT] = {
        [BACKGROUND] = "background", [FOREGROUND] = "foreground",
        [HIGHLIGHT] = "highlight", [MUTED] = "muted",
        [SELECTION_BACKGROUND] = "selection_background",
        [SELECTION_FOREGROUND] = "selection_foreground",
    };
    for (size_t i = 0; i < COLOR_COUNT; i++) {
        if (strcmp(key, names[i]) != 0)
            continue;
        if (value[0] != '#' || strlen(value) != 7 ||
            strspn(value + 1, "0123456789abcdefABCDEF") != 6)
            return "expected a color in #RRGGBB form";
        config->rgb[i] = (uint32_t)strtoul(value + 1, NULL, 16);
        return NULL;
    }
    return "unknown appearance option";
}

/* Positive if loaded, zero if absent, negative on an invalid or unreadable file. */
static int read_config(const char *path, struct Config *config)
{
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK) : -1;
    if (path && fd < 0 && errno == ENOENT)
        return 0;
    if (path && fd < 0) {
        fprintf(stderr, "net: %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct stat info;
    FILE *file = NULL;
    if (fd < 0) {
        path = "bundled net.conf";
        file = fmemopen((void *)bundled_config, sizeof bundled_config - 1, "r");
    } else if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
        file = fdopen(fd, "r");
    }
    if (!file) {
        fprintf(stderr, "net: %s: cannot open a regular configuration file\n", path);
        if (fd >= 0)
            close(fd);
        return -1;
    }

    enum { NO_SECTION, APPEARANCE_SECTION, GENERAL_SECTION, THEME_SECTION } section = NO_SECTION;
    char *line = NULL;
    size_t capacity = 0, number = 0;
    const char *error = NULL;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        number++;
        if (length > 4096 || memchr(line, '\0', (size_t)length)) {
            error = "line is too long or contains a NUL byte";
            break;
        }
        char *key = trim_config(line);
        if (!*key || *key == '#' || *key == ';')
            continue;
        if (*key == '[') {
            if (strcmp(key, "[appearance]") == 0)
                section = APPEARANCE_SECTION;
            else if (strcmp(key, "[general]") == 0)
                section = GENERAL_SECTION;
            else if (strcmp(key, "[theme]") == 0)
                section = THEME_SECTION;
            else {
                error = "unknown configuration section";
                break;
            }
            continue;
        }
        char *value = strchr(key, '=');
        if (section == NO_SECTION || !value) {
            error = "expected key = value inside a section";
            break;
        }
        *value++ = '\0';
        key = trim_config(key);
        value = trim_config(value);
        if (section == APPEARANCE_SECTION) {
            error = parse_config_color(config, key, value);
        } else if (section == THEME_SECTION) {
            if (strcmp(key, "source") || (strcmp(value, "manual") && strcmp(value, "wallpaper")))
                error = "expected [theme] source = manual or wallpaper";
            else
                config->wallpaper_theme = !strcmp(value, "wallpaper");
        } else if (strcmp(key, "default_view") != 0) {
            error = "unknown general option";
        } else if (strcmp(value, "available") == 0 || strcmp(value, "saved") == 0) {
            config->show_saved = strcmp(value, "saved") == 0;
        } else {
            error = "default_view must be available or saved";
        }
        if (error)
            break;
    }
    if (!error && (!feof(file) || ferror(file)))
        error = "could not read configuration";
    if (error)
        fprintf(stderr, "net: %s:%zu: %s\n", path, number, error);
    free(line);
    fclose(file);
    return error ? -1 : 1;
}

static int read_config_directory(const char *directory, const char *suffix, struct Config *config)
{
    char *path = NULL;
    if (asprintf(&path, "%s%s", directory, suffix) < 0) {
        fputs("net: could not allocate the configuration path\n", stderr);
        return -1;
    }
    int result = read_config(path, config);
    free(path);
    return result;
}

static bool load_config(struct Config *config, const char *override)
{
    *config = config_defaults;
    if (override) {
        int result = read_config(override, config);
        return result ? result > 0 : read_config(NULL, config) > 0;
    }

    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/') {
        int result = read_config_directory(xdg, "/net/net.conf", config);
        if (result)
            return result > 0;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        int result = read_config_directory(home, "/.config/net/net.conf", config);
        if (result)
            return result > 0;
    }
    return read_config(NULL, config) > 0;
}

static void apply_wallpaper_theme(struct Config *config)
{
    if (!config->wallpaper_theme)
        return;
    uint32_t colors[THEME_ROLE_COUNT];
    char *path = theme_palette_path();
    bool ok = load_theme_palette(path, colors);
    free(path);
    if (!ok)
        return;
    config->rgb[BACKGROUND] = colors[THEME_BACKGROUND];
    config->rgb[FOREGROUND] = colors[THEME_FOREGROUND];
    config->rgb[HIGHLIGHT] = colors[THEME_HIGHLIGHT];
    config->rgb[MUTED] = colors[THEME_MUTED];
    config->rgb[SELECTION_BACKGROUND] = colors[THEME_SELECTION_BACKGROUND];
    config->rgb[SELECTION_FOREGROUND] = colors[THEME_SELECTION_FOREGROUND];
}

static void format_style(char *text, size_t size, uint32_t background, uint32_t foreground)
{
    snprintf(text, size, "\033[48;2;%u;%u;%u;38;2;%u;%u;%um",
             (unsigned)((background >> 16) & 255), (unsigned)((background >> 8) & 255),
             (unsigned)(background & 255), (unsigned)((foreground >> 16) & 255),
             (unsigned)((foreground >> 8) & 255), (unsigned)(foreground & 255));
}

static void apply_config(struct App *app, const struct Config *config)
{
    const uint32_t *rgb = config->rgb;
    struct Theme *theme = &app->theme;
    format_style(theme->normal, sizeof theme->normal, rgb[BACKGROUND], rgb[FOREGROUND]);
    format_style(theme->highlight, sizeof theme->highlight, rgb[BACKGROUND], rgb[HIGHLIGHT]);
    format_style(theme->muted, sizeof theme->muted, rgb[BACKGROUND], rgb[MUTED]);
    format_style(theme->selection, sizeof theme->selection, rgb[SELECTION_BACKGROUND], rgb[SELECTION_FOREGROUND]);
    app->show_saved = config->show_saved;
}

static void terminal_restore(void)
{
    if (!terminal_active)
        return;

    fputs("\033[?2004l\033[0m\033[?1049l\033[?25h", stdout);
    fflush(stdout);
    while (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios) < 0 && errno == EINTR)
        continue;
    terminal_active = false;
}

static void *allocate(void *old, size_t count, size_t size)
{
    void *memory = reallocarray(old, count, size);
    if (!memory) {
        terminal_restore();
        fputs("net: out of memory\n", stderr);
        exit(EXIT_FAILURE);
    }
    return memory;
}

static void replace_string(char **dest, const char *value)
{
    char *copy = value ? strcpy(allocate(NULL, strlen(value) + 1, 1), value) : NULL;
    free(*dest);
    *dest = copy;
}

static void handle_signal(int signal)
{
    if (signal == SIGWINCH)
        resized = 1;
    else
        interrupted = signal;
}

static int setup_signals(sigset_t *wait_mask)
{
    static const int signals[] = { SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGQUIT };
    struct sigaction action = { .sa_handler = handle_signal };
    sigset_t blocked;

    sigemptyset(&blocked);
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
        sigaddset(&blocked, signals[i]);
    if (sigprocmask(SIG_BLOCK, &blocked, wait_mask) < 0)
        return -1;
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) {
        if (sigaction(signals[i], &action, NULL) < 0)
            return -1;
        sigdelset(wait_mask, signals[i]);
    }
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

static int terminal_enter(void)
{
    if (!isatty(STDOUT_FILENO)) {
        errno = ENOTTY;
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &saved_termios) < 0)
        return -1;

    struct termios raw = saved_termios;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0)
        return -1;

    terminal_active = true;
    fputs("\033[?1049h\033[?25l\033[?2004h", stdout);
    return fflush(stdout);
}

static void terminal_size(struct App *app)
{
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0)
        ws = (struct winsize){0};

    unsigned width = ws.ws_col ? ws.ws_col : 80;
    unsigned height = ws.ws_row ? ws.ws_row : 24;
    if (width == app->width && height == app->height)
        return;

    app->lines = allocate(app->lines, height, sizeof *app->lines);
    memset(app->lines, 0, height * sizeof *app->lines);
    app->width = width;
    app->height = height;
    app->clear_screen = app->screen_dirty = true;
}

static void dispose_children(struct Object *object);

static void dispose_child(struct Object *object)
{
    dispose_children(object);
    sd_bus_slot_unref(object->props.request);
    if (object->props.interface == DEVICE) {
        if (object->device.scan_request || object->device.scan_waiting)
            object->app->scan.failed++;
        sd_bus_slot_unref(object->device.wireless.request);
        sd_bus_slot_unref(object->device.scan_request);
        sd_bus_slot_unref(object->device.active_profile_request);
        free(object->device.name);
        free(object->device.active_connection);
        free(object->device.active_profile);
        free(object->device.permanent_address);
    } else if (object->props.interface == ACCESS_POINT) {
        free(object->ap.bssid);
    }
    free(object->path);
    free(object);
}

static void dispose_children(struct Object *object)
{
    while (object->children) {
        struct Object *child = object->children;
        object->children = child->next;
        dispose_child(child);
    }
}

static struct Object *find_child(const struct Object *parent, const char *path)
{
    for (struct Object *child = parent->children; child; child = child->next)
        if (strcmp(child->path, path) == 0)
            return child;
    return NULL;
}

static void request_properties(struct Properties *props);

static struct Object *ensure_child(struct Object *parent, const char *path)
{
    struct Object *child = find_child(parent, path);
    if (child)
        return child;

    child = allocate(NULL, 1, sizeof *child);
    *child = (struct Object){ .app = parent->app, .next = parent->children };
    replace_string(&child->path, path);
    child->props = (struct Properties){
        .object = child,
        .interface = parent->props.interface == MANAGER ? DEVICE : ACCESS_POINT,
    };
    if (child->props.interface == DEVICE) {
        child->device.wireless = (struct Properties){ .object = child, .interface = WIRELESS };
        child->device.last_scan = -1;
    }
    parent->children = child;
    request_properties(&child->props);
    return child;
}

static void prune_children(struct Object *parent)
{
    struct Object **link = &parent->children;
    struct Object *active = parent->props.interface == DEVICE ? parent->device.active : NULL;

    while (*link) {
        struct Object *child = *link;
        if (child->listed || child == active) {
            link = &child->next;
        } else {
            *link = child->next;
            dispose_child(child);
        }
    }
}

static int read_children(struct Object *parent, sd_bus_message *message)
{
    const char *path;
    int result = sd_bus_message_enter_container(message, 'a', "o");
    if (result <= 0)
        return result;

    for (struct Object *child = parent->children; child; child = child->next)
        child->listed = false;
    while ((result = sd_bus_message_read(message, "o", &path)) > 0)
        ensure_child(parent, path)->listed = true;
    if (result < 0)
        return result;

    prune_children(parent);
    return sd_bus_message_exit_container(message);
}

static bool relevant_property(enum Interface interface, const char *name)
{
    static const char *const names[][8] = {
        [MANAGER] = { "WirelessEnabled", "WirelessHardwareEnabled", "Devices" },
        [DEVICE] = { "DeviceType", "State", "StateReason", "ActiveConnection", "Interface" },
        [WIRELESS] = { "AccessPoints", "ActiveAccessPoint", "Mode", "LastScan", "PermHwAddress", "Bitrate" },
        [ACCESS_POINT] = { "Ssid", "Strength", "Flags", "WpaFlags", "RsnFlags",
                           "Mode", "HwAddress", "Frequency" },
    };
    for (size_t i = 0; i < sizeof names[0] / sizeof names[0][0]; i++)
        if (names[interface][i] && strcmp(names[interface][i], name) == 0)
            return true;
    return false;
}

static int read_string(sd_bus_message *message, const char *signature, char **dest)
{
    const char *value;
    int result = sd_bus_message_read(message, signature, &value);
    if (result > 0)
        replace_string(dest, value);
    return result;
}

static int read_ssid(sd_bus_message *message, struct Ssid *ssid)
{
    const void *bytes;
    size_t length;
    int result = sd_bus_message_read_array(message, 'y', &bytes, &length);
    if (result > 0) {
        if (length > sizeof ssid->bytes)
            return -EMSGSIZE;
        ssid->length = length;
        if (length)
            memcpy(ssid->bytes, bytes, length);
    }
    return result;
}

static int read_property(struct Properties *props, const char *name, sd_bus_message *message)
{
    struct Object *object = props->object;
    const char *value;
    int result;

    switch (props->interface) {
    case MANAGER:
        if (strcmp(name, "WirelessEnabled") == 0)
            return sd_bus_message_read(message, "b", &object->manager.enabled);
        if (strcmp(name, "WirelessHardwareEnabled") == 0)
            return sd_bus_message_read(message, "b", &object->manager.hardware_enabled);
        return read_children(object, message);
    case DEVICE:
        if (strcmp(name, "DeviceType") == 0) {
            result = sd_bus_message_read(message, "u", &object->device.type);
            if (result > 0 && object->device.type == NM_DEVICE_WIFI &&
                !object->device.wireless.ready)
                request_properties(&object->device.wireless);
            if (result > 0)
                request_active_profile(object);
            return result;
        }
        if (strcmp(name, "State") == 0)
            return sd_bus_message_read(message, "u", &object->device.state);
        if (strcmp(name, "StateReason") == 0)
            return sd_bus_message_read(message, "(uu)", NULL, &object->device.reason);
        if (strcmp(name, "ActiveConnection") == 0) {
            result = sd_bus_message_read(message, "o", &value);
            if (result > 0 && (!object->device.active_connection ||
                               strcmp(value, object->device.active_connection) != 0)) {
                object->device.active_profile_request =
                    sd_bus_slot_unref(object->device.active_profile_request);
                replace_string(&object->device.active_profile, NULL);
                replace_string(&object->device.active_connection, value);
            }
            if (result > 0)
                request_active_profile(object);
            return result;
        }
        return read_string(message, "s", &object->device.name);
    case WIRELESS:
        if (strcmp(name, "AccessPoints") == 0)
            return read_children(object, message);
        if (strcmp(name, "Mode") == 0)
            return sd_bus_message_read(message, "u", &object->device.mode);
        if (strcmp(name, "LastScan") == 0)
            return sd_bus_message_read(message, "x", &object->device.last_scan);
        if (strcmp(name, "Bitrate") == 0)
            return sd_bus_message_read(message, "u", &object->device.bitrate);
        if (strcmp(name, "PermHwAddress") == 0)
            return read_string(message, "s", &object->device.permanent_address);
        result = sd_bus_message_read(message, "o", &value);
        if (result > 0) {
            object->device.active = strcmp(value, "/") == 0 ? NULL : ensure_child(object, value);
            prune_children(object);
        }
        return result;
    case ACCESS_POINT:
        if (strcmp(name, "Ssid") == 0)
            return read_ssid(message, &object->ap.ssid);
        if (strcmp(name, "Strength") == 0)
            return sd_bus_message_read(message, "y", &object->ap.strength);
        if (strcmp(name, "Flags") == 0)
            return sd_bus_message_read(message, "u", &object->ap.flags);
        if (strcmp(name, "WpaFlags") == 0)
            return sd_bus_message_read(message, "u", &object->ap.wpa);
        if (strcmp(name, "HwAddress") == 0)
            return read_string(message, "s", &object->ap.bssid);
        if (strcmp(name, "Mode") == 0)
            return sd_bus_message_read(message, "u", &object->ap.mode);
        if (strcmp(name, "Frequency") == 0)
            return sd_bus_message_read(message, "u", &object->ap.frequency);
        return sd_bus_message_read(message, "u", &object->ap.rsn);
    }
    return -EINVAL;
}

/* GetAll replies and PropertiesChanged carry the same property dictionary. */
static int read_properties(struct Properties *props, sd_bus_message *message)
{
    int result = sd_bus_message_enter_container(message, 'a', "{sv}");
    if (result <= 0)
        return result;

    while ((result = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        const char *name, *signature;
        if ((result = sd_bus_message_read(message, "s", &name)) <= 0)
            return result;
        if (relevant_property(props->interface, name)) {
            if ((result = sd_bus_message_peek_type(message, NULL, &signature)) < 0 ||
                (result = sd_bus_message_enter_container(message, 'v', signature)) < 0 ||
                (result = read_property(props, name, message)) < 0 ||
                (result = sd_bus_message_exit_container(message)) < 0)
                return result;
            props->object->app->model_dirty = true;
        } else if ((result = sd_bus_message_skip(message, "v")) < 0) {
            return result;
        }
        if ((result = sd_bus_message_exit_container(message)) < 0)
            return result;
    }
    return result < 0 ? result : sd_bus_message_exit_container(message);
}

static int on_properties_reply(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct Properties *props = userdata;
    struct App *app = props->object->app;
    props->request = sd_bus_slot_unref(props->request);
    if (!sd_bus_message_is_method_error(message, NULL) && read_properties(props, message) > 0) {
        props->ready = true;
        if (props->interface == MANAGER) {
            if (!app->owner)
                replace_string(&app->owner, sd_bus_message_get_sender(message));
            request_saved_list(app);
        }
    }
    app->model_dirty = true;
    return 0;
}

static void request_properties(struct Properties *props)
{
    if (props->request)
        return;

    struct Object *object = props->object;
    sd_bus *bus = object->app->bus;
    sd_bus_message *message = NULL;
    if (sd_bus_message_new_method_call(bus, &message, NM, object->path, PROPERTIES, "GetAll") >= 0 &&
        sd_bus_message_set_auto_start(message, 0) >= 0 &&
        sd_bus_message_append(message, "s", interfaces[props->interface]) >= 0)
        sd_bus_call_async(bus, &props->request, message, on_properties_reply, props, 3000000);
    sd_bus_message_unref(message);
}

static struct Properties *find_properties(struct App *app, const char *path, const char *interface)
{
    if (strcmp(interface, NM) == 0 && strcmp(path, NM_PATH) == 0)
        return &app->manager.props;

    if (strcmp(interface, NM_DEVICE) == 0 || strcmp(interface, NM_WIRELESS) == 0) {
        struct Object *device = find_child(&app->manager, path);
        if (device)
            return strcmp(interface, NM_DEVICE) == 0 ? &device->props : &device->device.wireless;
    } else if (strcmp(interface, NM_AP) == 0) {
        for (struct Object *device = app->manager.children; device; device = device->next) {
            struct Object *ap = find_child(device, path);
            if (ap)
                return &ap->props;
        }
    }
    return NULL;
}

static void update_properties(struct App *app, sd_bus_message *message, const char *path)
{
    const char *interface, *name;
    if (sd_bus_message_read(message, "s", &interface) <= 0)
        return;

    if (ip_details_changed(app, path, interface))
        return;
    if (strcmp(interface, NM_PROFILE) == 0) {
        request_saved_profile(app, path);
        return;
    }
    struct Properties *props = find_properties(app, path, interface);
    if (!props || read_properties(props, message) <= 0)
        return;

    if (!props->ready)
        request_properties(props);
    if (sd_bus_message_enter_container(message, 'a', "s") > 0)
        while (sd_bus_message_read(message, "s", &name) > 0)
            if (relevant_property(props->interface, name)) {
                request_properties(props);
                break;
            }
}

static int on_nm_signal(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    const char *sender = sd_bus_message_get_sender(message);
    if (!sender || !app->owner || strcmp(sender, app->owner) != 0)
        return 0;

    const char *path = sd_bus_message_get_path(message);
    const char *member = sd_bus_message_get_member(message);
    const char *interface = sd_bus_message_get_interface(message);
    struct Object *parent;

    if (saved_signal(app, message, path, interface, member))
        return 0;

    if (strcmp(member, "StateChanged") == 0) {
        uint32_t state, reason;
        if (strcmp(interface, NM_ACTIVE) == 0 && app->operation.active &&
            strcmp(path, app->operation.active) == 0 &&
            sd_bus_message_read(message, "uu", &state, &reason) > 0)
            active_state_changed(app, state, reason);
        if (strcmp(interface, NM_DEVICE) == 0 &&
            (parent = find_child(&app->manager, path)) &&
            sd_bus_message_read(message, "uuu", &state, NULL, &reason) > 0) {
            parent->device.state = state;
            parent->device.reason = reason;
            app->model_dirty = true;
        }
        return 0;
    }

    if (strcmp(interface, PROPERTIES) == 0 && strcmp(member, "PropertiesChanged") == 0) {
        update_properties(app, message, path);
        return 0;
    }

    bool added;
    if (strcmp(interface, NM_WIRELESS) == 0) {
        if (strcmp(member, "AccessPointAdded") != 0 && strcmp(member, "AccessPointRemoved") != 0)
            return 0;
        parent = find_child(&app->manager, path);
        added = strcmp(member, "AccessPointAdded") == 0;
    } else if (strcmp(interface, NM) == 0 && strcmp(path, NM_PATH) == 0) {
        if (strcmp(member, "DeviceAdded") != 0 && strcmp(member, "DeviceRemoved") != 0)
            return 0;
        parent = &app->manager;
        added = strcmp(member, "DeviceAdded") == 0;
    } else {
        return 0;
    }

    if (!parent || sd_bus_message_read(message, "o", &path) <= 0)
        return 0;
    if (added) {
        ensure_child(parent, path)->listed = true;
    } else {
        struct Object *child = find_child(parent, path);
        if (child)
            child->listed = false;
        prune_children(parent);
    }
    app->model_dirty = true;
    return 0;
}

static void reset_manager(struct App *app)
{
    struct Object *manager = &app->manager;
    manager->props.request = sd_bus_slot_unref(manager->props.request);
    manager->props.ready = false;
    manager->manager.enabled = false;
    manager->manager.hardware_enabled = false;
    dispose_children(manager);
    reset_operations(app);
    close_details(app);
    app->radio_was_available = false;
    clear_saved_profiles(app);
    replace_string(&app->owner, NULL);
    app->model_dirty = true;
}

static int on_owner_change(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    const char *name, *old_owner, *new_owner;
    const char *sender = sd_bus_message_get_sender(message);
    if (!sender || strcmp(sender, DBUS) != 0)
        return 0;
    if (sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner) > 0) {
        reset_manager(app);
        if (*new_owner) {
            replace_string(&app->owner, new_owner);
            if (!app->matches_pending)
                request_properties(&app->manager.props);
        }
    }
    return 0;
}

static void close_bus(struct App *app)
{
    reset_manager(app);
    app->agent = sd_bus_slot_unref(app->agent);
    app->bus = sd_bus_close_unref(app->bus);
}

static int on_match_installed(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    if (sd_bus_message_is_method_error(message, NULL))
        app->match_failed = true;
    if (--app->matches_pending == 0 && !app->match_failed) {
        request_properties(&app->manager.props);
        app->screen_dirty = true;
    }
    return 0;
}

static void open_bus(struct App *app)
{
    if (sd_bus_open_system(&app->bus) < 0)
        return;

    app->matches_pending = 2;
    if (sd_bus_add_match_async(app->bus, NULL,
            "type='signal',sender='" DBUS "',path='" DBUS_PATH "',interface='" DBUS
            "',member='NameOwnerChanged',arg0='" NM "'",
            on_owner_change, on_match_installed, app) < 0 ||
        sd_bus_add_match_async(app->bus, NULL,
            "type='signal',sender='" NM "',path_namespace='" NM_PATH "'",
            on_nm_signal, on_match_installed, app) < 0)
        close_bus(app);
}

static enum Security security_group(const struct Object *ap)
{
    uint32_t flags = ap->ap.wpa | ap->ap.rsn;
    if (flags & NM_SEC_PSK)
        return PSK;
    if (ap->ap.rsn & NM_SEC_SAE)
        return SAE;
    if (flags & (NM_SEC_EAP | NM_SEC_SUITE_B))
        return ENTERPRISE;
    if (flags & (NM_SEC_OWE | NM_SEC_OWE_TRANSITION))
        return OWE;
    if (flags)
        return UNKNOWN_SECURITY;
    return ap->ap.flags & NM_AP_PRIVACY ? WEP : OPEN;
}

static const char *security_label(const struct Object *ap)
{
    uint32_t wpa = ap->ap.wpa, rsn = ap->ap.rsn;
    if (rsn & NM_SEC_SUITE_B)
        return "WPA3-Enterprise";
    if (rsn & NM_SEC_SAE)
        return rsn & NM_SEC_PSK ? "WPA2/WPA3" : "WPA3";
    if (rsn & (NM_SEC_OWE | NM_SEC_OWE_TRANSITION))
        return "OWE";
    if (rsn & NM_SEC_EAP)
        return "WPA2-Enterprise";
    if (rsn)
        return "WPA2";
    if (wpa & NM_SEC_EAP)
        return "WPA-Enterprise";
    if (wpa)
        return "WPA";
    return ap->ap.flags & NM_AP_PRIVACY ? "WEP" : "Open";
}

static int compare_ssids(const struct Ssid *left, const struct Ssid *right)
{
    int result = memcmp(left->bytes, right->bytes,
                        left->length < right->length ? left->length : right->length);
    if (result)
        return result;
    return (left->length > right->length) - (left->length < right->length);
}

static int compare_identity(const struct Row *left, const struct Row *right)
{
    const struct Ssid *a = &left->ap->ap.ssid, *b = &right->ap->ap.ssid;
    if (a->length || b->length) {
        int ssid = compare_ssids(a, b);
        return ssid ? ssid : (int)security_group(left->ap) - (int)security_group(right->ap);
    }
    return strcmp(left->ap->path, right->ap->path);
}

static int compare_rank(const void *left, const void *right)
{
    const struct Row *a = left, *b = right;
    if (a->connected != b->connected)
        return a->connected ? -1 : 1;
    if (a->ap->ap.strength != b->ap->ap.strength)
        return (int)b->ap->ap.strength - a->ap->ap.strength;
    int identity = compare_identity(a, b);
    return identity ? identity : strcmp(a->ap->path, b->ap->path);
}

static int compare_group(const void *left, const void *right)
{
    int result = compare_identity(left, right);
    return result ? result : compare_rank(left, right);
}

static void save_selection(struct App *app)
{
    const struct Object *ap = app->rows[app->selected].ap;
    app->selected_ssid = ap->ap.ssid;
    app->selected_security = security_group(ap);
    if (ap->ap.ssid.length)
        replace_string(&app->selected_hidden, NULL);
    else if (!app->selected_hidden || strcmp(app->selected_hidden, ap->path) != 0)
        replace_string(&app->selected_hidden, ap->path);
    app->have_selection = true;
}

static bool same_selection(const struct App *app, const struct Object *ap)
{
    if (app->selected_hidden)
        return strcmp(app->selected_hidden, ap->path) == 0;
    return app->selected_security == security_group(ap) &&
           compare_ssids(&app->selected_ssid, &ap->ap.ssid) == 0;
}

static void rebuild_rows(struct App *app)
{
    app->count = 0;
    rebuild_saved_rows(app);
    const struct Object *manager = &app->manager;
    if (manager->props.ready && manager->manager.enabled && manager->manager.hardware_enabled) {
        for (const struct Object *device = manager->children; device; device = device->next) {
            if (device->device.type != NM_DEVICE_WIFI || device->device.mode == NM_MODE_AP)
                continue;
            for (const struct Object *ap = device->children; ap; ap = ap->next) {
                bool connected = ap == device->device.active &&
                                 device->device.state == NM_DEVICE_ACTIVATED;
                if (!ap->props.ready || (!ap->listed && !connected))
                    continue;
                if (app->count == app->capacity) {
                    app->capacity = app->capacity ? app->capacity * 2 : 32;
                    app->rows = allocate(app->rows, app->capacity, sizeof *app->rows);
                }
                app->rows[app->count++] = (struct Row){
                    .ap = ap, .device = device, .connected = connected,
                };
            }
        }
    }

    mark_saved_networks(app);
    if (app->count > 1) {
        qsort(app->rows, app->count, sizeof *app->rows, compare_group);
        size_t unique = 1;
        for (size_t i = 1; i < app->count; i++) {
            if (compare_identity(&app->rows[unique - 1], &app->rows[i]) != 0)
                app->rows[unique++] = app->rows[i];
            else
                app->rows[unique - 1].saved |= app->rows[i].saved;
        }
        app->count = unique;
        qsort(app->rows, app->count, sizeof *app->rows, compare_rank);
    }

    if (app->count) {
        if (app->selected >= app->count)
            app->selected = app->count - 1;
        bool matched = false;
        if (app->have_selection) {
            for (size_t i = 0; i < app->count; i++) {
                if (same_selection(app, app->rows[i].ap)) {
                    app->selected = i;
                    matched = true;
                    break;
                }
            }
        }
        if (matched || !app->have_selection || !app->scan.deadline)
            save_selection(app);
    } else {
        app->top = 0;
    }

    app->model_dirty = false;
    app->screen_dirty = true;
}

static void clear_password(struct Operation *operation)
{
    explicit_bzero(operation->password, sizeof operation->password);
    operation->password_length = 0;
    operation->password_overflow = false;
    operation->password_error = NULL;
}

static void finish_operation(struct App *app, const char *notice)
{
    struct Operation *operation = &app->operation;
    if (operation->secret_request)
        sd_bus_reply_method_errorf(operation->secret_request, NM_AGENT ".UserCanceled",
                                   "The password prompt was closed");
    sd_bus_message_unref(operation->secret_request);
    sd_bus_message_unref(operation->profiles);
    sd_bus_slot_unref(operation->request);
    free(operation->device);
    free(operation->ap);
    free(operation->profile);
    free(operation->active);
    clear_password(operation);
    *operation = (struct Operation){0};
    app->notice = notice;
    app->screen_dirty = true;
}

static void reset_operations(struct App *app)
{
    bool interrupted_operation = app->operation.action != IDLE || app->scan.deadline;
    finish_operation(app, interrupted_operation ? "NetworkManager went away" : NULL);
    app->scan = (struct Scan){0};
    app->scan_notice = NULL;
    app->agent_registered = false;
}

static int new_method(struct App *app, sd_bus_message **message, const char *path,
                      const char *interface, const char *method, bool interactive)
{
    int result = sd_bus_message_new_method_call(app->bus, message, app->owner, path, interface, method);
    if (result >= 0 && interactive)
        result = sd_bus_message_set_allow_interactive_authorization(*message, true);
    return result;
}

static int send_method(struct App *app, sd_bus_slot **slot, sd_bus_message_handler_t callback,
                       void *userdata, uint64_t timeout, const char *path, const char *interface,
                       const char *method, bool interactive)
{
    sd_bus_message *message = NULL;
    int result = new_method(app, &message, path, interface, method, interactive);
    if (result >= 0)
        result = sd_bus_call_async(app->bus, slot, message, callback, userdata, timeout);
    sd_bus_message_unref(message);
    return result;
}

static int on_active_profile(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct Object *device = userdata;
    const char *path;
    device->device.active_profile_request = sd_bus_slot_unref(device->device.active_profile_request);
    if (sd_bus_message_read(message, "v", "o", &path) > 0 && strcmp(path, "/") != 0)
        replace_string(&device->device.active_profile, path);
    device->app->model_dirty = true;
    return 0;
}

static void request_active_profile(struct Object *device)
{
    struct App *app = device->app;
    const char *active = device->device.active_connection;
    if (!app->owner || device->device.type != NM_DEVICE_WIFI || !active ||
        strcmp(active, "/") == 0 || device->device.active_profile || device->device.active_profile_request)
        return;

    sd_bus_message *message = NULL;
    if (new_method(app, &message, active, PROPERTIES, "Get", false) >= 0 &&
        sd_bus_message_append(message, "ss", NM_ACTIVE, "Connection") >= 0)
        sd_bus_call_async(app->bus, &device->device.active_profile_request, message,
                          on_active_profile, device, 5000000);
    sd_bus_message_unref(message);
}

static bool error_named(const sd_bus_error *error, const char *suffix)
{
    if (!error || !error->name)
        return false;
    const char *last = strrchr(error->name, '.');
    return last && strcmp(last + 1, suffix) == 0;
}

static bool authorization_error(const sd_bus_error *error)
{
    return error_named(error, "PermissionDenied") || error_named(error, "AccessDenied") ||
           error_named(error, "NotAuthorized") || error_named(error, "AuthFailed") ||
           error_named(error, "InteractiveAuthorizationRequired") ||
           error_named(error, "NotAuthorizedCanObtain") || error_named(error, "NotAuthorizedRetained");
}

static const char *method_failure(sd_bus_message *message, const char *fallback)
{
    const sd_bus_error *error = sd_bus_message_get_error(message);
    if (authorization_error(error))
        return "Authorization denied | Check for a Polkit agent";
    if (error_named(error, "NoReply") || error_named(error, "Timeout"))
        return "Timed out waiting for NetworkManager";
    if (error_named(error, "UnknownDevice") || error_named(error, "UnknownObject") ||
        error_named(error, "AccessPointNotFound") || error_named(error, "SpecificObjectNotFound"))
        return "Network or wireless device is no longer available";
    if (error_named(error, "UnknownConnection"))
        return "The saved profile disappeared | Try again";
    if (error_named(error, "ConnectionNotAvailable"))
        return "NetworkManager cannot activate this connection on the selected device";
    return fallback;
}

static bool wifi_available(const struct App *app)
{
    return app->manager.props.ready && app->manager.manager.enabled &&
           app->manager.manager.hardware_enabled && app->owner;
}

static bool client_device(const struct Object *device)
{
    return device->device.type == NM_DEVICE_WIFI && device->device.mode != NM_MODE_AP &&
           device->device.state >= NM_DEVICE_DISCONNECTED &&
           device->device.state <= NM_DEVICE_ACTIVATED;
}

static int on_scan_reply(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct Object *device = userdata;
    struct Scan *scan = &device->app->scan;
    const sd_bus_error *error = sd_bus_message_get_error(message);
    device->device.scan_request = sd_bus_slot_unref(device->device.scan_request);
    if (!error) {
        device->device.scan_waiting = true;
    } else if (authorization_error(error)) {
        scan->denied++;
    } else if (error_named(error, "NoReply") || error_named(error, "Timeout")) {
        scan->timed_out++;
    } else if (error_named(error, "NotAllowed") && error->message &&
               (strstr(error->message, "recent") || strstr(error->message, "busy") ||
                strstr(error->message, "progress") || strstr(error->message, "rate") ||
                strstr(error->message, "previous scan"))) {
        scan->throttled++;
    } else {
        scan->failed++;
    }
    return 0;
}

static void start_scan(struct App *app)
{
    if (app->scan.deadline)
        return;
    app->notice = NULL;
    app->screen_dirty = true;
    if (!wifi_available(app)) {
        app->scan_notice = "Wi-Fi is unavailable";
        return;
    }

    app->scan = (struct Scan){ .deadline = monotonic_us() + 25000000 };
    app->scan_notice = "Scanning...";
    bool found = false;
    for (struct Object *device = app->manager.children; device; device = device->next) {
        if (!client_device(device) || !device->device.wireless.ready)
            continue;
        found = true;
        device->device.scan_baseline = device->device.last_scan;

        sd_bus_message *message = NULL;
        int result = new_method(app, &message, device->path, NM_WIRELESS, "RequestScan", true);
        if (result >= 0)
            result = sd_bus_message_append(message, "a{sv}", 0);
        if (result >= 0)
            result = sd_bus_call_async(app->bus, &device->device.scan_request, message,
                                       on_scan_reply, device, 20000000);
        sd_bus_message_unref(message);
        if (result < 0)
            app->scan.failed++;
    }
    if (!found) {
        app->scan.deadline = 0;
        app->scan_notice = "No wireless interface is ready to scan";
    }
}

static void refresh_scan(struct App *app, uint64_t now)
{
    if (!app->scan.deadline)
        return;
    bool pending = false;
    for (struct Object *device = app->manager.children; device; device = device->next) {
        if (device->device.scan_waiting && device->device.last_scan > device->device.scan_baseline) {
            device->device.scan_waiting = false;
            app->scan.completed++;
        }
        pending |= device->device.scan_request || device->device.scan_waiting;
    }
    if (pending && now < app->scan.deadline)
        return;

    for (struct Object *device = app->manager.children; device; device = device->next) {
        device->device.scan_request = sd_bus_slot_unref(device->device.scan_request);
        device->device.scan_waiting = false;
    }
    app->scan.deadline = 0;
    if (app->scan.completed)
        app->scan_notice = pending || app->scan.denied || app->scan.failed ||
                           app->scan.throttled || app->scan.timed_out ?
                           "Scan complete on some interfaces" : "Scan complete";
    else if (pending || app->scan.timed_out)
        app->scan_notice = "Timed out waiting for scan results";
    else if (app->scan.denied)
        app->scan_notice = "Scan denied | Check for a Polkit agent";
    else if (app->scan.throttled)
        app->scan_notice = "Scan throttled or already in progress | Try again shortly";
    else
        app->scan_notice = "Scan unavailable | Try again when Wi-Fi is ready";
    app->model_dirty = true;
}

enum { PROTO_WPA = 1, PROTO_RSN = 2 };

struct Profile {
    struct Ssid ssid;
    const char *type, *mode, *key_mgmt, *interface, *band, *uuid, *id;
    const void *bssid, *mac;
    size_t bssid_length, mac_length;
    uint32_t channel;
    unsigned protocols;
    int autoconnect;
    bool has_security;
};

struct SavedProfile {
    struct App *app;
    struct SavedProfile *next;
    char *path;
    sd_bus_slot *request;
    sd_bus_message *settings;
    struct Profile data;
    bool in_range, connected, duplicate, refresh_again, listed;
};

static int read_profile_field(sd_bus_message *message, struct Profile *profile,
                              const char *section, const char *name)
{
    if (strcmp(section, "connection") == 0) {
        if (strcmp(name, "type") == 0)
            return sd_bus_message_read(message, "s", &profile->type);
        if (strcmp(name, "interface-name") == 0)
            return sd_bus_message_read(message, "s", &profile->interface);
        if (strcmp(name, "uuid") == 0)
            return sd_bus_message_read(message, "s", &profile->uuid);
        if (strcmp(name, "id") == 0)
            return sd_bus_message_read(message, "s", &profile->id);
        if (strcmp(name, "autoconnect") == 0)
            return sd_bus_message_read(message, "b", &profile->autoconnect);
    } else if (strcmp(section, WIFI_SETTING) == 0) {
        if (strcmp(name, "ssid") == 0)
            return read_ssid(message, &profile->ssid);
        if (strcmp(name, "mode") == 0)
            return sd_bus_message_read(message, "s", &profile->mode);
        if (strcmp(name, "band") == 0)
            return sd_bus_message_read(message, "s", &profile->band);
        if (strcmp(name, "channel") == 0)
            return sd_bus_message_read(message, "u", &profile->channel);
        if (strcmp(name, "bssid") == 0)
            return sd_bus_message_read_array(message, 'y', &profile->bssid, &profile->bssid_length);
        if (strcmp(name, "mac-address") == 0)
            return sd_bus_message_read_array(message, 'y', &profile->mac, &profile->mac_length);
    } else if (strcmp(section, WIFI_SECURITY) == 0) {
        if (strcmp(name, "key-mgmt") == 0)
            return sd_bus_message_read(message, "s", &profile->key_mgmt);
        if (strcmp(name, "proto") == 0) {
            int result = sd_bus_message_enter_container(message, 'a', "s");
            if (result <= 0)
                return result;
            const char *protocol;
            while ((result = sd_bus_message_read(message, "s", &protocol)) > 0) {
                if (strcmp(protocol, "wpa") == 0)
                    profile->protocols |= PROTO_WPA;
                else if (strcmp(protocol, "rsn") == 0)
                    profile->protocols |= PROTO_RSN;
            }
            return result < 0 ? result : sd_bus_message_exit_container(message);
        }
    }
    return sd_bus_message_skip(message, NULL);
}

static int read_profile(sd_bus_message *message, struct Profile *profile)
{
    profile->autoconnect = true;
    int result = sd_bus_message_enter_container(message, 'a', "{sa{sv}}");
    if (result <= 0)
        return -EBADMSG;
    while ((result = sd_bus_message_enter_container(message, 'e', "sa{sv}")) > 0) {
        const char *section;
        if (sd_bus_message_read(message, "s", &section) <= 0 ||
            sd_bus_message_enter_container(message, 'a', "{sv}") <= 0)
            return -EBADMSG;
        if (strcmp(section, WIFI_SECURITY) == 0)
            profile->has_security = true;
        while ((result = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
            const char *name;
            if (sd_bus_message_read(message, "s", &name) <= 0 ||
                sd_bus_message_enter_container(message, 'v', NULL) <= 0 ||
                read_profile_field(message, profile, section, name) < 0 ||
                sd_bus_message_exit_container(message) < 0 ||
                sd_bus_message_exit_container(message) < 0)
                return -EBADMSG;
        }
        if (result < 0 || sd_bus_message_exit_container(message) < 0 ||
            sd_bus_message_exit_container(message) < 0)
            return -EBADMSG;
    }
    return result < 0 ? result : sd_bus_message_exit_container(message);
}

static bool wifi_profile(const struct Profile *profile)
{
    return profile->type && strcmp(profile->type, WIFI_SETTING) == 0 && profile->uuid &&
           strlen(profile->uuid) == UUID_SIZE - 1 &&
           strspn(profile->uuid, "0123456789abcdefABCDEF-") == UUID_SIZE - 1;
}

static struct SavedProfile *find_saved_profile(const struct App *app, const char *path)
{
    for (struct SavedProfile *profile = app->saved.profiles; profile; profile = profile->next)
        if (strcmp(profile->path, path) == 0)
            return profile;
    return NULL;
}

static void free_saved_profile(struct SavedProfile *profile)
{
    if (profile->request)
        profile->app->saved.pending--;
    sd_bus_slot_unref(profile->request);
    sd_bus_message_unref(profile->settings);
    free(profile->path);
    free(profile);
}

static void remove_saved_profile(struct App *app, const char *path)
{
    struct SavedProfile **link = &app->saved.profiles;
    while (*link && strcmp((*link)->path, path) != 0)
        link = &(*link)->next;
    if (*link) {
        struct SavedProfile *profile = *link;
        *link = profile->next;
        free_saved_profile(profile);
        app->model_dirty = true;
    }
}

static void clear_saved_profiles(struct App *app)
{
    struct SavedView *saved = &app->saved;
    saved->request = sd_bus_slot_unref(saved->request);
    while (saved->profiles) {
        struct SavedProfile *profile = saved->profiles;
        saved->profiles = profile->next;
        free_saved_profile(profile);
    }
    saved->error = NULL;
    saved->count = 0;
    saved->listed = false;
}

static void finish_saved_refresh(struct SavedProfile *profile)
{
    struct App *app = profile->app;
    app->saved.pending--;
    app->model_dirty = true;
    if (profile->refresh_again) {
        profile->refresh_again = false;
        request_saved_profile(app, profile->path);
    }
}

static int on_saved_settings(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct SavedProfile *profile = userdata;
    struct Profile data = {0};
    profile->request = sd_bus_slot_unref(profile->request);
    sd_bus_message_unref(profile->settings);
    profile->settings = NULL;
    profile->data = (struct Profile){0};
    sd_bus_message_sensitive(message);
    if (!sd_bus_message_is_method_error(message, NULL) &&
        read_profile(message, &data) > 0 && wifi_profile(&data)) {
        /* Strings and byte arrays in data point into this message. */
        profile->settings = sd_bus_message_ref(message);
        profile->data = data;
    }
    finish_saved_refresh(profile);
    return 1;
}

static bool persistent_profile(sd_bus_message *message)
{
    uint32_t flags = 0;
    const char *filename = NULL;
    if (sd_bus_message_enter_container(message, 'a', "{sv}") <= 0)
        return false;

    int result;
    while ((result = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        const char *name;
        if (sd_bus_message_read(message, "s", &name) <= 0)
            return false;
        if (strcmp(name, "Filename") == 0)
            result = sd_bus_message_read(message, "v", "s", &filename);
        else if (strcmp(name, "Flags") == 0)
            result = sd_bus_message_read(message, "v", "u", &flags);
        else
            result = sd_bus_message_skip(message, "v");
        if (result <= 0 || sd_bus_message_exit_container(message) < 0)
            return false;
    }
    return result == 0 && filename && *filename && strncmp(filename, "/run/", 5) != 0 &&
           !(flags & (NM_PROFILE_EXTERNAL | NM_PROFILE_VOLATILE));
}

static int on_saved_metadata(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct SavedProfile *profile = userdata;
    profile->request = sd_bus_slot_unref(profile->request);
    if (!sd_bus_message_is_method_error(message, NULL) && persistent_profile(message) &&
        send_method(profile->app, &profile->request, on_saved_settings, profile, 5000000,
                    profile->path, NM_PROFILE, "GetSettings", false) >= 0)
        return 1;

    profile->settings = sd_bus_message_unref(profile->settings);
    profile->data = (struct Profile){0};
    finish_saved_refresh(profile);
    return 1;
}

static void request_saved_profile(struct App *app, const char *path)
{
    struct SavedProfile *profile = find_saved_profile(app, path);
    if (!profile)
        return;
    if (profile->request) {
        profile->refresh_again = true;
        return;
    }

    sd_bus_message *message = NULL;
    int result = new_method(app, &message, path, PROPERTIES, "GetAll", false);
    if (result >= 0)
        result = sd_bus_message_append(message, "s", NM_PROFILE);
    if (result >= 0)
        result = sd_bus_call_async(app->bus, &profile->request, message,
                                   on_saved_metadata, profile, 5000000);
    sd_bus_message_unref(message);
    if (result >= 0)
        app->saved.pending++;
}

static struct SavedProfile *ensure_saved_profile(struct App *app, const char *path)
{
    struct SavedProfile *profile = find_saved_profile(app, path);
    if (profile)
        return profile;
    profile = allocate(NULL, 1, sizeof *profile);
    *profile = (struct SavedProfile){ .app = app, .next = app->saved.profiles };
    replace_string(&profile->path, path);
    app->saved.profiles = profile;
    request_saved_profile(app, path);
    return profile;
}

static int on_saved_list(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    struct SavedView *saved = &app->saved;
    saved->request = sd_bus_slot_unref(saved->request);
    app->model_dirty = true;
    if (sd_bus_message_is_method_error(message, NULL) ||
        sd_bus_message_enter_container(message, 'a', "o") <= 0) {
        saved->error = method_failure(message, "Could not read saved profiles");
        return 1;
    }

    for (struct SavedProfile *profile = saved->profiles; profile; profile = profile->next)
        profile->listed = false;

    const char *path;
    int result;
    while ((result = sd_bus_message_read(message, "o", &path)) > 0)
        ensure_saved_profile(app, path)->listed = true;

    if (result < 0) {
        saved->error = "Could not read saved profiles";
        return 1;
    }
    struct SavedProfile **link = &saved->profiles;
    while (*link) {
        struct SavedProfile *profile = *link;
        if (profile->listed) {
            link = &profile->next;
        } else {
            *link = profile->next;
            free_saved_profile(profile);
        }
    }
    saved->listed = true;
    return 1;
}

static void request_saved_list(struct App *app)
{
    struct SavedView *saved = &app->saved;
    if (!app->owner || saved->request || saved->listed)
        return;
    saved->error = NULL;
    if (send_method(app, &saved->request, on_saved_list, app, 5000000,
                    NM_SETTINGS_PATH, NM_SETTINGS, "ListConnections", false) < 0)
        saved->error = "Could not read saved profiles";
    app->screen_dirty = true;
}

static bool saved_signal(struct App *app, sd_bus_message *message, const char *path,
                         const char *interface, const char *member)
{
    if (strcmp(interface, NM_SETTINGS) == 0 && strcmp(path, NM_SETTINGS_PATH) == 0) {
        if (strcmp(member, "NewConnection") == 0 &&
            sd_bus_message_read(message, "o", &path) > 0)
            ensure_saved_profile(app, path);
        else if (strcmp(member, "ConnectionRemoved") == 0 &&
                 sd_bus_message_read(message, "o", &path) > 0)
            remove_saved_profile(app, path);
        return true;
    }

    if (strcmp(interface, NM_PROFILE) != 0)
        return false;
    if (strcmp(member, "Removed") == 0)
        remove_saved_profile(app, path);
    else if (strcmp(member, "Updated") == 0) {
        if (find_saved_profile(app, path))
            request_saved_profile(app, path);
        else
            ensure_saved_profile(app, path);
    }
    return true;
}

static bool address_matches(const void *bytes, size_t length, const char *text)
{
    if (!length)
        return true;
    struct ether_addr address;
    return length == sizeof address.ether_addr_octet && text && ether_aton_r(text, &address) &&
           memcmp(bytes, address.ether_addr_octet, length) == 0;
}

static uint32_t wifi_channel(uint32_t frequency, const char **band)
{
    if (frequency >= 2412 && frequency <= 2484) {
        *band = "bg";
        return frequency == 2484 ? 14 : (frequency - 2407) / 5;
    }
    if (frequency >= 4910 && frequency < 5925) {
        *band = "a";
        return (frequency - (frequency < 5000 ? 4000 : 5000)) / 5;
    }
    if (frequency == 5935 || (frequency >= 5955 && frequency <= 7115)) {
        *band = "6GHz";
        return frequency == 5935 ? 2 : (frequency - 5950) / 5;
    }
    if (frequency >= 58320 && frequency <= 69120) {
        *band = "60GHz";
        return (frequency - 58320) / 2160 + 1;
    }
    *band = NULL;
    return 0;
}

static bool frequency_matches(uint32_t frequency, const char *band, uint32_t channel)
{
    const char *actual_band;
    uint32_t actual_channel = wifi_channel(frequency, &actual_band);
    return (!band || (actual_band && strcmp(band, actual_band) == 0)) &&
           (!channel || channel == actual_channel);
}

static enum Security profile_security(const struct Profile *profile, const struct Object *ap)
{
    if (!profile->has_security)
        return security_group(ap) == OPEN ? OPEN : UNKNOWN_SECURITY;
    unsigned protocols = profile->protocols ? profile->protocols : PROTO_WPA | PROTO_RSN;
    if (!profile->key_mgmt)
        return UNKNOWN_SECURITY;
    if (strcmp(profile->key_mgmt, "sae") == 0 && (protocols & PROTO_RSN) &&
        (ap->ap.rsn & NM_SEC_SAE))
        return SAE;
    if (strcmp(profile->key_mgmt, "wpa-psk") == 0 &&
        (((protocols & PROTO_WPA) && (ap->ap.wpa & NM_SEC_PSK)) ||
         ((protocols & PROTO_RSN) && (ap->ap.rsn & (NM_SEC_PSK | NM_SEC_SAE)))))
        return PSK;
    if ((strcmp(profile->key_mgmt, "wpa-eap") == 0 &&
         (((protocols & PROTO_WPA) && (ap->ap.wpa & NM_SEC_EAP)) ||
          ((protocols & PROTO_RSN) && (ap->ap.rsn & NM_SEC_EAP)))) ||
        (strcmp(profile->key_mgmt, "wpa-eap-suite-b-192") == 0 &&
         (protocols & PROTO_RSN) && (ap->ap.rsn & NM_SEC_SUITE_B)))
        return ENTERPRISE;
    if (strcmp(profile->key_mgmt, "owe") == 0 &&
        (ap->ap.rsn & (NM_SEC_OWE | NM_SEC_OWE_TRANSITION)))
        return OWE;
    if ((strcmp(profile->key_mgmt, "none") == 0 || strcmp(profile->key_mgmt, "ieee8021x") == 0) &&
        security_group(ap) == WEP)
        return WEP;
    return UNKNOWN_SECURITY;
}

static enum Security match_profile(const struct Profile *profile, const struct Object *device,
                                   const struct Object *ap)
{
    if (!profile->ssid.length || compare_ssids(&profile->ssid, &ap->ap.ssid) != 0 ||
        (profile->mode && strcmp(profile->mode, "infrastructure") != 0) ||
        ap->ap.mode != NM_MODE_INFRA ||
        (profile->interface && (!device->device.name ||
                                strcmp(profile->interface, device->device.name) != 0)) ||
        !address_matches(profile->mac, profile->mac_length, device->device.permanent_address) ||
        !address_matches(profile->bssid, profile->bssid_length, ap->ap.bssid) ||
        !frequency_matches(ap->ap.frequency, profile->band, profile->channel))
        return UNKNOWN_SECURITY;
    return profile_security(profile, ap);
}

static enum Security configured_security(const struct Profile *profile)
{
    const char *key = profile->key_mgmt;
    if (!profile->has_security)
        return OPEN;
    if (!key)
        return UNKNOWN_SECURITY;
    if (strcmp(key, "wpa-psk") == 0)
        return PSK;
    if (strcmp(key, "sae") == 0)
        return SAE;
    if (strncmp(key, "wpa-eap", 7) == 0)
        return ENTERPRISE;
    if (strcmp(key, "owe") == 0)
        return OWE;
    if (strcmp(key, "none") == 0 || strcmp(key, "ieee8021x") == 0)
        return WEP;
    return UNKNOWN_SECURITY;
}

static const struct Object *hidden_device(const struct App *app, const struct Profile *profile)
{
    const struct Object *best = NULL;
    for (const struct Object *device = app->manager.children; device; device = device->next) {
        if (!client_device(device))
            continue;
        if (profile && ((profile->interface && (!device->device.name ||
                          strcmp(profile->interface, device->device.name) != 0)) ||
                        !address_matches(profile->mac, profile->mac_length, device->device.permanent_address)))
            continue;
        if (device->device.state == NM_DEVICE_DISCONNECTED)
            return device;
        if (!best)
            best = device;
    }
    return best;
}

static bool personal_security(enum Security security)
{
    return security == OPEN || security == PSK || security == SAE;
}

static int compare_saved_rows(const void *left, const void *right)
{
    const struct Profile *a = &(*(struct SavedProfile *const *)left)->data;
    const struct Profile *b = &(*(struct SavedProfile *const *)right)->data;
    int ssid = compare_ssids(&a->ssid, &b->ssid);
    if (ssid)
        return ssid;
    int name = strcmp(a->id ? a->id : "", b->id ? b->id : "");
    return name ? name : strcmp(a->uuid, b->uuid);
}

static bool profile_may_be_active(const struct App *app, const char *profile)
{
    for (const struct Object *device = app->manager.children; device; device = device->next) {
        if (device->device.type != NM_DEVICE_WIFI)
            continue;
        if (device->device.active_profile && strcmp(device->device.active_profile, profile) == 0)
            return true;
        if (!device->device.active_profile &&
            ((device->device.active_connection && strcmp(device->device.active_connection, "/") != 0) ||
             device->device.state == NM_DEVICE_ACTIVATED))
            return true;
    }
    return false;
}

static void save_saved_selection(struct SavedView *saved)
{
    memcpy(saved->selected_uuid, saved->rows[saved->selected]->data.uuid, UUID_SIZE);
}

static void rebuild_saved_rows(struct App *app)
{
    struct SavedView *saved = &app->saved;
    saved->count = 0;
    for (struct SavedProfile *profile = saved->profiles; profile; profile = profile->next) {
        if (!profile->settings)
            continue;
        profile->in_range = profile->connected = profile->duplicate = false;
        for (const struct Object *device = app->manager.children; device; device = device->next) {
            if (device->device.type == NM_DEVICE_WIFI && device->device.state == NM_DEVICE_ACTIVATED &&
                device->device.active_profile && strcmp(device->device.active_profile, profile->path) == 0) {
                profile->connected = true;
                break;
            }
        }
        if (saved->count == saved->capacity) {
            saved->capacity = saved->capacity ? saved->capacity * 2 : 16;
            saved->rows = allocate(saved->rows, saved->capacity, sizeof *saved->rows);
        }
        saved->rows[saved->count++] = profile;
    }

    if (!saved->count)
        return;
    if (saved->count > 1)
        qsort(saved->rows, saved->count, sizeof *saved->rows, compare_saved_rows);
    if (saved->selected >= saved->count)
        saved->selected = saved->count - 1;
    bool found = false;
    for (size_t i = 0; i < saved->count; i++) {
        struct SavedProfile *profile = saved->rows[i];
        if (i && compare_ssids(&profile->data.ssid, &saved->rows[i - 1]->data.ssid) == 0)
            profile->duplicate = saved->rows[i - 1]->duplicate = true;
        if (strcmp(profile->data.uuid, saved->selected_uuid) == 0) {
            saved->selected = i;
            found = true;
        }
    }
    if (found || !*saved->selected_uuid || (!saved->pending && !saved->request))
        save_saved_selection(saved);
}

static void mark_saved_networks(struct App *app)
{
    for (size_t i = 0; i < app->count; i++) {
        struct Row *row = &app->rows[i];
        for (size_t j = 0; j < app->saved.count; j++) {
            struct SavedProfile *profile = app->saved.rows[j];
            if (match_profile(&profile->data, row->device, row->ap) != UNKNOWN_SECURITY) {
                row->saved = true;
                profile->in_range = true;
            }
        }
    }
}

static bool target_ap(const struct Operation *operation, const struct Object *ap)
{
    return ap && ap->props.ready && ap->listed && ap->ap.mode == NM_MODE_INFRA &&
           security_group(ap) == operation->group &&
           compare_ssids(&ap->ap.ssid, &operation->ssid) == 0;
}

static bool find_profile_target(struct App *app, const struct Profile *profile)
{
    struct Operation *operation = &app->operation;
    if (!wifi_profile(profile) || compare_ssids(&profile->ssid, &operation->ssid) != 0 ||
        (operation->hidden && configured_security(profile) != operation->security))
        return false;

    const struct Object *best = NULL, *best_device = NULL;
    enum Security best_security = UNKNOWN_SECURITY;
    for (const struct Object *device = app->manager.children; device; device = device->next) {
        if (!client_device(device))
            continue;
        for (const struct Object *ap = device->children; ap; ap = ap->next) {
            if (!ap->props.ready || !ap->listed ||
                (operation->ap && security_group(ap) != operation->group))
                continue;
            enum Security security = match_profile(profile, device, ap);
            if (!personal_security(security))
                continue;
            bool preferred = operation->ap && strcmp(ap->path, operation->ap) == 0;
            if (preferred || !best || ap->ap.strength > best->ap.strength) {
                best = ap;
                best_device = device;
                best_security = security;
            }
            if (preferred)
                break;
        }
        if (best && operation->ap && strcmp(best->path, operation->ap) == 0)
            break;
    }
    if (!best && operation->hidden &&
        (!profile->mode || strcmp(profile->mode, "infrastructure") == 0)) {
        best_device = hidden_device(app, profile);
        if (best_device) {
            replace_string(&operation->device, best_device->path);
            replace_string(&operation->ap, NULL);
            return true;
        }
    }
    if (!best)
        return false;

    replace_string(&operation->ap, best->path);
    replace_string(&operation->device, best_device->path);
    operation->security = best_security;
    operation->group = security_group(best);
    return true;
}

static struct Object *operation_device(const struct App *app)
{
    return app->operation.device ? find_child(&app->manager, app->operation.device) : NULL;
}

static void start_activation(struct App *app);

static bool password_prompt(const struct App *app)
{
    return app->operation.action == ENTER_PASSWORD || app->operation.secret_request;
}

static int append_psk(sd_bus_message *message, const char *password)
{
    return sd_bus_message_append(message, "a{sa{sv}}", 1, WIFI_SECURITY, 1, "psk", "s", password);
}

static bool agent_sender(const struct App *app, sd_bus_message *message)
{
    const char *sender = sd_bus_message_get_sender(message);
    return sender && app->owner && strcmp(sender, app->owner) == 0;
}

static int on_get_secrets(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    struct App *app = userdata;
    struct Operation *operation = &app->operation;
    if (!agent_sender(app, message))
        return sd_bus_error_set_const(error, SD_BUS_ERROR_ACCESS_DENIED, "Only NetworkManager may request secrets");

    const char *path, *setting;
    uint32_t flags;
    struct Profile profile = {0};
    if (read_profile(message, &profile) < 0 ||
        sd_bus_message_read(message, "os", &path, &setting) <= 0 ||
        sd_bus_message_skip(message, "as") < 0 ||
        sd_bus_message_read(message, "u", &flags) <= 0)
        return -EBADMSG;

    if ((operation->action != START_CONNECTION && operation->action != ACTIVATING) ||
        operation->canceled || operation->secret_request || !operation->profile ||
        strcmp(path, operation->profile) != 0 || strcmp(setting, WIFI_SECURITY) != 0 ||
        compare_ssids(&operation->ssid, &profile.ssid) != 0 || !profile.key_mgmt ||
        strcmp(profile.key_mgmt, operation->security == SAE ? "sae" : "wpa-psk") != 0 ||
        !(flags & (NM_SECRET_ALLOW_INTERACTION | NM_SECRET_REQUEST_NEW)) ||
        operation->secret_attempts >= 3)
        return sd_bus_error_set_const(error, NM_AGENT ".NoSecrets", "No Wi-Fi secret is available");

    clear_password(operation);
    if (operation->secret_attempts || (flags & NM_SECRET_REQUEST_NEW))
        operation->password_error = "Try the Wi-Fi password again";
    operation->secret_attempts++;
    operation->secret_request = sd_bus_message_ref(message);
    operation->deadline = monotonic_us() + 90000000;
    app->screen_dirty = true;
    return 1;
}

static int on_cancel_secrets(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    struct App *app = userdata;
    struct Operation *operation = &app->operation;
    if (!agent_sender(app, message))
        return sd_bus_error_set_const(error, SD_BUS_ERROR_ACCESS_DENIED, "Only NetworkManager may cancel requests");

    const char *path, *setting;
    if (sd_bus_message_read(message, "os", &path, &setting) <= 0)
        return -EBADMSG;
    if (operation->secret_request && operation->profile &&
        strcmp(path, operation->profile) == 0 && strcmp(setting, WIFI_SECURITY) == 0) {
        sd_bus_reply_method_errorf(operation->secret_request, NM_AGENT ".AgentCanceled",
                                   "NetworkManager canceled the password request");
        operation->secret_request = sd_bus_message_unref(operation->secret_request);
        clear_password(operation);
        app->screen_dirty = true;
    }
    return sd_bus_reply_method_return(message, "");
}

static int on_modify_agent_secrets(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    if (!agent_sender(userdata, message))
        return sd_bus_error_set_const(error, SD_BUS_ERROR_ACCESS_DENIED, "Only NetworkManager may manage secrets");
    if (sd_bus_message_is_method_call(message, NM_AGENT, "DeleteSecrets"))
        return sd_bus_reply_method_return(message, "");
    return sd_bus_error_set_const(error, NM_AGENT ".Failed", "This agent has no credential store");
}

static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetSecrets", "a{sa{sv}}osasu", "a{sa{sv}}", on_get_secrets,
                  SD_BUS_VTABLE_UNPRIVILEGED | SD_BUS_VTABLE_SENSITIVE),
    SD_BUS_METHOD("CancelGetSecrets", "os", "", on_cancel_secrets, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveSecrets", "a{sa{sv}}o", "", on_modify_agent_secrets,
                  SD_BUS_VTABLE_UNPRIVILEGED | SD_BUS_VTABLE_SENSITIVE),
    SD_BUS_METHOD("DeleteSecrets", "a{sa{sv}}o", "", on_modify_agent_secrets, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

static int on_agent_registered(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    app->operation.request = sd_bus_slot_unref(app->operation.request);
    app->agent_registered = !sd_bus_message_is_method_error(message, NULL);
    start_activation(app);
    return 0;
}

static void activate_saved(struct App *app)
{
    struct Operation *operation = &app->operation;
    if (operation->security == OPEN || app->agent_registered) {
        start_activation(app);
        return;
    }
    if (!app->agent && sd_bus_add_object_vtable(app->bus, &app->agent, NM_AGENT_PATH, NM_AGENT,
                                               agent_vtable, app) < 0) {
        start_activation(app);
        return;
    }

    /* Let NetworkManager try existing credentials even if agent registration fails. */
    char identifier[64];
    snprintf(identifier, sizeof identifier, "org.net.Wifi.p%ld", (long)getpid());
    sd_bus_message *message = NULL;
    int result = new_method(app, &message, NM_AGENTS_PATH, NM_AGENTS, "Register", false);
    if (result >= 0)
        result = sd_bus_message_append(message, "s", identifier);
    if (result >= 0)
        result = sd_bus_call_async(app->bus, &operation->request, message,
                                   on_agent_registered, app, 5000000);
    sd_bus_message_unref(message);
    if (result < 0) {
        start_activation(app);
    } else {
        operation->action = REGISTER_AGENT;
        operation->deadline = monotonic_us() + 8000000;
        app->screen_dirty = true;
    }
}

static int append_new_connection(sd_bus_message *message, const struct App *app)
{
    const struct Operation *operation = &app->operation;
    int result;
    if ((result = sd_bus_message_sensitive(message)) < 0 ||
        (result = sd_bus_message_open_container(message, 'a', "{sa{sv}}")) < 0 ||
        (result = sd_bus_message_append(message, "{sa{sv}}", "connection", 2,
                "type", "s", WIFI_SETTING, "permissions", "as", 1, app->profile_permission)) < 0 ||
        (result = sd_bus_message_open_container(message, 'e', "sa{sv}")) < 0 ||
        (result = sd_bus_message_append(message, "s", WIFI_SETTING)) < 0 ||
        (result = sd_bus_message_open_container(message, 'a', "{sv}")) < 0 ||
        (result = sd_bus_message_append(message, "{sv}", "mode", "s", "infrastructure")) < 0 ||
        (operation->hidden && (result = sd_bus_message_append(message, "{sv}", "hidden", "b", 1)) < 0) ||
        (result = sd_bus_message_open_container(message, 'e', "sv")) < 0 ||
        (result = sd_bus_message_append(message, "s", "ssid")) < 0 ||
        (result = sd_bus_message_open_container(message, 'v', "ay")) < 0 ||
        (result = sd_bus_message_append_array(message, 'y', operation->ssid.bytes, operation->ssid.length)) < 0)
        return result;

    /* Close ssid's variant and entry, then the Wi-Fi setting's dictionary and entry. */
    for (unsigned i = 0; i < 4; i++)
        if ((result = sd_bus_message_close_container(message)) < 0)
            return result;

    if (operation->security != OPEN) {
        result = sd_bus_message_append(message, "{sa{sv}}", WIFI_SECURITY, 2,
                                       "key-mgmt", "s", operation->security == SAE ? "sae" : "wpa-psk",
                                       "psk", "s", operation->password);
        if (result < 0)
            return result;
    }

    result = sd_bus_message_close_container(message);
    if (result < 0)
        return result;
    return sd_bus_message_append(message, "ooa{sv}", operation->device, operation->ap ? operation->ap : "/", 1,
                                 "persist", "s", "disk");
}

static const char *activation_failure(const struct App *app, uint32_t reason)
{
    const struct Operation *operation = &app->operation;
    const struct Object *device = operation_device(app);
    uint32_t device_reason = device ? device->device.reason : 0;
    if (operation->canceled)
        return "Connection canceled";
    if (reason == NM_ACTIVE_REASON_IP_CONFIG)
        return "Connection failed | Could not obtain an IP address";
    if (reason == NM_ACTIVE_REASON_NO_SECRETS || reason == NM_ACTIVE_REASON_LOGIN_FAILED ||
        device_reason == NM_REASON_NO_SECRETS || device_reason == NM_REASON_SUPPLICANT_TIMEOUT ||
        device_reason == NM_REASON_SUPPLICANT_DISCONNECT) {
        if (operation->profile && !app->agent_registered && operation->security != OPEN &&
            !operation->password_submitted)
            return "Wi-Fi password required | NetworkManager secret agent unavailable";
        return "Authentication failed | Check the Wi-Fi password";
    }
    if (operation->hidden && device_reason == NM_REASON_SSID_NOT_FOUND)
        return "Hidden network was not found";
    if (!device || device_reason == NM_REASON_SSID_NOT_FOUND ||
        (operation->ap && !find_child(device, operation->ap)))
        return "Connection failed | Access point is no longer available";
    if (operation->security == SAE)
        return "WPA3 connection failed | Check the hardware and password";
    return "Connection failed";
}

static void active_state_changed(struct App *app, uint32_t state, uint32_t reason)
{
    if (app->operation.action != ACTIVATING)
        return;
    if (state == NM_ACTIVE_ACTIVATED)
        finish_operation(app, "Connected");
    else if (state == NM_ACTIVE_DEACTIVATED)
        finish_operation(app, activation_failure(app, reason));
}

static int on_active_query(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    app->operation.request = sd_bus_slot_unref(app->operation.request);
    uint32_t state;
    if (sd_bus_message_read(message, "v", "u", &state) > 0) {
        active_state_changed(app, state, 0);
    } else if (error_named(sd_bus_message_get_error(message), "UnknownObject")) {
        finish_operation(app, activation_failure(app, 0));
    }
    return 0;
}

static int on_activation_reply(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    struct Operation *operation = &app->operation;
    operation->request = sd_bus_slot_unref(operation->request);
    if (sd_bus_message_is_method_error(message, NULL)) {
        const char *fallback = operation->canceled ? "Connection canceled" :
                               operation->security == SAE ? "WPA3 activation rejected | Check hardware support" :
                               "NetworkManager rejected the connection";
        finish_operation(app, method_failure(message, fallback));
        return 0;
    }

    const char *active, *profile;
    int result = operation->profile ? sd_bus_message_read(message, "o", &active) :
                                      sd_bus_message_read(message, "oo", &profile, &active);
    if (result <= 0 || strcmp(active, "/") == 0) {
        finish_operation(app, "NetworkManager returned no active connection");
        return 0;
    }
    if (!operation->profile)
        replace_string(&operation->profile, profile);
    replace_string(&operation->active, active);
    operation->action = ACTIVATING;
    app->screen_dirty = true;

    /* An active-state signal may have arrived before the method reply. */
    sd_bus_message *query = NULL;
    result = new_method(app, &query, active, PROPERTIES, "Get", false);
    if (result >= 0)
        result = sd_bus_message_append(query, "ss", NM_ACTIVE, "State");
    if (result >= 0)
        sd_bus_call_async(app->bus, &operation->request, query, on_active_query, app, 5000000);
    sd_bus_message_unref(query);
    return 0;
}

static void start_activation(struct App *app)
{
    struct Operation *operation = &app->operation;
    struct Object *device = operation_device(app);
    if (!wifi_available(app) || !device || !client_device(device)) {
        finish_operation(app, "Wireless interface is no longer available");
        return;
    }
    if ((operation->ap && !target_ap(operation, find_child(device, operation->ap))) ||
        (!operation->ap && !operation->hidden)) {
        finish_operation(app, "Access point is no longer available");
        return;
    }
    if (device->device.state == NM_DEVICE_ACTIVATED && target_ap(operation, device->device.active) &&
        (!operation->profile || (device->device.active_profile &&
         strcmp(operation->profile, device->device.active_profile) == 0))) {
        finish_operation(app, "Already connected");
        return;
    }
    if (!operation->profile && !app->profile_permission) {
        finish_operation(app, "Could not resolve the current user for a Wi-Fi profile");
        return;
    }

    sd_bus_message *message = NULL;
    int result = new_method(app, &message, NM_PATH, NM,
                            operation->profile ? "ActivateConnection" : "AddAndActivateConnection2", true);
    if (result >= 0) {
        result = operation->profile ?
                 sd_bus_message_append(message, "ooo", operation->profile, operation->device,
                                       operation->ap ? operation->ap : "/") :
                 append_new_connection(message, app);
    }
    clear_password(operation);
    if (result >= 0)
        result = sd_bus_call_async(app->bus, &operation->request, message,
                                   on_activation_reply, app, 60000000);
    sd_bus_message_unref(message);
    if (result < 0) {
        finish_operation(app, "Could not send connection request");
    } else {
        operation->action = START_CONNECTION;
        operation->deadline = monotonic_us() + 90000000;
        app->screen_dirty = true;
    }
}

static void lookup_profiles(struct App *app);
static void next_profile(struct App *app);

static int on_profile_settings(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    struct Operation *operation = &app->operation;
    operation->request = sd_bus_slot_unref(operation->request);
    const sd_bus_error *error = sd_bus_message_get_error(message);
    if (error) {
        if (error_named(error, "UnknownObject") || error_named(error, "UnknownConnection"))
            next_profile(app);
        else
            finish_operation(app, method_failure(message, "Could not read saved Wi-Fi profiles"));
        return 0;
    }

    struct Profile profile = {0};
    if (read_profile(message, &profile) < 0) {
        finish_operation(app, "Could not read a saved Wi-Fi profile");
    } else if (find_profile_target(app, &profile)) {
        operation->profiles = sd_bus_message_unref(operation->profiles);
        clear_password(operation);
        operation->password_submitted = false;
        activate_saved(app);
    } else {
        next_profile(app);
    }
    return 0;
}

static void next_profile(struct App *app)
{
    struct Operation *operation = &app->operation;
    const char *path;
    int result = sd_bus_message_read(operation->profiles, "o", &path);
    if (result < 0) {
        finish_operation(app, "Could not read the saved profile list");
        return;
    }
    if (result > 0) {
        replace_string(&operation->profile, path);
        if (send_method(app, &operation->request, on_profile_settings, app, 5000000,
                        path, NM_PROFILE, "GetSettings", false) < 0)
            finish_operation(app, "Could not read saved Wi-Fi profiles");
        return;
    }

    operation->profiles = sd_bus_message_unref(operation->profiles);
    replace_string(&operation->profile, NULL);
    if (operation->security == OPEN || operation->password_submitted) {
        start_activation(app);
    } else {
        operation->action = ENTER_PASSWORD;
        operation->deadline = 0;
        app->screen_dirty = true;
    }
}

static int on_profiles(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    struct Operation *operation = &app->operation;
    operation->request = sd_bus_slot_unref(operation->request);
    if (sd_bus_message_is_method_error(message, NULL) ||
        sd_bus_message_enter_container(message, 'a', "o") <= 0) {
        finish_operation(app, method_failure(message, "Could not list saved Wi-Fi profiles"));
        return 0;
    }

    operation->profiles = sd_bus_message_ref(message);
    next_profile(app);
    return 1;
}

static void lookup_profiles(struct App *app)
{
    app->operation.action = LOOKUP;
    app->operation.deadline = monotonic_us() + 20000000;
    app->screen_dirty = true;
    if (send_method(app, &app->operation.request, on_profiles, app, 5000000,
                    NM_SETTINGS_PATH, NM_SETTINGS, "ListConnections", false) < 0)
        finish_operation(app, "Could not list saved Wi-Fi profiles");
}

static void set_notice(struct App *app, const char *notice)
{
    app->notice = notice;
    app->screen_dirty = true;
}

static bool connected_wifi(const struct App *app)
{
    for (const struct Object *device = app->manager.children; device; device = device->next)
        if (device->device.type == NM_DEVICE_WIFI && device->device.state == NM_DEVICE_ACTIVATED)
            return true;
    return false;
}

static int on_radio_set(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    struct Operation *operation = &app->operation;
    operation->request = sd_bus_slot_unref(operation->request);
    if (sd_bus_message_is_method_error(message, NULL)) {
        finish_operation(app, method_failure(message, "NetworkManager could not change Wi-Fi state"));
    } else {
        operation->action = WAIT_RADIO;
        operation->deadline = monotonic_us() + 8000000;
        app->screen_dirty = true;
        request_properties(&app->manager.props);
    }
    return 1;
}

static void set_radio(struct App *app, bool enabled)
{
    struct Operation *operation = &app->operation;
    if (!app->manager.props.ready || !app->owner) {
        finish_operation(app, "NetworkManager is unavailable");
        return;
    }
    if (enabled && !app->manager.manager.hardware_enabled) {
        finish_operation(app, "Wi-Fi is hardware-blocked | Unblock the wireless switch first");
        return;
    }
    if ((app->manager.manager.enabled != 0) == enabled) {
        finish_operation(app, enabled ? "Wi-Fi is already on" : "Wi-Fi is already off");
        return;
    }

    sd_bus_message *message = NULL;
    int result = new_method(app, &message, NM_PATH, PROPERTIES, "Set", true);
    if (result >= 0)
        result = sd_bus_message_append(message, "ssv", NM, "WirelessEnabled", "b", (int)enabled);
    if (result >= 0)
        result = sd_bus_call_async(app->bus, &operation->request, message, on_radio_set, app, 60000000);
    sd_bus_message_unref(message);
    if (result < 0) {
        finish_operation(app, "Could not request the Wi-Fi state change");
    } else {
        operation->action = SET_RADIO;
        operation->radio_enable = enabled;
        operation->deadline = monotonic_us() + 62000000;
        app->screen_dirty = true;
    }
}

static void toggle_radio(struct App *app)
{
    if (app->operation.action != IDLE)
        return;
    if (app->manager.props.ready && !app->manager.manager.hardware_enabled) {
        set_notice(app, "Wi-Fi is hardware-blocked | Unblock the wireless switch first");
    } else if (app->manager.props.ready && app->manager.manager.enabled && connected_wifi(app)) {
        app->operation.action = CONFIRM_RADIO_OFF;
        app->screen_dirty = true;
    } else {
        set_radio(app, !app->manager.manager.enabled);
    }
}

static void refresh_radio(struct App *app)
{
    bool available = wifi_available(app);
    if (available == app->radio_was_available)
        return;

    app->radio_was_available = available;
    for (struct Object *device = app->manager.children; device; device = device->next) {
        if (device->device.type != NM_DEVICE_WIFI)
            continue;
        if (available && device->device.wireless.ready)
            request_properties(&device->device.wireless);
        if (!available) {
            device->device.scan_request = sd_bus_slot_unref(device->device.scan_request);
            device->device.scan_waiting = false;
        }
    }
    if (!available) {
        app->scan = (struct Scan){0};
        app->scan_notice = NULL;
    }
}

static void open_hidden_form(struct App *app)
{
    if (app->operation.action != IDLE)
        return;
    if (!wifi_available(app)) {
        set_notice(app, "Wi-Fi is unavailable");
        return;
    }
    app->operation.hidden = true;
    app->operation.action = HIDDEN_SSID;
    app->screen_dirty = true;
}

static void select_hidden_security(struct App *app, enum Security security)
{
    struct Operation *operation = &app->operation;
    const struct Object *device = wifi_available(app) ? hidden_device(app, NULL) : NULL;
    if (!device) {
        finish_operation(app, "No wireless interface is ready to connect");
        return;
    }
    replace_string(&operation->device, device->path);
    operation->security = operation->group = security;
    lookup_profiles(app);
}

static void select_target(struct App *app, const struct Row *row)
{
    struct Operation *operation = &app->operation;
    replace_string(&operation->device, row->device->path);
    replace_string(&operation->ap, row->ap->path);
    operation->ssid = row->ap->ap.ssid;
    operation->group = operation->security = security_group(row->ap);
}

static void connect_selected(struct App *app)
{
    if (app->operation.action != IDLE)
        return;
    if (!app->count || !wifi_available(app)) {
        set_notice(app, "No Wi-Fi network selected");
        return;
    }

    const struct Row *row = &app->rows[app->selected];
    if (row->connected) {
        set_notice(app, "Already connected");
        return;
    }
    if (!row->ap->ap.ssid.length) {
        set_notice(app, "Unknown SSID | Press h to enter it");
        return;
    }
    if (row->ap->ap.mode != NM_MODE_INFRA) {
        set_notice(app, "This Wi-Fi network mode is unsupported");
        return;
    }
    enum Security group = security_group(row->ap);
    if (group != OPEN && group != PSK && group != SAE) {
        set_notice(app, group == ENTERPRISE ? "Enterprise Wi-Fi is unsupported" :
                   "This Wi-Fi security mode is unsupported");
        return;
    }
    select_target(app, row);
    lookup_profiles(app);
}

static const struct SavedProfile *selected_profile(const struct App *app)
{
    return app->saved.count ? app->saved.rows[app->saved.selected] : NULL;
}

static void set_profile_target(struct Operation *operation, const struct SavedProfile *profile)
{
    replace_string(&operation->profile, profile->path);
    memcpy(operation->profile_uuid, profile->data.uuid, UUID_SIZE);
    operation->ssid = profile->data.ssid;
}

static void connect_saved_profile(struct App *app)
{
    if (app->operation.action != IDLE)
        return;
    const struct SavedProfile *profile = selected_profile(app);
    if (!profile) {
        set_notice(app, "No saved Wi-Fi profile selected");
        return;
    }
    if (profile->connected) {
        set_notice(app, "This profile is already connected");
        return;
    }

    const struct Profile *data = &profile->data;
    if (data->mode && strcmp(data->mode, "infrastructure") != 0) {
        set_notice(app, "This Wi-Fi network mode is unsupported");
        return;
    }
    enum Security security = configured_security(data);
    if (!personal_security(security)) {
        set_notice(app, security == ENTERPRISE ?
                   "Enterprise Wi-Fi is unsupported" : "This Wi-Fi security mode is unsupported");
        return;
    }
    if (!wifi_available(app)) {
        set_notice(app, "Wi-Fi is unavailable");
        return;
    }

    set_profile_target(&app->operation, profile);
    if (find_profile_target(app, data))
        activate_saved(app);
    else
        finish_operation(app, profile->in_range ? "No wireless interface is ready to connect" :
                         "Saved network is out of range");
}

static bool have_profile_target(const struct App *app)
{
    const struct Operation *operation = &app->operation;
    const struct SavedProfile *profile = find_saved_profile(app, operation->profile);
    return profile && profile->settings && strcmp(operation->profile_uuid, profile->data.uuid) == 0;
}

/* Copy each section and variant verbatim, changing only connection.autoconnect. */
static int copy_autoconnect_setting(sd_bus_message *out, sd_bus_message *source, int autoconnect)
{
    int result = sd_bus_message_open_container(out, 'a', "{sv}");
    if (result < 0 || sd_bus_message_enter_container(source, 'a', "{sv}") <= 0)
        return -EBADMSG;

    while ((result = sd_bus_message_enter_container(source, 'e', "sv")) > 0) {
        const char *key;
        if (sd_bus_message_read(source, "s", &key) <= 0)
            return -EBADMSG;
        if (strcmp(key, "autoconnect") == 0) {
            if (sd_bus_message_skip(source, "v") <= 0)
                return -EBADMSG;
        } else if (sd_bus_message_open_container(out, 'e', "sv") < 0 ||
                   sd_bus_message_append(out, "s", key) < 0 ||
                   sd_bus_message_copy(out, source, false) <= 0 ||
                   sd_bus_message_close_container(out) < 0) {
            return -EBADMSG;
        }
        if (sd_bus_message_exit_container(source) < 0)
            return -EBADMSG;
    }
    if (result < 0 || sd_bus_message_exit_container(source) < 0)
        return -EBADMSG;
    result = sd_bus_message_append(out, "{sv}", "autoconnect", "b", autoconnect);
    return result < 0 ? result : sd_bus_message_close_container(out);
}

static int copy_profile_for_update(sd_bus_message *out, sd_bus_message *source, int autoconnect)
{
    if (sd_bus_message_rewind(source, true) <= 0 ||
        sd_bus_message_enter_container(source, 'a', "{sa{sv}}") <= 0 ||
        sd_bus_message_open_container(out, 'a', "{sa{sv}}") < 0)
        return -EBADMSG;

    int result;
    bool changed = false;
    while ((result = sd_bus_message_enter_container(source, 'e', "sa{sv}")) > 0) {
        const char *section;
        if (sd_bus_message_read(source, "s", &section) <= 0 ||
            sd_bus_message_open_container(out, 'e', "sa{sv}") < 0 ||
            sd_bus_message_append(out, "s", section) < 0)
            return -EBADMSG;

        if (strcmp(section, "connection") == 0) {
            if (changed || copy_autoconnect_setting(out, source, autoconnect) < 0)
                return -EBADMSG;
            changed = true;
        } else if (sd_bus_message_copy(out, source, false) <= 0) {
            return -EBADMSG;
        }

        if (sd_bus_message_exit_container(source) < 0 || sd_bus_message_close_container(out) < 0)
            return -EBADMSG;
    }
    if (!changed || result < 0 || sd_bus_message_exit_container(source) < 0)
        return -EBADMSG;
    return sd_bus_message_close_container(out);
}

static int on_autoconnect_updated(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    struct Operation *operation = &app->operation;
    operation->request = sd_bus_slot_unref(operation->request);
    const sd_bus_error *failure = sd_bus_message_get_error(message);
    request_saved_profile(app, operation->profile);
    if (error_named(failure, "VersionIdMismatch"))
        finish_operation(app, "Profile changed during the update | Try again");
    else if (failure)
        finish_operation(app, method_failure(message, "Could not update auto-connect"));
    else
        finish_operation(app, "Auto-connect updated");
    return 1;
}

static int on_autoconnect_settings(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    struct Operation *operation = &app->operation;
    operation->request = sd_bus_slot_unref(operation->request);

    if (sd_bus_message_is_method_error(message, NULL)) {
        finish_operation(app, method_failure(message, "Could not read the full profile"));
        return 1;
    }
    struct Profile profile = {0};
    sd_bus_message_sensitive(message);
    if (read_profile(message, &profile) <= 0 || !wifi_profile(&profile) ||
        strcmp(profile.uuid, operation->profile_uuid) != 0 || !have_profile_target(app)) {
        finish_operation(app, "The selected profile changed | Try again");
        return 1;
    }

    sd_bus_message *update = NULL;
    int result = new_method(app, &update, operation->profile, NM_PROFILE, "Update2", true);
    if (result >= 0)
        result = sd_bus_message_sensitive(update);
    if (result >= 0)
        result = copy_profile_for_update(update, message, !profile.autoconnect);
    if (result >= 0)
        result = sd_bus_message_append(update, "ua{sv}", (uint32_t)(NM_UPDATE_TO_DISK | NM_UPDATE_NO_REAPPLY),
                                       1, "version-id", "t", operation->profile_version);
    if (result >= 0)
        result = sd_bus_call_async(app->bus, &operation->request, update,
                                   on_autoconnect_updated, app, 60000000);
    sd_bus_message_unref(update);
    if (result < 0)
        finish_operation(app, "Could not send the complete profile update");
    return 1;
}

static int on_autoconnect_version(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    struct Operation *operation = &app->operation;
    operation->request = sd_bus_slot_unref(operation->request);

    if (sd_bus_message_read(message, "v", "t", &operation->profile_version) <= 0 ||
        !operation->profile_version) {
        finish_operation(app, method_failure(message, "NetworkManager did not provide a profile version"));
    } else if (send_method(app, &operation->request, on_autoconnect_settings, app, 5000000,
                           operation->profile, NM_PROFILE, "GetSettings", false) < 0) {
        finish_operation(app, "Could not read the full profile");
    }
    return 1;
}

static void toggle_autoconnect(struct App *app)
{
    if (app->operation.action != IDLE)
        return;
    const struct SavedProfile *profile = selected_profile(app);
    if (!profile) {
        set_notice(app, "No saved Wi-Fi profile selected");
        return;
    }

    struct Operation *operation = &app->operation;
    set_profile_target(operation, profile);
    operation->action = TOGGLE_AUTOCONNECT;
    operation->deadline = monotonic_us() + 72000000;
    app->screen_dirty = true;

    sd_bus_message *message = NULL;
    int result = new_method(app, &message, operation->profile, PROPERTIES, "Get", false);
    if (result >= 0)
        result = sd_bus_message_append(message, "ss", NM_PROFILE, "VersionId");
    if (result >= 0)
        result = sd_bus_call_async(app->bus, &operation->request, message,
                                   on_autoconnect_version, app, 5000000);
    sd_bus_message_unref(message);
    if (result < 0)
        finish_operation(app, "Could not read the profile version");
}

static void forget_saved_profile(struct App *app)
{
    if (app->operation.action != IDLE)
        return;
    const struct SavedProfile *profile = selected_profile(app);
    if (!profile) {
        set_notice(app, "No saved Wi-Fi profile selected");
        return;
    }
    set_profile_target(&app->operation, profile);
    app->operation.action = CONFIRM_FORGET;
    app->operation.forget_warning = profile_may_be_active(app, profile->path);
    app->screen_dirty = true;
}

static int on_profile_deleted(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    struct Operation *operation = &app->operation;
    operation->request = sd_bus_slot_unref(operation->request);
    if (sd_bus_message_is_method_error(message, NULL)) {
        finish_operation(app, method_failure(message, "Could not forget the selected profile"));
    } else {
        remove_saved_profile(app, operation->profile);
        finish_operation(app, "Saved profile forgotten");
    }
    return 1;
}

static void confirm_forget(struct App *app)
{
    struct Operation *operation = &app->operation;
    if (!have_profile_target(app)) {
        finish_operation(app, "The selected profile is no longer available");
        return;
    }
    if (!operation->forget_warning && profile_may_be_active(app, operation->profile)) {
        operation->forget_warning = true;
        operation->forget_prompt_shown = false;
        app->screen_dirty = true;
        return;
    }
    if (!operation->forget_prompt_shown)
        return;
    if (send_method(app, &operation->request, on_profile_deleted, app, 60000000,
                    operation->profile, NM_PROFILE, "Delete", true) < 0) {
        finish_operation(app, "Could not request profile deletion");
    } else {
        operation->action = FORGETTING;
        operation->deadline = monotonic_us() + 62000000;
        app->screen_dirty = true;
    }
}

static const char *password_validation(const struct Operation *operation)
{
    if (operation->password_overflow)
        return "Password is too long";
    size_t length = operation->password_length;
    const char *password = operation->password;
    if (operation->security == SAE) {
        if (!length)
            return "Enter a WPA3 password";
        mbstate_t state = {0};
        while (length) {
            wchar_t character;
            size_t used = mbrtowc(&character, password, length, &state);
            if (used == (size_t)-1 || used == (size_t)-2 || !used || iswcntrl((wint_t)character))
                return "Use a valid UTF-8 password";
            length -= used;
            password += used;
        }
        return NULL;
    }
    if (length == 64 && strspn(password, "0123456789abcdefABCDEF") == length)
        return NULL;
    if (length < 8 || length > 63)
        return "Use 8-63 characters or 64 hex digits";
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)password[i];
        if (byte < 0x20 || byte > 0x7e)
            return "Use ASCII characters for WPA/WPA2";
    }
    return NULL;
}

static void submit_password(struct App *app)
{
    struct Operation *operation = &app->operation;
    operation->password_error = password_validation(operation);
    app->screen_dirty = true;
    if (operation->password_error)
        return;

    operation->password_submitted = true;
    if (!operation->secret_request) {
        /* An external client could have created a profile while the prompt was open. */
        lookup_profiles(app);
        return;
    }

    sd_bus_message *reply = NULL;
    int result = sd_bus_message_new_method_return(operation->secret_request, &reply);
    if (result >= 0)
        result = sd_bus_message_sensitive(reply);
    if (result >= 0)
        result = append_psk(reply, operation->password);
    clear_password(operation);
    if (result >= 0)
        result = sd_bus_send(app->bus, reply, NULL);
    sd_bus_message_unref(reply);
    if (result < 0) {
        finish_operation(app, "Could not provide the Wi-Fi password");
    } else {
        operation->secret_request = sd_bus_message_unref(operation->secret_request);
        operation->deadline = monotonic_us() + 90000000;
    }
}

static void cancel_password(struct App *app)
{
    struct Operation *operation = &app->operation;
    if (!operation->secret_request) {
        finish_operation(app, "Connection canceled");
        return;
    }

    sd_bus_reply_method_errorf(operation->secret_request, NM_AGENT ".UserCanceled",
                               "The user canceled the password prompt");
    operation->secret_request = sd_bus_message_unref(operation->secret_request);
    clear_password(operation);
    operation->canceled = true;
    operation->deadline = monotonic_us() + 12000000;
    app->screen_dirty = true;
}

static void disconnect_selected(struct App *app)
{
    if (app->operation.action != IDLE)
        return;
    if (!app->count || !app->rows[app->selected].connected) {
        set_notice(app, "Select the connected network to disconnect");
        return;
    }

    const struct Row *row = &app->rows[app->selected];
    select_target(app, row);
    replace_string(&app->operation.active, row->device->device.active_connection);
    app->operation.action = CONFIRM_DISCONNECT;
    app->screen_dirty = true;
}

static int on_disconnect_reply(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    app->operation.request = sd_bus_slot_unref(app->operation.request);
    if (sd_bus_message_is_method_error(message, NULL)) {
        finish_operation(app, method_failure(message, "Could not disconnect the wireless device"));
    } else {
        app->operation.action = DISCONNECTING;
        app->screen_dirty = true;
    }
    return 0;
}

static void confirm_disconnect(struct App *app)
{
    struct Operation *operation = &app->operation;
    const struct Object *device = operation_device(app);
    if (!wifi_available(app) || !device || device->device.state != NM_DEVICE_ACTIVATED ||
        !device->device.active || strcmp(device->device.active->path, operation->ap) != 0 ||
        (operation->active && device->device.active_connection &&
         strcmp(operation->active, device->device.active_connection) != 0)) {
        finish_operation(app, "The selected connection changed");
        return;
    }

    if (send_method(app, &operation->request, on_disconnect_reply, app, 35000000,
                    device->path, NM_DEVICE, "Disconnect", true) < 0) {
        finish_operation(app, "Could not send disconnect request");
    } else {
        operation->action = START_DISCONNECT;
        operation->deadline = monotonic_us() + 45000000;
        app->screen_dirty = true;
    }
}

static void refresh_operation(struct App *app, uint64_t now)
{
    struct Operation *operation = &app->operation;
    if (operation->action == IDLE)
        return;

    if (operation->action == CONFIRM_RADIO_OFF)
        return;
    if (operation->action == SET_RADIO || operation->action == WAIT_RADIO) {
        if (operation->action == WAIT_RADIO && operation->radio_enable &&
            !app->manager.manager.hardware_enabled) {
            finish_operation(app, "Wi-Fi is hardware-blocked");
        } else if (operation->action == WAIT_RADIO &&
                   ((app->manager.manager.enabled != 0) == operation->radio_enable)) {
            finish_operation(app, operation->radio_enable ? "Wi-Fi enabled" : "Wi-Fi disabled");
        } else if (now >= operation->deadline) {
            finish_operation(app, "Timed out waiting for the Wi-Fi state change");
        }
        return;
    }
    if (operation->action == HIDDEN_SSID || operation->action == HIDDEN_SECURITY) {
        if (!wifi_available(app))
            finish_operation(app, "Wi-Fi is no longer available");
        return;
    }
    if (operation->action == CONFIRM_FORGET) {
        if (!have_profile_target(app)) {
            finish_operation(app, "The selected profile is no longer available");
        } else if (!operation->forget_warning && profile_may_be_active(app, operation->profile)) {
            operation->forget_warning = true;
            operation->forget_prompt_shown = false;
            app->screen_dirty = true;
        }
        return;
    }
    if (operation->action == TOGGLE_AUTOCONNECT || operation->action == FORGETTING) {
        if (now >= operation->deadline)
            finish_operation(app, "Timed out waiting for NetworkManager");
        return;
    }

    const struct Object *device = operation_device(app);
    bool same_activation = device && operation->active && device->device.active_connection &&
                           strcmp(operation->active, device->device.active_connection) == 0;
    if (operation->action == DISCONNECTING &&
        (!device || device->device.state <= NM_DEVICE_DISCONNECTED ||
         (operation->active && device->device.active_connection &&
          strcmp(operation->active, device->device.active_connection) != 0))) {
        finish_operation(app, "Disconnected");
    } else if (!device || !wifi_available(app)) {
        finish_operation(app, "Wireless interface is no longer available");
    } else if (operation->action == ENTER_PASSWORD && operation->ap &&
               !target_ap(operation, find_child(device, operation->ap))) {
        finish_operation(app, "Access point is no longer available");
    } else if (operation->action == ACTIVATING && same_activation &&
               device->device.state == NM_DEVICE_FAILED) {
        finish_operation(app, activation_failure(app, 0));
    } else if (operation->action == ACTIVATING && same_activation &&
               device->device.state == NM_DEVICE_ACTIVATED && device->device.active &&
               compare_ssids(&device->device.active->ap.ssid, &operation->ssid) == 0) {
        finish_operation(app, "Connected");
    } else if (operation->deadline && now >= operation->deadline) {
        const char *notice = "Timed out waiting for NetworkManager";
        if (operation->canceled)
            notice = "Connection canceled";
        else if (password_prompt(app))
            notice = "Wi-Fi password prompt timed out";
        else if (operation->action == ACTIVATING)
            notice = "Timed out waiting for the connection";
        finish_operation(app, notice);
    }
}

static const char *operation_status(const struct App *app)
{
    const struct Operation *operation = &app->operation;
    switch (operation->action) {
    case SET_RADIO:
    case WAIT_RADIO: return operation->radio_enable ? "Enabling Wi-Fi..." : "Disabling Wi-Fi...";
    case TOGGLE_AUTOCONNECT: return "Updating auto-connect...";
    case FORGETTING: return "Forgetting saved profile...";
    case LOOKUP: return "Checking saved profiles...";
    case REGISTER_AGENT:
    case START_CONNECTION: return operation->canceled ? "Canceling connection..." : "Connecting...";
    case ACTIVATING: {
        if (operation->canceled)
            return "Canceling connection...";
        const struct Object *device = operation_device(app);
        if (device && operation->active && device->device.active_connection &&
            strcmp(operation->active, device->device.active_connection) == 0) {
            if (device->device.state == NM_DEVICE_NEED_AUTH)
                return "Authenticating...";
            if (device->device.state >= NM_DEVICE_IP_CONFIG &&
                device->device.state <= NM_DEVICE_SECONDARIES)
                return "Obtaining IP address...";
        }
        return "Connecting...";
    }
    case START_DISCONNECT:
    case DISCONNECTING: return "Disconnecting...";
    default: return app->notice ? app->notice : app->scan_notice;
    }
}

/* Network names are byte strings. Only emit printable, positive-width characters. */
static unsigned fit_text_part(char *out, const char *input, size_t length, unsigned limit,
                              size_t *used)
{
    const char *start = input;
    unsigned columns = 0;
    mbstate_t state = {0};

    while (length && columns < limit) {
        wchar_t character;
        size_t consumed = mbrtowc(&character, input, length, &state);
        if (consumed == (size_t)-1 || consumed == (size_t)-2 || consumed == 0) {
            state = (mbstate_t){0};
            consumed = 1;
            character = L'\0';
        }
        int width = iswprint((wint_t)character) ? wcwidth(character) : -1;
        for (size_t i = 0; i < consumed; i++) {
            unsigned char byte = (unsigned char)input[i];
            if (byte < 0x20 || byte == 0x7f || (consumed == 1 && byte >= 0x80 && byte <= 0x9f))
                width = -1;
        }
        if (width <= 0) {
            *out++ = '?';
            columns++;
        } else {
            if ((unsigned)width > limit - columns)
                break;
            memcpy(out, input, consumed);
            out += consumed;
            columns += (unsigned)width;
        }
        input += consumed;
        length -= consumed;
    }

    *out = '\0';
    if (used)
        *used = (size_t)(input - start);
    return columns;
}

static unsigned fit_text(char *out, const char *input, size_t length, unsigned limit)
{
    return fit_text_part(out, input, length, limit, NULL);
}

static void add_ip_line(struct IpDetails *ip, const char *label, const char *value)
{
    if (ip->count == ip->capacity) {
        ip->capacity = ip->capacity ? ip->capacity * 2 : 8;
        ip->lines = allocate(ip->lines, ip->capacity, sizeof *ip->lines);
    }
    struct DetailLine *line = &ip->lines[ip->count++];
    line->label = label;
    snprintf(line->value, sizeof line->value, "%s", value);
}

static bool valid_ip(const struct IpDetails *ip, const char *address)
{
    unsigned char raw[sizeof(struct in6_addr)];
    return address && inet_pton(ip->ipv6 ? AF_INET6 : AF_INET, address, raw) == 1;
}

static int read_ip_entries(struct IpDetails *ip, sd_bus_message *message, bool dns)
{
    int result;
    if (sd_bus_message_enter_container(message, 'a', "a{sv}") <= 0)
        return -EBADMSG;
    while ((result = sd_bus_message_enter_container(message, 'a', "{sv}")) > 0) {
        const char *address = NULL, *uri = NULL;
        uint32_t prefix = UINT32_MAX;
        while ((result = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
            const char *key;
            if (sd_bus_message_read(message, "s", &key) <= 0)
                return -EBADMSG;
            if (strcmp(key, "address") == 0)
                result = sd_bus_message_read(message, "v", "s", &address);
            else if (dns && strcmp(key, "uri") == 0)
                result = sd_bus_message_read(message, "v", "s", &uri);
            else if (!dns && strcmp(key, "prefix") == 0)
                result = sd_bus_message_read(message, "v", "u", &prefix);
            else
                result = sd_bus_message_skip(message, "v");
            if (result <= 0 || sd_bus_message_exit_container(message) < 0)
                return -EBADMSG;
        }
        if (result < 0 || sd_bus_message_exit_container(message) < 0)
            return -EBADMSG;
        if (dns) {
            if (valid_ip(ip, address))
                add_ip_line(ip, "IPv4 DNS", address);
            else if (uri && !strchr(uri, '@') &&
                     (strncmp(uri, "dns+tls://", 10) == 0 || strncmp(uri, "dns+udp://", 10) == 0))
                add_ip_line(ip, "IPv4 DNS", uri);
        } else if (valid_ip(ip, address) && prefix <= (ip->ipv6 ? 128U : 32U)) {
            char value[INET6_ADDRSTRLEN + 5];
            snprintf(value, sizeof value, "%s/%u", address, prefix);
            add_ip_line(ip, ip->ipv6 ? "IPv6 address" : "IPv4 address", value);
        }
    }
    return result < 0 ? result : sd_bus_message_exit_container(message);
}

static int read_ipv6_dns(struct IpDetails *ip, sd_bus_message *message)
{
    int result = sd_bus_message_enter_container(message, 'a', "ay");
    if (result <= 0)
        return -EBADMSG;
    const void *address;
    size_t size;
    while ((result = sd_bus_message_read_array(message, 'y', &address, &size)) > 0) {
        char value[INET6_ADDRSTRLEN];
        if (size == sizeof(struct in6_addr) && inet_ntop(AF_INET6, address, value, sizeof value))
            add_ip_line(ip, "IPv6 DNS", value);
    }
    return result < 0 ? result : sd_bus_message_exit_container(message);
}

static int read_ip_details(struct IpDetails *ip, sd_bus_message *message)
{
    if (sd_bus_message_enter_container(message, 'a', "{sv}") <= 0)
        return -EBADMSG;
    int result;
    while ((result = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        const char *key;
        if (sd_bus_message_read(message, "s", &key) <= 0)
            return -EBADMSG;

        bool dns_data = !ip->ipv6 && strcmp(key, "NameserverData") == 0;
        if (dns_data || strcmp(key, "AddressData") == 0) {
            if (sd_bus_message_enter_container(message, 'v', "aa{sv}") <= 0 ||
                read_ip_entries(ip, message, dns_data) < 0)
                return -EBADMSG;
            result = sd_bus_message_exit_container(message);
        } else if (ip->ipv6 && strcmp(key, "Nameservers") == 0) {
            if (sd_bus_message_enter_container(message, 'v', "aay") <= 0 ||
                read_ipv6_dns(ip, message) < 0)
                return -EBADMSG;
            result = sd_bus_message_exit_container(message);
        } else if (strcmp(key, "Gateway") == 0) {
            const char *gateway;
            result = sd_bus_message_read(message, "v", "s", &gateway);
            if (result > 0 && valid_ip(ip, gateway))
                add_ip_line(ip, ip->ipv6 ? "IPv6 gateway" : "IPv4 gateway", gateway);
        } else {
            result = sd_bus_message_skip(message, "v");
        }
        if (result <= 0 || sd_bus_message_exit_container(message) < 0)
            return -EBADMSG;
    }
    return result < 0 ? result : sd_bus_message_exit_container(message);
}

static void request_ip_details(struct IpDetails *ip);

static int on_ip_details(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct IpDetails *ip = userdata;
    ip->request = sd_bus_slot_unref(ip->request);
    ip->count = 0;
    ip->failed = sd_bus_message_is_method_error(message, NULL) ||
                 read_ip_details(ip, message) <= 0;
    if (ip->failed)
        ip->count = 0;
    if (ip->refresh)
        request_ip_details(ip);
    ip->app->screen_dirty = true;
    return 1;
}

static void request_ip_details(struct IpDetails *ip)
{
    if (ip->request) {
        ip->refresh = true;
        return;
    }
    ip->refresh = false;
    struct App *app = ip->app;
    sd_bus_message *message = NULL;
    int result = new_method(app, &message, ip->path, PROPERTIES, "GetAll", false);
    if (result >= 0)
        result = sd_bus_message_append(message, "s", ip->ipv6 ? NM_IP6 : NM_IP4);
    if (result >= 0)
        result = sd_bus_call_async(app->bus, &ip->request, message, on_ip_details, ip, 5000000);
    sd_bus_message_unref(message);
    ip->failed = result < 0;
}

static void request_detail_links(struct App *app);

static bool ip_details_changed(struct App *app, const char *path, const char *interface)
{
    if (app->details.active_path && strcmp(path, app->details.active_path) == 0 &&
        strcmp(interface, NM_ACTIVE) == 0) {
        request_detail_links(app);
        app->screen_dirty = true;
        return true;
    }
    for (size_t i = 0; i < 2; i++) {
        struct IpDetails *ip = &app->details.ip[i];
        if (ip->path && strcmp(path, ip->path) == 0 &&
            strcmp(interface, ip->ipv6 ? NM_IP6 : NM_IP4) == 0) {
            request_ip_details(ip);
            app->screen_dirty = true;
            return true;
        }
    }
    return false;
}

static void sync_detail_ip(struct IpDetails *ip, const char *path)
{
    if (path && strcmp(path, "/") == 0)
        path = NULL;
    if (path && ip->path && strcmp(path, ip->path) == 0)
        return;
    if (!path && !ip->path)
        return;

    ip->request = sd_bus_slot_unref(ip->request);
    replace_string(&ip->path, path);
    ip->count = 0;
    ip->refresh = ip->failed = false;
    if (path)
        request_ip_details(ip);
}

static int read_detail_links(sd_bus_message *message, const char *paths[2])
{
    if (sd_bus_message_enter_container(message, 'a', "{sv}") <= 0)
        return -EBADMSG;
    int result;
    while ((result = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        const char *key;
        if (sd_bus_message_read(message, "s", &key) <= 0)
            return -EBADMSG;
        if (strcmp(key, "Ip4Config") == 0)
            result = sd_bus_message_read(message, "v", "o", &paths[0]);
        else if (strcmp(key, "Ip6Config") == 0)
            result = sd_bus_message_read(message, "v", "o", &paths[1]);
        else
            result = sd_bus_message_skip(message, "v");
        if (result <= 0 || sd_bus_message_exit_container(message) < 0)
            return -EBADMSG;
    }
    return result < 0 ? result : sd_bus_message_exit_container(message);
}

static int on_detail_links(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    struct Details *details = &app->details;
    details->active_request = sd_bus_slot_unref(details->active_request);
    const char *paths[2] = {0};
    details->active_failed = sd_bus_message_is_method_error(message, NULL) ||
                             read_detail_links(message, paths) <= 0;
    for (size_t i = 0; i < 2; i++)
        sync_detail_ip(&details->ip[i], details->active_failed ? NULL : paths[i]);
    if (details->active_refresh)
        request_detail_links(app);
    app->screen_dirty = true;
    return 1;
}

static void request_detail_links(struct App *app)
{
    struct Details *details = &app->details;
    if (details->active_request) {
        details->active_refresh = true;
        return;
    }
    details->active_refresh = false;
    sd_bus_message *message = NULL;
    int result = new_method(app, &message, details->active_path, PROPERTIES, "GetAll", false);
    if (result >= 0)
        result = sd_bus_message_append(message, "s", NM_ACTIVE);
    if (result >= 0)
        result = sd_bus_call_async(app->bus, &details->active_request, message, on_detail_links, app, 5000000);
    sd_bus_message_unref(message);
    details->active_failed = result < 0;
}

static void sync_detail_links(struct App *app, const char *path)
{
    struct Details *details = &app->details;
    if (path && strcmp(path, "/") == 0)
        path = NULL;
    if ((!path && !details->active_path) ||
        (path && details->active_path && strcmp(path, details->active_path) == 0))
        return;

    details->active_request = sd_bus_slot_unref(details->active_request);
    replace_string(&details->active_path, path);
    details->active_refresh = details->active_failed = false;
    for (size_t i = 0; i < 2; i++)
        sync_detail_ip(&details->ip[i], NULL);
    if (path)
        request_detail_links(app);
}

static int on_detail_settings(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    struct App *app = userdata;
    struct Details *details = &app->details;
    details->settings_request = sd_bus_slot_unref(details->settings_request);
    struct Profile profile = {0};
    sd_bus_message_sensitive(message);
    if (!sd_bus_message_is_method_error(message, NULL) &&
        read_profile(message, &profile) > 0 && wifi_profile(&profile)) {
        details->active_autoconnect = profile.autoconnect;
        details->active_autoconnect_known = true;
    }
    app->screen_dirty = true;
    return 1;
}

static void sync_detail_settings(struct App *app, const char *path)
{
    struct Details *details = &app->details;
    if (path && details->active_settings && strcmp(path, details->active_settings) == 0)
        return;
    details->settings_request = sd_bus_slot_unref(details->settings_request);
    replace_string(&details->active_settings, path);
    details->active_autoconnect_known = false;
    if (path)
        send_method(app, &details->settings_request, on_detail_settings, app, 5000000,
                    path, NM_PROFILE, "GetSettings", false);
}

static void close_details(struct App *app)
{
    struct Details *details = &app->details;
    if (!details->open)
        return;
    for (size_t i = 0; i < 2; i++) {
        sd_bus_slot_unref(details->ip[i].request);
        free(details->ip[i].path);
        free(details->ip[i].lines);
    }
    sd_bus_slot_unref(details->settings_request);
    sd_bus_slot_unref(details->active_request);
    free(details->active_path);
    free(details->active_settings);
    free(details->device);
    free(details->ap);
    free(details->profile);
    *details = (struct Details){0};
    app->screen_dirty = true;
}

static void open_details(struct App *app)
{
    if (app->operation.action != IDLE)
        return;
    struct Details *details = &app->details;
    if (app->show_saved) {
        const struct SavedProfile *profile = selected_profile(app);
        if (!profile) {
            set_notice(app, "No saved Wi-Fi profile selected");
            return;
        }
        details->ssid = profile->data.ssid;
        replace_string(&details->profile, profile->path);
        memcpy(details->profile_uuid, profile->data.uuid, UUID_SIZE);
    } else {
        if (!app->count) {
            set_notice(app, "No Wi-Fi network selected");
            return;
        }
        const struct Row *row = &app->rows[app->selected];
        details->ssid = row->ap->ap.ssid;
        replace_string(&details->device, row->device->path);
        replace_string(&details->ap, row->ap->path);
    }
    details->open = true;
    for (size_t i = 0; i < 2; i++)
        details->ip[i] = (struct IpDetails){ .app = app, .ipv6 = i == 1 };
    app->screen_dirty = true;
}

static const char *configured_security_label(enum Security security)
{
    switch (security) {
    case OPEN: return "Open";
    case PSK: return "WPA/WPA2 Personal";
    case SAE: return "WPA3 Personal";
    case ENTERPRISE: return "Enterprise";
    case OWE: return "OWE";
    case WEP: return "WEP";
    default: return "Unknown";
    }
}

static void add_detail(struct Details *details, const char *label, const char *value)
{
    if (!value || !*value || details->basic_count >= sizeof details->basic / sizeof details->basic[0])
        return;
    struct DetailLine *line = &details->basic[details->basic_count++];
    line->label = label;
    snprintf(line->value, sizeof line->value, "%s", value);
}

static const struct Object *saved_detail_device(const struct App *app, const struct SavedProfile *profile,
                                                 const struct Object **ap, bool *connected)
{
    const struct Object *best = NULL;
    for (const struct Object *device = app->manager.children; device; device = device->next) {
        if (device->device.type != NM_DEVICE_WIFI)
            continue;
        if (device->device.state == NM_DEVICE_ACTIVATED && device->device.active_profile &&
            strcmp(profile->path, device->device.active_profile) == 0) {
            *connected = true;
            *ap = device->device.active && device->device.active->props.ready ? device->device.active : NULL;
            return device;
        }
        if (!wifi_available(app))
            continue;
        for (const struct Object *candidate = device->children; candidate; candidate = candidate->next)
            if (candidate->listed && candidate->props.ready &&
                match_profile(&profile->data, device, candidate) != UNKNOWN_SECURITY &&
                (!*ap || candidate->ap.strength > (*ap)->ap.strength)) {
                *ap = candidate;
                best = device;
            }
    }
    return best;
}

static const char *detail_autoconnect(const struct App *app, const struct Object *device,
                                      const struct Object *ap, bool connected)
{
    if (connected && device->device.active_profile) {
        const struct SavedProfile *profile = find_saved_profile(app, device->device.active_profile);
        if (profile && profile->settings)
            return profile->data.autoconnect ? "On" : "Off";
        if (app->details.active_autoconnect_known)
            return app->details.active_autoconnect ? "On" : "Off";
        return NULL;
    }
    if (!ap)
        return NULL;
    bool on = false, off = false;
    for (size_t i = 0; i < app->saved.count; i++) {
        const struct Profile *profile = &app->saved.rows[i]->data;
        if (match_profile(profile, device, ap) == UNKNOWN_SECURITY)
            continue;
        if (profile->autoconnect)
            on = true;
        else
            off = true;
    }
    return on && off ? "Varies by profile" : on ? "On" : off ? "Off" : NULL;
}

static void update_details(struct App *app)
{
    struct Details *details = &app->details;
    const struct SavedProfile *profile = NULL;
    const struct Object *device = NULL, *ap = NULL;
    bool connected = false;

    if (details->profile) {
        profile = find_saved_profile(app, details->profile);
        if (profile && profile->settings && strcmp(profile->data.uuid, details->profile_uuid) == 0)
            device = saved_detail_device(app, profile, &ap, &connected);
        else
            profile = NULL;
    } else if (wifi_available(app)) {
        device = find_child(&app->manager, details->device);
        if (device) {
            ap = find_child(device, details->ap);
            if (ap && !ap->props.ready)
                ap = NULL;
            connected = ap && ap == device->device.active && device->device.state == NM_DEVICE_ACTIVATED;
        }
    }

    sync_detail_links(app, connected ? device->device.active_connection : NULL);
    const struct SavedProfile *active = connected && device->device.active_profile ?
                                       find_saved_profile(app, device->device.active_profile) : NULL;
    sync_detail_settings(app, connected && !profile && (!active || !active->settings) ?
                         device->device.active_profile : NULL);

    details->basic_count = 0;
    const struct Ssid *ssid = profile ? &profile->data.ssid :
                             ap && ap->ap.ssid.length ? &ap->ap.ssid : &details->ssid;
    char value[128];
    if (ssid->length)
        fit_text(value, (const char *)ssid->bytes, ssid->length, SSID_MAX);
    else
        strcpy(value, "(hidden)");
    add_detail(details, "SSID", value);
    add_detail(details, "Status", connected ? "Connected" :
               details->profile && !profile ? "Saved profile is no longer available" :
               !wifi_available(app) ? "Wi-Fi unavailable" :
               ap ? "Disconnected" : "Out of range");
    add_detail(details, "Interface", device ? device->device.name : profile ? profile->data.interface : NULL);
    const char *security = ap ? security_label(ap) :
                           profile ? configured_security_label(configured_security(&profile->data)) : NULL;
    add_detail(details, "Security", security);
    if (ap) {
        add_detail(details, "BSSID", ap->ap.bssid);
        snprintf(value, sizeof value, "%u%%", (unsigned)ap->ap.strength);
        add_detail(details, "Signal", value);
        if (ap->ap.frequency) {
            const char *band;
            uint32_t channel = wifi_channel(ap->ap.frequency, &band);
            snprintf(value, sizeof value, "%u MHz", ap->ap.frequency);
            if (channel)
                snprintf(value, sizeof value, "%u MHz (channel %u)", ap->ap.frequency, channel);
            add_detail(details, "Frequency", value);
        }
    } else if (profile && profile->data.bssid_length == sizeof(struct ether_addr)) {
        struct ether_addr address;
        memcpy(&address, profile->data.bssid, sizeof address);
        snprintf(value, sizeof value, "%02x:%02x:%02x:%02x:%02x:%02x",
                 (unsigned)address.ether_addr_octet[0], (unsigned)address.ether_addr_octet[1],
                 (unsigned)address.ether_addr_octet[2], (unsigned)address.ether_addr_octet[3],
                 (unsigned)address.ether_addr_octet[4], (unsigned)address.ether_addr_octet[5]);
        add_detail(details, "BSSID", value);
    }
    if (connected && device->device.bitrate) {
        snprintf(value, sizeof value, "%.1f Mbit/s", device->device.bitrate / 1000.0);
        add_detail(details, "Link speed", value);
    }
    add_detail(details, "Auto-connect", profile ? (profile->data.autoconnect ? "On" : "Off") :
               detail_autoconnect(app, device, ap, connected));
}

static const char *wifi_status(const struct App *app, char *text, size_t size)
{
    const struct Object *manager = &app->manager;
    if (!app->bus)
        return "System D-Bus unavailable";
    if (!manager->props.ready)
        return app->matches_pending || manager->props.request ?
               "Reading NetworkManager..." : "NetworkManager unavailable";

    size_t interfaces_found = 0;
    const char *name = NULL;
    bool loading = false;
    for (const struct Object *device = manager->children; device; device = device->next) {
        loading |= device->props.request != NULL;
        if (device->device.type == NM_DEVICE_WIFI) {
            interfaces_found++;
            name = device->device.name;
        }
    }

    if (!manager->manager.hardware_enabled)
        return "Wi-Fi hardware-blocked | Unblock the wireless switch";
    if (!manager->manager.enabled)
        return "Wi-Fi off | Press w to enable";
    if (!interfaces_found && !loading)
        return "Wi-Fi on | No wireless interfaces";
    if (!interfaces_found)
        return "Wi-Fi on | Reading interfaces...";

    const char *state = app->count && app->rows[0].connected ? "connected" : "disconnected";
    if (interfaces_found == 1 && name && *name)
        snprintf(text, size, "Wi-Fi on | %.48s | %s", name, state);
    else
        snprintf(text, size, "Wi-Fi on | %zu interfaces | %s", interfaces_found, state);
    return text;
}

static const char *empty_list(const struct App *app)
{
    if (!app->manager.props.ready || !app->manager.manager.enabled ||
        !app->manager.manager.hardware_enabled)
        return "";

    bool has_wifi = false;
    for (const struct Object *device = app->manager.children; device; device = device->next) {
        if (device->device.type != NM_DEVICE_WIFI || device->device.mode == NM_MODE_AP)
            continue;
        has_wifi = true;
        if (device->device.wireless.request)
            return "Reading networks...";
        for (const struct Object *ap = device->children; ap; ap = ap->next)
            if (ap->props.request)
                return "Reading networks...";
    }

    return has_wifi ? "No networks in view" : "";
}

static void paint_line(struct App *app, unsigned row, const char *style, const char *value)
{
    char line[sizeof app->lines[0].text];
    const char *margin = app->width >= 40 ? "  " : "";
    snprintf(line, sizeof line, "%s\033[K%s%s", style, margin, value);
    char *previous = app->lines[row - 1].text;
    if (strcmp(previous, line) != 0) {
        fprintf(stdout, "\033[%u;1H%s", row, line);
        strcpy(previous, line);
    }
}

static size_t layout_detail_line(struct App *app, const struct DetailLine *line, unsigned width,
                                  bool paint, size_t visible, size_t index)
{
    char text[160], fitted[sizeof text];
    snprintf(text, sizeof text, "%-*s %s", width >= 32 ? 14 : 0, line->label, line->value);
    const char *input = text;
    size_t length = strlen(text);
    size_t top = app->details.top;
    while (length) {
        size_t used;
        fit_text_part(fitted, input, length, width, &used);
        if (!used) {
            strcpy(fitted, "?");
            mbstate_t state = {0};
            used = mbrlen(input, length, &state);
            if (!used || used > length)
                used = 1;
        }
        if (paint && index >= top && index - top < visible)
            paint_line(app, (unsigned)(index - top) + 2, app->theme.normal, fitted);
        input += used;
        length -= used;
        index++;
    }
    return index;
}

static size_t layout_details(struct App *app, unsigned width, bool paint, size_t visible)
{
    struct Details *details = &app->details;
    size_t index = 0;
    for (size_t i = 0; i < details->basic_count; i++)
        index = layout_detail_line(app, &details->basic[i], width, paint, visible, index);
    if (details->active_failed ||
        (details->active_request && !details->ip[0].path && !details->ip[1].path)) {
        struct DetailLine status = { .label = "IP" };
        snprintf(status.value, sizeof status.value, "%s",
                 details->active_failed ? "Configuration unavailable" : "Reading configuration...");
        index = layout_detail_line(app, &status, width, paint, visible, index);
    }
    for (size_t i = 0; i < 2; i++) {
        const struct IpDetails *ip = &details->ip[i];
        if ((ip->request && !ip->count) || ip->failed) {
            struct DetailLine status = { .label = ip->ipv6 ? "IPv6" : "IPv4" };
            snprintf(status.value, sizeof status.value, "%s", ip->failed ? "Configuration unavailable" : "Reading configuration...");
            index = layout_detail_line(app, &status, width, paint, visible, index);
        }
        for (size_t j = 0; j < ip->count; j++)
            index = layout_detail_line(app, &ip->lines[j], width, paint, visible, index);
    }
    return index;
}

static int render_details(struct App *app, unsigned width)
{
    update_details(app);
    size_t visible = app->height > 2 ? app->height - 2 : 0;
    size_t count = layout_details(app, width, false, visible);
    struct Details *details = &app->details;
    details->last_top = count > visible ? count - visible : 0;
    if (details->top > details->last_top)
        details->top = details->last_top;

    char fitted[64];
    const char *heading = "Connection details";
    fit_text(fitted, heading, strlen(heading), width);
    if (app->height > 1)
        paint_line(app, 1, app->theme.highlight, fitted);
    if (visible)
        layout_details(app, width, true, visible);
    for (size_t i = count - details->top; i < visible; i++)
        paint_line(app, (unsigned)i + 2, app->theme.normal, "");

    const char *hints = width >= 44 ? "j/k Scroll  g/G First/last  Esc Back" : "j/k Scroll  Esc Back";
    fit_text(fitted, hints, strlen(hints), width);
    paint_line(app, app->height, app->theme.muted, fitted);
    app->screen_dirty = false;
    return fflush(stdout) == EOF || ferror(stdout) ? -1 : 0;
}

static void network_line(char *text, unsigned width, const struct Row *row)
{
    unsigned saved_width = width >= 26 ? 7 : 0;
    unsigned link_width = width >= 37 ? 11 : 0;
    unsigned signal_width = width >= 14 ? 8 : 0;
    unsigned security_width = width >= 53 ? 17 : 0;
    unsigned ssid_width = width - link_width - signal_width - security_width - saved_width;
    if (ssid_width > 34 && signal_width)
        ssid_width = 34;
    const char *prefix = !link_width && width >= 9 && row && row->connected ? "Up " : "";
    unsigned prefix_width = (unsigned)strlen(prefix);
    const char *label = "SSID";
    size_t length = strlen(label);

    if (row) {
        const struct Ssid *ssid = &row->ap->ap.ssid;
        label = ssid->length ? (const char *)ssid->bytes : "(hidden)";
        length = ssid->length ? ssid->length : strlen(label);
    }

    char ssid[SSID_MAX + 1];
    unsigned columns = fit_text(ssid, label, length, ssid_width - prefix_width) + prefix_width;
    text += sprintf(text, "%s%s", prefix, ssid);

    if (signal_width) {
        char signal[8] = "Signal";
        if (row)
            snprintf(signal, sizeof signal, "%u%%", (unsigned)row->ap->ap.strength);
        text += sprintf(text, "%*s  %-6s", (int)(ssid_width - columns), "", signal);
    }
    if (security_width)
        text += sprintf(text, "  %-15s", row ? security_label(row->ap) : "Security");
    if (link_width)
        text += sprintf(text, "  %-9s", row ? (row->connected ? "Connected" : "") : "State");
    if (saved_width)
        sprintf(text, "  %s", row ? (row->saved ? "Yes" : "") : "Saved");
}

static void saved_line(char *text, unsigned width, const struct SavedProfile *profile)
{
    unsigned auto_width = width >= 15 ? 6 : 0;
    unsigned state_width = width >= 27 ? 11 : 0;
    unsigned range_width = width >= 36 ? 7 : 0;
    unsigned id_width = width >= 68 ? 21 : 0;
    unsigned ssid_width = width - auto_width - state_width - range_width - id_width;
    if (ssid_width > 34 && auto_width)
        ssid_width = 34;

    char name[SSID_MAX + 1], short_id[11] = "";
    const char *label = profile ? "(hidden)" : "SSID";
    size_t length = strlen(label);
    if (profile && profile->data.ssid.length) {
        label = (const char *)profile->data.ssid.bytes;
        length = profile->data.ssid.length;
    }
    if (profile && profile->duplicate && !id_width && ssid_width >= 15)
        snprintf(short_id, sizeof short_id, " [%.7s]", profile->data.uuid);

    unsigned suffix = (unsigned)strlen(short_id);
    unsigned columns = fit_text(name, label, length, ssid_width - suffix) + suffix;
    text += sprintf(text, "%s%s", name, short_id);
    if (auto_width)
        text += sprintf(text, "%*s  %-4s", (int)(ssid_width - columns), "",
                        profile ? (profile->data.autoconnect ? "On" : "Off") : "Auto");
    if (state_width)
        text += sprintf(text, "  %-9s", profile ? (profile->connected ? "Connected" : "Inactive") : "State");
    if (range_width)
        text += sprintf(text, "  %-5s", profile ? (profile->in_range ? "Yes" : "No") : "Range");
    if (id_width) {
        const char *id = profile ? profile->data.id : "Profile";
        unsigned limit = profile && profile->duplicate ? 9 : id_width - 2;
        char fitted[4 * 19 + 1];
        if (!id)
            id = "";
        fit_text(fitted, id, strnlen(id, sizeof fitted - 1), limit);
        if (profile && profile->duplicate)
            sprintf(text, "  %s [%.7s]", fitted, profile->data.uuid);
        else
            sprintf(text, "  %s", fitted);
    }
}

static void target_name(const struct App *app, char *name, unsigned limit)
{
    const struct Ssid *ssid = &app->operation.ssid;
    if (ssid->length)
        fit_text(name, (const char *)ssid->bytes, ssid->length, limit);
    else
        strcpy(name, "(hidden)");
}

static const char *footer_message(const struct App *app, char *text, size_t size, unsigned width)
{
    const struct Operation *operation = &app->operation;
    char name[SSID_MAX + 1];
    if (operation->action == HIDDEN_SSID || operation->action == HIDDEN_SECURITY) {
        fit_text(name, (const char *)operation->ssid.bytes, operation->ssid.length, SSID_MAX);
        snprintf(text, size, "%s%s", operation->action == HIDDEN_SSID ? "Hidden SSID: " : "Security for ", name);
        return text;
    }
    if (password_prompt(app)) {
        size_t count = 0;
        for (size_t i = 0; i < operation->password_length; i++)
            if (((unsigned char)operation->password[i] & 0xc0) != 0x80)
                count++;
        char mask[37];
        size_t visible = count < sizeof mask - 1 ? count : sizeof mask - 1;
        memset(mask, '*', visible);
        mask[visible] = '\0';
        if (width >= 42) {
            unsigned limit = width >= 61 ? 23 : 12;
            target_name(app, name, limit);
            snprintf(text, size, "Password for %s: %s", name, mask);
        } else {
            snprintf(text, size, "Password: %s", mask);
        }
        return text;
    }
    if (operation->action == CONFIRM_DISCONNECT) {
        target_name(app, name, 30);
        snprintf(text, size, "Disconnect from %s?", name);
        return text;
    }
    if (operation->action == CONFIRM_RADIO_OFF)
        return width >= 48 ? "Disable Wi-Fi and disconnect? [y/N]" : "Disable Wi-Fi? [y/N]";
    if (operation->action == CONFIRM_FORGET) {
        const char *warning = operation->forget_warning ? "May disconnect | " : "";
        const struct SavedProfile *profile = find_saved_profile(app, operation->profile);
        bool duplicate = profile && profile->duplicate && width >= 40;
        unsigned overhead = (unsigned)strlen(warning) + (duplicate ? 27 : 17);
        if (width <= overhead + 3) {
            snprintf(text, size, "%sForget? [y/N]", warning);
        } else {
            target_name(app, name, width - overhead);
            if (duplicate)
                snprintf(text, size, "%sForget \"%s\" [%.7s]? [y/N]", warning, name, operation->profile_uuid);
            else
                snprintf(text, size, "%sForget \"%s\"? [y/N]", warning, name);
        }
        return text;
    }
    const char *status = operation_status(app);
    if (!status && !app->show_saved && width < 26 && app->count && app->rows[app->selected].saved)
        return "Saved profile available";
    return status ? status : "";
}

static const char *shortcut_hints(const struct App *app, char *text, size_t size, unsigned width)
{
    if (app->operation.action == HIDDEN_SSID) {
        if (app->operation.ssid_error) {
            snprintf(text, size, "%s  |  Enter Next  Esc Cancel", app->operation.ssid_error);
            return text;
        }
        return "Enter Next   Esc Cancel";
    }
    if (app->operation.action == HIDDEN_SECURITY)
        return width >= 49 ? "1 Open   2 WPA/WPA2   3 WPA3   Esc Cancel" : "1 Open  2 WPA/2  3 WPA3";
    if (password_prompt(app)) {
        if (app->operation.password_error) {
            snprintf(text, size, "%s  |  Enter Submit  Esc Cancel", app->operation.password_error);
            return text;
        }
        return "Enter Submit   Esc Cancel";
    }
    if (app->operation.action == CONFIRM_DISCONNECT)
        return "y Disconnect   n/Esc Cancel";
    if (app->operation.action == CONFIRM_RADIO_OFF)
        return "y Disable   n/Esc Cancel";
    if (app->operation.action == CONFIRM_FORGET)
        return "y Forget   n/Esc Cancel";
    if (app->show_saved) {
        if (width >= 77)
            return "Tab Available  j/k  Enter  a Auto  f Forget  w Wi-Fi  h Hidden  i Info  q Quit";
        if (width >= 54)
            return "Tab  Enter  a Auto  f Forget  w Wi-Fi  h Hidden  i  q";
        return "Tab Enter a f w h i q";
    }
    if (width >= 77)
        return "Tab Saved  j/k  Enter Join  r Scan  d Leave  w Wi-Fi  h Hidden  i Info  q Quit";
    if (width >= 54)
        return "Tab  Enter  r Scan  d Leave  w Wi-Fi  h Hidden  i  q";
    return "Tab Enter r d w h i q";
}

static int render(struct App *app)
{
    if (app->clear_screen) {
        fputs(app->theme.normal, stdout);
        fputs("\033[2J", stdout);
        app->clear_screen = false;
    }

    unsigned width = app->width >= 40 ? app->width - 2 : app->width;
    if (width > 80)
        width = 80;
    if (app->details.open)
        return render_details(app, width);
    unsigned footer = app->height >= 5 ? app->height - 1 : app->height;
    unsigned body = footer > 3 ? footer - 3 : 0;
    size_t count = app->show_saved ? app->saved.count : app->count;
    size_t selected = app->show_saved ? app->saved.selected : app->selected;
    size_t *top = app->show_saved ? &app->saved.top : &app->top;
    bool headings = count && body >= 2;
    size_t visible = body - (headings ? 1 : 0);
    if (count <= visible) {
        *top = 0;
    } else if (visible) {
        if (*top > count - visible)
            *top = count - visible;
        if (selected < *top)
            *top = selected;
        if (selected - *top >= visible)
            *top = selected - visible + 1;
    }

    char text[128], fitted[192], wifi[96];
    for (unsigned row = 1; row <= app->height; row++) {
        const char *style = app->theme.muted;
        const char *value = "";
        if (row == 1 && app->height > 1) {
            style = app->theme.highlight;
            value = app->show_saved ? "Network / Saved" : "Network / Available";
        } else if (row >= footer) {
            if (row == footer) {
                value = footer_message(app, text, sizeof text, width);
                if (*value)
                    style = app->theme.highlight;
            }
            if (row == app->height && (footer < row || !*value))
                value = shortcut_hints(app, text, sizeof text, width);
        } else if (row == 2) {
            style = app->theme.normal;
            bool hidden_prompt = app->operation.action == HIDDEN_SSID || app->operation.action == HIDDEN_SECURITY;
            if (footer == app->height && hidden_prompt) {
                value = shortcut_hints(app, text, sizeof text, width);
            } else {
                value = wifi_status(app, wifi, sizeof wifi);
                if (app->scan.deadline && app->operation.action != IDLE) {
                    snprintf(text, sizeof text, "%s | Scanning...", value);
                    value = text;
                }
            }
        } else if (row == 3 && headings) {
            if (app->show_saved)
                saved_line(fitted, width, NULL);
            else
                network_line(fitted, width, NULL);
            paint_line(app, row, style, fitted);
            continue;
        } else if (count) {
            size_t index = *top + row - (headings ? 4 : 3);
            if (index < count) {
                bool connected;
                if (app->show_saved) {
                    const struct SavedProfile *profile = app->saved.rows[index];
                    saved_line(fitted, width, profile);
                    connected = profile->connected;
                } else {
                    const struct Row *network = &app->rows[index];
                    network_line(fitted, width, network);
                    connected = network->connected;
                }
                style = index == selected ? app->theme.selection :
                        connected ? app->theme.highlight : app->theme.normal;
                paint_line(app, row, style, fitted);
                continue;
            }
        } else if (row == 3) {
            if (!app->show_saved)
                value = empty_list(app);
            else if (app->saved.error)
                value = app->saved.error;
            else if (app->saved.request || app->saved.pending)
                value = "Reading saved profiles...";
            else if (app->saved.listed)
                value = "No saved Wi-Fi profiles";
        }

        fit_text(fitted, value, strlen(value), width);
        paint_line(app, row, style, fitted);
    }

    app->screen_dirty = false;
    if (fflush(stdout) == EOF || ferror(stdout))
        return -1;
    if (app->operation.action == CONFIRM_FORGET)
        app->operation.forget_prompt_shown = true;
    if (app->operation.action == CONFIRM_RADIO_OFF)
        app->operation.radio_prompt_shown = true;
    return 0;
}

enum Key {
    KEY_NONE, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_QUIT, KEY_ESCAPE, KEY_SCAN,
    KEY_ENTER, KEY_DISCONNECT, KEY_YES, KEY_NO, KEY_PASTE_START, KEY_PASTE_END,
    KEY_TAB, KEY_AUTOCONNECT, KEY_FORGET,
    KEY_RADIO, KEY_DETAILS, KEY_HIDDEN, KEY_SECURITY_OPEN, KEY_SECURITY_PSK, KEY_SECURITY_SAE,
};

struct Input {
    enum { INPUT_TEXT, INPUT_ESCAPE, INPUT_CSI, INPUT_SS3 } state;
    unsigned parameter;
    bool first_parameter, paste;
    uint64_t deadline;
};

static uint64_t monotonic_us(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static enum Key sequence_key(unsigned char byte, unsigned parameter)
{
    switch (byte) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '~':
        if (parameter == 1 || parameter == 7)
            return KEY_HOME;
        if (parameter == 4 || parameter == 8)
            return KEY_END;
        if (parameter == 200)
            return KEY_PASTE_START;
        if (parameter == 201)
            return KEY_PASTE_END;
        break;
    }
    return KEY_NONE;
}

static enum Key input_key(struct Input *input, unsigned char byte)
{
    if (input->state == INPUT_TEXT) {
        switch (byte) {
        case 'k': return KEY_UP;
        case 'j': return KEY_DOWN;
        case 'g': return KEY_HOME;
        case 'G': return KEY_END;
        case 'r': return KEY_SCAN;
        case 'd': return KEY_DISCONNECT;
        case '\t': return KEY_TAB;
        case 'a': return KEY_AUTOCONNECT;
        case 'f': return KEY_FORGET;
        case 'w': return KEY_RADIO;
        case 'i': return KEY_DETAILS;
        case 'h': return KEY_HIDDEN;
        case '1': return KEY_SECURITY_OPEN;
        case '2': return KEY_SECURITY_PSK;
        case '3': return KEY_SECURITY_SAE;
        case 'y':
        case 'Y': return KEY_YES;
        case 'n':
        case 'N': return KEY_NO;
        case '\r':
        case '\n': return KEY_ENTER;
        case 'q':
        case '\003': return KEY_QUIT;
        case '\033':
            input->state = INPUT_ESCAPE;
            input->parameter = 0;
            input->first_parameter = true;
            input->deadline = monotonic_us() + 50000;
            break;
        }
    } else if (input->state == INPUT_ESCAPE) {
        if (byte == '[' || byte == 'O') {
            input->state = byte == '[' ? INPUT_CSI : INPUT_SS3;
            input->deadline = monotonic_us() + 50000;
        } else {
            input->state = INPUT_TEXT;
            return KEY_ESCAPE;
        }
    } else if (byte >= '@' && byte <= '~') {
        input->state = INPUT_TEXT;
        return sequence_key(byte, input->parameter);
    } else if (byte >= ' ' && byte <= '?') {
        if (input->first_parameter && byte >= '0' && byte <= '9') {
            if (input->parameter < 100)
                input->parameter = input->parameter * 10 + (unsigned)(byte - '0');
        } else {
            input->first_parameter = false;
        }
        input->deadline = monotonic_us() + 50000;
    } else {
        input->state = INPUT_TEXT;
        return input_key(input, byte);
    }
    return KEY_NONE;
}

static void navigate(struct App *app, enum Key key)
{
    size_t count = app->show_saved ? app->saved.count : app->count;
    if (!count)
        return;
    size_t *selected = app->show_saved ? &app->saved.selected : &app->selected;
    size_t previous = *selected;
    switch (key) {
    case KEY_UP:
        if (*selected)
            (*selected)--;
        break;
    case KEY_DOWN:
        if (*selected + 1 < count)
            (*selected)++;
        break;
    case KEY_HOME:
        *selected = 0;
        break;
    case KEY_END:
        *selected = count - 1;
        break;
    default:
        break;
    }
    if (*selected != previous) {
        if (app->show_saved)
            save_saved_selection(&app->saved);
        else
            save_selection(app);
        app->notice = NULL;
        if (!app->scan.deadline)
            app->scan_notice = NULL;
        app->screen_dirty = true;
    }
}

static bool handle_key(struct App *app, enum Key key)
{
    if (key == KEY_QUIT)
        return true;
    if (app->details.open) {
        struct Details *details = &app->details;
        if (key == KEY_ESCAPE)
            close_details(app);
        else if (key == KEY_UP && details->top) {
            details->top--;
            app->screen_dirty = true;
        } else if (key == KEY_DOWN && details->top < details->last_top) {
            details->top++;
            app->screen_dirty = true;
        } else if (key == KEY_HOME || key == KEY_END) {
            details->top = key == KEY_HOME ? 0 : details->last_top;
            app->screen_dirty = true;
        }
        return false;
    }
    if (password_prompt(app)) {
        if (key == KEY_ESCAPE)
            cancel_password(app);
        return false;
    }
    if (app->operation.action == HIDDEN_SSID || app->operation.action == HIDDEN_SECURITY) {
        if (key == KEY_ESCAPE)
            finish_operation(app, "Hidden connection canceled");
        else if (app->operation.action == HIDDEN_SECURITY) {
            if (key == KEY_SECURITY_OPEN)
                select_hidden_security(app, OPEN);
            else if (key == KEY_SECURITY_PSK)
                select_hidden_security(app, PSK);
            else if (key == KEY_SECURITY_SAE)
                select_hidden_security(app, SAE);
        }
        return false;
    }
    if (app->operation.action == CONFIRM_RADIO_OFF) {
        if (key == KEY_YES && app->operation.radio_prompt_shown)
            set_radio(app, false);
        else if (key == KEY_NO || key == KEY_ESCAPE || key == KEY_ENTER)
            finish_operation(app, "Wi-Fi change canceled");
        return false;
    }
    if (app->operation.action == CONFIRM_DISCONNECT) {
        if (key == KEY_YES)
            confirm_disconnect(app);
        else if (key == KEY_NO || key == KEY_ESCAPE)
            finish_operation(app, "Disconnect canceled");
        return false;
    }
    if (app->operation.action == CONFIRM_FORGET) {
        if (key == KEY_YES)
            confirm_forget(app);
        else if (key == KEY_NO || key == KEY_ESCAPE || key == KEY_ENTER)
            finish_operation(app, "Forget canceled");
        return false;
    }

    switch (key) {
    case KEY_ESCAPE: return true;
    case KEY_TAB:
        app->show_saved = !app->show_saved;
        if (app->show_saved)
            request_saved_list(app);
        app->screen_dirty = true;
        break;
    case KEY_SCAN: start_scan(app); break;
    case KEY_RADIO: toggle_radio(app); break;
    case KEY_DETAILS: open_details(app); break;
    case KEY_HIDDEN: open_hidden_form(app); break;
    case KEY_ENTER:
        if (app->show_saved)
            connect_saved_profile(app);
        else
            connect_selected(app);
        break;
    case KEY_DISCONNECT:
        if (!app->show_saved)
            disconnect_selected(app);
        break;
    case KEY_AUTOCONNECT:
        if (app->show_saved)
            toggle_autoconnect(app);
        break;
    case KEY_FORGET:
        if (app->show_saved)
            forget_saved_profile(app);
        break;
    default: navigate(app, key); break;
    }
    return false;
}

static void edit_password(struct App *app, unsigned char byte, bool paste)
{
    struct Operation *operation = &app->operation;
    if (byte == '\r' || byte == '\n') {
        if (!paste)
            submit_password(app);
        return;
    }
    if (!paste && (byte == '\b' || byte == 0x7f)) {
        operation->password_overflow = false;
        if (operation->password_length) {
            size_t end = operation->password_length;
            size_t start = end;
            do {
                start--;
            } while (start && ((unsigned char)operation->password[start] & 0xc0) == 0x80);
            explicit_bzero(operation->password + start, end - start);
            operation->password_length = start;
        }
    } else if (byte >= ' ' && byte != 0x7f) {
        if (operation->password_length == PASSWORD_MAX) {
            operation->password_overflow = true;
            operation->password_error = "Password is too long";
            app->screen_dirty = true;
            return;
        }
        operation->password[operation->password_length++] = (char)byte;
    } else {
        return;
    }
    operation->password_error = NULL;
    app->screen_dirty = true;
}

static void edit_hidden_ssid(struct App *app, unsigned char byte, bool paste)
{
    struct Operation *operation = &app->operation;
    struct Ssid *ssid = &operation->ssid;
    if (byte == '\r' || byte == '\n') {
        if (!paste) {
            if (!ssid->length || operation->ssid_overflow)
                operation->ssid_error = "Enter an SSID of 1–32 bytes";
            else
                operation->action = HIDDEN_SECURITY;
            app->screen_dirty = true;
        }
        return;
    }
    if (!paste && (byte == '\b' || byte == 0x7f)) {
        operation->ssid_overflow = false;
        if (ssid->length)
            do {
                ssid->length--;
            } while (ssid->length && (ssid->bytes[ssid->length] & 0xc0) == 0x80);
    } else if (byte >= ' ' && byte != 0x7f) {
        if (ssid->length == sizeof ssid->bytes) {
            operation->ssid_overflow = true;
            operation->ssid_error = "SSID is longer than 32 bytes";
            app->screen_dirty = true;
            return;
        }
        ssid->bytes[ssid->length++] = byte;
    } else {
        return;
    }
    operation->ssid_error = NULL;
    app->screen_dirty = true;
}

static bool handle_byte(struct App *app, struct Input *input, unsigned char byte)
{
    if (input->state == INPUT_TEXT && byte != '\033' && (input->paste || byte != '\003')) {
        if (password_prompt(app)) {
            edit_password(app, byte, input->paste);
            return false;
        }
        if (app->operation.action == HIDDEN_SSID) {
            edit_hidden_ssid(app, byte, input->paste);
            return false;
        }
        if (input->paste)
            return false;
    }
    enum Key key = input_key(input, byte);
    if (key == KEY_PASTE_START)
        input->paste = true;
    else if (key == KEY_PASTE_END)
        input->paste = false;
    else if (!input->paste)
        return handle_key(app, key);
    return false;
}

static int event_loop(struct App *app, const sigset_t *wait_mask)
{
    struct Input input = {0};

    while (!interrupted) {
        uint64_t now = monotonic_us();
        refresh_radio(app);
        refresh_scan(app, now);
        refresh_operation(app, now);
        if (resized) {
            resized = 0;
            terminal_size(app);
        }
        if (app->model_dirty)
            rebuild_rows(app);
        if (app->screen_dirty && render(app) < 0)
            return -1;

        struct pollfd fds[] = {{ .fd = STDIN_FILENO, .events = POLLIN }, { .fd = -1 }};
        uint64_t deadline = UINT64_MAX;
        if (app->bus) {
            int events = sd_bus_get_events(app->bus);
            fds[1].fd = sd_bus_get_fd(app->bus);
            if (events < 0 || fds[1].fd < 0 || sd_bus_get_timeout(app->bus, &deadline) < 0) {
                close_bus(app);
                continue;
            }
            fds[1].events = (short)events;
        }

        if (input.state != INPUT_TEXT && input.deadline < deadline)
            deadline = input.deadline;
        if (app->operation.deadline && app->operation.deadline < deadline)
            deadline = app->operation.deadline;
        if (app->scan.deadline && app->scan.deadline < deadline)
            deadline = app->scan.deadline;

        struct timespec timeout, *wait = NULL;
        if (deadline != UINT64_MAX) {
            now = monotonic_us();
            uint64_t remaining = deadline > now ? deadline - now : 0;
            timeout.tv_sec = (time_t)(remaining / 1000000);
            timeout.tv_nsec = (long)(remaining % 1000000) * 1000;
            wait = &timeout;
        }

        int result = ppoll(fds, sizeof fds / sizeof fds[0], wait, wait_mask);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (fds[0].revents & POLLIN) {
            unsigned char bytes[128];
            ssize_t length = read(STDIN_FILENO, bytes, sizeof bytes);
            bool quit = false;
            for (ssize_t i = 0; i < length; i++) {
                if (handle_byte(app, &input, bytes[i])) {
                    quit = true;
                    break;
                }
            }
            explicit_bzero(bytes, sizeof bytes);
            if (quit || length <= 0)
                return length < 0 ? -1 : 0;
        } else if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            return 0;
        }

        if (input.state != INPUT_TEXT && monotonic_us() >= input.deadline) {
            if (input.state == INPUT_ESCAPE && !input.paste && handle_key(app, KEY_ESCAPE))
                return 0;
            input.state = INPUT_TEXT;
        }

        /* Drain a bounded batch so a burst of D-Bus traffic cannot starve keys. */
        if (app->bus) {
            for (unsigned i = 0; i < 64; i++) {
                result = sd_bus_process(app->bus, NULL);
                if (result < 0 || app->match_failed) {
                    close_bus(app);
                    break;
                }
                if (result == 0)
                    break;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { 0 },
    };
    const char *override = NULL;
    bool help = false;
    int option;
    opterr = 0;
    while ((option = getopt_long(argc, argv, "hc:", options, NULL)) != -1) {
        if (option == 'h')
            help = true;
        else if (option == 'c' && *optarg)
            override = optarg;
        else
            break;
    }
    if (option != -1 || optind != argc) {
        fputs("Usage: net [-c PATH] [-h|--help]\n", stderr);
        return EXIT_FAILURE;
    }
    if (help) {
        puts("Usage: net [-c PATH]\n"
                 "Tab switches between Available and Saved. Enter connects.\n"
                 "w toggles Wi-Fi, i opens details, and h connects to a hidden network.\n"
                 "Available: r scans and d disconnects. Saved: a toggles auto-connect and f forgets.\n"
                 "Arrows or j/k move, Home/End or g/G jump, and q or Escape quits.");
        return EXIT_SUCCESS;
    }

    struct Config config;
    if (!load_config(&config, override))
        return EXIT_FAILURE;
    apply_wallpaper_theme(&config);
    struct App app = {0};
    apply_config(&app, &config);

    setlocale(LC_CTYPE, "");
    struct passwd *account = getpwuid(getuid());
    sigset_t wait_mask;
    if (setup_signals(&wait_mask) < 0 || terminal_enter() < 0) {
        int error = errno;
        terminal_restore();
        fprintf(stderr, "net: terminal setup: %s\n", strerror(error));
        return EXIT_FAILURE;
    }

    if (account && asprintf(&app.profile_permission, "user:%s:", account->pw_name) < 0)
        app.profile_permission = NULL;
    app.manager.app = &app;
    app.manager.path = NM_PATH;
    app.manager.props = (struct Properties){ .object = &app.manager, .interface = MANAGER };
    terminal_size(&app);
    open_bus(&app);

    int result = event_loop(&app, &wait_mask);
    int error = errno;
    clear_password(&app.operation);
    terminal_restore();
    close_bus(&app);
    free(app.lines);
    free(app.rows);
    free(app.saved.rows);
    free(app.selected_hidden);
    free(app.profile_permission);
    if (result < 0) {
        fprintf(stderr, "net: terminal I/O: %s\n", strerror(error));
        return EXIT_FAILURE;
    }
    return interrupted ? 128 + interrupted : EXIT_SUCCESS;
}
