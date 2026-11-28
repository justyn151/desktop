/* vol: audio and volume control. 0.1 is the command line: read and change the
   default output or input, and follow it for bar. It talks to PipeWire through
   pipewire-pulse with libpulse, which keeps the device's route volume that
   WirePlumber saves and restores. */
#define _GNU_SOURCE

#include "vol.h"

#include "desk.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pulse/pulseaudio.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum action { ACTION_GET, ACTION_UP, ACTION_DOWN, ACTION_SET, ACTION_MUTE, ACTION_FOLLOW };
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

/* Command line */

static void usage(FILE *out)
{
    fputs("Usage: vol [-s] [COMMAND]\n"
          "       vol [-s] --follow\n"
          "\n"
          "Read or change the volume of the default output.\n"
          "\n"
          "Commands:\n"
          "  (none)                   print the level, as wpctl does: Volume: 0.30 [MUTED]\n"
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
