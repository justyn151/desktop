/* vol: audio and volume control. The command line reads and changes the
   default output or input and follows it for bar; without a command in a
   terminal, vol is a mixer for every output, input, stream and card. It talks
   to PipeWire through pipewire-pulse with libpulse, which keeps the device's
   route volume that WirePlumber saves and restores. */
#define _GNU_SOURCE

#include "vol.h"

#include "desk.h"

#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

enum action { ACTION_GET, ACTION_UP, ACTION_DOWN, ACTION_SET, ACTION_MUTE, ACTION_FOLLOW, ACTION_MIXER };
enum mute_change { MUTE_TOGGLE, MUTE_ON, MUTE_OFF };

struct config {
    int step, max;
};

struct device {
    char name[256];
    pa_cvolume volume;
    bool muted, found;
};

struct app {
    struct config config;
    enum action action;
    enum mute_change mute;
    bool source;    /* The default input instead of the default output. */
    int amount;     /* Percent for up, down and set; -1 means the configured step. */

    pa_mainloop *loop;
    pa_mainloop_api *api;
    pa_context *context;
    pa_time_event *timer;
    struct device device;
    bool done;      /* finish() ran; later callbacks have nothing to add. */

    /* --follow: one query at a time, and one more if something changed meanwhile. */
    bool busy, dirty;
    char last[64];
};

static const char *noun(const struct app *app)
{
    return app->source ? "input" : "output";
}

static void finish(struct app *app, int status)
{
    if (app->done)
        return;
    app->done = true;
    pa_mainloop_quit(app->loop, status);
}

static void fail(struct app *app, const char *what)
{
    if (app->done)
        return;
    fprintf(stderr, "vol: %s: %s\n", what, pa_strerror(pa_context_errno(app->context)));
    finish(app, EXIT_FAILED);
}

/* Config */

struct config_parse {
    struct config *config;
    const char *path;
};

static bool parse_int(const char *text, int min, int max, int *value)
{
    char *end;
    errno = 0;
    long number = strtol(text, &end, 10);
    if (errno || end == text || *end || number < min || number > max)
        return false;
    *value = (int)number;
    return true;
}

static void config_entry(void *data, const char *section, const char *key, const char *value, int line)
{
    struct config_parse *parse = data;
    if (!key) {
        fprintf(stderr, "vol: %s:%d: expected key = value\n", parse->path, line);
        return;
    }
    if (strcmp(section, "volume")) {
        fprintf(stderr, "vol: %s:%d: unknown section [%s]\n", parse->path, line, section);
        return;
    }
    bool ok;
    if (!strcmp(key, "step"))
        ok = parse_int(value, 1, 100, &parse->config->step);
    else if (!strcmp(key, "max"))
        ok = parse_int(value, 1, VOL_MAX_LIMIT, &parse->config->max);
    else {
        fprintf(stderr, "vol: %s:%d: unknown key %s\n", parse->path, line, key);
        return;
    }
    if (!ok)
        fprintf(stderr, "vol: %s:%d: %s must be a whole number from 1 to %d\n", parse->path, line,
                key, !strcmp(key, "step") ? 100 : VOL_MAX_LIMIT);
}

/* The config is optional: without one, the defaults apply. */
static void load_config(struct config *config)
{
    *config = (struct config){.step = VOL_STEP_DEFAULT, .max = VOL_MAX_DEFAULT};
    char *path = desk_config_path("vol");
    if (!path)
        return;
    struct config_parse parse = {config, path};
    int error = desk_ini_load(path, config_entry, &parse);
    if (error && error != ENOENT)
        fprintf(stderr, "vol: can't read %s: %s\n", path, strerror(error));
    free(path);
}

/* Volume */

static int percent_of(pa_volume_t volume)
{
    return (int)lround((double)volume * 100.0 / PA_VOLUME_NORM);
}

static pa_volume_t volume_of(int percent)
{
    return (pa_volume_t)lround((double)percent * PA_VOLUME_NORM / 100.0);
}

/* The same line wpctl get-volume prints, which bar's [volume] module reads. */
static void describe(const struct device *device, char *line, size_t size)
{
    double level = (double)pa_cvolume_max(&device->volume) / PA_VOLUME_NORM;
    snprintf(line, size, "Volume: %.2f%s", level, device->muted ? " [MUTED]" : "");
}

/* The new level in percent. Up never lowers a level that is already above max. */
static int target_percent(const struct app *app, int now)
{
    int step = app->amount >= 0 ? app->amount : app->config.step;
    switch (app->action) {
    case ACTION_UP: {
        int next = now + step;
        if (next > app->config.max)
            next = now > app->config.max ? now : app->config.max;
        return next;
    }
    case ACTION_DOWN:
        return now - step < 0 ? 0 : now - step;
    case ACTION_SET:
        return app->amount;
    default:
        return now;
    }
}

/* Server queries */

static void query(struct app *app);

static void changed(pa_context *context, int success, void *data)
{
    (void)context;
    struct app *app = data;
    if (!success)
        fail(app, "the sound server refused the change");
    else
        finish(app, EXIT_OK);
}

static void act(struct app *app)
{
    struct device *device = &app->device;
    pa_operation *operation = NULL;
    switch (app->action) {
    case ACTION_GET: {
        char line[64];
        describe(device, line, sizeof line);
        puts(line);
        finish(app, EXIT_OK);
        return;
    }
    case ACTION_UP:
    case ACTION_DOWN:
    case ACTION_SET: {
        pa_cvolume volume = device->volume;
        /* Scaling keeps the balance between channels. */
        pa_cvolume_scale(&volume, volume_of(target_percent(app, percent_of(pa_cvolume_max(&volume)))));
        operation = app->source ?
            pa_context_set_source_volume_by_name(app->context, device->name, &volume, changed, app) :
            pa_context_set_sink_volume_by_name(app->context, device->name, &volume, changed, app);
        break;
    }
    case ACTION_MUTE: {
        int mute = app->mute == MUTE_TOGGLE ? !device->muted : app->mute == MUTE_ON;
        operation = app->source ?
            pa_context_set_source_mute_by_name(app->context, device->name, mute, changed, app) :
            pa_context_set_sink_mute_by_name(app->context, device->name, mute, changed, app);
        break;
    }
    case ACTION_MIXER:
        return;
    case ACTION_FOLLOW: {
        char line[64];
        describe(device, line, sizeof line);
        if (strcmp(line, app->last)) {
            strcpy(app->last, line);
            puts(line);
            fflush(stdout);
        }
        return;
    }
    }
    if (!operation)
        fail(app, "can't send the change");
    else
        pa_operation_unref(operation);
}

/* With --follow, a missing default device is a line of its own rather than an error. */
static void no_device(struct app *app)
{
    if (app->action != ACTION_FOLLOW) {
        fprintf(stderr, "vol: there is no default %s\n", noun(app));
        finish(app, EXIT_FAILED);
        return;
    }
    char line[64];
    snprintf(line, sizeof line, "No %s", noun(app));
    if (strcmp(line, app->last)) {
        strcpy(app->last, line);
        puts(line);
        fflush(stdout);
    }
}

/* A query ends here, found or not. --follow starts the next one if anything changed. */
static void query_done(struct app *app)
{
    if (app->action != ACTION_FOLLOW)
        return;
    app->busy = false;
    if (app->dirty) {
        app->dirty = false;
        query(app);
    }
}

static void got_device(struct app *app, int eol, const char *name, const pa_cvolume *volume, int mute)
{
    struct device *device = &app->device;
    if (eol == 0) {
        snprintf(device->name, sizeof device->name, "%s", name);
        device->volume = *volume;
        device->muted = mute != 0;
        device->found = true;
        return;
    }
    /* eol < 0 when the device went away between the two queries. */
    if (device->found)
        act(app);
    else
        no_device(app);
    query_done(app);
}

static void got_sink(pa_context *context, const pa_sink_info *info, int eol, void *data)
{
    (void)context;
    got_device(data, eol, info ? info->name : NULL, info ? &info->volume : NULL, info ? info->mute : 0);
}

static void got_source(pa_context *context, const pa_source_info *info, int eol, void *data)
{
    (void)context;
    got_device(data, eol, info ? info->name : NULL, info ? &info->volume : NULL, info ? info->mute : 0);
}

static void got_server(pa_context *context, const pa_server_info *info, void *data)
{
    struct app *app = data;
    const char *name = !info ? NULL : app->source ? info->default_source_name : info->default_sink_name;
    app->device.found = false;
    if (!name || !*name) {
        no_device(app);
        query_done(app);
        return;
    }
    pa_operation *operation = app->source ?
        pa_context_get_source_info_by_name(context, name, got_source, app) :
        pa_context_get_sink_info_by_name(context, name, got_sink, app);
    if (!operation) {
        fail(app, "can't ask for the default device");
        return;
    }
    pa_operation_unref(operation);
}

static void query(struct app *app)
{
    if (app->busy) {
        app->dirty = true;
        return;
    }
    app->busy = app->action == ACTION_FOLLOW;
    pa_operation *operation = pa_context_get_server_info(app->context, got_server, app);
    if (!operation) {
        fail(app, "can't ask the sound server");
        return;
    }
    pa_operation_unref(operation);
}

static void event(pa_context *context, pa_subscription_event_type_t type, uint32_t index, void *data)
{
    (void)context;
    (void)index;
    struct app *app = data;
    pa_subscription_event_type_t facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    if (facility == PA_SUBSCRIPTION_EVENT_SERVER || facility == PA_SUBSCRIPTION_EVENT_SINK ||
        facility == PA_SUBSCRIPTION_EVENT_SOURCE)
        query(app);
}

static void subscribed(pa_context *context, int success, void *data)
{
    (void)context;
    struct app *app = data;
    if (!success)
        fail(app, "can't follow the sound server");
    else
        query(app);
}

/* Connection */

static void connect_server(struct app *app);

static void set_timer(struct app *app, int ms, pa_time_event_cb_t callback)
{
    struct timeval when;
    pa_gettimeofday(&when);
    pa_timeval_add(&when, (pa_usec_t)ms * PA_USEC_PER_MSEC);
    if (app->timer)
        app->api->time_restart(app->timer, &when);
    else
        app->timer = app->api->time_new(app->api, &when, callback, app);
}

static void cancel_timer(struct app *app)
{
    if (app->timer)
        app->api->time_free(app->timer);
    app->timer = NULL;
}

static void timed_out(pa_mainloop_api *api, pa_time_event *timer, const struct timeval *when, void *data)
{
    (void)api;
    (void)timer;
    (void)when;
    struct app *app = data;
    if (!app->done)
        fprintf(stderr, "vol: the sound server didn't answer\n");
    finish(app, EXIT_FAILED);
}

static void reconnect(pa_mainloop_api *api, pa_time_event *timer, const struct timeval *when, void *data)
{
    (void)api;
    (void)timer;
    (void)when;
    struct app *app = data;
    cancel_timer(app);
    connect_server(app);
}

/* One-shot commands give up; --follow tries again until the server is back. */
static void lost(struct app *app)
{
    if (app->action != ACTION_FOLLOW)
        fail(app, "can't connect to the sound server");
    else
        set_timer(app, VOL_RECONNECT_MS, reconnect);
}

static void state_changed(pa_context *context, void *data)
{
    struct app *app = data;
    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY:
        if (app->action != ACTION_FOLLOW) {
            query(app);
            return;
        }
        /* Print the level again after a reconnect, even if it's the same. */
        app->last[0] = '\0';
        app->busy = app->dirty = false;
        pa_context_set_subscribe_callback(context, event, app);
        pa_operation *operation = pa_context_subscribe(context,
            PA_SUBSCRIPTION_MASK_SERVER | (app->source ? PA_SUBSCRIPTION_MASK_SOURCE : PA_SUBSCRIPTION_MASK_SINK),
            subscribed, app);
        if (!operation)
            fail(app, "can't follow the sound server");
        else
            pa_operation_unref(operation);
        return;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        lost(app);
        return;
    default:
        return;
    }
}

static void connect_server(struct app *app)
{
    if (app->context) {
        pa_context_set_state_callback(app->context, NULL, NULL);
        pa_context_set_subscribe_callback(app->context, NULL, NULL);
        pa_context_disconnect(app->context);
        pa_context_unref(app->context);
    }
    pa_proplist *properties = pa_proplist_new();
    pa_proplist_sets(properties, PA_PROP_APPLICATION_NAME, "vol");
    pa_proplist_sets(properties, PA_PROP_APPLICATION_ID, "vol");
    pa_proplist_sets(properties, PA_PROP_APPLICATION_VERSION, VOL_VERSION);
    app->context = pa_context_new_with_proplist(app->api, "vol", properties);
    pa_proplist_free(properties);
    if (!app->context) {
        fprintf(stderr, "vol: out of memory\n");
        finish(app, EXIT_FAILED);
        return;
    }
    pa_context_set_state_callback(app->context, state_changed, app);
    /* PipeWire starts pipewire-pulse on demand; never spawn a PulseAudio daemon. */
    if (pa_context_connect(app->context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
        lost(app);
}

/* The mixer: a terminal view of every output, input, playing stream and sound
   card, kept live from the server's events. It runs on libpulse's main loop,
   with the keyboard, signals and the palette file hooked into it. */

enum view { VIEW_OUTPUT, VIEW_INPUT, VIEW_PLAYBACK, VIEW_DEVICES, VIEW_COUNT };

static const char *const view_names[VIEW_COUNT] = {"Output", "Input", "Playback", "Devices"};

enum color { BACKGROUND, FOREGROUND, HIGHLIGHT, MUTED, SELECTION_BACKGROUND, SELECTION_FOREGROUND,
             COLOR_COUNT };

static const enum desk_role color_roles[COLOR_COUNT] = {
    [BACKGROUND] = DESK_BACKGROUND, [FOREGROUND] = DESK_FOREGROUND,
    [HIGHLIGHT] = DESK_HIGHLIGHT, [MUTED] = DESK_MUTED,
    [SELECTION_BACKGROUND] = DESK_SELECTION_BACKGROUND,
    [SELECTION_FOREGROUND] = DESK_SELECTION_FOREGROUND,
};

static const uint32_t color_defaults[COLOR_COUNT] = {
    0x353535, 0xc5ad81, 0xead4aa, 0x887d69, 0xc5ad81, 0x353535,
};

struct theme {
    char normal[48], highlight[48], muted[48], selection[48], selection_peak[48];
};

/* A sink, a source or a sink input: anything with a level. */
struct node {
    uint32_t index;
    char name[256];         /* Sinks and sources: what set-default takes. */
    char label[160];        /* The description, or the application's name. */
    char detail[160];       /* Sink inputs: what's playing. */
    pa_cvolume volume;
    bool muted, has_volume, corked;
    uint32_t sink;          /* Sink inputs: where they play. */
    uint32_t monitor;       /* Sinks: their monitor source, for the meter. */
};

struct nodes {
    struct node *items;
    size_t count, capacity;
};

struct profile {
    char name[128], label[160];
    bool available;
};

struct card {
    uint32_t index;
    char label[160], active[128];
    struct profile *profiles;
    size_t profile_count;
};

struct model {
    struct nodes sinks, sources, inputs;
    struct card *cards;
    size_t card_count, card_capacity;
    char default_sink[256], default_source[256];
};

/* A peak-detecting record stream on a source: a sink's monitor or a mic. */
struct meter {
    struct mixer *mixer;
    pa_stream *stream;
    uint32_t source;
    float peak;
    pa_usec_t at;
};

enum pick { PICK_NONE, PICK_SINK, PICK_PROFILE };

enum key {
    KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_TAB, KEY_BACKTAB,
    KEY_ENTER, KEY_DEFAULT, KEY_MUTE, KEY_QUIT, KEY_ESCAPE, KEY_VIEW,
};

struct input {
    enum { INPUT_TEXT, INPUT_ESCAPE, INPUT_CSI } state;
    int params[3];
    int count;
    bool mouse;
};

struct line {
    char text[1024];
};

struct mixer {
    struct config config;
    pa_mainloop *loop;
    pa_mainloop_api *api;
    pa_context *context;
    pa_time_event *reconnect_timer, *tick, *escape_timer;
    pa_io_event *keyboard, *palette_event;
    bool ready, listed, quit;
    int status, signal;

    /* Lists are read into next, then swapped in whole. One refresh at a time,
       and one more if anything changed meanwhile. */
    struct model now, next;
    int pending;
    bool again;

    struct meter **meters;
    size_t meter_count;

    enum view view;
    size_t cursor[VIEW_COUNT], top;
    uint32_t cursor_id[VIEW_COUNT];
    enum pick pick;
    uint32_t pick_target;
    size_t pick_cursor;

    struct input input;
    struct theme theme;
    char *palette_path;
    DeskWatch palette_watch;
    char notice[192];

    /* Where things were drawn, for the mouse. */
    struct line *lines;
    int *row_items;
    unsigned width, height;
    int tab_start[VIEW_COUNT], tab_end[VIEW_COUNT];
    int bar_x, bar_width;
    bool dirty, clear;
};

static struct termios saved_termios;
static bool terminal_active;

static void *grow(void *old, size_t count, size_t size)
{
    void *memory = reallocarray(old, count ? count : 1, size);
    if (!memory) {
        fputs("vol: out of memory\n", stderr);
        exit(EXIT_FAILED);
    }
    return memory;
}

static void copy_text(char *dest, size_t size, const char *text)
{
    snprintf(dest, size, "%s", text ? text : "");
}

static void notice(struct mixer *mixer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(mixer->notice, sizeof mixer->notice, format, args);
    va_end(args);
    mixer->dirty = true;
}

/* Theme */

static void format_style(char *text, size_t size, uint32_t background, uint32_t foreground)
{
    snprintf(text, size, "\033[48;2;%u;%u;%u;38;2;%u;%u;%um",
             (unsigned)(background >> 16 & 255), (unsigned)(background >> 8 & 255),
             (unsigned)(background & 255), (unsigned)(foreground >> 16 & 255),
             (unsigned)(foreground >> 8 & 255), (unsigned)(foreground & 255));
}

/* The wallpaper's palette when there is one, the defaults otherwise. */
static void apply_theme(struct mixer *mixer)
{
    uint32_t rgb[COLOR_COUNT];
    memcpy(rgb, color_defaults, sizeof rgb);
    DeskPalette palette;
    if (mixer->palette_path && desk_palette_load(mixer->palette_path, &palette) == 0)
        for (size_t i = 0; i < COLOR_COUNT; i++)
            rgb[i] = palette.rgb[color_roles[i]];
    struct theme *theme = &mixer->theme;
    format_style(theme->normal, sizeof theme->normal, rgb[BACKGROUND], rgb[FOREGROUND]);
    format_style(theme->highlight, sizeof theme->highlight, rgb[BACKGROUND], rgb[HIGHLIGHT]);
    format_style(theme->muted, sizeof theme->muted, rgb[BACKGROUND], rgb[MUTED]);
    format_style(theme->selection, sizeof theme->selection, rgb[SELECTION_BACKGROUND],
                 rgb[SELECTION_FOREGROUND]);
    format_style(theme->selection_peak, sizeof theme->selection_peak, rgb[SELECTION_BACKGROUND],
                 rgb[HIGHLIGHT]);
    mixer->clear = mixer->dirty = true;
    if (mixer->lines)
        memset(mixer->lines, 0, mixer->height * sizeof *mixer->lines);
}

/* Terminal */

static void terminal_restore(void)
{
    if (!terminal_active)
        return;
    fputs("\033[0m\033[?1006l\033[?1000l\033[?1049l\033[?25h", stdout);
    fflush(stdout);
    while (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios) < 0 && errno == EINTR)
        continue;
    terminal_active = false;
}

static int terminal_enter(void)
{
    if (tcgetattr(STDIN_FILENO, &saved_termios) < 0)
        return -1;
    struct termios raw = saved_termios;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0)
        return -1;
    terminal_active = true;
    /* The alternate screen, no cursor, and clicks and the wheel as SGR reports. */
    fputs("\033[?1049h\033[?25l\033[?1000h\033[?1006h", stdout);
    return fflush(stdout);
}

static void terminal_size(struct mixer *mixer)
{
    struct winsize size = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0)
        size = (struct winsize){0};
    unsigned width = size.ws_col ? size.ws_col : 80;
    unsigned height = size.ws_row ? size.ws_row : 24;
    if (width == mixer->width && height == mixer->height)
        return;
    mixer->lines = grow(mixer->lines, height, sizeof *mixer->lines);
    memset(mixer->lines, 0, height * sizeof *mixer->lines);
    mixer->row_items = grow(mixer->row_items, height + 1, sizeof *mixer->row_items);
    mixer->width = width;
    mixer->height = height;
    mixer->clear = mixer->dirty = true;
}

/* Copy at most limit columns of input into out, with '?' for what can't be shown.
   Returns the columns used. */
static unsigned fit_text(char *out, const char *input, unsigned limit)
{
    unsigned columns = 0;
    size_t length = strlen(input);
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

/* Fit text into exactly width columns, padding with spaces. */
static char *pad_text(char *out, const char *input, unsigned width)
{
    unsigned columns = fit_text(out, input, width);
    char *end = out + strlen(out);
    while (columns++ < width)
        *end++ = ' ';
    *end = '\0';
    return end;
}

/* The model */

static void free_model(struct model *model)
{
    free(model->sinks.items);
    free(model->sources.items);
    free(model->inputs.items);
    for (size_t i = 0; i < model->card_count; i++)
        free(model->cards[i].profiles);
    free(model->cards);
    *model = (struct model){0};
}

static struct node *add_node(struct nodes *nodes)
{
    if (nodes->count == nodes->capacity) {
        nodes->capacity = nodes->capacity ? 2 * nodes->capacity : 8;
        nodes->items = grow(nodes->items, nodes->capacity, sizeof *nodes->items);
    }
    struct node *node = &nodes->items[nodes->count++];
    *node = (struct node){.sink = PA_INVALID_INDEX, .monitor = PA_INVALID_INDEX};
    return node;
}

static struct node *find_node(struct nodes *nodes, uint32_t index)
{
    for (size_t i = 0; i < nodes->count; i++)
        if (nodes->items[i].index == index)
            return &nodes->items[i];
    return NULL;
}

static struct card *find_card(struct model *model, uint32_t index)
{
    for (size_t i = 0; i < model->card_count; i++)
        if (model->cards[i].index == index)
            return &model->cards[i];
    return NULL;
}

/* The rows of a view: nodes for the first three, cards for Devices. */
static struct nodes *view_nodes(struct mixer *mixer, enum view view)
{
    switch (view) {
    case VIEW_OUTPUT: return &mixer->now.sinks;
    case VIEW_INPUT: return &mixer->now.sources;
    case VIEW_PLAYBACK: return &mixer->now.inputs;
    default: return NULL;
    }
}

static size_t view_count(struct mixer *mixer, enum view view)
{
    struct nodes *nodes = view_nodes(mixer, view);
    return nodes ? nodes->count : mixer->now.card_count;
}

static uint32_t view_id(struct mixer *mixer, enum view view, size_t row)
{
    struct nodes *nodes = view_nodes(mixer, view);
    return nodes ? nodes->items[row].index : mixer->now.cards[row].index;
}

static struct node *selected_node(struct mixer *mixer)
{
    struct nodes *nodes = view_nodes(mixer, mixer->view);
    size_t row = mixer->cursor[mixer->view];
    return nodes && row < nodes->count ? &nodes->items[row] : NULL;
}

static struct card *selected_card(struct mixer *mixer)
{
    size_t row = mixer->cursor[VIEW_DEVICES];
    return mixer->view == VIEW_DEVICES && row < mixer->now.card_count ? &mixer->now.cards[row] : NULL;
}

static size_t pick_count(struct mixer *mixer)
{
    if (mixer->pick == PICK_SINK)
        return mixer->now.sinks.count;
    struct card *card = mixer->pick == PICK_PROFILE ? find_card(&mixer->now, mixer->pick_target) : NULL;
    return card ? card->profile_count : 0;
}

static void select_row(struct mixer *mixer, size_t row)
{
    size_t count = view_count(mixer, mixer->view);
    if (!count)
        return;
    if (row >= count)
        row = count - 1;
    if (mixer->cursor[mixer->view] != row)
        mixer->notice[0] = '\0';
    mixer->cursor[mixer->view] = row;
    mixer->cursor_id[mixer->view] = view_id(mixer, mixer->view, row);
    mixer->dirty = true;
}

/* Meters */

static void meter_read(pa_stream *stream, size_t length, void *data)
{
    (void)length;
    struct meter *meter = data;
    const void *samples;
    size_t size;
    while (pa_stream_readable_size(stream) > 0) {
        if (pa_stream_peek(stream, &samples, &size) < 0 || !size)
            return;
        if (samples && size >= sizeof(float)) {
            float peak = ((const float *)samples)[size / sizeof(float) - 1];
            meter->peak = peak < 0 ? 0 : peak > 1 ? 1 : peak;
            meter->at = pa_rtclock_now();
            meter->mixer->dirty = true;
        }
        pa_stream_drop(stream);
    }
}

static void close_meter(struct meter *meter)
{
    if (meter->stream) {
        pa_stream_set_read_callback(meter->stream, NULL, NULL);
        pa_stream_disconnect(meter->stream);
        pa_stream_unref(meter->stream);
    }
    free(meter);
}

static void close_meters(struct mixer *mixer)
{
    for (size_t i = 0; i < mixer->meter_count; i++)
        close_meter(mixer->meters[i]);
    mixer->meter_count = 0;
}

/* A mic's meter keeps it awake, which is the point; a monitor's doesn't keep
   the output awake when nothing plays. */
static struct meter *open_meter(struct mixer *mixer, uint32_t source, bool monitor)
{
    static const pa_sample_spec spec = {.format = PA_SAMPLE_FLOAT32NE, .rate = MIXER_METER_RATE, .channels = 1};
    struct meter *meter = grow(NULL, 1, sizeof *meter);
    *meter = (struct meter){.mixer = mixer, .source = source};
    meter->stream = pa_stream_new(mixer->context, "Peak meter", &spec, NULL);
    if (!meter->stream)
        return meter;
    pa_buffer_attr attributes = {.maxlength = (uint32_t)-1, .fragsize = sizeof(float)};
    char device[16];
    snprintf(device, sizeof device, "%u", source);
    pa_stream_set_read_callback(meter->stream, meter_read, meter);
    pa_stream_flags_t flags = PA_STREAM_DONT_MOVE | PA_STREAM_PEAK_DETECT | PA_STREAM_ADJUST_LATENCY;
    if (monitor)
        flags |= PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND;
    if (pa_stream_connect_record(meter->stream, device, &attributes, flags) < 0) {
        pa_stream_unref(meter->stream);
        meter->stream = NULL;
    }
    return meter;
}

static struct meter *find_meter(struct mixer *mixer, uint32_t source)
{
    for (size_t i = 0; i < mixer->meter_count; i++)
        if (mixer->meters[i]->source == source)
            return mixer->meters[i];
    return NULL;
}

/* Meters for the rows on screen: each output's monitor, or each input. */
static void sync_meters(struct mixer *mixer)
{
    struct nodes *nodes = !mixer->ready ? NULL : mixer->view == VIEW_OUTPUT ? &mixer->now.sinks :
                          mixer->view == VIEW_INPUT ? &mixer->now.sources : NULL;
    bool monitor = mixer->view == VIEW_OUTPUT;
    size_t kept = 0;
    for (size_t i = 0; i < mixer->meter_count; i++) {
        struct meter *meter = mixer->meters[i];
        bool wanted = false;
        for (size_t j = 0; nodes && j < nodes->count && !wanted; j++)
            wanted = (monitor ? nodes->items[j].monitor : nodes->items[j].index) == meter->source;
        if (wanted)
            mixer->meters[kept++] = meter;
        else
            close_meter(meter);
    }
    mixer->meter_count = kept;
    for (size_t j = 0; nodes && j < nodes->count; j++) {
        uint32_t source = monitor ? nodes->items[j].monitor : nodes->items[j].index;
        if (source == PA_INVALID_INDEX || find_meter(mixer, source))
            continue;
        mixer->meters = grow(mixer->meters, mixer->meter_count + 1, sizeof *mixer->meters);
        mixer->meters[mixer->meter_count++] = open_meter(mixer, source, monitor);
    }
}

/* A peak on the same scale as the level, or -1 without a recent one. */
static int meter_percent(struct mixer *mixer, uint32_t source)
{
    struct meter *meter = source == PA_INVALID_INDEX ? NULL : find_meter(mixer, source);
    if (!meter || !meter->at || pa_rtclock_now() - meter->at > 300 * PA_USEC_PER_MSEC)
        return -1;
    return percent_of(pa_sw_volume_from_linear(meter->peak));
}

/* Reading the server */

static void refresh(struct mixer *mixer);

/* All five lists are in: swap them in, keep each view's selection on the same thing. */
static void list_done(struct mixer *mixer)
{
    if (--mixer->pending > 0)
        return;
    free_model(&mixer->now);
    mixer->now = mixer->next;
    mixer->next = (struct model){0};
    mixer->listed = true;
    for (enum view view = 0; view < VIEW_COUNT; view++) {
        size_t count = view_count(mixer, view);
        size_t row = mixer->cursor[view] < count ? mixer->cursor[view] : count ? count - 1 : 0;
        for (size_t i = 0; i < count; i++)
            if (view_id(mixer, view, i) == mixer->cursor_id[view])
                row = i;
        mixer->cursor[view] = row;
        if (count)
            mixer->cursor_id[view] = view_id(mixer, view, row);
    }
    if (mixer->pick != PICK_NONE) {
        bool gone = mixer->pick == PICK_SINK ? !find_node(&mixer->now.inputs, mixer->pick_target) :
                    !find_card(&mixer->now, mixer->pick_target);
        if (gone) {
            mixer->pick = PICK_NONE;
            notice(mixer, mixer->view == VIEW_PLAYBACK ? "The stream ended" : "The card went away");
        } else if (mixer->pick_cursor >= pick_count(mixer)) {
            mixer->pick_cursor = pick_count(mixer) ? pick_count(mixer) - 1 : 0;
        }
    }
    sync_meters(mixer);
    mixer->dirty = true;
    if (mixer->again) {
        mixer->again = false;
        refresh(mixer);
    }
}

static void got_server_info(pa_context *context, const pa_server_info *info, void *data)
{
    (void)context;
    struct mixer *mixer = data;
    if (info) {
        copy_text(mixer->next.default_sink, sizeof mixer->next.default_sink, info->default_sink_name);
        copy_text(mixer->next.default_source, sizeof mixer->next.default_source, info->default_source_name);
    }
    list_done(mixer);
}

static void got_sink_entry(pa_context *context, const pa_sink_info *info, int eol, void *data)
{
    (void)context;
    struct mixer *mixer = data;
    if (eol) {
        list_done(mixer);
        return;
    }
    struct node *node = add_node(&mixer->next.sinks);
    node->index = info->index;
    copy_text(node->name, sizeof node->name, info->name);
    copy_text(node->label, sizeof node->label, info->description ? info->description : info->name);
    node->volume = info->volume;
    node->muted = info->mute;
    node->has_volume = true;
    node->monitor = info->monitor_source;
}

static void got_source_entry(pa_context *context, const pa_source_info *info, int eol, void *data)
{
    (void)context;
    struct mixer *mixer = data;
    if (eol) {
        list_done(mixer);
        return;
    }
    /* An output's monitor is a source too, but not an input anyone means. */
    if (info->monitor_of_sink != PA_INVALID_INDEX)
        return;
    struct node *node = add_node(&mixer->next.sources);
    node->index = info->index;
    copy_text(node->name, sizeof node->name, info->name);
    copy_text(node->label, sizeof node->label, info->description ? info->description : info->name);
    node->volume = info->volume;
    node->muted = info->mute;
    node->has_volume = true;
}

static void got_input_entry(pa_context *context, const pa_sink_input_info *info, int eol, void *data)
{
    (void)context;
    struct mixer *mixer = data;
    if (eol) {
        list_done(mixer);
        return;
    }
    struct node *node = add_node(&mixer->next.inputs);
    const char *application = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    node->index = info->index;
    copy_text(node->label, sizeof node->label, application ? application : info->name);
    if (application && info->name && strcmp(info->name, application))
        copy_text(node->detail, sizeof node->detail, info->name);
    node->volume = info->volume;
    node->muted = info->mute;
    node->has_volume = info->has_volume && info->volume_writable;
    node->corked = info->corked;
    node->sink = info->sink;
}

static void got_card_entry(pa_context *context, const pa_card_info *info, int eol, void *data)
{
    (void)context;
    struct mixer *mixer = data;
    if (eol) {
        list_done(mixer);
        return;
    }
    struct model *model = &mixer->next;
    if (model->card_count == model->card_capacity) {
        model->card_capacity = model->card_capacity ? 2 * model->card_capacity : 4;
        model->cards = grow(model->cards, model->card_capacity, sizeof *model->cards);
    }
    struct card *card = &model->cards[model->card_count++];
    *card = (struct card){.index = info->index};
    const char *label = pa_proplist_gets(info->proplist, PA_PROP_DEVICE_DESCRIPTION);
    copy_text(card->label, sizeof card->label, label ? label : info->name);
    if (info->active_profile2)
        copy_text(card->active, sizeof card->active, info->active_profile2->name);
    card->profiles = grow(NULL, info->n_profiles, sizeof *card->profiles);
    for (uint32_t i = 0; i < info->n_profiles; i++) {
        const pa_card_profile_info2 *source = info->profiles2[i];
        struct profile *profile = &card->profiles[card->profile_count++];
        copy_text(profile->name, sizeof profile->name, source->name);
        copy_text(profile->label, sizeof profile->label, source->description ? source->description : source->name);
        profile->available = source->available != 0;
    }
}

static void send(struct mixer *mixer, pa_operation *operation)
{
    if (!operation)
        notice(mixer, "Couldn't ask the sound server: %s", pa_strerror(pa_context_errno(mixer->context)));
    else
        pa_operation_unref(operation);
}

static void refresh(struct mixer *mixer)
{
    if (!mixer->ready)
        return;
    if (mixer->pending) {
        mixer->again = true;
        return;
    }
    free_model(&mixer->next);
    mixer->pending = 5;
    pa_context *context = mixer->context;
    pa_operation *operations[] = {
        pa_context_get_server_info(context, got_server_info, mixer),
        pa_context_get_sink_info_list(context, got_sink_entry, mixer),
        pa_context_get_source_info_list(context, got_source_entry, mixer),
        pa_context_get_sink_input_info_list(context, got_input_entry, mixer),
        pa_context_get_card_info_list(context, got_card_entry, mixer),
    };
    for (size_t i = 0; i < sizeof operations / sizeof operations[0]; i++) {
        if (operations[i])
            pa_operation_unref(operations[i]);
        else
            list_done(mixer);
    }
}

static void mixer_event(pa_context *context, pa_subscription_event_type_t type, uint32_t index, void *data)
{
    (void)context;
    (void)type;
    (void)index;
    refresh(data);
}

/* Changes */

static void changed_reply(pa_context *context, int success, void *data)
{
    struct mixer *mixer = data;
    if (!success)
        notice(mixer, "The sound server refused: %s", pa_strerror(pa_context_errno(context)));
}

static void set_level(struct mixer *mixer, struct node *node, int percent)
{
    if (!node->has_volume) {
        notice(mixer, "This stream's level can't be changed");
        return;
    }
    if (!node->volume.channels)
        return;
    if (percent < 0)
        percent = 0;
    if (percent > VOL_MAX_LIMIT)
        percent = VOL_MAX_LIMIT;
    /* Scaling keeps the balance between channels. Kept locally too, so held keys add up. */
    pa_cvolume_scale(&node->volume, volume_of(percent));
    pa_context *context = mixer->context;
    switch (mixer->view) {
    case VIEW_OUTPUT:
        send(mixer, pa_context_set_sink_volume_by_index(context, node->index, &node->volume, changed_reply, mixer));
        break;
    case VIEW_INPUT:
        send(mixer, pa_context_set_source_volume_by_index(context, node->index, &node->volume, changed_reply, mixer));
        break;
    case VIEW_PLAYBACK:
        send(mixer, pa_context_set_sink_input_volume(context, node->index, &node->volume, changed_reply, mixer));
        break;
    default:
        break;
    }
    mixer->dirty = true;
}

/* Up stops at [volume] max, but never lowers a level already above it. */
static void step_level(struct mixer *mixer, struct node *node, bool up)
{
    int now = percent_of(pa_cvolume_max(&node->volume)), step = mixer->config.step;
    int next = up ? now + step : now - step;
    if (up && next > mixer->config.max)
        next = now > mixer->config.max ? now : mixer->config.max;
    if (next != now)
        set_level(mixer, node, next);
}

static void toggle_mute(struct mixer *mixer, struct node *node)
{
    node->muted = !node->muted;
    pa_context *context = mixer->context;
    if (mixer->view == VIEW_OUTPUT)
        send(mixer, pa_context_set_sink_mute_by_index(context, node->index, node->muted, changed_reply, mixer));
    else if (mixer->view == VIEW_INPUT)
        send(mixer, pa_context_set_source_mute_by_index(context, node->index, node->muted, changed_reply, mixer));
    else
        send(mixer, pa_context_set_sink_input_mute(context, node->index, node->muted, changed_reply, mixer));
    mixer->dirty = true;
}

static void make_default(struct mixer *mixer, struct node *node)
{
    if (mixer->view == VIEW_OUTPUT) {
        copy_text(mixer->now.default_sink, sizeof mixer->now.default_sink, node->name);
        send(mixer, pa_context_set_default_sink(mixer->context, node->name, changed_reply, mixer));
    } else {
        copy_text(mixer->now.default_source, sizeof mixer->now.default_source, node->name);
        send(mixer, pa_context_set_default_source(mixer->context, node->name, changed_reply, mixer));
    }
    mixer->dirty = true;
}

static void open_picker(struct mixer *mixer)
{
    if (mixer->view == VIEW_PLAYBACK) {
        struct node *node = selected_node(mixer);
        if (!node || !mixer->now.sinks.count)
            return;
        mixer->pick = PICK_SINK;
        mixer->pick_target = node->index;
        mixer->pick_cursor = 0;
        for (size_t i = 0; i < mixer->now.sinks.count; i++)
            if (mixer->now.sinks.items[i].index == node->sink)
                mixer->pick_cursor = i;
    } else {
        struct card *card = selected_card(mixer);
        if (!card || !card->profile_count)
            return;
        mixer->pick = PICK_PROFILE;
        mixer->pick_target = card->index;
        mixer->pick_cursor = 0;
        for (size_t i = 0; i < card->profile_count; i++)
            if (!strcmp(card->profiles[i].name, card->active))
                mixer->pick_cursor = i;
    }
    mixer->notice[0] = '\0';
    mixer->dirty = true;
}

static void choose(struct mixer *mixer)
{
    if (mixer->pick_cursor >= pick_count(mixer))
        return;
    if (mixer->pick == PICK_SINK) {
        struct node *sink = &mixer->now.sinks.items[mixer->pick_cursor];
        send(mixer, pa_context_move_sink_input_by_index(mixer->context, mixer->pick_target, sink->index,
                                                        changed_reply, mixer));
    } else {
        struct card *card = find_card(&mixer->now, mixer->pick_target);
        struct profile *profile = &card->profiles[mixer->pick_cursor];
        if (!profile->available) {
            notice(mixer, "%s isn't available now", profile->label);
            return;
        }
        send(mixer, pa_context_set_card_profile_by_index(mixer->context, card->index, profile->name,
                                                         changed_reply, mixer));
    }
    mixer->pick = PICK_NONE;
    mixer->dirty = true;
}

/* Connection */

static void mixer_connect(struct mixer *mixer);

static void mixer_reconnect(pa_mainloop_api *api, pa_time_event *timer, const struct timeval *when, void *data)
{
    (void)when;
    struct mixer *mixer = data;
    api->time_free(timer);
    mixer->reconnect_timer = NULL;
    mixer_connect(mixer);
}

static void mixer_lost(struct mixer *mixer)
{
    mixer->ready = mixer->listed = false;
    mixer->pending = 0;
    mixer->again = false;
    mixer->pick = PICK_NONE;
    close_meters(mixer);
    free_model(&mixer->now);
    free_model(&mixer->next);
    mixer->dirty = true;
    if (mixer->reconnect_timer)
        return;
    struct timeval when;
    pa_gettimeofday(&when);
    pa_timeval_add(&when, (pa_usec_t)VOL_RECONNECT_MS * PA_USEC_PER_MSEC);
    mixer->reconnect_timer = mixer->api->time_new(mixer->api, &when, mixer_reconnect, mixer);
}

static void mixer_subscribed(pa_context *context, int success, void *data)
{
    (void)context;
    struct mixer *mixer = data;
    if (!success)
        notice(mixer, "Can't follow the sound server; changes won't show until restarted");
}

static void mixer_state(pa_context *context, void *data)
{
    struct mixer *mixer = data;
    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY:
        mixer->ready = true;
        mixer->notice[0] = '\0';
        pa_context_set_subscribe_callback(context, mixer_event, mixer);
        send(mixer, pa_context_subscribe(context,
            PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
            PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_CARD, mixer_subscribed, mixer));
        refresh(mixer);
        return;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        mixer_lost(mixer);
        return;
    default:
        return;
    }
}

static void mixer_disconnect(struct mixer *mixer)
{
    close_meters(mixer);
    if (!mixer->context)
        return;
    pa_context_set_state_callback(mixer->context, NULL, NULL);
    pa_context_set_subscribe_callback(mixer->context, NULL, NULL);
    pa_context_disconnect(mixer->context);
    pa_context_unref(mixer->context);
    mixer->context = NULL;
}

static void mixer_connect(struct mixer *mixer)
{
    mixer_disconnect(mixer);
    pa_proplist *properties = pa_proplist_new();
    pa_proplist_sets(properties, PA_PROP_APPLICATION_NAME, "vol");
    pa_proplist_sets(properties, PA_PROP_APPLICATION_ID, "vol");
    pa_proplist_sets(properties, PA_PROP_APPLICATION_VERSION, VOL_VERSION);
    mixer->context = pa_context_new_with_proplist(mixer->api, "vol", properties);
    pa_proplist_free(properties);
    if (!mixer->context) {
        mixer->quit = true;
        mixer->status = EXIT_FAILED;
        return;
    }
    pa_context_set_state_callback(mixer->context, mixer_state, mixer);
    if (pa_context_connect(mixer->context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
        mixer_lost(mixer);
}

/* Drawing */

static void paint_line(struct mixer *mixer, unsigned row, const char *style, const char *value)
{
    char line[sizeof mixer->lines[0].text];
    const char *margin = mixer->width >= 40 ? "  " : "";
    snprintf(line, sizeof line, "%s\033[K%s%s", style, margin, value);
    char *previous = mixer->lines[row - 1].text;
    if (strcmp(previous, line)) {
        printf("\033[%u;1H%s", row, line);
        strcpy(previous, line);
    }
}

struct columns {
    unsigned name, bar, level, output;
};

static struct columns node_columns(const struct mixer *mixer, unsigned width)
{
    struct columns columns = {
        .bar = width >= 64 ? 24 : width >= 48 ? 14 : 0,
        .level = 5,
        .output = mixer->view == VIEW_PLAYBACK && width >= 72 ? 18 : 0,
    };
    unsigned used = 2 + (columns.bar ? columns.bar + 2 : 0) + columns.level +
                    (columns.output ? columns.output + 2 : 0);
    columns.name = width > used + 2 ? width - used - 2 : 1;
    return columns;
}

/* A level bar: the level as a line, the meter's peak as blocks over it in the highlight color. */
static char *level_bar(char *out, unsigned width, int level, int peak, const char *peak_style,
                       const char *line_style, const char *rest_style)
{
    unsigned filled = (unsigned)(level >= 100 ? (int)width : (level * (int)width + 50) / 100);
    unsigned peaked = peak < 0 ? 0 : (unsigned)(peak >= 100 ? (int)width : (peak * (int)width + 50) / 100);
    if (peaked > filled)
        peaked = filled;
    const char *current = NULL;
    for (unsigned i = 0; i < width; i++) {
        const char *style = i < peaked ? peak_style : i < filled ? line_style : rest_style;
        if (style != current) {
            out += sprintf(out, "%s", style);
            current = style;
        }
        out += sprintf(out, "%s", i < peaked ? "█" : i < filled ? "━" : "─");
    }
    return out;
}

static void node_line(struct mixer *mixer, char *out, unsigned width, const struct node *node,
                      bool selected)
{
    const struct theme *theme = &mixer->theme;
    struct columns columns = node_columns(mixer, width);
    const char *base = selected ? theme->selection : node->muted ? theme->muted : theme->normal;
    bool is_default = (mixer->view == VIEW_OUTPUT && !strcmp(node->name, mixer->now.default_sink)) ||
                      (mixer->view == VIEW_INPUT && !strcmp(node->name, mixer->now.default_source));
    char label[sizeof node->label + sizeof node->detail + 8];
    if (node->detail[0])
        snprintf(label, sizeof label, "%s: %s", node->label, node->detail);
    else
        snprintf(label, sizeof label, "%s", node->label);
    out += sprintf(out, "%s%s", base, is_default ? "* " : "  ");
    out = pad_text(out, label, columns.name);
    int level = percent_of(pa_cvolume_max(&node->volume));
    if (columns.bar) {
        out += sprintf(out, "  ");
        uint32_t source = mixer->view == VIEW_OUTPUT ? node->monitor :
                          mixer->view == VIEW_INPUT ? node->index : PA_INVALID_INDEX;
        int peak = node->muted ? -1 : meter_percent(mixer, source);
        const char *peak_style = selected ? theme->selection_peak : theme->highlight;
        const char *rest = selected ? theme->selection : theme->muted;
        out = level_bar(out, columns.bar, node->muted ? 0 : level, peak, peak_style, base, rest);
        out += sprintf(out, "%s", base);
    }
    out += sprintf(out, "  ");
    if (node->muted)
        out += sprintf(out, "muted");
    else
        out += sprintf(out, "%4d%%", level);
    if (columns.output) {
        struct node *sink = find_node(&mixer->now.sinks, node->sink);
        out += sprintf(out, "  ");
        pad_text(out, sink ? sink->label : "", columns.output);
    }
}

static void card_line(struct mixer *mixer, char *out, unsigned width, const struct card *card, bool selected)
{
    const char *active = "Off";
    for (size_t i = 0; i < card->profile_count; i++)
        if (!strcmp(card->profiles[i].name, card->active))
            active = card->profiles[i].label;
    unsigned name_width = width >= 60 ? width / 2 - 2 : width - 2;
    out += sprintf(out, "%s  ", selected ? mixer->theme.selection : mixer->theme.normal);
    out = pad_text(out, card->label, name_width);
    if (width >= 60) {
        out += sprintf(out, "  ");
        fit_text(out, active, width - name_width - 4);
    }
}

static void pick_line(struct mixer *mixer, char *out, unsigned width, size_t row, bool selected)
{
    const struct theme *theme = &mixer->theme;
    const char *label = "";
    bool current = false, available = true;
    if (mixer->pick == PICK_SINK) {
        struct node *input = find_node(&mixer->now.inputs, mixer->pick_target);
        struct node *sink = &mixer->now.sinks.items[row];
        label = sink->label;
        current = input && input->sink == sink->index;
    } else {
        struct card *card = find_card(&mixer->now, mixer->pick_target);
        struct profile *profile = &card->profiles[row];
        label = profile->label;
        current = !strcmp(profile->name, card->active);
        available = profile->available;
    }
    const char *style = selected ? theme->selection : !available ? theme->muted : theme->normal;
    out += sprintf(out, "%s%s", style, current ? "* " : "  ");
    out = pad_text(out, label, width - 2 - (available ? 0 : 14));
    if (!available)
        sprintf(out, "  unavailable");
}

static void tab_line(struct mixer *mixer, char *out)
{
    int column = mixer->width >= 40 ? 3 : 1;
    for (enum view view = 0; view < VIEW_COUNT; view++) {
        bool current = view == mixer->view;
        const char *style = current ? mixer->theme.highlight : mixer->theme.muted;
        out += sprintf(out, "%s%s%s%s", style, current ? "[" : " ", view_names[view], current ? "]" : " ");
        mixer->tab_start[view] = column;
        column += (int)strlen(view_names[view]) + 2;
        mixer->tab_end[view] = column;
        out += sprintf(out, "  ");
        column += 2;
    }
}

static const char *status_text(struct mixer *mixer, char *text, size_t size)
{
    if (!mixer->ready)
        return mixer->reconnect_timer ? "Sound server not running | trying again" : "Connecting...";
    if (!mixer->listed)
        return "Reading...";
    if (mixer->pick == PICK_SINK) {
        struct node *input = find_node(&mixer->now.inputs, mixer->pick_target);
        snprintf(text, size, "Play %s on:", input ? input->label : "this stream");
        return text;
    }
    if (mixer->pick == PICK_PROFILE) {
        struct card *card = find_card(&mixer->now, mixer->pick_target);
        snprintf(text, size, "Profile for %s:", card ? card->label : "this card");
        return text;
    }
    switch (mixer->view) {
    case VIEW_OUTPUT:
    case VIEW_INPUT: {
        bool output = mixer->view == VIEW_OUTPUT;
        struct nodes *nodes = view_nodes(mixer, mixer->view);
        const char *name = output ? mixer->now.default_sink : mixer->now.default_source;
        for (size_t i = 0; i < nodes->count; i++)
            if (!strcmp(nodes->items[i].name, name)) {
                snprintf(text, size, "Default %s: %s", output ? "output" : "input", nodes->items[i].label);
                return text;
            }
        return output ? "No default output" : "No default input";
    }
    case VIEW_PLAYBACK: {
        size_t playing = 0;
        for (size_t i = 0; i < mixer->now.inputs.count; i++)
            playing += !mixer->now.inputs.items[i].corked;
        snprintf(text, size, "%zu stream%s, %zu playing", mixer->now.inputs.count,
                 mixer->now.inputs.count == 1 ? "" : "s", playing);
        return text;
    }
    default:
        snprintf(text, size, "%zu card%s", mixer->now.card_count, mixer->now.card_count == 1 ? "" : "s");
        return text;
    }
}

static const char *empty_text(const struct mixer *mixer)
{
    if (!mixer->listed)
        return "";
    static const char *const texts[VIEW_COUNT] = {"No outputs", "No inputs", "Nothing is playing",
                                                  "No sound cards"};
    return texts[mixer->view];
}

static const char *hint_text(const struct mixer *mixer, unsigned width)
{
    if (mixer->pick != PICK_NONE)
        return "j/k  Enter Choose  Esc Back";
    if (width < 56)
        return "Tab  j/k  h/l  m  Enter  q";
    switch (mixer->view) {
    case VIEW_OUTPUT:
    case VIEW_INPUT: return "Tab View  j/k  h/l Level  m Mute  Enter Default  q Quit";
    case VIEW_PLAYBACK: return "Tab View  j/k  h/l Level  m Mute  Enter Move  q Quit";
    default: return "Tab View  j/k  Enter Profile  q Quit";
    }
}

static void heading_line(struct mixer *mixer, char *out, unsigned width)
{
    if (mixer->pick != PICK_NONE) {
        snprintf(out, 64, "  %s", mixer->pick == PICK_SINK ? "Output" : "Profile");
        return;
    }
    if (mixer->view == VIEW_DEVICES) {
        out += sprintf(out, "  ");
        out = pad_text(out, "Card", width >= 60 ? width / 2 - 2 : width - 2);
        if (width >= 60)
            sprintf(out, "  Profile");
        return;
    }
    struct columns columns = node_columns(mixer, width);
    out += sprintf(out, "  ");
    out = pad_text(out, "Name", columns.name);
    if (columns.bar) {
        out += sprintf(out, "  ");
        out = pad_text(out, "Level", columns.bar);
    }
    out += sprintf(out, "       ");
    if (columns.output)
        sprintf(out, "Output");
}

static int render(struct mixer *mixer)
{
    if (mixer->clear) {
        fputs(mixer->theme.normal, stdout);
        fputs("\033[2J", stdout);
        mixer->clear = false;
    }
    unsigned width = mixer->width >= 40 ? mixer->width - 2 : mixer->width;
    if (width > 90)
        width = 90;
    unsigned footer = mixer->height >= 5 ? mixer->height - 1 : mixer->height;
    unsigned first = 4;
    size_t count = mixer->pick != PICK_NONE ? pick_count(mixer) : view_count(mixer, mixer->view);
    size_t selected = mixer->pick != PICK_NONE ? mixer->pick_cursor : mixer->cursor[mixer->view];
    size_t visible = footer > first ? footer - first : 0;
    if (count <= visible) {
        mixer->top = 0;
    } else if (visible) {
        if (mixer->top > count - visible)
            mixer->top = count - visible;
        if (selected < mixer->top)
            mixer->top = selected;
        if (selected - mixer->top >= visible)
            mixer->top = selected - visible + 1;
    }
    struct columns columns = node_columns(mixer, width);
    mixer->bar_width = mixer->pick == PICK_NONE && mixer->view != VIEW_DEVICES ? (int)columns.bar : 0;
    mixer->bar_x = (mixer->width >= 40 ? 2 : 0) + 2 + (int)columns.name + 2 + 1;

    char text[sizeof mixer->lines[0].text - 64], fitted[sizeof text];
    for (unsigned row = 1; row <= mixer->height; row++) {
        mixer->row_items[row] = -1;
        const char *style = mixer->theme.muted, *value = "";
        if (row == 1 && mixer->height > 1) {
            tab_line(mixer, text);
            paint_line(mixer, row, mixer->theme.muted, text);
            continue;
        } else if (row >= footer) {
            if (row == footer && mixer->notice[0]) {
                style = mixer->theme.highlight;
                value = mixer->notice;
            }
            if (row == mixer->height && (footer < row || !*value)) {
                style = mixer->theme.muted;
                value = hint_text(mixer, width);
            }
        } else if (row == 2) {
            style = mixer->theme.normal;
            value = status_text(mixer, fitted, sizeof fitted);
        } else if (row == 3) {
            if (count) {
                heading_line(mixer, text, width);
                value = text;
            }
        } else if (count) {
            size_t index = mixer->top + row - first;
            if (index < count) {
                bool is_selected = index == selected;
                if (mixer->pick != PICK_NONE)
                    pick_line(mixer, text, width, index, is_selected);
                else if (mixer->view == VIEW_DEVICES)
                    card_line(mixer, text, width, &mixer->now.cards[index], is_selected);
                else
                    node_line(mixer, text, width, &view_nodes(mixer, mixer->view)->items[index], is_selected);
                mixer->row_items[row] = (int)index;
                paint_line(mixer, row, mixer->theme.normal, text);
                continue;
            }
        } else if (row == first) {
            value = empty_text(mixer);
        }
        fit_text(fitted, value, width);
        paint_line(mixer, row, style, fitted);
    }
    mixer->dirty = false;
    return fflush(stdout) == EOF || ferror(stdout) ? -1 : 0;
}

/* Input */

static void switch_view(struct mixer *mixer, enum view view)
{
    if (view == mixer->view)
        return;
    mixer->view = view;
    mixer->pick = PICK_NONE;
    mixer->top = 0;
    mixer->notice[0] = '\0';
    sync_meters(mixer);
    mixer->dirty = true;
}

static void move_cursor(struct mixer *mixer, enum key key)
{
    size_t count = mixer->pick != PICK_NONE ? pick_count(mixer) : view_count(mixer, mixer->view);
    if (!count)
        return;
    size_t row = mixer->pick != PICK_NONE ? mixer->pick_cursor : mixer->cursor[mixer->view];
    if (key == KEY_UP && row > 0)
        row--;
    else if (key == KEY_DOWN && row + 1 < count)
        row++;
    else if (key == KEY_HOME)
        row = 0;
    else if (key == KEY_END)
        row = count - 1;
    if (mixer->pick != PICK_NONE) {
        mixer->pick_cursor = row;
        mixer->dirty = true;
    } else {
        select_row(mixer, row);
    }
}

static void handle_key(struct mixer *mixer, enum key key, int view)
{
    if (mixer->pick != PICK_NONE) {
        if (key == KEY_ESCAPE)
            mixer->pick = PICK_NONE, mixer->dirty = true;
        else if (key == KEY_ENTER || key == KEY_DEFAULT)
            choose(mixer);
        else if (key == KEY_QUIT)
            mixer->quit = true;
        else
            move_cursor(mixer, key);
        return;
    }
    struct node *node = selected_node(mixer);
    switch (key) {
    case KEY_QUIT:
    case KEY_ESCAPE:
        mixer->quit = true;
        return;
    case KEY_TAB:
        switch_view(mixer, (mixer->view + 1) % VIEW_COUNT);
        return;
    case KEY_BACKTAB:
        switch_view(mixer, (mixer->view + VIEW_COUNT - 1) % VIEW_COUNT);
        return;
    case KEY_VIEW:
        switch_view(mixer, (enum view)view);
        return;
    case KEY_LEFT:
    case KEY_RIGHT:
        if (node && mixer->ready)
            step_level(mixer, node, key == KEY_RIGHT);
        return;
    case KEY_MUTE:
        if (node && mixer->ready)
            toggle_mute(mixer, node);
        return;
    case KEY_ENTER:
    case KEY_DEFAULT:
        if (!mixer->ready)
            return;
        if ((mixer->view == VIEW_OUTPUT || mixer->view == VIEW_INPUT) && node)
            make_default(mixer, node);
        else if (mixer->view == VIEW_PLAYBACK || mixer->view == VIEW_DEVICES)
            open_picker(mixer);
        return;
    default:
        move_cursor(mixer, key);
        return;
    }
}

/* SGR mouse report: button, column and row, all 1-based. */
static void handle_mouse(struct mixer *mixer, int button, int x, int y)
{
    if (y == 1) {
        for (enum view view = 0; view < VIEW_COUNT; view++)
            if (button == 0 && x >= mixer->tab_start[view] && x < mixer->tab_end[view])
                switch_view(mixer, view);
        return;
    }
    if (y < 1 || (unsigned)y > mixer->height || mixer->row_items[y] < 0)
        return;
    size_t row = (size_t)mixer->row_items[y];
    if (mixer->pick != PICK_NONE) {
        if (button == 0) {
            mixer->pick_cursor = row;
            choose(mixer);
        }
        return;
    }
    select_row(mixer, row);
    struct node *node = selected_node(mixer);
    if (!node || !mixer->ready)
        return;
    if (button == 64 || button == 65) {
        step_level(mixer, node, button == 64);
    } else if (button == 0 && mixer->bar_width && x >= mixer->bar_x && x < mixer->bar_x + mixer->bar_width) {
        /* The cell clicked, as a level: the last cell is 100%. */
        set_level(mixer, node, (x - mixer->bar_x + 1) * 100 / mixer->bar_width);
    }
}

static void escape_expired(pa_mainloop_api *api, pa_time_event *timer, const struct timeval *when, void *data)
{
    (void)when;
    struct mixer *mixer = data;
    api->time_free(timer);
    mixer->escape_timer = NULL;
    if (mixer->input.state == INPUT_ESCAPE) {
        mixer->input.state = INPUT_TEXT;
        handle_key(mixer, KEY_ESCAPE, 0);
    } else {
        mixer->input.state = INPUT_TEXT;
    }
}

static void input_byte(struct mixer *mixer, unsigned char byte)
{
    struct input *input = &mixer->input;
    if (input->state == INPUT_TEXT) {
        static const struct { unsigned char byte; enum key key; } keys[] = {
            {'k', KEY_UP}, {'j', KEY_DOWN}, {'h', KEY_LEFT}, {'l', KEY_RIGHT}, {'-', KEY_LEFT},
            {'+', KEY_RIGHT}, {'=', KEY_RIGHT}, {'g', KEY_HOME}, {'G', KEY_END}, {'\t', KEY_TAB},
            {'\r', KEY_ENTER}, {'\n', KEY_ENTER}, {'d', KEY_DEFAULT}, {'m', KEY_MUTE}, {'q', KEY_QUIT},
            {'\003', KEY_QUIT},
        };
        if (byte == '\033') {
            input->state = INPUT_ESCAPE;
            if (!mixer->escape_timer) {
                struct timeval when;
                pa_gettimeofday(&when);
                pa_timeval_add(&when, (pa_usec_t)MIXER_ESCAPE_MS * PA_USEC_PER_MSEC);
                mixer->escape_timer = mixer->api->time_new(mixer->api, &when, escape_expired, mixer);
            }
            return;
        }
        if (byte >= '1' && byte <= '0' + VIEW_COUNT) {
            handle_key(mixer, KEY_VIEW, byte - '1');
            return;
        }
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            if (keys[i].byte == byte)
                handle_key(mixer, keys[i].key, 0);
        return;
    }
    if (input->state == INPUT_ESCAPE) {
        if (byte == '[' || byte == 'O') {
            input->state = INPUT_CSI;
            input->count = 0;
            input->mouse = false;
            memset(input->params, 0, sizeof input->params);
            return;
        }
        input->state = INPUT_TEXT;
        handle_key(mixer, KEY_ESCAPE, 0);
        input_byte(mixer, byte);
        return;
    }
    if (byte == '<' && !input->count && !input->params[0]) {
        input->mouse = true;
        return;
    }
    if (byte >= '0' && byte <= '9') {
        int *param = &input->params[input->count < 3 ? input->count : 2];
        if (*param < 10000)
            *param = *param * 10 + (byte - '0');
        return;
    }
    if (byte == ';') {
        if (input->count < 2)
            input->count++;
        return;
    }
    if (byte >= ' ' && byte <= '?')
        return;
    input->state = INPUT_TEXT;
    if (input->mouse) {
        /* Presses only; the wheel arrives as presses of 64 and 65. */
        if (byte == 'M')
            handle_mouse(mixer, input->params[0] & ~(4 | 8 | 16), input->params[1], input->params[2]);
        return;
    }
    switch (byte) {
    case 'A': handle_key(mixer, KEY_UP, 0); break;
    case 'B': handle_key(mixer, KEY_DOWN, 0); break;
    case 'C': handle_key(mixer, KEY_RIGHT, 0); break;
    case 'D': handle_key(mixer, KEY_LEFT, 0); break;
    case 'H': handle_key(mixer, KEY_HOME, 0); break;
    case 'F': handle_key(mixer, KEY_END, 0); break;
    case 'Z': handle_key(mixer, KEY_BACKTAB, 0); break;
    case '~':
        if (input->params[0] == 1 || input->params[0] == 7)
            handle_key(mixer, KEY_HOME, 0);
        else if (input->params[0] == 4 || input->params[0] == 8)
            handle_key(mixer, KEY_END, 0);
        break;
    }
}

static void keyboard_ready(pa_mainloop_api *api, pa_io_event *event, int fd, pa_io_event_flags_t flags, void *data)
{
    (void)api;
    (void)event;
    struct mixer *mixer = data;
    unsigned char bytes[256];
    ssize_t length = (flags & PA_IO_EVENT_INPUT) ? read(fd, bytes, sizeof bytes) : 0;
    if (length < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (length <= 0) {
        mixer->quit = true;
        return;
    }
    for (ssize_t i = 0; i < length && !mixer->quit; i++)
        input_byte(mixer, bytes[i]);
}

static void palette_ready(pa_mainloop_api *api, pa_io_event *event, int fd, pa_io_event_flags_t flags, void *data)
{
    (void)api;
    (void)event;
    (void)fd;
    struct mixer *mixer = data;
    if (desk_watch_changed(&mixer->palette_watch, (flags & PA_IO_EVENT_INPUT) ? POLLIN : POLLERR))
        apply_theme(mixer);
}

/* While meters run, redraw now and then so peaks fade when the sound stops. */
static void tick(pa_mainloop_api *api, pa_time_event *timer, const struct timeval *when, void *data)
{
    (void)when;
    struct mixer *mixer = data;
    if (mixer->meter_count)
        mixer->dirty = true;
    struct timeval next;
    pa_gettimeofday(&next);
    pa_timeval_add(&next, (pa_usec_t)MIXER_TICK_MS * PA_USEC_PER_MSEC);
    api->time_restart(timer, &next);
}

static void on_signal(pa_mainloop_api *api, pa_signal_event *event, int signal, void *data)
{
    (void)api;
    (void)event;
    struct mixer *mixer = data;
    if (signal == SIGWINCH) {
        terminal_size(mixer);
        return;
    }
    mixer->signal = signal;
    mixer->quit = true;
}

static int run_mixer(const struct config *config, bool source)
{
    struct mixer mixer = {
        .config = *config,
        .view = source ? VIEW_INPUT : VIEW_OUTPUT,
        .palette_watch = {.fd = -1},
        .status = EXIT_OK,
    };
    setlocale(LC_CTYPE, "");
    mixer.palette_path = desk_palette_path();
    if (mixer.palette_path && !desk_watch_open(&mixer.palette_watch, mixer.palette_path))
        mixer.palette_watch.fd = -1;
    apply_theme(&mixer);

    mixer.loop = pa_mainloop_new();
    if (!mixer.loop) {
        fputs("vol: out of memory\n", stderr);
        return EXIT_FAILED;
    }
    mixer.api = pa_mainloop_get_api(mixer.loop);
    signal(SIGPIPE, SIG_IGN);
    if (pa_signal_init(mixer.api) < 0 || terminal_enter() < 0) {
        int error = errno;
        terminal_restore();
        fprintf(stderr, "vol: terminal setup: %s\n", strerror(error));
        pa_mainloop_free(mixer.loop);
        return EXIT_FAILED;
    }
    static const int signals[] = {SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
        pa_signal_new(signals[i], on_signal, &mixer);
    mixer.keyboard = mixer.api->io_new(mixer.api, STDIN_FILENO, PA_IO_EVENT_INPUT | PA_IO_EVENT_HANGUP,
                                       keyboard_ready, &mixer);
    if (mixer.palette_watch.fd >= 0)
        mixer.palette_event = mixer.api->io_new(mixer.api, mixer.palette_watch.fd, PA_IO_EVENT_INPUT,
                                                palette_ready, &mixer);
    struct timeval when;
    pa_gettimeofday(&when);
    mixer.tick = mixer.api->time_new(mixer.api, &when, tick, &mixer);
    terminal_size(&mixer);
    mixer_connect(&mixer);

    int result = 0;
    while (!mixer.quit) {
        if (mixer.dirty && render(&mixer) < 0) {
            mixer.status = EXIT_FAILED;
            break;
        }
        if (pa_mainloop_iterate(mixer.loop, 1, &result) < 0) {
            mixer.status = EXIT_FAILED;
            break;
        }
    }

    terminal_restore();
    mixer_disconnect(&mixer);
    free_model(&mixer.now);
    free_model(&mixer.next);
    free(mixer.meters);
    mixer.api->io_free(mixer.keyboard);
    if (mixer.palette_event)
        mixer.api->io_free(mixer.palette_event);
    mixer.api->time_free(mixer.tick);
    if (mixer.escape_timer)
        mixer.api->time_free(mixer.escape_timer);
    if (mixer.reconnect_timer)
        mixer.api->time_free(mixer.reconnect_timer);
    pa_signal_done();
    pa_mainloop_free(mixer.loop);
    desk_watch_close(&mixer.palette_watch);
    free(mixer.palette_path);
    free(mixer.lines);
    free(mixer.row_items);
    return mixer.signal ? 128 + mixer.signal : mixer.status;
}

/* Command line */

static void usage(FILE *out)
{
    fputs("Usage: vol [-s] [COMMAND]\n"
          "       vol [-s] --follow\n"
          "\n"
          "Read or change the volume of the default output, or mix every output,\n"
          "input, stream and card.\n"
          "\n"
          "Commands:\n"
          "  (none)                   in a terminal, open the mixer (on Input with -s);\n"
          "                           otherwise the same as get\n"
          "  get                      print the level, as wpctl does: Volume: 0.30 [MUTED]\n"
          "  up [N] | down [N]        change it by N percent (default: [volume] step, 5)\n"
          "  set N                    set it to N percent (0 to 150)\n"
          "  mute [on|off|toggle]     mute or unmute (default: toggle)\n"
          "\n"
          "Options:\n"
          "  -s, --source             the default input (the mic) instead of the output\n"
          "  -f, --follow             print the level again on every change, until killed\n"
          "  -h, --help               show this help\n"
          "  -v, --version            show the version\n"
          "\n"
          "up stops at [volume] max (100 by default) in ~/.config/vol/vol.conf.\n", out);
}

/* "30" or "30%". */
static bool parse_percent(const char *text, int *value)
{
    char number[8];
    size_t length = strlen(text);
    if (length && text[length - 1] == '%')
        --length;
    if (!length || length >= sizeof number)
        return false;
    memcpy(number, text, length);
    number[length] = '\0';
    return parse_int(number, 0, VOL_MAX_LIMIT, value);
}

static bool parse_command(struct app *app, int count, char **words)
{
    app->amount = -1;
    if (count == 0)
        return true;
    const char *command = words[0];
    if (!strcmp(command, "get")) {
        app->action = ACTION_GET;
        return count == 1;
    }
    if (!strcmp(command, "up") || !strcmp(command, "down")) {
        app->action = !strcmp(command, "up") ? ACTION_UP : ACTION_DOWN;
        return count == 1 || (count == 2 && parse_percent(words[1], &app->amount));
    }
    if (!strcmp(command, "set")) {
        app->action = ACTION_SET;
        return count == 2 && parse_percent(words[1], &app->amount);
    }
    if (!strcmp(command, "mute")) {
        app->action = ACTION_MUTE;
        if (count == 1 || (count == 2 && !strcmp(words[1], "toggle")))
            app->mute = MUTE_TOGGLE;
        else if (count == 2 && !strcmp(words[1], "on"))
            app->mute = MUTE_ON;
        else if (count == 2 && !strcmp(words[1], "off"))
            app->mute = MUTE_OFF;
        else
            return false;
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"source", no_argument, NULL, 's'},
        {"follow", no_argument, NULL, 'f'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {0},
    };
    struct app app = {.action = ACTION_GET};
    bool follow = false;
    int option;
    /* "+" stops at the first command word, so "vol down 5" isn't read as options. */
    while ((option = getopt_long(argc, argv, "+sfhv", options, NULL)) != -1) {
        switch (option) {
        case 's':
            app.source = true;
            break;
        case 'f':
            follow = true;
            break;
        case 'h':
            usage(stdout);
            return EXIT_OK;
        case 'v':
            puts("vol " VOL_VERSION);
            return EXIT_OK;
        default:
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (!parse_command(&app, argc - optind, argv + optind) || (follow && optind != argc)) {
        usage(stderr);
        return EXIT_USAGE;
    }
    if (follow)
        app.action = ACTION_FOLLOW;
    load_config(&app.config);
    if (optind == argc && !follow && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
        return run_mixer(&app.config, app.source);

    app.loop = pa_mainloop_new();
    if (!app.loop) {
        fprintf(stderr, "vol: out of memory\n");
        return EXIT_FAILED;
    }
    app.api = pa_mainloop_get_api(app.loop);
    if (app.action != ACTION_FOLLOW)
        set_timer(&app, VOL_TIMEOUT_MS, timed_out);
    connect_server(&app);

    int status = EXIT_FAILED;
    if (pa_mainloop_run(app.loop, &status) < 0)
        status = EXIT_FAILED;
    cancel_timer(&app);
    if (app.context) {
        /* Disconnecting ends in TERMINATED, which isn't a failure here. */
        pa_context_set_state_callback(app.context, NULL, NULL);
        pa_context_disconnect(app.context);
        pa_context_unref(app.context);
    }
    pa_mainloop_free(app.loop);
    return status;
}
