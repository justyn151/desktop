/* files --portal: org.freedesktop.impl.portal.FileChooser on the session
   bus, so browsers and other applications get files as their file dialog
   through xdg-desktop-portal.

   Each OpenFile, SaveFile or SaveFiles runs `files --pick` or `--save` as a
   child process and answers when it exits, so a crashing dialog never takes
   the service down and several dialogs can be open at once. Each request
   exports org.freedesktop.impl.portal.Request at its handle; Close kills the
   child. With no dialog open for a minute, the service exits, and D-Bus
   activation starts it again on the next request. */

#define _GNU_SOURCE
#include "files.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#define PORTAL_NAME "org.freedesktop.impl.portal.desktop.files"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define IDLE_USEC (60ull * 1000000ull)

extern char **environ;

/* A filter as the application gave it: a name and (type, pattern) pairs,
   type 0 a glob and 1 a MIME type. */
struct portal_filter {
    char *name;
    uint32_t *types;
    char **patterns;
    size_t count;
};

struct portal_choice {
    char *id, *value;
};

struct request {
    struct request *next;
    struct portal *portal;
    sd_bus_message *call;   /* Answered when the child exits. */
    sd_bus_slot *slot;      /* The Request object. */
    pid_t pid;
    int out;
    sd_event_source *io, *child;
    char *buffer;
    size_t length, capacity;
    bool done_reading, exited, closed;
    int status;
    bool save_files;        /* SaveFiles: the names go into the chosen folder. */
    char **names;
    size_t name_count;
    struct portal_filter *filters;
    size_t filter_count;
    size_t *order;          /* The child's filter index to the application's. */
    struct portal_choice *choices;
    size_t choice_count;
};

struct portal {
    sd_bus *bus;
    sd_event *event;
    sd_event_source *idle;
    struct request *requests;
};

static void filters_free(struct portal_filter *filters, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        free(filters[i].name);
        free(filters[i].types);
        for (size_t j = 0; j < filters[i].count; ++j)
            free(filters[i].patterns[j]);
        free(filters[i].patterns);
    }
    free(filters);
}

static void request_free(struct request *request)
{
    sd_event_source_unref(request->io);
    sd_event_source_unref(request->child);
    sd_bus_slot_unref(request->slot);
    sd_bus_message_unref(request->call);
    if (request->out >= 0)
        close(request->out);
    free(request->buffer);
    for (size_t i = 0; i < request->name_count; ++i)
        free(request->names[i]);
    free(request->names);
    filters_free(request->filters, request->filter_count);
    free(request->order);
    for (size_t i = 0; i < request->choice_count; ++i) {
        free(request->choices[i].id);
        free(request->choices[i].value);
    }
    free(request->choices);
    free(request);
}

static int idle_expired(sd_event_source *source, uint64_t usec, void *data)
{
    (void)source;
    (void)usec;
    struct portal *portal = data;
    if (!portal->requests)
        sd_event_exit(portal->event, 0);
    return 0;
}

static void idle_arm(struct portal *portal)
{
    uint64_t now;
    sd_event_now(portal->event, CLOCK_MONOTONIC, &now);
    if (portal->idle) {
        sd_event_source_set_time(portal->idle, now + IDLE_USEC);
        sd_event_source_set_enabled(portal->idle, SD_EVENT_ONESHOT);
    } else {
        sd_event_add_time(portal->event, &portal->idle, CLOCK_MONOTONIC, now + IDLE_USEC, 0, idle_expired, portal);
    }
}

/* file:// with everything but unreserved characters and '/' escaped, from
   the raw bytes, so names that aren't UTF-8 survive. */
static char *file_uri(const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t length = strlen(path);
    char *uri = malloc(8 + 3 * length);
    if (!uri)
        return NULL;
    char *at = stpcpy(uri, "file://");
    for (const unsigned char *c = (const unsigned char *)path; *c; ++c) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
            strchr("/-._~!$&'()*+,;=:@", *c)) {
            *at++ = (char)*c;
        } else {
            *at++ = '%';
            *at++ = hex[*c >> 4];
            *at++ = hex[*c & 15];
        }
    }
    *at = 0;
    return uri;
}

static int append_filter(sd_bus_message *reply, const struct portal_filter *filter)
{
    int r = sd_bus_message_open_container(reply, 'r', "sa(us)");
    if (r >= 0)
        r = sd_bus_message_append(reply, "s", filter->name);
    if (r >= 0)
        r = sd_bus_message_open_container(reply, 'a', "(us)");
    for (size_t i = 0; r >= 0 && i < filter->count; ++i)
        r = sd_bus_message_append(reply, "(us)", filter->types[i], filter->patterns[i]);
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    return r;
}

/* The child is gone and its output read: answer the application. */
static void request_finish(struct request *request)
{
    struct portal *portal = request->portal;
    /* 0 chosen, 1 cancelled, 2 failed. */
    uint32_t response = request->closed ? 1 : request->status == 0 ? 0 : request->status == 3 ? 1 : 2;
    sd_bus_message *reply = NULL;
    int r = sd_bus_message_new_method_return(request->call, &reply);
    if (r >= 0)
        r = sd_bus_message_append(reply, "u", response);
    if (r >= 0)
        r = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (r >= 0 && response == 0) {
        /* NUL-separated: "filter:N", then the paths. The last may lack its
           NUL; there is always room for one. */
        if (request->buffer)
            request->buffer[request->length] = 0;
        size_t filter = (size_t)-1;
        char **uris = NULL;
        size_t uri_count = 0;
        for (size_t at = 0; at < request->length;) {
            const char *record = request->buffer + at;
            size_t length = strnlen(record, request->length - at);
            at += length + 1;
            if (strncmp(record, "filter:", 7) == 0) {
                filter = strtoul(record + 7, NULL, 10);
                continue;
            }
            if (record[0] != '/')
                continue;
            size_t wanted = request->save_files ? request->name_count : 1;
            char **grown = realloc(uris, (uri_count + wanted) * sizeof *grown);
            if (!grown)
                break;
            uris = grown;
            if (!request->save_files) {
                if ((uris[uri_count] = file_uri(record)))
                    ++uri_count;
                continue;
            }
            for (size_t i = 0; i < request->name_count; ++i) {
                char *path = NULL;
                if (asprintf(&path, "%s/%s", strcmp(record, "/") == 0 ? "" : record, request->names[i]) >= 0 &&
                    (uris[uri_count] = file_uri(path)))
                    ++uri_count;
                free(path);
            }
        }
        r = sd_bus_message_open_container(reply, 'e', "sv");
        if (r >= 0)
            r = sd_bus_message_append(reply, "s", "uris");
        if (r >= 0)
            r = sd_bus_message_open_container(reply, 'v', "as");
        if (r >= 0)
            r = sd_bus_message_open_container(reply, 'a', "s");
        for (size_t i = 0; r >= 0 && i < uri_count; ++i)
            r = sd_bus_message_append(reply, "s", uris[i]);
        for (int i = 0; r >= 0 && i < 3; ++i)
            r = sd_bus_message_close_container(reply);
        for (size_t i = 0; i < uri_count; ++i)
            free(uris[i]);
        free(uris);
        if (r >= 0 && filter < request->filter_count && request->order[filter] < request->filter_count) {
            r = sd_bus_message_open_container(reply, 'e', "sv");
            if (r >= 0)
                r = sd_bus_message_append(reply, "s", "current_filter");
            if (r >= 0)
                r = sd_bus_message_open_container(reply, 'v', "(sa(us))");
            if (r >= 0)
                r = append_filter(reply, &request->filters[request->order[filter]]);
            for (int i = 0; r >= 0 && i < 2; ++i)
                r = sd_bus_message_close_container(reply);
        }
        if (r >= 0 && request->choice_count) {
            /* The application's extra choices, at their defaults. */
            r = sd_bus_message_open_container(reply, 'e', "sv");
            if (r >= 0)
                r = sd_bus_message_append(reply, "s", "choices");
            if (r >= 0)
                r = sd_bus_message_open_container(reply, 'v', "a(ss)");
            if (r >= 0)
                r = sd_bus_message_open_container(reply, 'a', "(ss)");
            for (size_t i = 0; r >= 0 && i < request->choice_count; ++i)
                r = sd_bus_message_append(reply, "(ss)", request->choices[i].id, request->choices[i].value);
            for (int i = 0; r >= 0 && i < 3; ++i)
                r = sd_bus_message_close_container(reply);
        }
    }
    if (r >= 0)
        r = sd_bus_message_close_container(reply);
    if (r >= 0)
        sd_bus_send(portal->bus, reply, NULL);
    else
        sd_bus_reply_method_errorf(request->call, SD_BUS_ERROR_FAILED, "files: couldn't build the reply");
    sd_bus_message_unref(reply);

    for (struct request **at = &portal->requests; *at; at = &(*at)->next) {
        if (*at == request) {
            *at = request->next;
            break;
        }
    }
    request_free(request);
    if (!portal->requests)
        idle_arm(portal);
}

static void request_check(struct request *request)
{
    if (request->exited && request->done_reading)
        request_finish(request);
}

static int request_output(sd_event_source *source, int fd, uint32_t events, void *data)
{
    (void)source;
    (void)events;
    struct request *request = data;
    for (;;) {
        if (request->length + 4097 > request->capacity) {
            size_t capacity = request->capacity ? request->capacity * 2 : 8192;
            char *grown = capacity <= (16u << 20) ? realloc(request->buffer, capacity) : NULL;
            if (!grown)
                break;
            request->buffer = grown;
            request->capacity = capacity;
        }
        ssize_t got = read(fd, request->buffer + request->length, request->capacity - request->length - 1);
        if (got > 0) {
            request->length += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0 && errno == EAGAIN)
            return 0;
        break;
    }
    request->done_reading = true;
    request->io = sd_event_source_unref(request->io);
    close(request->out);
    request->out = -1;
    request_check(request);
    return 0;
}

static int request_exited(sd_event_source *source, const siginfo_t *info, void *data)
{
    (void)source;
    struct request *request = data;
    request->exited = true;
    request->status = info->si_code == CLD_EXITED ? info->si_status : 2;
    request_check(request);
    return 0;
}

/* org.freedesktop.impl.portal.Request.Close: the application gave up. */
static int request_close(sd_bus_message *message, void *data, sd_bus_error *error)
{
    (void)error;
    struct request *request = data;
    request->closed = true;
    if (request->pid > 0 && !request->exited)
        kill(request->pid, SIGTERM);
    return sd_bus_reply_method_return(message, "");
}

static const sd_bus_vtable request_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Close", "", "", request_close, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

/* The bytes of an "ay" option, as a string. */
static int read_bytes(sd_bus_message *message, char **out)
{
    const void *bytes;
    size_t size;
    int r = sd_bus_message_read_array(message, 'y', &bytes, &size);
    if (r < 0)
        return r;
    free(*out);
    *out = strndup(bytes, size); /* NUL-terminated by the spec; strndup stops there. */
    return *out ? 0 : -ENOMEM;
}

/* (sa(us)) */
static int read_filter(sd_bus_message *message, struct portal_filter *filter)
{
    *filter = (struct portal_filter){0};
    const char *name;
    int r = sd_bus_message_enter_container(message, 'r', "sa(us)");
    if (r < 0)
        return r;
    if ((r = sd_bus_message_read(message, "s", &name)) < 0)
        return r;
    filter->name = strdup(name);
    if ((r = sd_bus_message_enter_container(message, 'a', "(us)")) < 0)
        return r;
    uint32_t type;
    const char *pattern;
    /* At the end, reading a struct fails rather than returning 0. */
    while ((r = sd_bus_message_at_end(message, false)) == 0 &&
           (r = sd_bus_message_read(message, "(us)", &type, &pattern)) > 0) {
        uint32_t *types = realloc(filter->types, (filter->count + 1) * sizeof *types);
        char **patterns = realloc(filter->patterns, (filter->count + 1) * sizeof *patterns);
        if (types)
            filter->types = types;
        if (patterns)
            filter->patterns = patterns;
        if (!types || !patterns)
            return -ENOMEM;
        filter->types[filter->count] = type;
        if (!(filter->patterns[filter->count] = strdup(pattern)))
            return -ENOMEM;
        ++filter->count;
    }
    if (r < 0)
        return r;
    if ((r = sd_bus_message_exit_container(message)) < 0)
        return r;
    return sd_bus_message_exit_container(message);
}

/* "Images:*.png;image/jpeg" for --filter. */
static char *filter_argument(const struct portal_filter *filter)
{
    size_t length = strlen(filter->name) + 2;
    for (size_t i = 0; i < filter->count; ++i)
        length += strlen(filter->patterns[i]) + 1;
    char *text = malloc(length);
    if (!text)
        return NULL;
    char *at = text;
    for (const char *c = filter->name; *c; ++c)
        *at++ = *c == ':' ? ' ' : *c; /* The first ':' ends the name. */
    *at++ = ':';
    for (size_t i = 0; i < filter->count; ++i)
        at += sprintf(at, "%s%s", i ? ";" : "", filter->patterns[i]);
    *at = 0;
    return text;
}

static bool same_filter(const struct portal_filter *a, const struct portal_filter *b)
{
    if (strcmp(a->name, b->name) != 0 || a->count != b->count)
        return false;
    for (size_t i = 0; i < a->count; ++i) {
        if (a->types[i] != b->types[i] || strcmp(a->patterns[i], b->patterns[i]) != 0)
            return false;
    }
    return true;
}

enum chooser { CHOOSE_OPEN, CHOOSE_SAVE, CHOOSE_SAVE_FILES };

static int choose(sd_bus_message *message, void *data, sd_bus_error *error, enum chooser kind)
{
    struct portal *portal = data;
    const char *handle, *app_id, *parent, *title;
    int r = sd_bus_message_read(message, "osss", &handle, &app_id, &parent, &title);
    if (r < 0)
        return r;
    (void)parent; /* An xdg-foreign handle; dwl doesn't export them yet. */

    struct request *request = calloc(1, sizeof *request);
    if (!request)
        return -ENOMEM;
    request->portal = portal;
    request->out = -1;
    request->save_files = kind == CHOOSE_SAVE_FILES;
    char *accept = NULL, *current_name = NULL, *current_folder = NULL, *current_file = NULL;
    bool multiple = false, directory = false, have_current = false;
    struct portal_filter current = {0};

    if ((r = sd_bus_message_enter_container(message, 'a', "{sv}")) < 0)
        goto fail;
    while ((r = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        const char *key;
        if ((r = sd_bus_message_read(message, "s", &key)) < 0)
            goto fail;
        if (strcmp(key, "accept_label") == 0 || strcmp(key, "current_name") == 0) {
            const char *value;
            if ((r = sd_bus_message_read(message, "v", "s", &value)) < 0)
                goto fail;
            char **slot = key[0] == 'a' ? &accept : &current_name;
            free(*slot);
            *slot = strdup(value);
        } else if (strcmp(key, "multiple") == 0 || strcmp(key, "directory") == 0) {
            int value;
            if ((r = sd_bus_message_read(message, "v", "b", &value)) < 0)
                goto fail;
            *(key[0] == 'm' ? &multiple : &directory) = value;
        } else if (strcmp(key, "current_folder") == 0 || strcmp(key, "current_file") == 0) {
            if ((r = sd_bus_message_enter_container(message, 'v', "ay")) < 0 ||
                (r = read_bytes(message, strcmp(key, "current_folder") == 0 ? &current_folder : &current_file)) < 0 ||
                (r = sd_bus_message_exit_container(message)) < 0)
                goto fail;
        } else if (strcmp(key, "filters") == 0) {
            if ((r = sd_bus_message_enter_container(message, 'v', "a(sa(us))")) < 0 ||
                (r = sd_bus_message_enter_container(message, 'a', "(sa(us))")) < 0)
                goto fail;
            while ((r = sd_bus_message_at_end(message, false)) == 0) {
                struct portal_filter *grown =
                    realloc(request->filters, (request->filter_count + 1) * sizeof *grown);
                if (!grown) {
                    r = -ENOMEM;
                    goto fail;
                }
                request->filters = grown;
                r = read_filter(message, &request->filters[request->filter_count++]);
                if (r < 0)
                    goto fail;
            }
            if (r < 0 || (r = sd_bus_message_exit_container(message)) < 0 ||
                (r = sd_bus_message_exit_container(message)) < 0)
                goto fail;
        } else if (strcmp(key, "current_filter") == 0) {
            if ((r = sd_bus_message_enter_container(message, 'v', "(sa(us))")) < 0 ||
                (r = read_filter(message, &current)) < 0 || (r = sd_bus_message_exit_container(message)) < 0)
                goto fail;
            have_current = true;
        } else if (strcmp(key, "files") == 0) {
            if ((r = sd_bus_message_enter_container(message, 'v', "aay")) < 0 ||
                (r = sd_bus_message_enter_container(message, 'a', "ay")) < 0)
                goto fail;
            while ((r = sd_bus_message_at_end(message, false)) == 0) {
                char *name = NULL;
                if ((r = read_bytes(message, &name)) < 0)
                    goto fail;
                char **grown = realloc(request->names, (request->name_count + 1) * sizeof *grown);
                if (!grown) {
                    free(name);
                    r = -ENOMEM;
                    goto fail;
                }
                request->names = grown;
                /* Only the name: nothing may climb out of the folder chosen. */
                char *slash = strrchr(name, '/');
                request->names[request->name_count++] = slash ? strdup(slash + 1) : name;
                if (slash)
                    free(name);
            }
            if (r < 0 || (r = sd_bus_message_exit_container(message)) < 0 ||
                (r = sd_bus_message_exit_container(message)) < 0)
                goto fail;
        } else if (strcmp(key, "choices") == 0) {
            /* a(ssa(ss)s): id, label, options, default. */
            if ((r = sd_bus_message_enter_container(message, 'v', "a(ssa(ss)s)")) < 0 ||
                (r = sd_bus_message_enter_container(message, 'a', "(ssa(ss)s)")) < 0)
                goto fail;
            while ((r = sd_bus_message_enter_container(message, 'r', "ssa(ss)s")) > 0) {
                const char *id, *label, *value;
                if ((r = sd_bus_message_read(message, "ss", &id, &label)) < 0 ||
                    (r = sd_bus_message_skip(message, "a(ss)")) < 0 ||
                    (r = sd_bus_message_read(message, "s", &value)) < 0 ||
                    (r = sd_bus_message_exit_container(message)) < 0)
                    goto fail;
                struct portal_choice *grown =
                    realloc(request->choices, (request->choice_count + 1) * sizeof *grown);
                if (!grown) {
                    r = -ENOMEM;
                    goto fail;
                }
                request->choices = grown;
                request->choices[request->choice_count++] = (struct portal_choice){strdup(id), strdup(value)};
            }
            if (r < 0 || (r = sd_bus_message_exit_container(message)) < 0 ||
                (r = sd_bus_message_exit_container(message)) < 0)
                goto fail;
        } else {
            if ((r = sd_bus_message_skip(message, "v")) < 0)
                goto fail;
        }
        if ((r = sd_bus_message_exit_container(message)) < 0)
            goto fail;
    }
    if (r < 0 || (r = sd_bus_message_exit_container(message)) < 0)
        goto fail;

    /* The command line for the dialog. */
    char *argv[64 + 2 * 32];
    size_t argc = 0;
    char *owned[40];
    size_t owned_count = 0;
    argv[argc++] = "files";
    if (kind == CHOOSE_SAVE) {
        const char *name = current_name ? current_name : current_file ? strrchr(current_file, '/') : NULL;
        if (name && name[0] == '/')
            ++name;
        argv[argc++] = "--save";
        argv[argc++] = (char *)(name && *name ? name : "Untitled");
    } else {
        argv[argc++] = "--pick";
        if (multiple && kind == CHOOSE_OPEN)
            argv[argc++] = "--multiple";
        if (directory || kind == CHOOSE_SAVE_FILES)
            argv[argc++] = "--directory";
    }
    argv[argc++] = "-0";
    argv[argc++] = "--print-filter";
    if (title && *title) {
        argv[argc++] = "--title";
        argv[argc++] = (char *)title;
    }
    const char *label = accept ? accept : kind == CHOOSE_SAVE_FILES ? "Save here" : NULL;
    if (label) {
        argv[argc++] = "--accept";
        argv[argc++] = (char *)label;
    }
    if (app_id && *app_id) {
        argv[argc++] = "--app-id";
        argv[argc++] = (char *)app_id;
    }
    /* Filters, the current one first; order maps them back. */
    request->order = calloc(request->filter_count + 1, sizeof *request->order);
    if (!request->order) {
        r = -ENOMEM;
        goto fail;
    }
    size_t first = request->filter_count;
    for (size_t i = 0; have_current && i < request->filter_count; ++i) {
        if (same_filter(&current, &request->filters[i]))
            first = i;
    }
    if (have_current && first == request->filter_count && current.name) {
        /* Current but not among the filters: it comes first all the same. */
        struct portal_filter *grown = realloc(request->filters, (request->filter_count + 1) * sizeof *grown);
        if (grown) {
            request->filters = grown;
            request->filters[request->filter_count++] = current;
            current = (struct portal_filter){0};
            free(request->order);
            request->order = calloc(request->filter_count + 1, sizeof *request->order);
            if (!request->order) {
                r = -ENOMEM;
                goto fail;
            }
        }
    }
    size_t index = 0;
    for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < request->filter_count && owned_count < 32; ++i) {
            if ((pass == 0) != (i == first))
                continue;
            char *text = filter_argument(&request->filters[i]);
            if (!text)
                continue;
            owned[owned_count++] = text;
            argv[argc++] = "--filter";
            argv[argc++] = text;
            request->order[index++] = i;
        }
    }
    for (size_t i = index; i <= request->filter_count; ++i)
        request->order[i] = (size_t)-1; /* All files: no filter to report. */
    char *start = current_folder && current_folder[0] == '/' ? current_folder : NULL;
    char *file_folder = NULL;
    if (!start && current_file && current_file[0] == '/') {
        file_folder = strdup(current_file);
        char *slash = file_folder ? strrchr(file_folder, '/') : NULL;
        if (slash)
            *(slash == file_folder ? slash + 1 : slash) = 0;
        start = file_folder;
    }
    if (start)
        argv[argc++] = start;
    argv[argc] = NULL;

    int fds[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    if (pipe2(fds, O_CLOEXEC) != 0) {
        r = -errno;
    } else if ((r = -posix_spawn_file_actions_init(&actions)) == 0) {
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        r = -posix_spawn(&request->pid, "/proc/self/exe", &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    if (fds[1] >= 0)
        close(fds[1]);
    request->out = fds[0];
    for (size_t i = 0; i < owned_count; ++i)
        free(owned[i]);
    free(file_folder);
    if (r < 0)
        goto fail;
    fcntl(request->out, F_SETFL, O_NONBLOCK);
    if ((r = sd_event_add_io(portal->event, &request->io, request->out, EPOLLIN, request_output, request)) < 0 ||
        (r = sd_event_add_child(portal->event, &request->child, request->pid, WEXITED, request_exited, request)) < 0)
        goto fail;
    sd_bus_add_object_vtable(portal->bus, &request->slot, handle, "org.freedesktop.impl.portal.Request",
                             request_vtable, request);
    request->call = sd_bus_message_ref(message);
    request->next = portal->requests;
    portal->requests = request;
    if (portal->idle)
        sd_event_source_set_enabled(portal->idle, SD_EVENT_OFF);
    free(accept);
    free(current_name);
    free(current_folder);
    free(current_file);
    free(current.name);
    free(current.types);
    for (size_t i = 0; i < current.count; ++i)
        free(current.patterns[i]);
    free(current.patterns);
    return 1; /* Answered when the dialog closes. */

fail:
    if (request->pid > 0) {
        kill(request->pid, SIGTERM);
        waitpid(request->pid, NULL, 0);
    }
    request_free(request);
    free(accept);
    free(current_name);
    free(current_folder);
    free(current_file);
    free(current.name);
    free(current.types);
    for (size_t i = 0; i < current.count; ++i)
        free(current.patterns[i]);
    free(current.patterns);
    return sd_bus_error_set_errnof(error, -r, "files: %s", strerror(-r));
}

static int open_file(sd_bus_message *message, void *data, sd_bus_error *error)
{
    return choose(message, data, error, CHOOSE_OPEN);
}

static int save_file(sd_bus_message *message, void *data, sd_bus_error *error)
{
    return choose(message, data, error, CHOOSE_SAVE);
}

static int save_files(sd_bus_message *message, void *data, sd_bus_error *error)
{
    return choose(message, data, error, CHOOSE_SAVE_FILES);
}

static const sd_bus_vtable chooser_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("OpenFile", "osssa{sv}", "ua{sv}", open_file, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFile", "osssa{sv}", "ua{sv}", save_file, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFiles", "osssa{sv}", "ua{sv}", save_files, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

int portal_main(void)
{
    struct portal portal = {0};
    /* Children are waited for through sd-event, which needs SIGCHLD blocked. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int r = sd_event_default(&portal.event);
    if (r >= 0)
        r = sd_bus_open_user(&portal.bus);
    if (r >= 0)
        r = sd_bus_add_object_vtable(portal.bus, NULL, PORTAL_PATH, "org.freedesktop.impl.portal.FileChooser",
                                     chooser_vtable, &portal);
    if (r >= 0)
        r = sd_bus_request_name(portal.bus, PORTAL_NAME, 0);
    if (r >= 0)
        r = sd_bus_attach_event(portal.bus, portal.event, SD_EVENT_PRIORITY_NORMAL);
    if (r < 0) {
        fprintf(stderr, "files: can't serve %s: %s\n", PORTAL_NAME, strerror(-r));
        sd_bus_unref(portal.bus);
        sd_event_unref(portal.event);
        return EXIT_FAILED;
    }
    idle_arm(&portal);
    r = sd_event_loop(portal.event);
    while (portal.requests) {
        struct request *next = portal.requests->next;
        if (portal.requests->pid > 0 && !portal.requests->exited)
            kill(portal.requests->pid, SIGTERM);
        request_free(portal.requests);
        portal.requests = next;
    }
    sd_event_source_unref(portal.idle);
    sd_bus_flush_close_unref(portal.bus);
    sd_event_unref(portal.event);
    return r < 0 ? EXIT_FAILED : EXIT_CLOSED;
}
