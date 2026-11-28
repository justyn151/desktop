/* blue: a Bluetooth manager for the terminal, over BlueZ's D-Bus API.
   It lists the devices the adapter knows and the ones nearby, keeps them live
   from BlueZ's signals, pairs, connects, trusts, renames and removes them,
   and turns the adapter on and off. While it runs it is BlueZ's pairing agent,
   so passkeys and PINs are asked for here. It looks and behaves like net. */
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
#include <stddef.h>
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
    bool powered, discovering, pairable, discoverable;
};

enum Op { OP_NONE, OP_CONNECT, OP_DISCONNECT, OP_PAIR };

struct Device {
    char *path, *adapter, *address, *alias, *name, *icon;
    bool paired, bonded, trusted, connected;
    int battery;            /* Percent, or -1 without org.bluez.Battery1. */
    int rssi;               /* dBm while discovering, or INT16_MIN. */
    sd_bus_slot *request;   /* The Pair, Connect or Disconnect waiting for its reply. */
    enum Op op;             /* Which one that is. */
    bool failed;            /* The last attempt failed; shown until the next one. */
};

/* A line of the list: a device, or a heading or note that can't be selected. */
struct Item {
    struct Device *device;
    char text[64];
    bool heading;
};

/* A question on the line above the hints. Agent questions hold BlueZ's call
   until they're answered. */
enum PromptKind {
    PROMPT_NONE,
    PROMPT_CONFIRM,     /* RequestConfirmation: does the device show this passkey? */
    PROMPT_AUTHORIZE,   /* RequestAuthorization or AuthorizeService */
    PROMPT_DISPLAY,     /* DisplayPasskey or DisplayPinCode: type this on the device */
    PROMPT_PIN,         /* RequestPinCode */
    PROMPT_PASSKEY,     /* RequestPasskey */
    PROMPT_REMOVE,
    PROMPT_RENAME,
};

struct Prompt {
    enum PromptKind kind;
    sd_bus_message *request;
    char *device;
    uint32_t passkey;
    unsigned entered;       /* Keys typed so far, from DisplayPasskey. */
    char code[17];          /* A PIN to show. */
    char service[32];
    char text[PROMPT_TEXT_SIZE];
    size_t length;
    bool shown;             /* y only counts once the question has been drawn. */
};

struct Line {
    char text[320];
};

struct App {
    sd_bus *bus;
    sd_bus_slot *list_request, *power_request, *scan_request, *agent_request;
    char *bluez_owner;      /* Agent calls are only taken from this name. */
    bool bluez_running, listed, agent_registered;
    struct Adapter adapters[MAX_ADAPTERS];
    size_t adapter_count;
    struct Device **devices;
    size_t device_count, device_capacity;

    bool scanning, restore_pairable;
    uint64_t scan_deadline;

    struct Item *items;
    size_t item_count, item_capacity, selected, top;
    char *selected_path;
    struct Prompt prompt;

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
    fputs("\033[0m\033[?2004l\033[?1049l\033[?25h", stdout);
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

static uint64_t monotonic_us(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
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
    /* Bracketed paste, so a pasted PIN arrives as text and not as keys. */
    fputs("\033[?1049h\033[?25l\033[?2004h", stdout);
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

/* A short name for prompts and notices, so the question still fits. */
static const char *short_name(const struct Device *device, char *out)
{
    const char *name = device ? device_name(device) : "the device";
    fit_text(out, name, strlen(name), 28);
    return out;
}

static struct Adapter *find_adapter(struct App *app, const char *path)
{
    for (size_t i = 0; path && i < app->adapter_count; i++)
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
    for (size_t i = 0; path && i < app->device_count; i++)
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
    *device = (struct Device){ .battery = -1, .rssi = INT16_MIN };
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
    free(device->name);
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

/* Answer an agent call with an error, or drop a prompt of our own. */
static void close_prompt(struct App *app, const char *error)
{
    struct Prompt *prompt = &app->prompt;
    if (prompt->request) {
        if (error)
            sd_bus_reply_method_errorf(prompt->request, error, "Rejected by the user");
        sd_bus_message_unref(prompt->request);
    }
    free(prompt->device);
    explicit_bzero(prompt, sizeof *prompt);
    app->screen_dirty = true;
}

static void clear_model(struct App *app)
{
    close_prompt(app, BLUEZ_ERROR "Canceled");
    for (size_t i = 0; i < app->device_count; i++)
        free_device(app->devices[i]);
    app->device_count = 0;
    for (size_t i = 0; i < app->adapter_count; i++)
        free_adapter(&app->adapters[i]);
    app->adapter_count = 0;
    app->power_request = sd_bus_slot_unref(app->power_request);
    app->scan_request = sd_bus_slot_unref(app->scan_request);
    app->agent_request = sd_bus_slot_unref(app->agent_request);
    app->scanning = app->restore_pairable = app->agent_registered = false;
    app->listed = false;
    app->model_dirty = app->screen_dirty = true;
}

/* Devices the adapter remembers, as opposed to ones it only sees nearby. */
static bool known(const struct Device *device)
{
    return device->paired || device->bonded || device->trusted || device->connected;
}

static int compare_known(const void *left, const void *right)
{
    const struct Device *a = *(struct Device *const *)left, *b = *(struct Device *const *)right;
    if (a->connected != b->connected)
        return a->connected ? -1 : 1;
    int order = strcasecmp(device_name(a), device_name(b));
    return order ? order : strcmp(a->path, b->path);
}

/* Nearby devices: strongest signal first. */
static int compare_nearby(const void *left, const void *right)
{
    const struct Device *a = *(struct Device *const *)left, *b = *(struct Device *const *)right;
    if (a->rssi != b->rssi)
        return a->rssi > b->rssi ? -1 : 1;
    int order = strcasecmp(device_name(a), device_name(b));
    return order ? order : strcmp(a->path, b->path);
}

static struct Item *add_item(struct App *app)
{
    if (app->item_count == app->item_capacity) {
        app->item_capacity = app->item_capacity ? 2 * app->item_capacity : 32;
        app->items = allocate(app->items, app->item_capacity, sizeof *app->items);
    }
    struct Item *item = &app->items[app->item_count++];
    *item = (struct Item){0};
    return item;
}

static struct Device *selected_device(const struct App *app)
{
    return app->selected < app->item_count ? app->items[app->selected].device : NULL;
}

static void rebuild_rows(struct App *app)
{
    app->model_dirty = false;
    app->item_count = 0;
    const struct Adapter *adapter = current_adapter(app);
    struct Device **sorted = allocate(NULL, app->device_count, sizeof *sorted);
    size_t known_count = 0, nearby_count = 0, unnamed = 0;
    for (size_t i = 0; adapter && i < app->device_count; i++) {
        struct Device *device = app->devices[i];
        if (!device->adapter || strcmp(device->adapter, adapter->path))
            continue;
        if (known(device))
            sorted[known_count++] = device;
    }
    qsort(sorted, known_count, sizeof *sorted, compare_known);
    for (size_t i = 0; i < known_count; i++)
        add_item(app)->device = sorted[i];

    /* A device without a name is usually something nobody wants to pair with. */
    for (size_t i = 0; adapter && i < app->device_count; i++) {
        struct Device *device = app->devices[i];
        if (!device->adapter || strcmp(device->adapter, adapter->path) || known(device))
            continue;
        if (device->name || device->request)
            sorted[nearby_count++] = device;
        else
            unnamed++;
    }
    qsort(sorted, nearby_count, sizeof *sorted, compare_nearby);
    if (nearby_count || app->scanning) {
        struct Item *heading = add_item(app);
        heading->heading = true;
        if (unnamed)
            snprintf(heading->text, sizeof heading->text, "Nearby (%zu without a name hidden)", unnamed);
        else
            snprintf(heading->text, sizeof heading->text, "Nearby");
        for (size_t i = 0; i < nearby_count; i++)
            add_item(app)->device = sorted[i];
        if (!nearby_count)
            snprintf(add_item(app)->text, sizeof app->items[0].text, "Searching...");
    }
    free(sorted);

    /* Keep the selection on the same device as rows move, or on the first device. */
    size_t first = app->item_count;
    app->selected = app->item_count;
    for (size_t i = 0; i < app->item_count; i++) {
        struct Device *device = app->items[i].device;
        if (!device)
            continue;
        if (first == app->item_count)
            first = i;
        if (app->selected_path && !strcmp(device->path, app->selected_path))
            app->selected = i;
    }
    if (app->selected == app->item_count)
        app->selected = first;
    struct Device *device = selected_device(app);
    replace_string(&app->selected_path, device ? device->path : NULL);
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
    case 'n': {
        int16_t value;
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
    static const struct { const char *name; char type; size_t offset; } fields[] = {
        { "Alias", 's', offsetof(struct Adapter, alias) },
        { "Address", 's', offsetof(struct Adapter, address) },
        { "PowerState", 's', offsetof(struct Adapter, power_state) },
        { "Powered", 'b', offsetof(struct Adapter, powered) },
        { "Discovering", 'b', offsetof(struct Adapter, discovering) },
        { "Pairable", 'b', offsetof(struct Adapter, pairable) },
        { "Discoverable", 'b', offsetof(struct Adapter, discoverable) },
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
        if (!strcmp(name, fields[i].name))
            return read_variant(message, fields[i].type, (char *)adapter + fields[i].offset);
    return sd_bus_message_skip(message, "v");
}

static int read_device_property(struct Device *device, const char *name, sd_bus_message *message)
{
    static const struct { const char *name; char type; size_t offset; } fields[] = {
        { "Adapter", 'o', offsetof(struct Device, adapter) },
        { "Address", 's', offsetof(struct Device, address) },
        { "Alias", 's', offsetof(struct Device, alias) },
        { "Name", 's', offsetof(struct Device, name) },
        { "Icon", 's', offsetof(struct Device, icon) },
        { "RSSI", 'n', offsetof(struct Device, rssi) },
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

/* BlueZ's errors, in words. */
static const char *bluez_error(const sd_bus_error *error)
{
    static const struct { const char *name, *text; } names[] = {
        { BLUEZ_ERROR "InProgress", "already in progress" },
        { BLUEZ_ERROR "NotReady", "Bluetooth is off" },
        { BLUEZ_ERROR "Blocked", "Bluetooth is blocked; try rfkill unblock bluetooth" },
        { BLUEZ_ERROR "AuthenticationCanceled", "pairing was canceled" },
        { BLUEZ_ERROR "AuthenticationFailed", "wrong code, or the device refused" },
        { BLUEZ_ERROR "AuthenticationRejected", "the device refused" },
        { BLUEZ_ERROR "AuthenticationTimeout", "no answer from the device in time" },
        { BLUEZ_ERROR "ConnectionAttemptFailed", "the device is off or out of range" },
        { BLUEZ_ERROR "DoesNotExist", "the device is gone" },
        { "org.freedesktop.DBus.Error.NoReply", "no answer in time" },
        { "org.freedesktop.DBus.Error.Timeout", "no answer in time" },
        { "org.freedesktop.DBus.Error.AccessDenied", "not allowed by D-Bus policy" },
    };
    const char *name = error->name ? error->name : "", *message = error->message ? error->message : "";
    if (strstr(message, "page-timeout") || strstr(message, "Host is down") ||
        strstr(message, "le-connection-abort") || strstr(message, "Connection refused"))
        return "the device is off or out of range";
    if (strstr(message, "profile-unavailable"))
        return "no profile it can use; is PipeWire running?";
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(name, names[i].name))
            return names[i].text;
    return *message ? message : *name ? name : "unknown error";
}

/* Calls */

/* A call whose answer only matters if it fails. */
static int on_simple_reply(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    const sd_bus_error *error = sd_bus_message_get_error(message);
    if (error)
        set_notice(app, "BlueZ refused: %s", bluez_error(error));
    return 0;
}

/* Set a property, fire and forget: the change arrives as a signal. */
static void set_property(struct App *app, const char *path, const char *interface,
                         const char *name, const char *type, ...)
{
    sd_bus_message *call = NULL;
    va_list args;
    va_start(args, type);
    int result = sd_bus_message_new_method_call(app->bus, &call, BLUEZ, path, PROPERTIES, "Set");
    if (result >= 0)
        result = sd_bus_message_append(call, "ss", interface, name);
    if (result >= 0)
        result = sd_bus_message_open_container(call, 'v', type);
    if (result >= 0)
        result = sd_bus_message_appendv(call, type, args);
    if (result >= 0)
        result = sd_bus_message_close_container(call);
    if (result >= 0)
        result = sd_bus_call_async(app->bus, NULL, call, on_simple_reply, app, CALL_TIMEOUT_US);
    va_end(args);
    sd_bus_message_unref(call);
    if (result < 0)
        set_notice(app, "Couldn't ask BlueZ: %s", strerror(-result));
}

/* The reply handler needs the app as well as the device; this pairs them. */
struct Pending {
    struct App *app;
    struct Device *device;
};

static void start_device_call(struct App *app, struct Device *device, enum Op op);

static int on_device_reply(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct Pending *pending = userdata;
    struct App *app = pending->app;
    struct Device *device = pending->device;
    enum Op op = device->op;
    const sd_bus_error *error = sd_bus_message_get_error(message);
    char name[4 * 28 + 1];
    short_name(device, name);
    /* The slot owns pending; take what's needed first. */
    device->request = sd_bus_slot_unref(device->request);
    device->op = OP_NONE;
    app->model_dirty = app->screen_dirty = true;

    if (op == OP_PAIR) {
        if (app->prompt.kind == PROMPT_DISPLAY && app->prompt.device &&
            !strcmp(app->prompt.device, device->path))
            close_prompt(app, NULL);
        if (error && strcmp(error->name, BLUEZ_ERROR "AlreadyExists")) {
            device->failed = true;
            set_notice(app, "Couldn't pair with %s: %s", name, bluez_error(error));
            return 0;
        }
        /* Paired: trust it so it may reconnect on its own, then connect. */
        set_property(app, device->path, BLUEZ_DEVICE, "Trusted", "b", 1);
        start_device_call(app, device, OP_CONNECT);
        return 0;
    }
    device->failed = error != NULL;
    if (error)
        set_notice(app, "Couldn't %s %s: %s", op == OP_CONNECT ? "connect to" : "disconnect",
                   name, bluez_error(error));
    else
        set_notice(app, "%s %s", op == OP_CONNECT ? "Connected to" : "Disconnected", name);
    return 0;
}

static void free_pending(void *userdata)
{
    free(userdata);
}

static void start_device_call(struct App *app, struct Device *device, enum Op op)
{
    static const char *const methods[] = { [OP_CONNECT] = "Connect", [OP_DISCONNECT] = "Disconnect", [OP_PAIR] = "Pair" };
    static const uint64_t timeouts[] = { [OP_CONNECT] = CONNECT_TIMEOUT_US, [OP_DISCONNECT] = CALL_TIMEOUT_US, [OP_PAIR] = PAIR_TIMEOUT_US };
    struct Pending *pending = allocate(NULL, 1, sizeof *pending);
    *pending = (struct Pending){ app, device };
    sd_bus_message *call = NULL;
    int result = sd_bus_message_new_method_call(app->bus, &call, BLUEZ, device->path, BLUEZ_DEVICE, methods[op]);
    if (result >= 0)
        result = sd_bus_call_async(app->bus, &device->request, call, on_device_reply, pending, timeouts[op]);
    sd_bus_message_unref(call);
    char name[4 * 28 + 1];
    short_name(device, name);
    if (result < 0) {
        free(pending);
        set_notice(app, "Couldn't ask BlueZ: %s", strerror(-result));
        return;
    }
    sd_bus_slot_set_destroy_callback(device->request, free_pending);
    device->op = op;
    device->failed = false;
    app->model_dirty = true;
    set_notice(app, "%s %s...", op == OP_PAIR ? "Pairing with" : op == OP_CONNECT ? "Connecting to" : "Disconnecting", name);
}

/* Enter: pair a nearby device, or connect or disconnect a known one. */
static void activate_selected(struct App *app)
{
    struct Device *device = selected_device(app);
    struct Adapter *adapter = current_adapter(app);
    if (!device)
        return;
    char name[4 * 28 + 1];
    short_name(device, name);
    if (device->request) {
        set_notice(app, "Still working on %s...", name);
        return;
    }
    bool off = !adapter || !adapter->powered;
    if (!known(device) || !device->paired) {
        if (off) {
            set_notice(app, "Bluetooth is off. Press p to turn it on");
            return;
        }
        /* Trusted but never paired (set up by hand): connect without pairing. */
        start_device_call(app, device, known(device) && !device->connected && device->trusted &&
                          !device->bonded ? OP_CONNECT : device->connected ? OP_DISCONNECT : OP_PAIR);
        return;
    }
    if (!device->connected && off) {
        set_notice(app, "Bluetooth is off. Press p to turn it on");
        return;
    }
    start_device_call(app, device, device->connected ? OP_DISCONNECT : OP_CONNECT);
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

/* Searching */

static void stop_scan(struct App *app, const char *notice)
{
    struct Adapter *adapter = current_adapter(app);
    if (!app->scanning)
        return;
    app->scanning = false;
    app->scan_request = sd_bus_slot_unref(app->scan_request);
    if (adapter) {
        sd_bus_call_method_async(app->bus, NULL, BLUEZ, adapter->path, BLUEZ_ADAPTER,
                                 "StopDiscovery", NULL, NULL, "");
        /* Pairable only while searching, as it was before. */
        if (app->restore_pairable)
            set_property(app, adapter->path, BLUEZ_ADAPTER, "Pairable", "b", 0);
    }
    app->restore_pairable = false;
    app->model_dirty = true;
    set_notice(app, "%s", notice);
}

static int on_scan_reply(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    app->scan_request = sd_bus_slot_unref(app->scan_request);
    const sd_bus_error *error = sd_bus_message_get_error(message);
    if (error) {
        app->scanning = true;  /* So stop_scan undoes Pairable. */
        stop_scan(app, "");
        set_notice(app, "Couldn't search: %s", bluez_error(error));
    }
    return 0;
}

static void start_scan(struct App *app)
{
    struct Adapter *adapter = current_adapter(app);
    if (!adapter)
        return;
    if (!adapter->powered) {
        set_notice(app, "Bluetooth is off. Press p to turn it on");
        return;
    }
    if (!adapter->pairable) {
        set_property(app, adapter->path, BLUEZ_ADAPTER, "Pairable", "b", 1);
        app->restore_pairable = true;
    }
    int result = sd_bus_call_method_async(app->bus, &app->scan_request, BLUEZ, adapter->path,
                                          BLUEZ_ADAPTER, "StartDiscovery", on_scan_reply, app, "");
    if (result < 0) {
        set_notice(app, "Couldn't ask BlueZ: %s", strerror(-result));
        return;
    }
    app->scanning = true;
    app->scan_deadline = monotonic_us() + SCAN_TIMEOUT_US;
    app->model_dirty = true;
    set_notice(app, "Searching for a minute. Put the device in pairing mode");
}

/* The pairing agent */

static bool from_bluez(const struct App *app, sd_bus_message *message)
{
    const char *sender = sd_bus_message_get_sender(message);
    return sender && app->bluez_owner && !strcmp(sender, app->bluez_owner);
}

/* Start a question from BlueZ. Only one at a time: a second one is refused. */
static int begin_agent_prompt(struct App *app, sd_bus_message *message, enum PromptKind kind,
                              const char *device)
{
    if (!from_bluez(app, message))
        return sd_bus_reply_method_errorf(message, BLUEZ_ERROR "Rejected", "Not from BlueZ");
    if (app->prompt.request)
        return sd_bus_reply_method_errorf(message, BLUEZ_ERROR "Rejected", "Busy with another request");
    close_prompt(app, NULL);
    app->prompt.kind = kind;
    replace_string(&app->prompt.device, device);
    app->prompt.request = sd_bus_message_ref(message);
    app->notice[0] = '\0';
    return 1;  /* Answered later, from the keyboard. */
}

static int agent_release(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    app->agent_registered = false;
    close_prompt(app, BLUEZ_ERROR "Canceled");
    return sd_bus_reply_method_return(message, "");
}

static int agent_request_pin(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    const char *device;
    int result = sd_bus_message_read(message, "o", &device);
    return result < 0 ? result : begin_agent_prompt(userdata, message, PROMPT_PIN, device);
}

static int agent_request_passkey(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    const char *device;
    int result = sd_bus_message_read(message, "o", &device);
    return result < 0 ? result : begin_agent_prompt(userdata, message, PROMPT_PASSKEY, device);
}

static int agent_request_confirmation(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    const char *device;
    uint32_t passkey;
    int result = sd_bus_message_read(message, "ou", &device, &passkey);
    if (result < 0)
        return result;
    result = begin_agent_prompt(app, message, PROMPT_CONFIRM, device);
    if (result > 0)
        app->prompt.passkey = passkey;
    return result;
}

static int agent_request_authorization(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    const char *device;
    int result = sd_bus_message_read(message, "o", &device);
    return result < 0 ? result : begin_agent_prompt(userdata, message, PROMPT_AUTHORIZE, device);
}

/* What a service UUID is for, in a word or two. */
static const char *service_name(const char *uuid)
{
    static const struct { const char *prefix, *name; } services[] = {
        { "0000110a", "audio" }, { "0000110b", "audio" }, { "0000110d", "audio" },
        { "0000110c", "media controls" }, { "0000110e", "media controls" },
        { "0000111e", "calls" }, { "0000111f", "calls" }, { "00001108", "calls" },
        { "00001112", "calls" }, { "00001124", "input" }, { "00001812", "input" },
        { "00001105", "file transfer" }, { "00001106", "file transfer" },
        { "00001115", "networking" }, { "00001116", "networking" },
    };
    for (size_t i = 0; i < sizeof services / sizeof services[0]; i++)
        if (!strncasecmp(uuid, services[i].prefix, 8))
            return services[i].name;
    return "a service";
}

static int agent_authorize_service(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    const char *device, *uuid;
    int result = sd_bus_message_read(message, "os", &device, &uuid);
    if (result < 0)
        return result;
    result = begin_agent_prompt(app, message, PROMPT_AUTHORIZE, device);
    if (result > 0)
        snprintf(app->prompt.service, sizeof app->prompt.service, "%s", service_name(uuid));
    return result;
}

/* Display calls are answered at once; the code stays on screen until pairing ends. */
static int agent_display(struct App *app, sd_bus_message *message, const char *device)
{
    if (!from_bluez(app, message))
        return sd_bus_reply_method_errorf(message, BLUEZ_ERROR "Rejected", "Not from BlueZ");
    if (app->prompt.kind != PROMPT_DISPLAY || !app->prompt.device || strcmp(app->prompt.device, device)) {
        if (app->prompt.request)
            return sd_bus_reply_method_errorf(message, BLUEZ_ERROR "Rejected", "Busy with another request");
        close_prompt(app, NULL);
        app->prompt.kind = PROMPT_DISPLAY;
        replace_string(&app->prompt.device, device);
    }
    app->screen_dirty = true;
    return sd_bus_reply_method_return(message, "");
}

static int agent_display_pin(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    const char *device, *pin;
    int result = sd_bus_message_read(message, "os", &device, &pin);
    if (result < 0)
        return result;
    result = agent_display(app, message, device);
    if (app->prompt.kind == PROMPT_DISPLAY)
        snprintf(app->prompt.code, sizeof app->prompt.code, "%s", pin);
    return result;
}

static int agent_display_passkey(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    const char *device;
    uint32_t passkey;
    uint16_t entered;
    int result = sd_bus_message_read(message, "ouq", &device, &passkey, &entered);
    if (result < 0)
        return result;
    result = agent_display(app, message, device);
    if (app->prompt.kind == PROMPT_DISPLAY) {
        snprintf(app->prompt.code, sizeof app->prompt.code, "%06u", passkey % 1000000);
        app->prompt.entered = entered;
    }
    return result;
}

static int agent_cancel(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    if (!from_bluez(app, message))
        return sd_bus_reply_method_errorf(message, BLUEZ_ERROR "Rejected", "Not from BlueZ");
    if (app->prompt.kind >= PROMPT_CONFIRM && app->prompt.kind <= PROMPT_PASSKEY) {
        /* BlueZ gave up on the call, so there's nothing left to answer. */
        app->prompt.request = sd_bus_message_unref(app->prompt.request);
        close_prompt(app, NULL);
        set_notice(app, "Pairing canceled");
    }
    return sd_bus_reply_method_return(message, "");
}

static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", agent_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPinCode", "o", "s", agent_request_pin, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPinCode", "os", "", agent_display_pin, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey", "o", "u", agent_request_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_display_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", agent_request_confirmation, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o", "", agent_request_authorization, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService", "os", "", agent_authorize_service, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", agent_cancel, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

static int on_default_agent(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    app->agent_request = sd_bus_slot_unref(app->agent_request);
    const sd_bus_error *error = sd_bus_message_get_error(message);
    app->agent_registered = !error;
    if (error)
        set_notice(app, "Can't answer pairing requests: %s", bluez_error(error));
    return 0;
}

static int on_agent_registered(sd_bus_message *message, void *userdata, sd_bus_error *unused)
{
    (void)unused;
    struct App *app = userdata;
    app->agent_request = sd_bus_slot_unref(app->agent_request);
    const sd_bus_error *error = sd_bus_message_get_error(message);
    if (error && strcmp(error->name, BLUEZ_ERROR "AlreadyExists")) {
        set_notice(app, "Can't answer pairing requests: %s", bluez_error(error));
        return 0;
    }
    /* The default agent also answers pairing that a phone or keyboard starts. */
    if (sd_bus_call_method_async(app->bus, &app->agent_request, BLUEZ, BLUEZ_PATH, BLUEZ_AGENT_MANAGER,
                                 "RequestDefaultAgent", on_default_agent, app, "o", AGENT_PATH) < 0)
        set_notice(app, "Can't answer pairing requests");
    return 0;
}

static void register_agent(struct App *app)
{
    app->agent_request = sd_bus_slot_unref(app->agent_request);
    if (sd_bus_call_method_async(app->bus, &app->agent_request, BLUEZ, BLUEZ_PATH, BLUEZ_AGENT_MANAGER,
                                 "RegisterAgent", on_agent_registered, app, "os", AGENT_PATH,
                                 "KeyboardDisplay") < 0)
        set_notice(app, "Can't answer pairing requests");
}

/* D-Bus */

static void close_bus(struct App *app)
{
    clear_model(app);
    app->list_request = sd_bus_slot_unref(app->list_request);
    app->bus = sd_bus_flush_close_unref(app->bus);
    app->bluez_running = false;
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
    replace_string(&app->bluez_owner, sd_bus_message_get_sender(message));
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
    register_agent(app);
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
            if (app->prompt.device && !strcmp(app->prompt.device, path) && !app->prompt.request)
                close_prompt(app, NULL);
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
    replace_string(&app->bluez_owner, NULL);
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
    if (sd_bus_add_object_vtable(app->bus, NULL, AGENT_PATH, BLUEZ_AGENT, agent_vtable, app) < 0 ||
        sd_bus_match_signal(app->bus, NULL, BLUEZ, NULL, OBJECT_MANAGER, "InterfacesAdded",
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

/* Prompts and the other actions */

static void answer_prompt(struct App *app, bool yes)
{
    struct Prompt *prompt = &app->prompt;
    struct Device *device = find_device(app, prompt->device);
    char name[4 * 28 + 1];
    short_name(device, name);
    switch (prompt->kind) {
    case PROMPT_CONFIRM:
    case PROMPT_AUTHORIZE:
        if (!yes) {
            close_prompt(app, BLUEZ_ERROR "Rejected");
            set_notice(app, "Refused %s", name);
            return;
        }
        sd_bus_reply_method_return(prompt->request, "");
        close_prompt(app, NULL);
        if (device && device->op == OP_PAIR)
            set_notice(app, "Pairing with %s...", name);
        return;
    case PROMPT_REMOVE:
        if (yes && device && device->adapter)
            sd_bus_call_method_async(app->bus, NULL, BLUEZ, device->adapter, BLUEZ_ADAPTER,
                                     "RemoveDevice", on_simple_reply, app, "o", device->path);
        close_prompt(app, NULL);
        if (yes)
            set_notice(app, "Removed %s", name);
        return;
    default:
        return;
    }
}

/* Enter in a text prompt. */
static void submit_prompt(struct App *app)
{
    struct Prompt *prompt = &app->prompt;
    struct Device *device = find_device(app, prompt->device);
    switch (prompt->kind) {
    case PROMPT_PIN:
        if (!prompt->length)
            return;
        sd_bus_reply_method_return(prompt->request, "s", prompt->text);
        close_prompt(app, NULL);
        return;
    case PROMPT_PASSKEY: {
        if (!prompt->length)
            return;
        unsigned long passkey = strtoul(prompt->text, NULL, 10);
        sd_bus_reply_method_return(prompt->request, "u", (uint32_t)passkey);
        close_prompt(app, NULL);
        return;
    }
    case PROMPT_RENAME:
        /* An empty name gives the device back the one it announces. */
        if (device)
            set_property(app, device->path, BLUEZ_DEVICE, "Alias", "s", prompt->text);
        close_prompt(app, NULL);
        return;
    default:
        return;
    }
}

/* Escape: refuse, cancel, or just close. */
static void cancel_prompt(struct App *app)
{
    struct Prompt *prompt = &app->prompt;
    if (prompt->kind == PROMPT_DISPLAY && prompt->device)
        sd_bus_call_method_async(app->bus, NULL, BLUEZ, prompt->device, BLUEZ_DEVICE,
                                 "CancelPairing", NULL, NULL, "");
    bool agent = prompt->request != NULL;
    close_prompt(app, agent && (prompt->kind == PROMPT_CONFIRM || prompt->kind == PROMPT_AUTHORIZE) ?
                      BLUEZ_ERROR "Rejected" : BLUEZ_ERROR "Canceled");
    if (agent)
        set_notice(app, "Pairing canceled");
}

static bool text_prompt(const struct Prompt *prompt)
{
    return prompt->kind == PROMPT_PIN || prompt->kind == PROMPT_PASSKEY || prompt->kind == PROMPT_RENAME;
}

/* A typed or pasted byte in a text prompt. PINs are up to 16 characters,
   passkeys up to six digits, names whatever BlueZ takes. */
static void edit_prompt(struct App *app, unsigned char byte)
{
    struct Prompt *prompt = &app->prompt;
    size_t limit = prompt->kind == PROMPT_PIN ? 16 : prompt->kind == PROMPT_PASSKEY ? 6 :
                   sizeof prompt->text - 1;
    if (byte == '\b' || byte == 0x7f) {
        while (prompt->length && ((unsigned char)prompt->text[--prompt->length] & 0xc0) == 0x80)
            continue;
        prompt->text[prompt->length] = '\0';
    } else if (byte >= ' ' && prompt->length < limit &&
               (prompt->kind != PROMPT_PASSKEY || isdigit(byte))) {
        prompt->text[prompt->length++] = (char)byte;
        prompt->text[prompt->length] = '\0';
    } else {
        return;
    }
    app->screen_dirty = true;
}

static void start_prompt(struct App *app, enum PromptKind kind)
{
    struct Device *device = selected_device(app);
    if (!device || app->prompt.kind != PROMPT_NONE)
        return;
    app->prompt.kind = kind;
    replace_string(&app->prompt.device, device->path);
    if (kind == PROMPT_RENAME) {
        snprintf(app->prompt.text, sizeof app->prompt.text, "%s", device_name(device));
        app->prompt.length = strlen(app->prompt.text);
    }
    app->notice[0] = '\0';
    app->screen_dirty = true;
}

static void toggle_trust(struct App *app)
{
    struct Device *device = selected_device(app);
    if (!device)
        return;
    char name[4 * 28 + 1];
    set_property(app, device->path, BLUEZ_DEVICE, "Trusted", "b", (int)!device->trusted);
    set_notice(app, device->trusted ? "%s will ask before connecting" : "%s may connect on its own",
               short_name(device, name));
}

static void toggle_visible(struct App *app)
{
    struct Adapter *adapter = current_adapter(app);
    if (!adapter)
        return;
    if (!adapter->powered) {
        set_notice(app, "Bluetooth is off. Press p to turn it on");
        return;
    }
    set_property(app, adapter->path, BLUEZ_ADAPTER, "Discoverable", "b", (int)!adapter->discoverable);
    set_notice(app, adapter->discoverable ? "Hidden from other devices" :
               "Visible to other devices for 3 minutes");
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
    switch (device->op) {
    case OP_PAIR: return "Pairing";
    case OP_CONNECT: return "Connecting";
    case OP_DISCONNECT: return "Leaving";
    case OP_NONE: break;
    }
    if (device->connected)
        return "Connected";
    if (device->failed)
        return "Failed";
    if (known(device))
        return device->trusted ? "" : "Not trusted";
    if (device->rssi == INT16_MIN)
        return "";
    return device->rssi >= -60 ? "Strong" : device->rssi >= -75 ? "Good" : "Weak";
}

/* One row of the table, or its headings when device is NULL. */
static void device_line(char *text, unsigned width, const struct Device *device)
{
    unsigned state_width = width >= 30 ? 13 : 0;
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
    for (size_t i = 0; i < app->device_count; i++)
        connected += app->devices[i]->connected;
    char name[4 * 24 + 1];
    const char *label = adapter->alias ? adapter->alias : adapter->path;
    fit_text(name, label, strlen(label), 24);
    snprintf(text, size, "Bluetooth on | %s | %zu connected%s%s", name, connected,
             adapter->discovering ? " | searching" : "", adapter->discoverable ? " | visible" : "");
    return text;
}

static const char *empty_list(struct App *app)
{
    const struct Adapter *adapter = current_adapter(app);
    if (!app->listed || !adapter)
        return "";
    return "No paired devices. Press s to search for one";
}

/* The question on the line above the hints. */
static const char *prompt_text(struct App *app, char *text, size_t size)
{
    struct Prompt *prompt = &app->prompt;
    char name[4 * 28 + 1];
    short_name(find_device(app, prompt->device), name);
    switch (prompt->kind) {
    case PROMPT_CONFIRM:
        snprintf(text, size, "Does %s show %06u?", name, prompt->passkey % 1000000);
        break;
    case PROMPT_AUTHORIZE:
        if (prompt->service[0])
            snprintf(text, size, "Allow %s to use %s?", name, prompt->service);
        else
            snprintf(text, size, "Pair with %s?", name);
        break;
    case PROMPT_DISPLAY:
        if (prompt->entered)
            snprintf(text, size, "Type %s on %s, then Enter (%u typed)", prompt->code, name, prompt->entered);
        else
            snprintf(text, size, "Type %s on %s, then Enter", prompt->code, name);
        break;
    case PROMPT_PIN:
        snprintf(text, size, "PIN for %s (often 0000): %s_", name, prompt->text);
        break;
    case PROMPT_PASSKEY:
        snprintf(text, size, "Passkey shown on %s: %s_", name, prompt->text);
        break;
    case PROMPT_REMOVE:
        snprintf(text, size, "Remove %s? It will need pairing again", name);
        break;
    case PROMPT_RENAME:
        snprintf(text, size, "Name: %s_", prompt->text);
        break;
    case PROMPT_NONE:
        return app->notice;
    }
    return text;
}

static const char *shortcut_hints(const struct App *app, unsigned width)
{
    switch (app->prompt.kind) {
    case PROMPT_CONFIRM: return "y Pair   n/Esc Refuse";
    case PROMPT_AUTHORIZE: return "y Allow   n/Esc Refuse";
    case PROMPT_DISPLAY: return "Esc Cancel";
    case PROMPT_PIN:
    case PROMPT_PASSKEY: return "Enter Send   Esc Cancel";
    case PROMPT_REMOVE: return "y Remove   n/Esc Keep";
    case PROMPT_RENAME: return "Enter Save (empty for its own name)   Esc Cancel";
    case PROMPT_NONE: break;
    }
    const struct Device *device = selected_device(app);
    const char *enter = !device ? "" : !known(device) ? "Enter Pair  " :
                        device->connected ? "Enter Disconnect  " : "Enter Connect  ";
    static char text[128];
    if (width >= 78)
        snprintf(text, sizeof text, "j/k  %ss Search  t Trust  r Rename  x Remove  p Power  q Quit", enter);
    else if (width >= 56)
        snprintf(text, sizeof text, "%ss Search  t r x  v Visible  p Power  q", enter);
    else
        snprintf(text, sizeof text, "Enter  s  p  q");
    return text;
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
    bool headings = app->item_count && body >= 2;
    size_t visible = body - (headings ? 1 : 0);
    if (app->item_count <= visible) {
        app->top = 0;
    } else if (visible) {
        if (app->top > app->item_count - visible)
            app->top = app->item_count - visible;
        if (app->selected < app->item_count && app->selected < app->top)
            app->top = app->selected;
        if (app->selected < app->item_count && app->selected - app->top >= visible)
            app->top = app->selected - visible + 1;
    }

    char text[NOTICE_SIZE + PROMPT_TEXT_SIZE], fitted[sizeof app->lines[0].text];
    for (unsigned row = 1; row <= app->height; row++) {
        const char *style = app->theme.muted, *value = "";
        if (row == 1 && app->height > 1) {
            style = app->theme.highlight;
            value = "Bluetooth / Devices";
        } else if (row >= footer) {
            if (row == footer) {
                value = prompt_text(app, text, sizeof text);
                if (*value)
                    style = app->theme.highlight;
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
        } else if (app->item_count) {
            size_t index = app->top + row - (headings ? 4 : 3);
            if (index < app->item_count) {
                const struct Item *item = &app->items[index];
                if (item->device) {
                    device_line(fitted, width, item->device);
                    style = index == app->selected ? app->theme.selection :
                            item->device->connected ? app->theme.highlight : app->theme.normal;
                    paint_line(app, row, style, fitted);
                    continue;
                }
                style = item->heading ? app->theme.highlight : app->theme.muted;
                value = item->text;
            }
        } else if (row == 3) {
            value = empty_list(app);
        }
        fit_text(fitted, value, strlen(value), width);
        paint_line(app, row, style, fitted);
    }
    app->prompt.shown = app->prompt.kind != PROMPT_NONE;
    app->screen_dirty = false;
    return fflush(stdout) == EOF || ferror(stdout) ? -1 : 0;
}

/* Input */

enum Key {
    KEY_NONE, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_ENTER, KEY_POWER, KEY_QUIT, KEY_ESCAPE,
    KEY_SEARCH, KEY_TRUST, KEY_RENAME, KEY_REMOVE, KEY_VISIBLE, KEY_YES, KEY_NO,
    KEY_PASTE_START, KEY_PASTE_END,
};

struct Input {
    enum { INPUT_TEXT, INPUT_ESCAPE, INPUT_CSI } state;
    unsigned parameter;
    bool paste;
    uint64_t deadline;
};

static enum Key input_key(struct Input *input, unsigned char byte)
{
    if (input->state == INPUT_TEXT) {
        switch (byte) {
        case 'k': return KEY_UP;
        case 'j': return KEY_DOWN;
        case 'g': return KEY_HOME;
        case 'G': return KEY_END;
        case 'p': return KEY_POWER;
        case 's': return KEY_SEARCH;
        case 't': return KEY_TRUST;
        case 'r': return KEY_RENAME;
        case 'x': return KEY_REMOVE;
        case 'v': return KEY_VISIBLE;
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
        if (input->parameter == 200)
            return KEY_PASTE_START;
        if (input->parameter == 201)
            return KEY_PASTE_END;
        break;
    }
    return KEY_NONE;
}

static void navigate(struct App *app, enum Key key)
{
    size_t previous = app->selected, count = app->item_count;
    if (key == KEY_UP) {
        for (size_t i = app->selected; i-- > 0;)
            if (app->items[i].device) {
                app->selected = i;
                break;
            }
    } else if (key == KEY_DOWN) {
        for (size_t i = app->selected + 1; i < count; i++)
            if (app->items[i].device) {
                app->selected = i;
                break;
            }
    } else if (key == KEY_HOME || key == KEY_END) {
        for (size_t n = 0; n < count; n++) {
            size_t i = key == KEY_HOME ? n : count - 1 - n;
            if (app->items[i].device) {
                app->selected = i;
                break;
            }
        }
    }
    if (app->selected != previous && app->selected < count) {
        replace_string(&app->selected_path, app->items[app->selected].device->path);
        app->notice[0] = '\0';
        app->screen_dirty = true;
    }
}

/* Return true to quit. */
static bool handle_key(struct App *app, enum Key key)
{
    struct Prompt *prompt = &app->prompt;
    if (key == KEY_QUIT && !text_prompt(prompt))
        return true;
    if (prompt->kind != PROMPT_NONE) {
        if (key == KEY_ESCAPE || key == KEY_QUIT || (key == KEY_NO && !text_prompt(prompt)))
            cancel_prompt(app);
        else if (key == KEY_YES && prompt->shown && !text_prompt(prompt))
            answer_prompt(app, true);
        else if (key == KEY_ENTER && text_prompt(prompt))
            submit_prompt(app);
        return false;
    }
    if (!app->bus)
        return key == KEY_ESCAPE;
    switch (key) {
    case KEY_ESCAPE: return true;
    case KEY_ENTER: activate_selected(app); break;
    case KEY_POWER: toggle_power(app); break;
    case KEY_SEARCH:
        if (app->scanning)
            stop_scan(app, "Stopped searching");
        else
            start_scan(app);
        break;
    case KEY_TRUST: toggle_trust(app); break;
    case KEY_RENAME: start_prompt(app, PROMPT_RENAME); break;
    case KEY_REMOVE: start_prompt(app, PROMPT_REMOVE); break;
    case KEY_VISIBLE: toggle_visible(app); break;
    default: navigate(app, key); break;
    }
    return false;
}

/* Return true to quit. Text prompts take printable bytes before they become keys. */
static bool handle_byte(struct App *app, struct Input *input, unsigned char byte)
{
    if (input->state == INPUT_TEXT && text_prompt(&app->prompt) && byte != '\033' &&
        byte != '\r' && byte != '\n' && byte != '\003') {
        edit_prompt(app, byte);
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
        if (resized) {
            resized = 0;
            terminal_size(app);
        }
        if (app->scanning && monotonic_us() >= app->scan_deadline)
            stop_scan(app, "Stopped searching after a minute");
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
        if (app->scanning && app->scan_deadline < deadline)
            deadline = app->scan_deadline;

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
            bool quit = false;
            for (ssize_t i = 0; i < length && !quit; i++)
                quit = handle_byte(app, &input, bytes[i]);
            explicit_bzero(bytes, sizeof bytes);
            if (quit)
                return 0;
        } else if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            return 0;
        }
        /* A lone Escape: nothing followed it in time. */
        if (input.state != INPUT_TEXT && monotonic_us() >= input.deadline) {
            bool lone = input.state == INPUT_ESCAPE;
            input.state = INPUT_TEXT;
            if (lone && !input.paste && handle_key(app, KEY_ESCAPE))
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

/* Leave the adapter as it was: not pairable if blue made it so. Discovery and
   the agent end with the connection. */
static void restore_adapter(struct App *app)
{
    struct Adapter *adapter = current_adapter(app);
    if (!app->bus || !adapter || !app->restore_pairable)
        return;
    sd_bus_call_method(app->bus, BLUEZ, adapter->path, PROPERTIES, "Set", NULL, NULL, "ssv",
                       BLUEZ_ADAPTER, "Pairable", "b", 0);
}

static void usage(FILE *out)
{
    fputs("Usage: blue [-c PATH]\n"
          "\n"
          "Bluetooth devices in the terminal, over BlueZ.\n"
          "Enter pairs a nearby device, or connects or disconnects a known one.\n"
          "s searches for a minute, t trusts, r renames, x removes, v makes this\n"
          "computer visible, p turns Bluetooth on or off. Arrows or j/k move,\n"
          "Home/End or g/G jump, q or Escape quits. While blue runs, pairing\n"
          "requests are answered here.\n"
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
    restore_adapter(&app);
    close_bus(&app);
    desk_watch_close(&app.palette_watch);
    free(app.palette_path);
    free(app.bluez_owner);
    free(app.devices);
    free(app.items);
    free(app.selected_path);
    free(app.lines);
    if (result < 0) {
        fprintf(stderr, "blue: terminal I/O: %s\n", strerror(error));
        return EXIT_FAILURE;
    }
    return interrupted ? 128 + interrupted : EXIT_SUCCESS;
}
