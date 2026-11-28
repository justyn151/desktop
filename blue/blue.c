/* blue: a Bluetooth manager for the terminal, over BlueZ's D-Bus API.
   0.1 lists the devices the adapter already knows, keeps them live from
   BlueZ's signals, connects and disconnects them, and turns the adapter on
   and off. It looks and behaves like net. */
#define _GNU_SOURCE

#include "blue.h"

#include "desk.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <systemd/sd-bus.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

/* Config and colors */

enum Color {
    BACKGROUND, FOREGROUND, HIGHLIGHT, MUTED, SELECTION_BACKGROUND, SELECTION_FOREGROUND,
    COLOR_COUNT
};

static const char *const color_names[COLOR_COUNT] = {
    [BACKGROUND] = "background", [FOREGROUND] = "foreground", [HIGHLIGHT] = "highlight",
    [MUTED] = "muted", [SELECTION_BACKGROUND] = "selection_background",
    [SELECTION_FOREGROUND] = "selection_foreground",
};

/* The palette roles each color follows with [theme] source = wallpaper. */
static const enum desk_role color_roles[COLOR_COUNT] = {
    [BACKGROUND] = DESK_BACKGROUND, [FOREGROUND] = DESK_FOREGROUND,
    [HIGHLIGHT] = DESK_HIGHLIGHT, [MUTED] = DESK_MUTED,
    [SELECTION_BACKGROUND] = DESK_SELECTION_BACKGROUND,
    [SELECTION_FOREGROUND] = DESK_SELECTION_FOREGROUND,
};

struct Config {
    uint32_t rgb[COLOR_COUNT];
    bool wallpaper_theme;
};

static const struct Config config_defaults = {
    .rgb = { 0x353535, 0xc5ad81, 0xead4aa, 0x887d69, 0xc5ad81, 0x353535 },
    .wallpaper_theme = true,
};

struct Theme {
    char normal[48], highlight[48], muted[48], selection[48];
};

/* BlueZ objects */

struct Adapter {
    char *path, *alias, *address, *power_state;
    bool powered, discovering;
};

struct Device {
    char *path, *adapter, *address, *alias, *icon;
    bool paired, bonded, trusted, connected;
    int battery;            /* Percent, or -1 without org.bluez.Battery1. */
    sd_bus_slot *request;   /* Connect or Disconnect waiting for its reply. */
    bool connecting;        /* Which of the two the request is. */
    bool failed;            /* The last attempt failed; shown until the next one. */
};

struct Line {
    char text[320];
};

struct App {
    sd_bus *bus;
    sd_bus_slot *list_request, *power_request;
    bool bluez_running, listed;
    struct Adapter adapters[MAX_ADAPTERS];
    size_t adapter_count;
    struct Device **devices;
    size_t device_count, device_capacity;

    /* The visible rows: known devices on the current adapter, sorted. */
    struct Device **rows;
    size_t row_count, row_capacity, selected, top;
    char *selected_path;

    struct Config config;
    struct Theme theme;
    char *palette_path;
    DeskWatch palette_watch;
    char notice[NOTICE_SIZE];

    struct Line *lines;
    unsigned width, height;
    bool model_dirty, screen_dirty, clear_screen;
};

static struct termios saved_termios;
static bool terminal_active;
static volatile sig_atomic_t interrupted, resized;

static void terminal_restore(void)
{
    if (!terminal_active)
        return;
    fputs("\033[0m\033[?1049l\033[?25h", stdout);
    fflush(stdout);
    while (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios) < 0 && errno == EINTR)
        continue;
    terminal_active = false;
}

static void *allocate(void *old, size_t count, size_t size)
{
    void *memory = reallocarray(old, count ? count : 1, size);
    if (!memory) {
        terminal_restore();
        fputs("blue: out of memory\n", stderr);
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

static void set_notice(struct App *app, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(app->notice, sizeof app->notice, format, args);
    va_end(args);
    app->screen_dirty = true;
}

struct ConfigParse {
    struct Config *config;
    const char *path;
};

static void config_entry(void *data, const char *section, const char *key, const char *value, int line)
{
    struct ConfigParse *parse = data;
    if (!key) {
        fprintf(stderr, "blue: %s:%d: expected key = value\n", parse->path, line);
        return;
    }
    if (!strcmp(section, "appearance")) {
        for (size_t i = 0; i < COLOR_COUNT; i++) {
            if (strcmp(key, color_names[i]))
                continue;
            if (!desk_parse_rgb(value, &parse->config->rgb[i]))
                fprintf(stderr, "blue: %s:%d: %s must be #RRGGBB\n", parse->path, line, key);
            return;
        }
    } else if (!strcmp(section, "theme") && !strcmp(key, "source")) {
        if (!strcmp(value, "wallpaper") || !strcmp(value, "manual"))
            parse->config->wallpaper_theme = !strcmp(value, "wallpaper");
        else
            fprintf(stderr, "blue: %s:%d: source must be wallpaper or manual\n", parse->path, line);
        return;
    }
    fprintf(stderr, "blue: %s:%d: unknown option [%s] %s\n", parse->path, line, section, key);
}

/* ~/.config/blue/blue.conf is optional; without it the defaults and the wallpaper apply. */
static void load_config(struct Config *config, const char *override)
{
    *config = config_defaults;
    char *path = override ? strdup(override) : desk_config_path("blue");
    if (!path)
        return;
    struct ConfigParse parse = { config, path };
    int error = desk_ini_load(path, config_entry, &parse);
    if (error && (error != ENOENT || override))
        fprintf(stderr, "blue: can't read %s: %s\n", path, strerror(error));
    free(path);
}

static void format_style(char *text, size_t size, uint32_t background, uint32_t foreground)
{
    snprintf(text, size, "\033[48;2;%u;%u;%u;38;2;%u;%u;%um",
             (unsigned)(background >> 16 & 255), (unsigned)(background >> 8 & 255),
             (unsigned)(background & 255), (unsigned)(foreground >> 16 & 255),
             (unsigned)(foreground >> 8 & 255), (unsigned)(foreground & 255));
}

/* Build the escape sequences from the config, with the wallpaper's palette on top. */
static void apply_theme(struct App *app)
{
    uint32_t rgb[COLOR_COUNT];
    memcpy(rgb, app->config.rgb, sizeof rgb);
    DeskPalette palette;
    if (app->config.wallpaper_theme && app->palette_path &&
        desk_palette_load(app->palette_path, &palette) == 0)
        for (size_t i = 0; i < COLOR_COUNT; i++)
            rgb[i] = palette.rgb[color_roles[i]];
    struct Theme *theme = &app->theme;
    format_style(theme->normal, sizeof theme->normal, rgb[BACKGROUND], rgb[FOREGROUND]);
    format_style(theme->highlight, sizeof theme->highlight, rgb[BACKGROUND], rgb[HIGHLIGHT]);
    format_style(theme->muted, sizeof theme->muted, rgb[BACKGROUND], rgb[MUTED]);
    format_style(theme->selection, sizeof theme->selection, rgb[SELECTION_BACKGROUND],
                 rgb[SELECTION_FOREGROUND]);
    app->clear_screen = app->screen_dirty = true;
    if (app->lines)
        memset(app->lines, 0, app->height * sizeof *app->lines);
}

/* Terminal */

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
    /* Signals are only taken inside ppoll, so nothing else is interrupted. */
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
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
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
    fputs("\033[?1049h\033[?25l", stdout);
    return fflush(stdout);
}

static void terminal_size(struct App *app)
{
    struct winsize size = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0)
        size = (struct winsize){0};
    unsigned width = size.ws_col ? size.ws_col : 80;
    unsigned height = size.ws_row ? size.ws_row : 24;
    if (width == app->width && height == app->height)
        return;
    app->lines = allocate(app->lines, height, sizeof *app->lines);
    memset(app->lines, 0, height * sizeof *app->lines);
    app->width = width;
    app->height = height;
    app->clear_screen = app->screen_dirty = true;
}

/* Copy at most limit columns of input into out, replacing what can't be shown with '?'. */
static unsigned fit_text(char *out, const char *input, size_t length, unsigned limit)
{
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
    return columns;
}

/* The model */

static const char *device_name(const struct Device *device)
{
    if (device->alias && *device->alias)
        return device->alias;
    return device->address ? device->address : device->path;
}

static struct Adapter *find_adapter(struct App *app, const char *path)
{
    for (size_t i = 0; i < app->adapter_count; i++)
        if (!strcmp(app->adapters[i].path, path))
            return &app->adapters[i];
    return NULL;
}

/* The adapter blue shows: the first by path, usually hci0. */
static struct Adapter *current_adapter(struct App *app)
{
    struct Adapter *best = NULL;
    for (size_t i = 0; i < app->adapter_count; i++)
        if (!best || strcmp(app->adapters[i].path, best->path) < 0)
            best = &app->adapters[i];
    return best;
}

static struct Adapter *ensure_adapter(struct App *app, const char *path)
{
    struct Adapter *adapter = find_adapter(app, path);
    if (adapter || app->adapter_count == MAX_ADAPTERS)
        return adapter;
    adapter = &app->adapters[app->adapter_count++];
    *adapter = (struct Adapter){0};
    replace_string(&adapter->path, path);
    return adapter;
}

static void free_adapter(struct Adapter *adapter)
{
    free(adapter->path);
    free(adapter->alias);
    free(adapter->address);
    free(adapter->power_state);
}

static void remove_adapter(struct App *app, const char *path)
{
    struct Adapter *adapter = find_adapter(app, path);
    if (!adapter)
        return;
    free_adapter(adapter);
    *adapter = app->adapters[--app->adapter_count];
}

static struct Device *find_device(struct App *app, const char *path)
{
    for (size_t i = 0; i < app->device_count; i++)
        if (!strcmp(app->devices[i]->path, path))
            return app->devices[i];
    return NULL;
}

/* Devices are allocated one by one, so a pending call can keep a pointer. */
static struct Device *ensure_device(struct App *app, const char *path)
{
    struct Device *device = find_device(app, path);
    if (device)
        return device;
    if (app->device_count == app->device_capacity) {
        app->device_capacity = app->device_capacity ? 2 * app->device_capacity : 16;
        app->devices = allocate(app->devices, app->device_capacity, sizeof *app->devices);
    }
    device = allocate(NULL, 1, sizeof *device);
    *device = (struct Device){ .battery = -1 };
    replace_string(&device->path, path);
    app->devices[app->device_count++] = device;
    return device;
}

static void free_device(struct Device *device)
{
    /* Dropping the slot cancels the callback, which would otherwise see freed memory. */
    sd_bus_slot_unref(device->request);
    free(device->path);
    free(device->adapter);
    free(device->address);
    free(device->alias);
    free(device->icon);
    free(device);
}

static void remove_device(struct App *app, const char *path)
{
    for (size_t i = 0; i < app->device_count; i++) {
        if (strcmp(app->devices[i]->path, path))
            continue;
        free_device(app->devices[i]);
        app->devices[i] = app->devices[--app->device_count];
        return;
    }
}

static void clear_model(struct App *app)
{
    for (size_t i = 0; i < app->device_count; i++)
        free_device(app->devices[i]);
    app->device_count = 0;
    for (size_t i = 0; i < app->adapter_count; i++)
        free_adapter(&app->adapters[i]);
    app->adapter_count = 0;
    app->power_request = sd_bus_slot_unref(app->power_request);
    app->listed = false;
    app->model_dirty = app->screen_dirty = true;
}

/* 0.1 shows what the adapter already knows; scanning for new devices is 0.2. */
static bool known(const struct Device *device)
{
    return device->paired || device->bonded || device->trusted || device->connected;
}

static int compare_rows(const void *left, const void *right)
{
    const struct Device *a = *(struct Device *const *)left, *b = *(struct Device *const *)right;
    if (a->connected != b->connected)
        return a->connected ? -1 : 1;
    int order = strcasecmp(device_name(a), device_name(b));
    return order ? order : strcmp(a->path, b->path);
}

static void rebuild_rows(struct App *app)
{
    app->model_dirty = false;
    const struct Adapter *adapter = current_adapter(app);
    if (app->row_capacity < app->device_count) {
        app->row_capacity = app->device_count;
        app->rows = allocate(app->rows, app->row_capacity, sizeof *app->rows);
    }
    app->row_count = 0;
    for (size_t i = 0; i < app->device_count; i++) {
        struct Device *device = app->devices[i];
        if (adapter && known(device) && device->adapter && !strcmp(device->adapter, adapter->path))
            app->rows[app->row_count++] = device;
    }
    qsort(app->rows, app->row_count, sizeof *app->rows, compare_rows);

    /* Keep the selection on the same device as rows move. */
    app->selected = 0;
    for (size_t i = 0; app->selected_path && i < app->row_count; i++)
        if (!strcmp(app->rows[i]->path, app->selected_path))
            app->selected = i;
    replace_string(&app->selected_path, app->row_count ? app->rows[app->selected]->path : NULL);
    app->screen_dirty = true;
}

/* Reading properties */

/* Read one variant into dest when it holds the expected type; skip it otherwise. */
static int read_variant(sd_bus_message *message, char type, void *dest)
{
    const char *contents = NULL;
    int result = sd_bus_message_peek_type(message, NULL, &contents);
    if (result < 0)
        return result;
    if (!contents || contents[0] != type || contents[1])
        return sd_bus_message_skip(message, "v");
    const char signature[2] = { type, '\0' };
    switch (type) {
    case 'b': {
        int value;
        result = sd_bus_message_read(message, "v", signature, &value);
        if (result >= 0)
            *(bool *)dest = value != 0;
        return result;
    }
    case 'y': {
        uint8_t value;
        result = sd_bus_message_read(message, "v", signature, &value);
        if (result >= 0)
            *(int *)dest = value;
        return result;
    }
    default: {
        const char *value;
        result = sd_bus_message_read(message, "v", signature, &value);
        if (result >= 0)
            replace_string(dest, value);
        return result;
    }
    }
}

static int read_adapter_property(struct Adapter *adapter, const char *name, sd_bus_message *message)
{
    if (!strcmp(name, "Alias"))
        return read_variant(message, 's', &adapter->alias);
    if (!strcmp(name, "Address"))
        return read_variant(message, 's', &adapter->address);
    if (!strcmp(name, "PowerState"))
        return read_variant(message, 's', &adapter->power_state);
    if (!strcmp(name, "Powered"))
        return read_variant(message, 'b', &adapter->powered);
    if (!strcmp(name, "Discovering"))
        return read_variant(message, 'b', &adapter->discovering);
    return sd_bus_message_skip(message, "v");
}

static int read_device_property(struct Device *device, const char *name, sd_bus_message *message)
{
    static const struct { const char *name; char type; size_t offset; } fields[] = {
        { "Adapter", 'o', offsetof(struct Device, adapter) },
        { "Address", 's', offsetof(struct Device, address) },
        { "Alias", 's', offsetof(struct Device, alias) },
        { "Icon", 's', offsetof(struct Device, icon) },
        { "Paired", 'b', offsetof(struct Device, paired) },
        { "Bonded", 'b', offsetof(struct Device, bonded) },
        { "Trusted", 'b', offsetof(struct Device, trusted) },
        { "Connected", 'b', offsetof(struct Device, connected) },
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
        if (!strcmp(name, fields[i].name))
            return read_variant(message, fields[i].type, (char *)device + fields[i].offset);
    return sd_bus_message_skip(message, "v");
}

/* Read an a{sv} of one interface's properties into the object at path. */
static int read_interface(struct App *app, const char *path, const char *interface,
                          sd_bus_message *message, bool create)
{
    struct Adapter *adapter = NULL;
    struct Device *device = NULL;
    bool battery = !strcmp(interface, BLUEZ_BATTERY);
    if (!strcmp(interface, BLUEZ_ADAPTER))
        adapter = create ? ensure_adapter(app, path) : find_adapter(app, path);
    else if (!strcmp(interface, BLUEZ_DEVICE) || battery)
        device = create ? ensure_device(app, path) : find_device(app, path);
    if (!adapter && !device)
        return sd_bus_message_skip(message, "a{sv}");

    int result = sd_bus_message_enter_container(message, 'a', "{sv}");
    if (result < 0)
        return result;
    while ((result = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        const char *name;
        result = sd_bus_message_read(message, "s", &name);
        if (result >= 0) {
            if (adapter)
                result = read_adapter_property(adapter, name, message);
            else if (battery && !strcmp(name, "Percentage"))
                result = read_variant(message, 'y', &device->battery);
            else if (battery)
                result = sd_bus_message_skip(message, "v");
            else
                result = read_device_property(device, name, message);
        }
        if (result < 0)
            return result;
        result = sd_bus_message_exit_container(message);
        if (result < 0)
            return result;
    }
    if (result < 0)
        return result;
    app->model_dirty = app->screen_dirty = true;
    return sd_bus_message_exit_container(message);
}

/* An a{sa{sv}}: every interface of one object. */
static int read_interfaces(struct App *app, const char *path, sd_bus_message *message)
{
    int result = sd_bus_message_enter_container(message, 'a', "{sa{sv}}");
    if (result < 0)
        return result;
    while ((result = sd_bus_message_enter_container(message, 'e', "sa{sv}")) > 0) {
        const char *interface;
        result = sd_bus_message_read(message, "s", &interface);
        if (result >= 0)
            result = read_interface(app, path, interface, message, true);
        if (result >= 0)
            result = sd_bus_message_exit_container(message);
        if (result < 0)
            return result;
    }
    if (result < 0)
        return result;
    return sd_bus_message_exit_container(message);
}

/* D-Bus */

static void close_bus(struct App *app)
{
    clear_model(app);
    app->list_request = sd_bus_slot_unref(app->list_request);
    app->bus = sd_bus_flush_close_unref(app->bus);
    app->bluez_running = false;
}

static const char *bluez_error(const sd_bus_error *error)
{
    const char *name = error->name ? error->name : "", *message = error->message ? error->message : "";
    if (strstr(message, "page-timeout") || strstr(message, "Host is down") ||
        strstr(message, "le-connection-abort") || strstr(message, "Connection refused"))
        return "the device is off or out of range";
    if (strstr(message, "profile-unavailable"))
        return "no profile it can use; is PipeWire running?";
    if (!strcmp(name, BLUEZ_ERROR "InProgress"))
        return "already in progress";
    if (!strcmp(name, BLUEZ_ERROR "NotReady"))
        return "Bluetooth is off";
    if (!strcmp(name, BLUEZ_ERROR "Blocked"))
        return "Bluetooth is blocked; try rfkill unblock bluetooth";
    if (!strcmp(name, BLUEZ_ERROR "AuthenticationFailed") || !strcmp(name, BLUEZ_ERROR "AuthenticationRejected"))
        return "the device refused; pair it again";
    if (!strcmp(name, "org.freedesktop.DBus.Error.NoReply") || !strcmp(name, "org.freedesktop.DBus.Error.Timeout"))
        return "no answer in time";
    if (!strcmp(name, "org.freedesktop.DBus.Error.AccessDenied"))
        return "not allowed by D-Bus policy";
    return *message ? message : *name ? name : "unknown error";
}

static int on_list(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    app->list_request = sd_bus_slot_unref(app->list_request);
    const sd_bus_error *error = sd_bus_message_get_error(message);
    clear_model(app);
    if (error) {
        /* ServiceUnknown or NameHasNoOwner: bluetoothd isn't running. */
        app->bluez_running = false;
        return 0;
    }
    app->bluez_running = true;
    int result = sd_bus_message_enter_container(message, 'a', "{oa{sa{sv}}}");
    while (result >= 0 && (result = sd_bus_message_enter_container(message, 'e', "oa{sa{sv}}")) > 0) {
        const char *path;
        result = sd_bus_message_read(message, "o", &path);
        if (result >= 0)
            result = read_interfaces(app, path, message);
        if (result >= 0)
            result = sd_bus_message_exit_container(message);
    }
    if (result < 0)
        set_notice(app, "Couldn't read BlueZ's objects: %s", strerror(-result));
    app->listed = true;
    app->model_dirty = app->screen_dirty = true;
    return 0;
}

static void request_list(struct App *app)
{
    app->list_request = sd_bus_slot_unref(app->list_request);
    if (sd_bus_call_method_async(app->bus, &app->list_request, BLUEZ, "/", OBJECT_MANAGER,
                                 "GetManagedObjects", on_list, app, "") < 0)
        set_notice(app, "Couldn't ask BlueZ for its devices");
    app->screen_dirty = true;
}

static int on_interfaces_added(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    const char *path;
    if (!app->listed || sd_bus_message_read(message, "o", &path) < 0)
        return 0;
    read_interfaces(app, path, message);
    return 0;
}

static int on_interfaces_removed(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    const char *path;
    if (!app->listed || sd_bus_message_read(message, "o", &path) < 0 ||
        sd_bus_message_enter_container(message, 'a', "s") < 0)
        return 0;
    const char *interface;
    while (sd_bus_message_read(message, "s", &interface) > 0) {
        if (!strcmp(interface, BLUEZ_DEVICE)) {
            remove_device(app, path);
        } else if (!strcmp(interface, BLUEZ_ADAPTER)) {
            remove_adapter(app, path);
        } else if (!strcmp(interface, BLUEZ_BATTERY)) {
            struct Device *device = find_device(app, path);
            if (device)
                device->battery = -1;
        }
    }
    app->model_dirty = app->screen_dirty = true;
    return 0;
}

static int on_properties_changed(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    const char *interface, *path = sd_bus_message_get_path(message);
    if (app->listed && path && sd_bus_message_read(message, "s", &interface) >= 0)
        read_interface(app, path, interface, message, false);
    return 0;
}

/* bluetoothd started or stopped: start over from its objects, or show that it's gone. */
static int on_owner_changed(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    const char *name, *old_owner, *new_owner;
    if (sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner) < 0 || strcmp(name, BLUEZ))
        return 0;
    clear_model(app);
    app->bluez_running = *new_owner != '\0';
    if (app->bluez_running)
        request_list(app);
    return 0;
}

static void open_bus(struct App *app)
{
    if (sd_bus_open_system(&app->bus) < 0) {
        app->bus = NULL;
        return;
    }
    /* Synchronous, so no signal can slip in before GetManagedObjects is sent. */
    if (sd_bus_match_signal(app->bus, NULL, BLUEZ, NULL, OBJECT_MANAGER, "InterfacesAdded",
                            on_interfaces_added, app) < 0 ||
        sd_bus_match_signal(app->bus, NULL, BLUEZ, NULL, OBJECT_MANAGER, "InterfacesRemoved",
                            on_interfaces_removed, app) < 0 ||
        sd_bus_match_signal(app->bus, NULL, BLUEZ, NULL, PROPERTIES, "PropertiesChanged",
                            on_properties_changed, app) < 0 ||
        sd_bus_add_match(app->bus, NULL,
                         "type='signal',sender='" DBUS "',path='" DBUS_PATH "',interface='" DBUS "',"
                         "member='NameOwnerChanged',arg0='" BLUEZ "'", on_owner_changed, app) < 0) {
        close_bus(app);
        return;
    }
    request_list(app);
}

/* Actions */

/* The reply handler needs the app as well as the device; this pairs them. */
struct Pending {
    struct App *app;
    struct Device *device;
    bool connecting;
};

static int on_device_reply(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct Pending *pending = userdata;
    struct App *app = pending->app;
    struct Device *device = pending->device;
    const sd_bus_error *error = sd_bus_message_get_error(message);
    device->request = sd_bus_slot_unref(device->request);
    device->failed = error != NULL;
    if (error)
        set_notice(app, "Couldn't %s %s: %s", pending->connecting ? "connect to" : "disconnect",
                   device_name(device), bluez_error(error));
    else
        set_notice(app, "%s %s", pending->connecting ? "Connected to" : "Disconnected", device_name(device));
    app->model_dirty = app->screen_dirty = true;
    return 0;
}

static void destroy_pending(void *userdata)
{
    free(userdata);
}

static void toggle_connection(struct App *app)
{
    if (!app->row_count)
        return;
    struct Device *device = app->rows[app->selected];
    struct Adapter *adapter = current_adapter(app);
    if (device->request) {
        set_notice(app, "Still %s %s...", device->connecting ? "connecting to" : "disconnecting",
                   device_name(device));
        return;
    }
    bool connecting = !device->connected;
    if (connecting && adapter && !adapter->powered) {
        set_notice(app, "Bluetooth is off. Press p to turn it on");
        return;
    }
    struct Pending *pending = allocate(NULL, 1, sizeof *pending);
    *pending = (struct Pending){ app, device, connecting };
    sd_bus_message *call = NULL;
    int result = sd_bus_message_new_method_call(app->bus, &call, BLUEZ, device->path, BLUEZ_DEVICE,
                                                connecting ? "Connect" : "Disconnect");
    if (result >= 0)
        result = sd_bus_call_async(app->bus, &device->request, call, on_device_reply, pending,
                                   connecting ? CONNECT_TIMEOUT_US : CALL_TIMEOUT_US);
    sd_bus_message_unref(call);
    if (result < 0) {
        free(pending);
        set_notice(app, "Couldn't ask BlueZ: %s", strerror(-result));
        return;
    }
    sd_bus_slot_set_destroy_callback(device->request, destroy_pending);
    device->connecting = connecting;
    device->failed = false;
    set_notice(app, "%s %s...", connecting ? "Connecting to" : "Disconnecting", device_name(device));
}

static int on_power_reply(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    app->power_request = sd_bus_slot_unref(app->power_request);
    const sd_bus_error *error = sd_bus_message_get_error(message);
    if (error)
        set_notice(app, "Couldn't change Bluetooth: %s", bluez_error(error));
    else
        app->notice[0] = '\0';
    app->screen_dirty = true;
    return 0;
}

static void toggle_power(struct App *app)
{
    struct Adapter *adapter = current_adapter(app);
    if (!adapter || app->power_request)
        return;
    if (adapter->power_state && !strcmp(adapter->power_state, "off-blocked")) {
        set_notice(app, "Bluetooth is blocked. Run rfkill unblock bluetooth");
        return;
    }
    bool on = !adapter->powered;
    int result = sd_bus_call_method_async(app->bus, &app->power_request, BLUEZ, adapter->path,
                                          PROPERTIES, "Set", on_power_reply, app, "ssv",
                                          BLUEZ_ADAPTER, "Powered", "b", (int)on);
    if (result < 0)
        set_notice(app, "Couldn't ask BlueZ: %s", strerror(-result));
    else
        set_notice(app, "Turning Bluetooth %s...", on ? "on" : "off");
}

/* Drawing */

static void paint_line(struct App *app, unsigned row, const char *style, const char *value)
{
    char line[sizeof app->lines[0].text];
    const char *margin = app->width >= 40 ? "  " : "";
    snprintf(line, sizeof line, "%s\033[K%s%s", style, margin, value);
    char *previous = app->lines[row - 1].text;
    if (strcmp(previous, line)) {
        printf("\033[%u;1H%s", row, line);
        strcpy(previous, line);
    }
}

/* What the device is, from the freedesktop icon name BlueZ derives from its class. */
static const char *device_kind(const struct Device *device)
{
    static const struct { const char *icon, *kind; } kinds[] = {
        { "audio-headset", "Headset" }, { "audio-headphones", "Headphones" },
        { "audio-card", "Speaker" }, { "input-keyboard", "Keyboard" },
        { "input-mouse", "Mouse" }, { "input-tablet", "Tablet" },
        { "input-gaming", "Gamepad" }, { "phone", "Phone" }, { "computer", "Computer" },
        { "camera-video", "Camera" }, { "camera-photo", "Camera" }, { "printer", "Printer" },
        { "modem", "Modem" }, { "network-wireless", "Network" }, { "video-display", "Display" },
        { "multimedia-player", "Player" }, { "scanner", "Scanner" },
    };
    for (size_t i = 0; device->icon && i < sizeof kinds / sizeof kinds[0]; i++)
        if (!strcmp(device->icon, kinds[i].icon))
            return kinds[i].kind;
    return "";
}

static const char *device_state(const struct Device *device)
{
    if (device->request)
        return device->connecting ? "Connecting" : "Leaving";
    if (device->connected)
        return "Connected";
    return device->failed ? "Failed" : "";
}

/* One row of the table, or its headings when device is NULL. */
static void device_line(char *text, unsigned width, const struct Device *device)
{
    unsigned state_width = width >= 30 ? 12 : 0;
    unsigned battery_width = width >= 44 ? 9 : 0;
    unsigned kind_width = width >= 56 ? 12 : 0;
    unsigned name_width = width - state_width - battery_width - kind_width;
    if (name_width > 36 && state_width)
        name_width = 36;

    char name[4 * 36 + 1];
    const char *label = device ? device_name(device) : "Device";
    unsigned columns = fit_text(name, label, strlen(label), name_width);
    text += sprintf(text, "%s%*s", name, (int)(name_width - columns), "");
    if (kind_width)
        text += sprintf(text, "  %-10s", device ? device_kind(device) : "Type");
    if (battery_width) {
        char battery[8] = "Battery";
        if (device)
            snprintf(battery, sizeof battery, device->battery >= 0 ? "%d%%" : "", device->battery);
        text += sprintf(text, "  %-7s", battery);
    }
    if (state_width)
        sprintf(text, "  %s", device ? device_state(device) : "State");
}

static const char *bluetooth_status(struct App *app, char *text, size_t size)
{
    if (!app->bus)
        return "System D-Bus unavailable";
    if (!app->listed)
        return app->list_request ? "Reading BlueZ..." : "BlueZ isn't running | Start bluetooth.service";
    const struct Adapter *adapter = current_adapter(app);
    if (!adapter)
        return "No Bluetooth adapter";
    const char *state = adapter->power_state ? adapter->power_state : "";
    if (!strcmp(state, "off-blocked"))
        return "Bluetooth blocked | Run rfkill unblock bluetooth";
    if (!strcmp(state, "off-enabling"))
        return "Bluetooth turning on...";
    if (!strcmp(state, "on-disabling"))
        return "Bluetooth turning off...";
    if (!adapter->powered)
        return "Bluetooth off | Press p to turn on";

    size_t connected = 0;
    for (size_t i = 0; i < app->row_count; i++)
        connected += app->rows[i]->connected;
    char name[4 * 32 + 1];
    const char *label = adapter->alias ? adapter->alias : adapter->path;
    fit_text(name, label, strlen(label), 32);
    snprintf(text, size, "Bluetooth on | %s | %zu connected", name, connected);
    return text;
}

static const char *empty_list(struct App *app)
{
    const struct Adapter *adapter = current_adapter(app);
    if (!app->listed || !adapter)
        return "";
    return "No paired devices. Pair one with bluetoothctl for now";
}

static const char *shortcut_hints(const struct App *app, unsigned width)
{
    bool disconnect = app->row_count && app->rows[app->selected]->connected;
    if (width >= 52)
        return disconnect ? "j/k  Enter Disconnect  p Power  q Quit" : "j/k  Enter Connect  p Power  q Quit";
    return "Enter  p Power  q Quit";
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
    unsigned footer = app->height >= 5 ? app->height - 1 : app->height;
    unsigned body = footer > 3 ? footer - 3 : 0;
    bool headings = app->row_count && body >= 2;
    size_t visible = body - (headings ? 1 : 0);
    if (app->row_count <= visible) {
        app->top = 0;
    } else if (visible) {
        if (app->top > app->row_count - visible)
            app->top = app->row_count - visible;
        if (app->selected < app->top)
            app->top = app->selected;
        if (app->selected - app->top >= visible)
            app->top = app->selected - visible + 1;
    }

    char text[192], fitted[sizeof app->lines[0].text];
    for (unsigned row = 1; row <= app->height; row++) {
        const char *style = app->theme.muted, *value = "";
        if (row == 1 && app->height > 1) {
            style = app->theme.highlight;
            value = "Bluetooth / Devices";
        } else if (row >= footer) {
            if (row == footer && app->notice[0]) {
                style = app->theme.highlight;
                value = app->notice;
            }
            if (row == app->height && (footer < row || !*value))
                value = shortcut_hints(app, width);
        } else if (row == 2) {
            style = app->theme.normal;
            value = bluetooth_status(app, text, sizeof text);
        } else if (row == 3 && headings) {
            device_line(fitted, width, NULL);
            paint_line(app, row, style, fitted);
            continue;
        } else if (app->row_count) {
            size_t index = app->top + row - (headings ? 4 : 3);
            if (index < app->row_count) {
                const struct Device *device = app->rows[index];
                device_line(fitted, width, device);
                style = index == app->selected ? app->theme.selection :
                        device->connected ? app->theme.highlight : app->theme.normal;
                paint_line(app, row, style, fitted);
                continue;
            }
        } else if (row == 3) {
            value = empty_list(app);
        }
        fit_text(fitted, value, strlen(value), width);
        paint_line(app, row, style, fitted);
    }
    app->screen_dirty = false;
    return fflush(stdout) == EOF || ferror(stdout) ? -1 : 0;
}

/* Input */

enum Key { KEY_NONE, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_ENTER, KEY_POWER, KEY_QUIT, KEY_ESCAPE };

struct Input {
    enum { INPUT_TEXT, INPUT_ESCAPE, INPUT_CSI } state;
    unsigned parameter;
    uint64_t deadline;
};

static uint64_t monotonic_us(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static enum Key input_key(struct Input *input, unsigned char byte)
{
    if (input->state == INPUT_TEXT) {
        switch (byte) {
        case 'k': return KEY_UP;
        case 'j': return KEY_DOWN;
        case 'g': return KEY_HOME;
        case 'G': return KEY_END;
        case 'p': return KEY_POWER;
        case '\r':
        case '\n': return KEY_ENTER;
        case 'q':
        case '\003': return KEY_QUIT;
        case '\033':
            input->state = INPUT_ESCAPE;
            input->parameter = 0;
            input->deadline = monotonic_us() + ESCAPE_TIMEOUT_US;
            break;
        }
        return KEY_NONE;
    }
    if (input->state == INPUT_ESCAPE) {
        if (byte == '[' || byte == 'O') {
            input->state = INPUT_CSI;
            input->deadline = monotonic_us() + ESCAPE_TIMEOUT_US;
            return KEY_NONE;
        }
        input->state = INPUT_TEXT;
        return KEY_ESCAPE;
    }
    if (byte >= '0' && byte <= '9') {
        if (input->parameter < 1000)
            input->parameter = input->parameter * 10 + (unsigned)(byte - '0');
        return KEY_NONE;
    }
    if (byte >= ' ' && byte <= '?')
        return KEY_NONE;
    input->state = INPUT_TEXT;
    switch (byte) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '~':
        if (input->parameter == 1 || input->parameter == 7)
            return KEY_HOME;
        if (input->parameter == 4 || input->parameter == 8)
            return KEY_END;
        break;
    }
    return KEY_NONE;
}

static void navigate(struct App *app, enum Key key)
{
    if (!app->row_count)
        return;
    size_t previous = app->selected;
    if (key == KEY_UP && app->selected)
        app->selected--;
    else if (key == KEY_DOWN && app->selected + 1 < app->row_count)
        app->selected++;
    else if (key == KEY_HOME)
        app->selected = 0;
    else if (key == KEY_END)
        app->selected = app->row_count - 1;
    if (app->selected != previous) {
        replace_string(&app->selected_path, app->rows[app->selected]->path);
        app->notice[0] = '\0';
        app->screen_dirty = true;
    }
}

/* Return true to quit. */
static bool handle_key(struct App *app, enum Key key)
{
    switch (key) {
    case KEY_QUIT:
    case KEY_ESCAPE:
        return true;
    case KEY_ENTER:
        if (app->bus)
            toggle_connection(app);
        break;
    case KEY_POWER:
        if (app->bus)
            toggle_power(app);
        break;
    default:
        navigate(app, key);
        break;
    }
    return false;
}

static int event_loop(struct App *app, const sigset_t *wait_mask)
{
    struct Input input = {0};
    while (!interrupted) {
        if (resized) {
            resized = 0;
            terminal_size(app);
        }
        if (app->model_dirty)
            rebuild_rows(app);
        if (app->screen_dirty && render(app) < 0)
            return -1;

        struct pollfd fds[] = {
            { .fd = STDIN_FILENO, .events = POLLIN },
            { .fd = -1 },
            { .fd = app->palette_watch.fd, .events = POLLIN },
        };
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

        struct timespec timeout, *wait = NULL;
        if (deadline != UINT64_MAX) {
            uint64_t now = monotonic_us(), remaining = deadline > now ? deadline - now : 0;
            timeout.tv_sec = (time_t)(remaining / 1000000);
            timeout.tv_nsec = (long)(remaining % 1000000) * 1000;
            wait = &timeout;
        }
        if (ppoll(fds, sizeof fds / sizeof fds[0], wait, wait_mask) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (fds[0].revents & POLLIN) {
            unsigned char bytes[128];
            ssize_t length = read(STDIN_FILENO, bytes, sizeof bytes);
            if (length <= 0)
                return length < 0 && errno != EINTR && errno != EAGAIN ? -1 : 0;
            for (ssize_t i = 0; i < length; i++)
                if (handle_key(app, input_key(&input, bytes[i])))
                    return 0;
        } else if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            return 0;
        }
        /* A lone Escape: nothing followed it in time. */
        if (input.state != INPUT_TEXT && monotonic_us() >= input.deadline) {
            bool lone = input.state == INPUT_ESCAPE;
            input.state = INPUT_TEXT;
            if (lone && handle_key(app, KEY_ESCAPE))
                return 0;
        }
        if (fds[2].revents && desk_watch_changed(&app->palette_watch, fds[2].revents))
            apply_theme(app);

        /* A bounded batch, so a burst of signals can't starve the keyboard. */
        for (unsigned i = 0; app->bus && i < 64; i++) {
            int result = sd_bus_process(app->bus, NULL);
            if (result < 0) {
                close_bus(app);
                set_notice(app, "Lost the system bus: %s", strerror(-result));
            }
            if (result <= 0)
                break;
        }
    }
    return 0;
}

static void usage(FILE *out)
{
    fputs("Usage: blue [-c PATH]\n"
          "\n"
          "Bluetooth devices in the terminal, over BlueZ.\n"
          "Enter connects or disconnects the selected device, p turns Bluetooth\n"
          "on or off, arrows or j/k move, Home/End or g/G jump, q or Escape quits.\n"
          "\n"
          "  -c PATH        read this config instead of ~/.config/blue/blue.conf\n"
          "  -h, --help     show this help\n"
          "  -v, --version  show the version\n", out);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "version", no_argument, NULL, 'v' },
        { 0 },
    };
    const char *override = NULL;
    int option;
    opterr = 0;
    while ((option = getopt_long(argc, argv, "c:hv", options, NULL)) != -1) {
        if (option == 'h') {
            usage(stdout);
            return EXIT_SUCCESS;
        }
        if (option == 'v') {
            puts("blue " BLUE_VERSION);
            return EXIT_SUCCESS;
        }
        if (option != 'c' || !*optarg) {
            usage(stderr);
            return 2;
        }
        override = optarg;
    }
    if (optind != argc) {
        usage(stderr);
        return 2;
    }

    struct App app = { .palette_watch = { .fd = -1 } };
    load_config(&app.config, override);
    setlocale(LC_CTYPE, "");
    if (app.config.wallpaper_theme) {
        app.palette_path = desk_palette_path();
        if (app.palette_path && !desk_watch_open(&app.palette_watch, app.palette_path))
            app.palette_watch.fd = -1;
    }
    apply_theme(&app);

    sigset_t wait_mask;
    if (setup_signals(&wait_mask) < 0 || terminal_enter() < 0) {
        int error = errno;
        terminal_restore();
        fprintf(stderr, "blue: terminal setup: %s\n", strerror(error));
        return EXIT_FAILURE;
    }
    terminal_size(&app);
    open_bus(&app);

    int result = event_loop(&app, &wait_mask);
    int error = errno;
    terminal_restore();
    close_bus(&app);
    desk_watch_close(&app.palette_watch);
    free(app.palette_path);
    free(app.devices);
    free(app.rows);
    free(app.selected_path);
    free(app.lines);
    if (result < 0) {
        fprintf(stderr, "blue: terminal I/O: %s\n", strerror(error));
        return EXIT_FAILURE;
    }
    return interrupted ? 128 + interrupted : EXIT_SUCCESS;
}
