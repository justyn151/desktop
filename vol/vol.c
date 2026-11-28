/* vol: audio and volume control. The command line reads and changes the
   default output or input and follows it for bar; vol mixer is a window with
   every output, input, stream and card. It talks
   to PipeWire through pipewire-pulse with libpulse, which keeps the device's
   route volume that WirePlumber saves and restores. */
#define _GNU_SOURCE

#include "vol.h"

#include "desk.h"

#include "cursor-shape-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
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
#include <sys/file.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>

enum action { ACTION_GET, ACTION_UP, ACTION_DOWN, ACTION_SET, ACTION_MUTE, ACTION_FOLLOW, ACTION_MIXER };
enum mute_change { MUTE_TOGGLE, MUTE_ON, MUTE_OFF };

struct config {
    int step, max;
    int start_view;         /* The mixer's first view: 0 Output to 3 Devices. */
    bool show_monitors;     /* List outputs' monitors under Input. */
};

static const char *const start_views[] = {"output", "input", "playback", "devices"};

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

static void mixer_entry(struct config_parse *parse, const char *key, const char *value, int line)
{
    if (!strcmp(key, "start_view")) {
        for (int i = 0; i < 4; i++)
            if (!strcmp(value, start_views[i])) {
                parse->config->start_view = i;
                return;
            }
        fprintf(stderr, "vol: %s:%d: start_view must be output, input, playback or devices\n", parse->path, line);
    } else if (!strcmp(key, "show_monitors")) {
        if (!strcmp(value, "true"))
            parse->config->show_monitors = true;
        else if (!strcmp(value, "false"))
            parse->config->show_monitors = false;
        else
            fprintf(stderr, "vol: %s:%d: show_monitors must be true or false\n", parse->path, line);
    } else {
        fprintf(stderr, "vol: %s:%d: unknown key %s\n", parse->path, line, key);
    }
}

static void config_entry(void *data, const char *section, const char *key, const char *value, int line)
{
    struct config_parse *parse = data;
    if (!key) {
        fprintf(stderr, "vol: %s:%d: expected key = value\n", parse->path, line);
        return;
    }
    if (!strcmp(section, "mixer")) {
        mixer_entry(parse, key, value, line);
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

/* The mixer: a window with every output, input, playing stream and sound card,
   kept live from the server's events. Wayland and the sound server share
   libpulse's main loop: the display, key repeat and palette file are its io
   events, so there is one loop and no threads. */

enum view { VIEW_OUTPUT, VIEW_INPUT, VIEW_PLAYBACK, VIEW_DEVICES, VIEW_COUNT };

static const char *const view_names[VIEW_COUNT] = {"Output", "Input", "Playback", "Devices"};

#define FONT_POINTS 11.0     /* At 96 dpi, like files. */
#define PADDING 12           /* Logical pixels. */
#define WINDOW_WIDTH 560
#define WINDOW_HEIGHT 480
#define WHEEL_PIXELS 15.0    /* Touchpad distance per level step. */
#define MAX_REPEATS 8        /* Repeats handled at once after a stall. */

/* Symbols Nerd Font (Font Awesome range). */
#define ICON_SPEAKER 0xf028
#define ICON_SPEAKER_OFF 0xf026
#define ICON_MIC 0xf130
#define ICON_MIC_OFF 0xf131

struct colors {
    uint32_t background, foreground, accent, highlight, muted, urgent;
};

/* Without a palette, the same colors as files. */
static const struct colors default_colors = {
    .background = 0xff1a1b26,
    .foreground = 0xffc0caf5,
    .accent = 0xff7aa2f7,
    .highlight = 0xffbb9af7,
    .muted = 0xff565f89,
    .urgent = 0xfff7768e,
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
    bool is_monitor;        /* Sources: an output's monitor ([mixer] show_monitors). */
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

/* What's under the pointer. */
enum target_kind {
    TARGET_NONE, TARGET_TAB, TARGET_ROW, TARGET_DEFAULT, TARGET_MUTE, TARGET_SLIDER, TARGET_DROPDOWN,
    TARGET_CHOICE, TARGET_PANEL, TARGET_OUTSIDE,
};

struct target {
    enum target_kind kind;
    size_t index;
};

struct hit {
    struct target target;
    int left, top, right, bottom;   /* Buffer pixels. */
};

struct mixer {
    struct config config;
    pa_mainloop *loop;
    pa_mainloop_api *api;
    pa_context *context;
    pa_time_event *reconnect_timer, *tick;
    pa_io_event *wayland_event, *repeat_event, *palette_event;
    bool ready, listed, running;
    int status, signal;

    /* Lists are read into next, then swapped in whole. One refresh at a time,
       and one more if anything changed meanwhile. */
    struct model now, next;
    int pending;
    bool again;

    struct meter **meters;
    size_t meter_count;

    enum view view;
    size_t cursor[VIEW_COUNT];
    uint32_t cursor_id[VIEW_COUNT];
    int scroll;                     /* Buffer pixels the list is scrolled by. */
    bool reveal;                    /* Scroll the cursor's row into view on the next paint. */
    enum pick pick;
    uint32_t pick_target;
    size_t pick_cursor;
    int pick_scroll;

    /* Wayland */
    struct wl_display *display;
    struct wl_registry *registry;
    DeskGlobals globals;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *wl_keyboard;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape;
    DeskWindow window;
    DeskKeyboard keyboard;
    DeskRepeat repeat;
    bool read_prepared;

    DeskFont *font;
    struct colors colors;
    char *palette_path;
    DeskWatch palette_watch;
    char notice[192];

    /* The pointer, in logical pixels, and what the last paint put where. */
    double pointer_x, pointer_y;
    bool pointer_inside;
    struct target hover;
    struct hit *hits;
    size_t hit_count, hit_capacity;
    int row_height, list_top, list_bottom, pick_row_height, pick_rows;
    int scroll_steps;
    double scroll_pixels;
    bool dragging;                  /* A slider follows the pointer. */
    uint32_t drag_id;
    int drag_left, drag_right;
};

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

static void redraw(struct mixer *mixer)
{
    desk_window_redraw(&mixer->window);
}

static void notice(struct mixer *mixer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(mixer->notice, sizeof mixer->notice, format, args);
    va_end(args);
    redraw(mixer);
}

static void update_colors(struct mixer *mixer)
{
    DeskPalette palette;
    if (!mixer->palette_path || desk_palette_load(mixer->palette_path, &palette) != 0) {
        mixer->colors = default_colors;
        return;
    }
    mixer->colors = (struct colors){
        .background = 0xff000000u | palette.rgb[DESK_BACKGROUND],
        .foreground = 0xff000000u | palette.rgb[DESK_FOREGROUND],
        .accent = 0xff000000u | palette.rgb[DESK_ACCENT],
        .highlight = 0xff000000u | palette.rgb[DESK_HIGHLIGHT],
        .muted = 0xff000000u | palette.rgb[DESK_MUTED],
        .urgent = 0xff000000u | palette.rgb[DESK_URGENT],
    };
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

static struct node *node_at(struct mixer *mixer, size_t row)
{
    struct nodes *nodes = view_nodes(mixer, mixer->view);
    return nodes && row < nodes->count ? &nodes->items[row] : NULL;
}

static struct node *selected_node(struct mixer *mixer)
{
    return node_at(mixer, mixer->cursor[mixer->view]);
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
    redraw(mixer);
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
            redraw(meter->mixer);
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

/* Meters for the view on screen: each output's monitor, or each input. */
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
        mixer->meters[mixer->meter_count++] = open_meter(mixer, source, monitor || nodes->items[j].is_monitor);
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
    redraw(mixer);
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
    /* An output's monitor is a source too, but not an input anyone means,
       unless asked for. */
    if (info->monitor_of_sink != PA_INVALID_INDEX && !mixer->config.show_monitors)
        return;
    struct node *node = add_node(&mixer->next.sources);
    node->is_monitor = info->monitor_of_sink != PA_INVALID_INDEX;
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
    if (!node->volume.channels || !mixer->ready)
        return;
    if (percent < 0)
        percent = 0;
    if (percent > VOL_MAX_LIMIT)
        percent = VOL_MAX_LIMIT;
    if (percent == percent_of(pa_cvolume_max(&node->volume)))
        return;
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
    redraw(mixer);
}

/* Up stops at [volume] max, but never lowers a level already above it. */
static void step_level(struct mixer *mixer, struct node *node, bool up)
{
    int now = percent_of(pa_cvolume_max(&node->volume)), step = mixer->config.step;
    int next = up ? now + step : now - step;
    if (up && next > mixer->config.max)
        next = now > mixer->config.max ? now : mixer->config.max;
    set_level(mixer, node, next);
}

static void toggle_mute(struct mixer *mixer, struct node *node)
{
    if (!mixer->ready)
        return;
    node->muted = !node->muted;
    pa_context *context = mixer->context;
    if (mixer->view == VIEW_OUTPUT)
        send(mixer, pa_context_set_sink_mute_by_index(context, node->index, node->muted, changed_reply, mixer));
    else if (mixer->view == VIEW_INPUT)
        send(mixer, pa_context_set_source_mute_by_index(context, node->index, node->muted, changed_reply, mixer));
    else
        send(mixer, pa_context_set_sink_input_mute(context, node->index, node->muted, changed_reply, mixer));
    redraw(mixer);
}

static void make_default(struct mixer *mixer, struct node *node)
{
    if (!mixer->ready || (mixer->view != VIEW_OUTPUT && mixer->view != VIEW_INPUT))
        return;
    if (mixer->view == VIEW_OUTPUT) {
        copy_text(mixer->now.default_sink, sizeof mixer->now.default_sink, node->name);
        send(mixer, pa_context_set_default_sink(mixer->context, node->name, changed_reply, mixer));
    } else {
        copy_text(mixer->now.default_source, sizeof mixer->now.default_source, node->name);
        send(mixer, pa_context_set_default_source(mixer->context, node->name, changed_reply, mixer));
    }
    redraw(mixer);
}

/* The chooser: another output for a stream, or a profile for a card. */
static void open_picker(struct mixer *mixer, size_t row)
{
    if (!mixer->ready)
        return;
    if (mixer->view == VIEW_PLAYBACK) {
        struct node *node = node_at(mixer, row);
        if (!node || !mixer->now.sinks.count)
            return;
        mixer->pick = PICK_SINK;
        mixer->pick_target = node->index;
        mixer->pick_cursor = 0;
        for (size_t i = 0; i < mixer->now.sinks.count; i++)
            if (mixer->now.sinks.items[i].index == node->sink)
                mixer->pick_cursor = i;
    } else if (mixer->view == VIEW_DEVICES && row < mixer->now.card_count) {
        struct card *card = &mixer->now.cards[row];
        if (!card->profile_count)
            return;
        mixer->pick = PICK_PROFILE;
        mixer->pick_target = card->index;
        mixer->pick_cursor = 0;
        for (size_t i = 0; i < card->profile_count; i++)
            if (!strcmp(card->profiles[i].name, card->active))
                mixer->pick_cursor = i;
    } else {
        return;
    }
    mixer->pick_scroll = -1;    /* Centre the current choice on the first paint. */
    mixer->notice[0] = '\0';
    redraw(mixer);
}

static void close_picker(struct mixer *mixer)
{
    mixer->pick = PICK_NONE;
    redraw(mixer);
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
    close_picker(mixer);
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
    mixer->ready = mixer->listed = mixer->dragging = false;
    mixer->pending = 0;
    mixer->again = false;
    mixer->pick = PICK_NONE;
    close_meters(mixer);
    free_model(&mixer->now);
    free_model(&mixer->next);
    redraw(mixer);
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
        notice(mixer, "Can't follow the sound server; changes won't show until reopened");
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
        mixer->running = false;
        mixer->status = EXIT_FAILED;
        return;
    }
    pa_context_set_state_callback(mixer->context, mixer_state, mixer);
    if (pa_context_connect(mixer->context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
        mixer_lost(mixer);
}

/* Drawing */

static uint32_t mix(uint32_t a, uint32_t b, unsigned amount)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        unsigned x = (a >> shift) & 0xff, y = (b >> shift) & 0xff;
        out |= ((x * (255 - amount) + y * amount + 127) / 255) << shift;
    }
    return out;
}

static int scaled(const struct mixer *mixer, double logical)
{
    return (int)lround(logical * desk_window_scale(&mixer->window));
}

/* A disc, smoothed by sampling each pixel 4×4. */
static void fill_circle(DeskCanvas *canvas, double cx, double cy, double radius, uint32_t color)
{
    int left = (int)floor(cx - radius), right = (int)ceil(cx + radius);
    int top = (int)floor(cy - radius), bottom = (int)ceil(cy + radius);
    for (int y = top; y < bottom; y++)
        for (int x = left; x < right; x++) {
            unsigned inside = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    double dx = x + (sx + 0.5) / 4 - cx, dy = y + (sy + 0.5) / 4 - cy;
                    inside += dx * dx + dy * dy <= radius * radius;
                }
            if (inside) {
                uint32_t alpha = (color >> 24) * inside / 16;
                desk_canvas_fill(canvas, x, y, 1, 1, alpha << 24 | (color & 0xffffff));
            }
        }
}

/* A small downward triangle with its top edge at x, y. */
static void draw_arrow(DeskCanvas *canvas, int x, int y, int width, uint32_t color)
{
    for (int row = 0; row * 2 < width; row++)
        desk_canvas_fill(canvas, x + row, y + row, width - 2 * row, 1, color);
}

/* Text cut to fit between left and right, with an ellipsis when it doesn't. */
static int draw_text(DeskFont *font, DeskCanvas *canvas, int left, int right, int baseline, const char *text,
                     uint32_t color)
{
    size_t length = strlen(text);
    if (right <= left)
        return left;
    if (desk_text_width(font, text, length) <= right - left)
        return desk_text_draw(font, canvas, left, baseline, text, length, color);
    static const char ellipsis[] = "…";
    int room = right - left - desk_text_width(font, ellipsis, strlen(ellipsis));
    size_t fit = room > 0 ? desk_text_fit(font, text, length, room) : 0;
    int x = desk_text_draw(font, canvas, left, baseline, text, fit, color);
    return desk_text_draw(font, canvas, x, baseline, ellipsis, strlen(ellipsis), color);
}

static void draw_icon(DeskFont *font, DeskCanvas *canvas, int left, int width, int baseline, uint32_t code,
                      uint32_t color)
{
    char utf8[4] = {(char)(0xe0 | code >> 12), (char)(0x80 | (code >> 6 & 0x3f)), (char)(0x80 | (code & 0x3f)), 0};
    int advance = desk_text_width(font, utf8, 3);
    desk_text_draw(font, canvas, left + (width - advance) / 2, baseline, utf8, 3, color);
}

static void add_hit(struct mixer *mixer, enum target_kind kind, size_t index, int left, int top, int right,
                    int bottom)
{
    if (mixer->hit_count == mixer->hit_capacity) {
        mixer->hit_capacity = mixer->hit_capacity ? 2 * mixer->hit_capacity : 64;
        mixer->hits = grow(mixer->hits, mixer->hit_capacity, sizeof *mixer->hits);
    }
    mixer->hits[mixer->hit_count++] = (struct hit){{kind, index}, left, top, right, bottom};
}

/* The last thing painted under the pointer wins. */
static struct target target_at(const struct mixer *mixer)
{
    if (!mixer->pointer_inside)
        return (struct target){TARGET_NONE, 0};
    double scale = desk_window_scale(&mixer->window);
    int x = (int)lround(mixer->pointer_x * scale), y = (int)lround(mixer->pointer_y * scale);
    for (size_t i = mixer->hit_count; i-- > 0;) {
        const struct hit *hit = &mixer->hits[i];
        if (x >= hit->left && x < hit->right && y >= hit->top && y < hit->bottom)
            return hit->target;
    }
    return (struct target){TARGET_NONE, 0};
}

static bool hovering(const struct mixer *mixer, enum target_kind kind, size_t index)
{
    return mixer->hover.kind == kind && mixer->hover.index == index;
}

/* The metrics of one paint, in buffer pixels. */
struct layout {
    int padding, line, ascent, width, height;
};

static void paint_tabs(struct mixer *mixer, DeskCanvas *canvas, const struct layout *ui)
{
    const struct colors *colors = &mixer->colors;
    int top = ui->padding / 2, height = ui->line + ui->padding;
    int left = ui->padding, width = (ui->width - 2 * ui->padding) / VIEW_COUNT;
    int underline = scaled(mixer, 2) > 1 ? scaled(mixer, 2) : 1;
    for (enum view view = 0; view < VIEW_COUNT; view++) {
        int x = left + (int)view * width;
        bool current = view == mixer->view;
        if (current)
            desk_canvas_fill(canvas, x, top, width - ui->padding / 4, height, mix(colors->background, colors->muted, 35));
        else if (hovering(mixer, TARGET_TAB, view))
            desk_canvas_fill(canvas, x, top, width - ui->padding / 4, height, mix(colors->background, colors->muted, 20));
        if (current)
            desk_canvas_fill(canvas, x, top + height - underline, width - ui->padding / 4, underline, colors->accent);
        const char *name = view_names[view];
        int text = desk_text_width(mixer->font, name, strlen(name));
        desk_text_draw(mixer->font, canvas, x + (width - text) / 2, top + ui->padding / 2 + ui->ascent, name,
                       strlen(name), current ? colors->foreground : colors->muted);
        add_hit(mixer, TARGET_TAB, view, x, top, x + width, top + height);
    }
}

/* A slider from left to right centred on y: the level as a filled track
   with a knob, the meter as a thin line under it. */
static void paint_slider(struct mixer *mixer, DeskCanvas *canvas, const struct layout *ui, int left, int right,
                         int y, int level, int peak, bool muted, bool lit)
{
    const struct colors *colors = &mixer->colors;
    int track = scaled(mixer, 4) > 2 ? scaled(mixer, 4) : 2;
    int span = right - left;
    int filled = (int)lround(span * (level > 100 ? 1.0 : level / 100.0));
    uint32_t fill = muted ? colors->muted : colors->accent;
    desk_canvas_fill(canvas, left, y - track / 2, span, track, mix(colors->background, colors->muted, 45));
    desk_canvas_fill(canvas, left, y - track / 2, filled, track, fill);
    if (peak >= 0 && !muted) {
        int meter = scaled(mixer, 3) > 2 ? scaled(mixer, 3) : 2;
        int width = (int)lround(span * (peak > 100 ? 1.0 : peak / 100.0));
        desk_canvas_fill(canvas, left, y + track / 2 + meter, width, meter, colors->highlight);
    }
    double radius = ui->line * (lit ? 0.36 : 0.3);
    fill_circle(canvas, left + filled, y, radius, muted ? colors->muted : colors->foreground);
}

static void paint_node(struct mixer *mixer, DeskCanvas *canvas, const struct layout *ui, size_t row, int top)
{
    const struct colors *colors = &mixer->colors;
    struct node *node = node_at(mixer, row);
    int left = ui->padding, right = ui->width - ui->padding, inset = ui->padding * 3 / 4;
    int icon = ui->line + ui->padding / 2, text_left = left + icon + ui->padding / 2;
    int baseline = top + inset + ui->ascent;
    int line2 = top + inset + ui->line + ui->padding / 2, middle = line2 + ui->line / 2;

    /* The default: a dot to click, filled for the one in use. */
    int name_right = right;
    if (mixer->view != VIEW_PLAYBACK) {
        bool is_default = !strcmp(node->name, mixer->view == VIEW_OUTPUT ? mixer->now.default_sink :
                                                                           mixer->now.default_source);
        double cx = left + icon / 2.0, cy = top + inset + ui->line / 2.0, radius = ui->line * 0.3;
        uint32_t ring = hovering(mixer, TARGET_DEFAULT, row) ? colors->foreground : colors->muted;
        if (is_default) {
            fill_circle(canvas, cx, cy, radius, colors->accent);
        } else {
            fill_circle(canvas, cx, cy, radius, ring);
            fill_circle(canvas, cx, cy, radius - (scaled(mixer, 1.5) > 1 ? scaled(mixer, 1.5) : 1),
                        mix(colors->background, colors->muted, row == mixer->cursor[mixer->view] ? 25 : 0));
        }
        add_hit(mixer, TARGET_DEFAULT, row, left, top, left + icon, top + inset + ui->line + ui->padding / 4);
    } else {
        /* Where it plays, as a dropdown on the right. */
        struct node *sink = find_node(&mixer->now.sinks, node->sink);
        const char *label = sink ? sink->label : "Unknown output";
        int arrow = ui->line / 2, text = desk_text_width(mixer->font, label, strlen(label));
        int room = (right - text_left) / 2;
        int width = text + arrow + ui->padding * 3 / 2;
        if (width > room)
            width = room;
        int box_left = right - width;
        if (hovering(mixer, TARGET_DROPDOWN, row))
            desk_canvas_fill(canvas, box_left, top + inset - ui->padding / 4, width, ui->line + ui->padding / 2,
                             mix(colors->background, colors->muted, 40));
        draw_text(mixer->font, canvas, box_left + ui->padding / 2, right - arrow - ui->padding, baseline, label,
                  colors->muted);
        draw_arrow(canvas, right - arrow - ui->padding / 2, top + inset + ui->line / 2 - arrow / 4, arrow,
                   colors->muted);
        add_hit(mixer, TARGET_DROPDOWN, row, box_left, top + inset - ui->padding / 4, right,
                top + inset + ui->line + ui->padding / 4);
        name_right = box_left - ui->padding;
    }
    int x = draw_text(mixer->font, canvas, text_left, name_right, baseline, node->label, colors->foreground);
    if (node->detail[0] || node->corked) {
        char detail[sizeof node->detail + 16];
        snprintf(detail, sizeof detail, "%s%s%s", node->detail, node->detail[0] && node->corked ? " · " : "",
                 node->corked ? "paused" : "");
        x = desk_text_draw(mixer->font, canvas, x, baseline, "  ", 2, colors->muted);
        draw_text(mixer->font, canvas, x, name_right, baseline, detail, colors->muted);
    }

    /* Mute, the slider and the percentage. */
    uint32_t code = mixer->view == VIEW_INPUT && !node->is_monitor ? (node->muted ? ICON_MIC_OFF : ICON_MIC) :
                    (node->muted ? ICON_SPEAKER_OFF : ICON_SPEAKER);
    if (hovering(mixer, TARGET_MUTE, row))
        desk_canvas_fill(canvas, left, line2 - ui->padding / 4, icon, ui->line + ui->padding / 2,
                         mix(colors->background, colors->muted, 40));
    draw_icon(mixer->font, canvas, left, icon, line2 + ui->ascent, code, node->muted ? colors->urgent : colors->foreground);
    add_hit(mixer, TARGET_MUTE, row, left, line2 - ui->padding / 4, left + icon, line2 + ui->line + ui->padding / 4);

    int level = percent_of(pa_cvolume_max(&node->volume));
    char percent[16];
    snprintf(percent, sizeof percent, "%d%%", level);
    int percent_width = desk_text_width(mixer->font, "150%", 4);
    int text = desk_text_width(mixer->font, percent, strlen(percent));
    desk_text_draw(mixer->font, canvas, right - text, line2 + ui->ascent, percent, strlen(percent),
                   node->muted ? colors->muted : colors->foreground);
    int slider_left = text_left + ui->line / 3, slider_right = right - percent_width - ui->padding - ui->line / 3;
    uint32_t source = mixer->view == VIEW_OUTPUT ? node->monitor :
                      mixer->view == VIEW_INPUT ? node->index : PA_INVALID_INDEX;
    bool lit = hovering(mixer, TARGET_SLIDER, row) || (mixer->dragging && mixer->drag_id == node->index);
    if (node->has_volume)
        paint_slider(mixer, canvas, ui, slider_left, slider_right, middle, level, meter_percent(mixer, source),
                     node->muted, lit);
    else
        draw_text(mixer->font, canvas, slider_left, slider_right, line2 + ui->ascent, "Fixed level", colors->muted);
    add_hit(mixer, TARGET_SLIDER, row, slider_left - ui->line / 2, line2 - ui->padding / 4,
            slider_right + ui->line / 2, line2 + ui->line + ui->padding / 4);
}

static void paint_card(struct mixer *mixer, DeskCanvas *canvas, const struct layout *ui, size_t row, int top)
{
    const struct colors *colors = &mixer->colors;
    struct card *card = &mixer->now.cards[row];
    int left = ui->padding, right = ui->width - ui->padding, inset = ui->padding * 3 / 4;
    int icon = ui->line + ui->padding / 2, text_left = left + icon + ui->padding / 2;
    draw_icon(mixer->font, canvas, left, icon, top + inset + ui->ascent, ICON_SPEAKER, colors->muted);
    draw_text(mixer->font, canvas, text_left, right, top + inset + ui->ascent, card->label, colors->foreground);

    const char *active = "Off";
    for (size_t i = 0; i < card->profile_count; i++)
        if (!strcmp(card->profiles[i].name, card->active))
            active = card->profiles[i].label;
    int line2 = top + inset + ui->line + ui->padding / 2, arrow = ui->line / 2;
    int box_top = line2 - ui->padding / 4, box_height = ui->line + ui->padding / 2;
    desk_canvas_fill(canvas, text_left, box_top, right - text_left, box_height,
                     mix(colors->background, colors->muted, hovering(mixer, TARGET_DROPDOWN, row) ? 70 : 50));
    draw_text(mixer->font, canvas, text_left + ui->padding / 2, right - arrow - ui->padding, line2 + ui->ascent,
              active, colors->foreground);
    draw_arrow(canvas, right - arrow - ui->padding / 2, line2 + ui->line / 2 - arrow / 4, arrow, colors->foreground);
    add_hit(mixer, TARGET_DROPDOWN, row, text_left, box_top, right, box_top + box_height);
}

static const char *empty_text(const struct mixer *mixer)
{
    if (!mixer->ready)
        return mixer->reconnect_timer ? "The sound server isn't running. Trying again..." : "Connecting...";
    if (!mixer->listed)
        return "";
    static const char *const texts[VIEW_COUNT] = {"No outputs", "No inputs", "Nothing is playing",
                                                  "No sound cards"};
    return texts[mixer->view];
}

static void paint_list(struct mixer *mixer, DeskCanvas *canvas, const struct layout *ui)
{
    const struct colors *colors = &mixer->colors;
    size_t count = mixer->ready ? view_count(mixer, mixer->view) : 0;
    int top = mixer->list_top, bottom = mixer->list_bottom, height = mixer->row_height;
    if (!count) {
        const char *text = empty_text(mixer);
        int width = desk_text_width(mixer->font, text, strlen(text));
        desk_text_draw(mixer->font, canvas, (ui->width - width) / 2, top + (bottom - top) / 2, text, strlen(text),
                       colors->muted);
        return;
    }
    int content = (int)count * height, visible = bottom - top;
    if (mixer->reveal) {
        int row_top = (int)mixer->cursor[mixer->view] * height;
        if (row_top < mixer->scroll)
            mixer->scroll = row_top;
        if (row_top + height > mixer->scroll + visible)
            mixer->scroll = row_top + height - visible;
        mixer->reveal = false;
    }
    if (mixer->scroll > content - visible)
        mixer->scroll = content - visible;
    if (mixer->scroll < 0)
        mixer->scroll = 0;
    DeskCanvas clipped = *canvas;
    desk_canvas_clip(&clipped, 0, top, ui->width, bottom);
    size_t hits_before = mixer->hit_count;
    for (size_t row = 0; row < count; row++) {
        int y = top + (int)row * height - mixer->scroll;
        if (y + height <= top || y >= bottom)
            continue;
        bool selected = row == mixer->cursor[mixer->view];
        if (selected || hovering(mixer, TARGET_ROW, row))
            desk_canvas_fill(&clipped, ui->padding / 2, y, ui->width - ui->padding, height - ui->padding / 4,
                             mix(colors->background, colors->muted, selected ? 25 : 12));
        add_hit(mixer, TARGET_ROW, row, 0, y, ui->width, y + height);
        if (mixer->view == VIEW_DEVICES)
            paint_card(mixer, &clipped, ui, row, y);
        else
            paint_node(mixer, &clipped, ui, row, y);
    }
    /* Nothing outside the list's area is clickable. */
    for (size_t i = hits_before; i < mixer->hit_count; i++) {
        struct hit *hit = &mixer->hits[i];
        if (hit->top < top)
            hit->top = top;
        if (hit->bottom > bottom)
            hit->bottom = bottom;
    }
}

/* The chooser over everything, the rest dimmed. */
static void paint_picker(struct mixer *mixer, DeskCanvas *canvas, const struct layout *ui)
{
    const struct colors *colors = &mixer->colors;
    desk_canvas_fill(canvas, 0, 0, ui->width, ui->height, 0xa0000000u | (colors->background & 0xffffff));
    add_hit(mixer, TARGET_OUTSIDE, 0, 0, 0, ui->width, ui->height);

    char title[256];
    if (mixer->pick == PICK_SINK) {
        struct node *input = find_node(&mixer->now.inputs, mixer->pick_target);
        snprintf(title, sizeof title, "Play %s on", input ? input->label : "this stream");
    } else {
        struct card *card = find_card(&mixer->now, mixer->pick_target);
        snprintf(title, sizeof title, "Profile for %s", card ? card->label : "this card");
    }
    size_t count = pick_count(mixer);
    int row = ui->line + ui->padding, header = ui->line + ui->padding * 3 / 2;
    int width = scaled(mixer, 420) < ui->width - 2 * ui->padding ? scaled(mixer, 420) : ui->width - 2 * ui->padding;
    int room = ui->height - 4 * ui->padding - header - ui->padding / 2;
    int rows = room / row < (int)count ? room / row : (int)count;
    if (rows < 1)
        rows = 1;
    int height = header + rows * row + ui->padding / 2;
    int left = (ui->width - width) / 2, top = (ui->height - height) / 2;
    mixer->pick_row_height = row;
    mixer->pick_rows = rows;
    if (mixer->pick_scroll < 0)
        mixer->pick_scroll = (int)mixer->pick_cursor - rows / 2;
    if ((int)mixer->pick_cursor < mixer->pick_scroll)
        mixer->pick_scroll = (int)mixer->pick_cursor;
    if ((int)mixer->pick_cursor >= mixer->pick_scroll + rows)
        mixer->pick_scroll = (int)mixer->pick_cursor - rows + 1;
    if (mixer->pick_scroll > (int)count - rows)
        mixer->pick_scroll = (int)count - rows;
    if (mixer->pick_scroll < 0)
        mixer->pick_scroll = 0;

    uint32_t panel = mix(colors->background, colors->muted, 18) | 0xff000000u;
    desk_canvas_fill(canvas, left, top, width, height, panel);
    int line = scaled(mixer, 1) > 1 ? scaled(mixer, 1) : 1;
    desk_canvas_fill(canvas, left, top, width, line, colors->muted);
    desk_canvas_fill(canvas, left, top + height - line, width, line, colors->muted);
    desk_canvas_fill(canvas, left, top, line, height, colors->muted);
    desk_canvas_fill(canvas, left + width - line, top, line, height, colors->muted);
    add_hit(mixer, TARGET_PANEL, 0, left, top, left + width, top + height);
    draw_text(mixer->font, canvas, left + ui->padding, left + width - ui->padding, top + ui->padding + ui->ascent,
              title, colors->foreground);

    struct node *input = mixer->pick == PICK_SINK ? find_node(&mixer->now.inputs, mixer->pick_target) : NULL;
    struct card *card = mixer->pick == PICK_PROFILE ? find_card(&mixer->now, mixer->pick_target) : NULL;
    for (int i = 0; i < rows; i++) {
        size_t index = (size_t)(mixer->pick_scroll + i);
        if (index >= count)
            break;
        int y = top + header + i * row;
        const char *label;
        bool current, available = true;
        if (input) {
            struct node *sink = &mixer->now.sinks.items[index];
            label = sink->label;
            current = input->sink == sink->index;
        } else {
            struct profile *profile = &card->profiles[index];
            label = profile->label;
            current = !strcmp(profile->name, card->active);
            available = profile->available;
        }
        if (index == mixer->pick_cursor || hovering(mixer, TARGET_CHOICE, index))
            desk_canvas_fill(canvas, left + line, y, width - 2 * line, row,
                             mix(panel, colors->muted, index == mixer->pick_cursor ? 70 : 40));
        int dot = ui->line;
        if (current)
            fill_circle(canvas, left + ui->padding + dot / 2.0, y + row / 2.0, ui->line * 0.22, colors->accent);
        int right = left + width - ui->padding;
        if (!available) {
            static const char note[] = "unavailable";
            int note_width = desk_text_width(mixer->font, note, strlen(note));
            desk_text_draw(mixer->font, canvas, right - note_width, y + ui->padding / 2 + ui->ascent, note,
                           strlen(note), colors->muted);
            right -= note_width + ui->padding;
        }
        draw_text(mixer->font, canvas, left + ui->padding + dot, right, y + ui->padding / 2 + ui->ascent, label,
                  available ? colors->foreground : colors->muted);
        add_hit(mixer, TARGET_CHOICE, index, left, y, left + width, y + row);
    }
}

static void paint(struct mixer *mixer, DeskCanvas *canvas)
{
    const struct colors *colors = &mixer->colors;
    double scale = desk_window_scale(&mixer->window);
    double size = FONT_POINTS * 96.0 / 72.0 * scale;
    if (fabs(desk_font_size(mixer->font) - size) > 0.01)
        desk_font_resize(mixer->font, size);
    DeskFontMetrics metrics = desk_font_metrics(mixer->font);
    struct layout ui = {
        .padding = (int)lround(PADDING * scale),
        .line = metrics.height,
        .ascent = metrics.ascent,
        .width = canvas->width,
        .height = canvas->height,
    };
    mixer->hit_count = 0;
    for (int y = 0; y < canvas->height; ++y)
        for (int x = 0; x < canvas->width; ++x)
            canvas->pixels[(size_t)y * (size_t)canvas->stride + x] = colors->background;

    paint_tabs(mixer, canvas, &ui);
    mixer->row_height = 2 * ui.line + ui.padding / 2 + 2 * (ui.padding * 3 / 4) + ui.padding / 4;
    mixer->list_top = ui.padding / 2 + ui.line + ui.padding + ui.padding / 2;
    mixer->list_bottom = ui.height - ui.padding / 2;
    if (mixer->notice[0]) {
        int strip = ui.line + ui.padding;
        mixer->list_bottom -= strip;
        desk_canvas_fill(canvas, 0, ui.height - strip, ui.width, strip, mix(colors->background, colors->muted, 25));
        draw_text(mixer->font, canvas, ui.padding, ui.width - ui.padding, ui.height - strip + ui.padding / 2 + ui.ascent,
                  mixer->notice, colors->foreground);
    }
    paint_list(mixer, canvas, &ui);
    if (mixer->pick != PICK_NONE)
        paint_picker(mixer, canvas, &ui);
}

static void window_draw(void *data, DeskWindow *window)
{
    struct mixer *mixer = data;
    DeskBuffer *buffer = desk_window_buffer(window, WL_SHM_FORMAT_XRGB8888);
    if (!buffer)
        return; /* Both busy: retried after a release. */
    DeskCanvas canvas = desk_canvas(buffer->pixels, buffer->width, buffer->height);
    paint(mixer, &canvas);
    desk_window_commit(window, buffer);
    /* The layout may have moved under a still pointer. */
    struct target hover = target_at(mixer);
    if (hover.kind != mixer->hover.kind || hover.index != mixer->hover.index) {
        mixer->hover = hover;
        window->dirty = true;
    }
}

static void window_close(void *data, DeskWindow *window)
{
    (void)window;
    struct mixer *mixer = data;
    mixer->running = false;
}

static void window_initial_size(void *data, DeskWindow *window, int *width, int *height)
{
    (void)data;
    (void)window;
    *width = WINDOW_WIDTH;
    *height = WINDOW_HEIGHT;
}

static const DeskWindowListener window_listener = {
    .draw = window_draw,
    .close = window_close,
    .initial_size = window_initial_size,
};

/* Input */

static void switch_view(struct mixer *mixer, enum view view)
{
    if (view == mixer->view)
        return;
    mixer->view = view;
    mixer->pick = PICK_NONE;
    mixer->dragging = false;
    mixer->scroll = 0;
    mixer->reveal = true;
    mixer->notice[0] = '\0';
    sync_meters(mixer);
    redraw(mixer);
}

static void move_cursor(struct mixer *mixer, int delta, bool absolute)
{
    if (mixer->pick != PICK_NONE) {
        size_t count = pick_count(mixer);
        if (!count)
            return;
        long row = absolute ? (delta < 0 ? 0 : (long)count - 1) : (long)mixer->pick_cursor + delta;
        mixer->pick_cursor = row < 0 ? 0 : row >= (long)count ? count - 1 : (size_t)row;
        redraw(mixer);
        return;
    }
    size_t count = view_count(mixer, mixer->view);
    if (!count)
        return;
    long row = absolute ? (delta < 0 ? 0 : (long)count - 1) : (long)mixer->cursor[mixer->view] + delta;
    select_row(mixer, row < 0 ? 0 : (size_t)row);
    mixer->reveal = true;
}

/* Return whether the key may repeat. */
static bool handle_key(struct mixer *mixer, uint32_t key)
{
    xkb_keysym_t sym = desk_keyboard_sym(&mixer->keyboard, key);
    unsigned mods = desk_keyboard_mods(&mixer->keyboard);
    bool ctrl = mods & DESK_MOD_CTRL;
    if (ctrl && (sym == XKB_KEY_q || sym == XKB_KEY_w)) {
        mixer->running = false;
        return false;
    }
    if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab || (ctrl && (sym == XKB_KEY_Page_Down || sym == XKB_KEY_Page_Up))) {
        bool back = sym == XKB_KEY_ISO_Left_Tab || (mods & DESK_MOD_SHIFT) || sym == XKB_KEY_Page_Up;
        switch_view(mixer, (mixer->view + (back ? VIEW_COUNT - 1 : 1)) % VIEW_COUNT);
        return false;
    }
    if (sym >= XKB_KEY_1 && sym < XKB_KEY_1 + VIEW_COUNT) {
        switch_view(mixer, (enum view)(sym - XKB_KEY_1));
        return false;
    }
    switch (sym) {
    case XKB_KEY_Escape:
        if (mixer->pick != PICK_NONE)
            close_picker(mixer);
        else
            mixer->running = false;
        return false;
    case XKB_KEY_q:
        mixer->running = false;
        return false;
    case XKB_KEY_Up:
    case XKB_KEY_k:
        move_cursor(mixer, -1, false);
        return true;
    case XKB_KEY_Down:
    case XKB_KEY_j:
        move_cursor(mixer, 1, false);
        return true;
    case XKB_KEY_Home:
    case XKB_KEY_g:
        move_cursor(mixer, -1, true);
        return false;
    case XKB_KEY_End:
    case XKB_KEY_G:
        move_cursor(mixer, 1, true);
        return false;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_d:
        if (mixer->pick != PICK_NONE)
            choose(mixer);
        else if (mixer->view == VIEW_PLAYBACK || mixer->view == VIEW_DEVICES)
            open_picker(mixer, mixer->cursor[mixer->view]);
        else if (selected_node(mixer))
            make_default(mixer, selected_node(mixer));
        return false;
    default:
        break;
    }
    struct node *node = mixer->pick == PICK_NONE ? selected_node(mixer) : NULL;
    if (!node)
        return false;
    switch (sym) {
    case XKB_KEY_Left:
    case XKB_KEY_h:
    case XKB_KEY_minus:
    case XKB_KEY_KP_Subtract:
        step_level(mixer, node, false);
        return true;
    case XKB_KEY_Right:
    case XKB_KEY_l:
    case XKB_KEY_plus:
    case XKB_KEY_equal:
    case XKB_KEY_KP_Add:
        step_level(mixer, node, true);
        return true;
    case XKB_KEY_m:
        toggle_mute(mixer, node);
        return false;
    default:
        return false;
    }
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    (void)keyboard;
    struct mixer *mixer = data;
    desk_keyboard_keymap(&mixer->keyboard, format, fd, size);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
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
    struct mixer *mixer = data;
    desk_repeat_cancel(&mixer->repeat);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    struct mixer *mixer = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        desk_repeat_release(&mixer->repeat, key);
        return;
    }
    if (!mixer->keyboard.state)
        return;
    if (handle_key(mixer, key) && desk_keyboard_repeats(&mixer->keyboard, key))
        desk_repeat_start(&mixer->repeat, key);
    else
        desk_repeat_cancel(&mixer->repeat);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)keyboard;
    (void)serial;
    struct mixer *mixer = data;
    desk_keyboard_modifiers(&mixer->keyboard, depressed, latched, locked, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)keyboard;
    struct mixer *mixer = data;
    desk_repeat_info(&mixer->repeat, rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* The level for the pointer's place on the slider being dragged. */
static void drag_to_pointer(struct mixer *mixer)
{
    struct node *node = find_node(view_nodes(mixer, mixer->view) ? view_nodes(mixer, mixer->view) : &mixer->now.sinks,
                                  mixer->drag_id);
    if (!node || mixer->drag_right <= mixer->drag_left) {
        mixer->dragging = false;
        return;
    }
    double x = mixer->pointer_x * desk_window_scale(&mixer->window);
    double fraction = (x - mixer->drag_left) / (mixer->drag_right - mixer->drag_left);
    set_level(mixer, node, (int)lround((fraction < 0 ? 0 : fraction > 1 ? 1 : fraction) * 100));
}

static void update_hover(struct mixer *mixer)
{
    struct target hover = target_at(mixer);
    if (hover.kind != mixer->hover.kind || hover.index != mixer->hover.index) {
        mixer->hover = hover;
        redraw(mixer);
    }
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)surface;
    struct mixer *mixer = data;
    mixer->pointer_inside = true;
    mixer->pointer_x = wl_fixed_to_double(x);
    mixer->pointer_y = wl_fixed_to_double(y);
    if (mixer->cursor_shape)
        wp_cursor_shape_device_v1_set_shape(mixer->cursor_shape, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    update_hover(mixer);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    struct mixer *mixer = data;
    mixer->pointer_inside = mixer->dragging = false;
    mixer->scroll_pixels = 0;
    update_hover(mixer);
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)time;
    struct mixer *mixer = data;
    mixer->pointer_x = wl_fixed_to_double(x);
    mixer->pointer_y = wl_fixed_to_double(y);
    if (mixer->dragging)
        drag_to_pointer(mixer);
    else
        update_hover(mixer);
}

static void click(struct mixer *mixer)
{
    struct target target = target_at(mixer);
    if (mixer->pick != PICK_NONE) {
        if (target.kind == TARGET_CHOICE) {
            mixer->pick_cursor = target.index;
            choose(mixer);
        } else if (target.kind == TARGET_OUTSIDE) {
            close_picker(mixer);
        }
        return;
    }
    if (target.kind == TARGET_TAB) {
        switch_view(mixer, (enum view)target.index);
        return;
    }
    if (target.kind == TARGET_NONE)
        return;
    select_row(mixer, target.index);
    struct node *node = node_at(mixer, target.index);
    switch (target.kind) {
    case TARGET_DEFAULT:
        if (node)
            make_default(mixer, node);
        break;
    case TARGET_MUTE:
        if (node)
            toggle_mute(mixer, node);
        break;
    case TARGET_SLIDER:
        if (node && node->has_volume) {
            /* The slider's own extent, without the grab margin. */
            for (size_t i = mixer->hit_count; i-- > 0;)
                if (mixer->hits[i].target.kind == TARGET_SLIDER && mixer->hits[i].target.index == target.index) {
                    mixer->drag_left = mixer->hits[i].left + mixer->row_height / 6;
                    mixer->drag_right = mixer->hits[i].right - mixer->row_height / 6;
                    break;
                }
            mixer->dragging = true;
            mixer->drag_id = node->index;
            drag_to_pointer(mixer);
        }
        break;
    case TARGET_DROPDOWN:
        open_picker(mixer, target.index);
        break;
    default:
        break;
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    (void)pointer;
    (void)serial;
    (void)time;
    struct mixer *mixer = data;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (button == BTN_LEFT && pressed) {
        click(mixer);
    } else if (button == BTN_LEFT && mixer->dragging) {
        mixer->dragging = false;
        update_hover(mixer);
        redraw(mixer);
    } else if (button == BTN_MIDDLE && pressed && mixer->pick == PICK_NONE) {
        /* Middle click mutes, as on bar. */
        struct target target = target_at(mixer);
        struct node *node = target.kind >= TARGET_ROW && target.kind <= TARGET_DROPDOWN ? node_at(mixer, target.index) : NULL;
        if (node) {
            select_row(mixer, target.index);
            toggle_mute(mixer, node);
        }
    } else if (button == BTN_RIGHT && pressed && mixer->pick != PICK_NONE) {
        close_picker(mixer);
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer;
    (void)time;
    struct mixer *mixer = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        mixer->scroll_pixels += wl_fixed_to_double(value);
}

/* The wheel changes the level over a slider or mute button, a step a notch,
   and scrolls anywhere else. */
static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)pointer;
    struct mixer *mixer = data;
    int steps = mixer->scroll_steps;
    double pixels = mixer->scroll_pixels;
    mixer->scroll_steps = 0;
    if (!steps && pixels == 0)
        return;
    struct target target = target_at(mixer);
    if (mixer->pick != PICK_NONE) {
        if (!steps) {
            steps = (int)(pixels / WHEEL_PIXELS);
            mixer->scroll_pixels -= steps * WHEEL_PIXELS;
        } else {
            mixer->scroll_pixels = 0;
        }
        mixer->pick_scroll += steps;
        if (mixer->pick_scroll < 0)
            mixer->pick_scroll = 0;
        redraw(mixer);
        return;
    }
    struct node *node = target.kind == TARGET_SLIDER || target.kind == TARGET_MUTE ? node_at(mixer, target.index) : NULL;
    if (node) {
        if (!steps) {
            steps = (int)(pixels / WHEEL_PIXELS);
            mixer->scroll_pixels -= steps * WHEEL_PIXELS;
        } else {
            mixer->scroll_pixels = 0;
        }
        for (int i = 0; i < abs(steps) && i < 5; i++)
            step_level(mixer, node, steps < 0);   /* Up, away from you, is louder. */
        return;
    }
    double scale = desk_window_scale(&mixer->window);
    int distance = steps ? steps * mixer->row_height / 2 : (int)lround(pixels * scale);
    mixer->scroll_pixels = 0;
    mixer->scroll += distance;
    redraw(mixer);
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)data;
    (void)pointer;
    (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
    (void)pointer;
    (void)time;
    struct mixer *mixer = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        mixer->scroll_pixels = 0;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)pointer;
    struct mixer *mixer = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        mixer->scroll_steps += discrete;
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
};

/* Seat and registry */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct mixer *mixer = data;
    bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && !mixer->wl_keyboard) {
        mixer->wl_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(mixer->wl_keyboard, &keyboard_listener, mixer);
    } else if (!has_keyboard && mixer->wl_keyboard) {
        desk_repeat_cancel(&mixer->repeat);
        wl_keyboard_release(mixer->wl_keyboard);
        mixer->wl_keyboard = NULL;
    }
    bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && !mixer->pointer) {
        mixer->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(mixer->pointer, &pointer_listener, mixer);
        if (mixer->cursor_shape_manager)
            mixer->cursor_shape = wp_cursor_shape_manager_v1_get_pointer(mixer->cursor_shape_manager, mixer->pointer);
    } else if (!has_pointer && mixer->pointer) {
        if (mixer->cursor_shape)
            wp_cursor_shape_device_v1_destroy(mixer->cursor_shape);
        mixer->cursor_shape = NULL;
        wl_pointer_release(mixer->pointer);
        mixer->pointer = NULL;
        mixer->pointer_inside = mixer->dragging = false;
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

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                            uint32_t version)
{
    struct mixer *mixer = data;
    if (desk_globals_bind(&mixer->globals, registry, name, interface, version))
        return;
    if (!strcmp(interface, wp_cursor_shape_manager_v1_interface.name)) {
        mixer->cursor_shape_manager = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name) && !mixer->seat) {
        /* Version 5 adds wl_keyboard.release. */
        mixer->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(mixer->seat, &seat_listener, mixer);
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

static bool open_window(struct mixer *mixer)
{
    char error[256];
    if (!(mixer->font = desk_font_open("sans-serif", FONT_POINTS * 96.0 / 72.0, error, sizeof error))) {
        fprintf(stderr, "vol: %s\n", error);
        return false;
    }
    if (desk_keyboard_open(&mixer->keyboard) != 0 || desk_repeat_open(&mixer->repeat) != 0) {
        fprintf(stderr, "vol: out of memory\n");
        return false;
    }
    mixer->display = wl_display_connect(NULL);
    if (!mixer->display) {
        fprintf(stderr, "vol: can't connect to the Wayland display\n");
        return false;
    }
    mixer->registry = wl_display_get_registry(mixer->display);
    wl_registry_add_listener(mixer->registry, &registry_listener, mixer);
    if (wl_display_roundtrip(mixer->display) < 0) {
        fprintf(stderr, "vol: lost the Wayland connection\n");
        return false;
    }
    const char *missing = desk_globals_missing(&mixer->globals);
    if (missing) {
        fprintf(stderr, "vol: the compositor doesn't offer %s\n", missing);
        return false;
    }
    /* The seat's capabilities may have arrived before the cursor shape manager. */
    if (mixer->pointer && mixer->cursor_shape_manager && !mixer->cursor_shape)
        mixer->cursor_shape = wp_cursor_shape_manager_v1_get_pointer(mixer->cursor_shape_manager, mixer->pointer);
    if (!desk_window_open(&mixer->window, &mixer->globals, "vol", &window_listener, mixer)) {
        fprintf(stderr, "vol: out of memory\n");
        return false;
    }
    desk_window_set_title(&mixer->window, "Sound");
    return true;
}

static void close_window(struct mixer *mixer)
{
    if (mixer->window.surface)
        desk_window_close(&mixer->window);
    if (mixer->wl_keyboard)
        wl_keyboard_release(mixer->wl_keyboard);
    if (mixer->cursor_shape)
        wp_cursor_shape_device_v1_destroy(mixer->cursor_shape);
    if (mixer->pointer)
        wl_pointer_release(mixer->pointer);
    if (mixer->cursor_shape_manager)
        wp_cursor_shape_manager_v1_destroy(mixer->cursor_shape_manager);
    if (mixer->seat)
        wl_seat_destroy(mixer->seat);
    desk_globals_finish(&mixer->globals);
    if (mixer->registry)
        wl_registry_destroy(mixer->registry);
    if (mixer->display)
        wl_display_disconnect(mixer->display);
    desk_repeat_close(&mixer->repeat);
    desk_keyboard_close(&mixer->keyboard);
    if (mixer->font)
        desk_font_close(mixer->font);
}

/* Main loop hooks */

static void wayland_ready(pa_mainloop_api *api, pa_io_event *event, int fd, pa_io_event_flags_t flags, void *data)
{
    (void)api;
    (void)event;
    (void)fd;
    struct mixer *mixer = data;
    if (mixer->read_prepared) {
        mixer->read_prepared = false;
        if ((flags & PA_IO_EVENT_INPUT) ? wl_display_read_events(mixer->display) < 0 :
                                          (wl_display_cancel_read(mixer->display), false)) {
            mixer->running = false;
            mixer->status = EXIT_FAILED;
        }
    }
    if (flags & (PA_IO_EVENT_HANGUP | PA_IO_EVENT_ERROR)) {
        mixer->running = false;
        mixer->status = EXIT_FAILED;
    }
}

static void repeat_ready(pa_mainloop_api *api, pa_io_event *event, int fd, pa_io_event_flags_t flags, void *data)
{
    (void)api;
    (void)event;
    (void)fd;
    (void)flags;
    struct mixer *mixer = data;
    unsigned count = desk_repeat_fired(&mixer->repeat);
    if (count > MAX_REPEATS)
        count = MAX_REPEATS;
    for (unsigned i = 0; i < count && mixer->repeat.active && mixer->running; i++)
        handle_key(mixer, mixer->repeat.key);
}

static void palette_ready(pa_mainloop_api *api, pa_io_event *event, int fd, pa_io_event_flags_t flags, void *data)
{
    (void)api;
    (void)event;
    (void)fd;
    struct mixer *mixer = data;
    if (desk_watch_changed(&mixer->palette_watch, (flags & PA_IO_EVENT_INPUT) ? POLLIN : POLLERR)) {
        update_colors(mixer);
        redraw(mixer);
    }
}

/* While meters run, redraw now and then so peaks fade when the sound stops. */
static void tick(pa_mainloop_api *api, pa_time_event *timer, const struct timeval *when, void *data)
{
    (void)when;
    struct mixer *mixer = data;
    if (mixer->meter_count)
        redraw(mixer);
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
    mixer->signal = signal;
    mixer->running = false;
}

static int run_mixer(const struct config *config, bool source)
{
    struct mixer mixer = {
        .config = *config,
        .view = source ? VIEW_INPUT : (enum view)config->start_view,
        .palette_watch = {.fd = -1},
        .repeat = {.fd = -1},
        .status = EXIT_OK,
        .running = true,
    };
    setlocale(LC_CTYPE, "");
    /* One mixer at a time: a second click on bar doesn't open another. */
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    int lock = -1;
    if (runtime && *runtime) {
        char path[4096];
        snprintf(path, sizeof path, "%s/vol-mixer.lock", runtime);
        lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) < 0) {
            close(lock);
            if (errno != EWOULDBLOCK)
                lock = -1;
            else {
                fputs("vol: the mixer is already open\n", stderr);
                return EXIT_OK;
            }
        }
    }
    mixer.palette_path = desk_palette_path();
    if (mixer.palette_path && !desk_watch_open(&mixer.palette_watch, mixer.palette_path))
        mixer.palette_watch.fd = -1;
    update_colors(&mixer);
    if (!open_window(&mixer)) {
        close_window(&mixer);
        desk_watch_close(&mixer.palette_watch);
        free(mixer.palette_path);
        return EXIT_FAILED;
    }

    mixer.loop = pa_mainloop_new();
    if (!mixer.loop) {
        fputs("vol: out of memory\n", stderr);
        return EXIT_FAILED;
    }
    mixer.api = pa_mainloop_get_api(mixer.loop);
    signal(SIGPIPE, SIG_IGN);
    if (pa_signal_init(mixer.api) == 0) {
        static const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
        for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
            pa_signal_new(signals[i], on_signal, &mixer);
    }
    mixer.wayland_event = mixer.api->io_new(mixer.api, wl_display_get_fd(mixer.display), PA_IO_EVENT_INPUT,
                                            wayland_ready, &mixer);
    if (mixer.repeat.fd >= 0)
        mixer.repeat_event = mixer.api->io_new(mixer.api, mixer.repeat.fd, PA_IO_EVENT_INPUT, repeat_ready, &mixer);
    if (mixer.palette_watch.fd >= 0)
        mixer.palette_event = mixer.api->io_new(mixer.api, mixer.palette_watch.fd, PA_IO_EVENT_INPUT,
                                                palette_ready, &mixer);
    struct timeval when;
    pa_gettimeofday(&when);
    mixer.tick = mixer.api->time_new(mixer.api, &when, tick, &mixer);
    mixer_connect(&mixer);

    /* Wayland's read protocol around libpulse's poll: prepare and flush, let
       the loop wait on the display's fd with everything else, read if it
       woke, then dispatch and draw. */
    struct wl_display *display = mixer.display;
    int result = 0;
    while (mixer.running) {
        while (wl_display_prepare_read(display) != 0)
            if (wl_display_dispatch_pending(display) < 0) {
                mixer.status = EXIT_FAILED;
                goto done;
            }
        mixer.read_prepared = true;
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            mixer.status = EXIT_FAILED;
            break;
        }
        int iterated = pa_mainloop_iterate(mixer.loop, 1, &result);
        if (mixer.read_prepared) {
            wl_display_cancel_read(display);
            mixer.read_prepared = false;
        }
        if (iterated < 0 || wl_display_dispatch_pending(display) < 0) {
            mixer.status = EXIT_FAILED;
            break;
        }
        desk_window_update(&mixer.window);
    }
done:
    mixer_disconnect(&mixer);
    free_model(&mixer.now);
    free_model(&mixer.next);
    free(mixer.meters);
    free(mixer.hits);
    mixer.api->io_free(mixer.wayland_event);
    if (mixer.repeat_event)
        mixer.api->io_free(mixer.repeat_event);
    if (mixer.palette_event)
        mixer.api->io_free(mixer.palette_event);
    mixer.api->time_free(mixer.tick);
    if (mixer.reconnect_timer)
        mixer.api->time_free(mixer.reconnect_timer);
    pa_signal_done();
    pa_mainloop_free(mixer.loop);
    close_window(&mixer);
    desk_watch_close(&mixer.palette_watch);
    free(mixer.palette_path);
    if (lock >= 0)
        close(lock);
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
          "  (none)                   in a terminal, the same as mixer; otherwise get\n"
          "  mixer                    open the mixer window (on Input with -s)\n"
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
          "up stops at [volume] max (100 by default) in ~/.config/vol/vol.conf;\n"
          "[mixer] start_view picks the mixer's first view.\n", out);
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
    if (!strcmp(command, "mixer")) {
        app->action = ACTION_MIXER;
        return count == 1;
    }
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
    if (app.action == ACTION_MIXER || (optind == argc && !follow && isatty(STDOUT_FILENO)))
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
