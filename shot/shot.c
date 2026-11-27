#define _GNU_SOURCE
#include "shot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <png.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void warn(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void warn(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("shot: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

/* Configuration */

static const char default_config[] = {
#include "default-config.h"
};

struct config_parse {
    struct config *config;
    const char *label;
};

static bool parse_int(const char *text, int minimum, int maximum, int *value)
{
    char *end;
    errno = 0;
    long number = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != 0 || number < minimum || number > maximum) return false;
    *value = (int)number;
    return true;
}

static bool parse_double(const char *text, double minimum, double maximum, double *value)
{
    char *end;
    errno = 0;
    double number = strtod(text, &end);
    if (errno != 0 || end == text || *end != 0 || !(number >= minimum && number <= maximum)) return false;
    *value = number;
    return true;
}

static bool set_string(char **target, const char *value)
{
    char *copy = strdup(value);
    if (copy == NULL) return false;
    free(*target);
    *target = copy;
    return true;
}

static void config_entry(void *data, const char *section, const char *key, const char *value, int line)
{
    struct config_parse *parse = data;
    struct config *config = parse->config;
    if (key == NULL) {
        warn("%s:%d: expected [section] or key = value", parse->label, line);
        return;
    }
    bool known = true, valid = true;
    if (strcmp(section, "save") == 0 && strcmp(key, "directory") == 0) {
        valid = value[0] != 0 && set_string(&config->directory, value);
    } else if (strcmp(section, "save") == 0 && strcmp(key, "name") == 0) {
        valid = value[0] != 0 && strchr(value, '/') == NULL && set_string(&config->name, value);
    } else if (strcmp(section, "save") == 0 && strcmp(key, "clipboard") == 0) {
        valid = desk_parse_bool(value, &config->clipboard);
    } else if (strcmp(section, "save") == 0 && strcmp(key, "open") == 0) {
        valid = desk_parse_bool(value, &config->open);
    } else if (strcmp(section, "capture") == 0 && strcmp(key, "cursor") == 0) {
        valid = desk_parse_bool(value, &config->cursor);
    } else if (strcmp(section, "png") == 0 && strcmp(key, "compression") == 0) {
        valid = parse_int(value, 0, 9, &config->compression);
    } else if (strcmp(section, "region") == 0 && strcmp(key, "dim") == 0) {
        valid = parse_double(value, 0, 1, &config->dim);
    } else if (strcmp(section, "region") == 0 && strcmp(key, "border") == 0) {
        valid = parse_int(value, 0, 64, &config->border);
    } else if (strcmp(section, "region") == 0 && strcmp(key, "color") == 0) {
        if (strcmp(value, "accent") == 0) config->border_accent = true;
        else if ((valid = desk_parse_rgb(value, &config->border_rgb))) config->border_accent = false;
    } else {
        known = false;
    }
    if (!known) warn("%s:%d: unknown key %s in [%s]", parse->label, line, key, section);
    else if (!valid) warn("%s:%d: invalid value for %s, keeping %s", parse->label, line, key,
                          strcmp(parse->label, "built-in config") == 0 ? "nothing" : "the default");
}

/* Load the built-in defaults, then the user's file, creating it when it's
   missing so there's always a file to edit. Problems are reported and the
   defaults stay in effect. */
static void config_load(struct config *config)
{
    *config = (struct config){0};
    struct config_parse parse = {.config = config, .label = "built-in config"};
    desk_ini_parse(default_config, sizeof default_config, config_entry, &parse);
    char *path = desk_config_path("shot");
    if (path == NULL) {
        warn("neither XDG_CONFIG_HOME nor HOME is set, using the built-in config");
        return;
    }
    int error = desk_write_default(path, default_config, sizeof default_config);
    if (error == 0) {
        warn("created %s", path);
    } else if (error != EEXIST) {
        warn("can't create %s: %s", path, strerror(error));
    } else {
        parse.label = path;
        error = desk_ini_load(path, config_entry, &parse);
        if (error != 0 && error != ENOENT) warn("can't read %s: %s", path, strerror(error));
    }
    free(path);
}

static void config_free(struct config *config)
{
    free(config->directory);
    free(config->name);
    *config = (struct config){0};
}

/* dwl IPC. Only the focused monitor matters; every other event is ignored. */

static void ipc_tags(void *data, struct zdwl_ipc_manager_v2 *manager, uint32_t amount)
{
    (void)data, (void)manager, (void)amount;
}

static void ipc_layout(void *data, struct zdwl_ipc_manager_v2 *manager, const char *name)
{
    (void)data, (void)manager, (void)name;
}

static const struct zdwl_ipc_manager_v2_listener ipc_manager_listener = {
    .tags = ipc_tags,
    .layout = ipc_layout,
};

static void ipc_output_toggle_visibility(void *data, struct zdwl_ipc_output_v2 *ipc)
{
    (void)data, (void)ipc;
}

static void ipc_output_active(void *data, struct zdwl_ipc_output_v2 *ipc, uint32_t active)
{
    (void)ipc;
    struct output *output = data;
    output->active = active != 0;
}

static void ipc_output_tag(void *data, struct zdwl_ipc_output_v2 *ipc, uint32_t tag, uint32_t state,
                           uint32_t clients, uint32_t focused)
{
    (void)data, (void)ipc, (void)tag, (void)state, (void)clients, (void)focused;
}

static void ipc_output_layout(void *data, struct zdwl_ipc_output_v2 *ipc, uint32_t layout)
{
    (void)data, (void)ipc, (void)layout;
}

static void ipc_output_string(void *data, struct zdwl_ipc_output_v2 *ipc, const char *text)
{
    (void)data, (void)ipc, (void)text;
}

static void ipc_output_frame(void *data, struct zdwl_ipc_output_v2 *ipc)
{
    (void)data, (void)ipc;
}

static void ipc_output_flag(void *data, struct zdwl_ipc_output_v2 *ipc, uint32_t flag)
{
    (void)data, (void)ipc, (void)flag;
}

static void ipc_output_focused_geometry(void *data, struct zdwl_ipc_output_v2 *ipc, int32_t x, int32_t y,
                                        int32_t width, int32_t height)
{
    (void)data, (void)ipc, (void)x, (void)y, (void)width, (void)height;
}

static const struct zdwl_ipc_output_v2_listener ipc_output_listener = {
    .toggle_visibility = ipc_output_toggle_visibility,
    .active = ipc_output_active,
    .tag = ipc_output_tag,
    .layout = ipc_output_layout,
    .title = ipc_output_string,
    .appid = ipc_output_string,
    .layout_symbol = ipc_output_string,
    .frame = ipc_output_frame,
    .fullscreen = ipc_output_flag,
    .floating = ipc_output_flag,
    .focused_geometry = ipc_output_focused_geometry,
};

/* Outputs */

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel, const char *make, const char *model,
                            int32_t transform)
{
    (void)data, (void)wl_output, (void)x, (void)y, (void)physical_width, (void)physical_height;
    (void)subpixel, (void)make, (void)model, (void)transform;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height,
                        int32_t refresh)
{
    (void)wl_output, (void)refresh;
    struct output *output = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        output->width = width;
        output->height = height;
    }
}

static void output_done(void *data, struct wl_output *wl_output)
{
    (void)data, (void)wl_output;
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    (void)data, (void)wl_output, (void)factor;
}

static void replace_string(char **target, const char *text)
{
    char *copy = strdup(text);
    if (copy == NULL) return;
    free(*target);
    *target = copy;
}

static void output_name(void *data, struct wl_output *wl_output, const char *name)
{
    (void)wl_output;
    struct output *output = data;
    replace_string(&output->name, name);
}

static void output_description(void *data, struct wl_output *wl_output, const char *description)
{
    (void)wl_output;
    struct output *output = data;
    replace_string(&output->description, description);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void capture_finish(struct capture *capture);
static void overlay_destroy(struct overlay *overlay);

static void output_destroy(struct output *output)
{
    wl_list_remove(&output->link);
    struct selection *selection = &output->shot->selection;
    if (selection->pointer_output == output) selection->pointer_output = NULL;
    if (selection->output == output) {
        selection->output = NULL;
        if (selection->state == SELECTION_WAITING) selection->state = SELECTION_CANCELLED;
    }
    capture_finish(&output->capture);
    overlay_destroy(&output->overlay);
    free(output->frame.rgb);
    if (output->ipc != NULL) zdwl_ipc_output_v2_release(output->ipc);
    if (wl_output_get_version(output->wl_output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
        wl_output_release(output->wl_output);
    else
        wl_output_destroy(output->wl_output);
    free(output->name);
    free(output->description);
    free(output);
}

static const struct wl_seat_listener seat_listener;

/* Registry */

static uint32_t min_version(uint32_t offered, uint32_t wanted)
{
    return offered < wanted ? offered : wanted;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                            uint32_t version)
{
    struct shot *shot = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        shot->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        shot->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, min_version(version, 4));
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && shot->seat == NULL) {
        shot->seat = wl_registry_bind(registry, name, &wl_seat_interface, min_version(version, 5));
        wl_seat_add_listener(shot->seat, &seat_listener, shot);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        shot->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, min_version(version, 4));
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        shot->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        shot->cursor_shape_manager = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
        shot->source_manager =
            wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
        shot->copy_manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (strcmp(interface, zdwl_ipc_manager_v2_interface.name) == 0) {
        shot->ipc_manager =
            wl_registry_bind(registry, name, &zdwl_ipc_manager_v2_interface, min_version(version, 2));
        zdwl_ipc_manager_v2_add_listener(shot->ipc_manager, &ipc_manager_listener, shot);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct output *output = calloc(1, sizeof *output);
        if (output == NULL) return;
        output->shot = shot;
        output->global = name;
        output->wl_output = wl_registry_bind(registry, name, &wl_output_interface, min_version(version, 4));
        wl_output_add_listener(output->wl_output, &output_listener, output);
        wl_list_insert(shot->outputs.prev, &output->link);
        ++shot->output_count;
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)registry;
    struct shot *shot = data;
    struct output *output, *next;
    wl_list_for_each_safe(output, next, &shot->outputs, link) {
        if (output->global != name) continue;
        output_destroy(output);
        --shot->output_count;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static bool roundtrip(struct shot *shot)
{
    if (wl_display_roundtrip(shot->display) >= 0) return true;
    warn("lost the Wayland connection: %s", strerror(wl_display_get_error(shot->display)));
    return false;
}

static bool shot_connect(struct shot *shot)
{
    wl_list_init(&shot->outputs);
    shot->display = wl_display_connect(NULL);
    if (shot->display == NULL) {
        warn("can't connect to the Wayland display (is WAYLAND_DISPLAY set?)");
        return false;
    }
    shot->registry = wl_display_get_registry(shot->display);
    wl_registry_add_listener(shot->registry, &registry_listener, shot);
    if (!roundtrip(shot)) return false;
    if (shot->ipc_manager != NULL) {
        struct output *output;
        wl_list_for_each(output, &shot->outputs, link) {
            output->ipc = zdwl_ipc_manager_v2_get_output(shot->ipc_manager, output->wl_output);
            zdwl_ipc_output_v2_add_listener(output->ipc, &ipc_output_listener, output);
        }
    }
    /* Output names and the dwl IPC state arrive after the binds above. */
    return roundtrip(shot);
}

static void shot_disconnect(struct shot *shot)
{
    struct output *output, *next;
    wl_list_for_each_safe(output, next, &shot->outputs, link) output_destroy(output);
    if (shot->cursor_shape_device != NULL) wp_cursor_shape_device_v1_destroy(shot->cursor_shape_device);
    if (shot->pointer != NULL) wl_pointer_release(shot->pointer);
    if (shot->keyboard != NULL) wl_keyboard_release(shot->keyboard);
    if (shot->seat != NULL) wl_seat_release(shot->seat);
    if (shot->cursor_shape_manager != NULL) wp_cursor_shape_manager_v1_destroy(shot->cursor_shape_manager);
    if (shot->viewporter != NULL) wp_viewporter_destroy(shot->viewporter);
    if (shot->layer_shell != NULL) zwlr_layer_shell_v1_destroy(shot->layer_shell);
    if (shot->compositor != NULL) wl_compositor_destroy(shot->compositor);
    if (shot->ipc_manager != NULL) zdwl_ipc_manager_v2_release(shot->ipc_manager);
    if (shot->copy_manager != NULL) ext_image_copy_capture_manager_v1_destroy(shot->copy_manager);
    if (shot->source_manager != NULL) ext_output_image_capture_source_manager_v1_destroy(shot->source_manager);
    if (shot->shm != NULL) wl_shm_destroy(shot->shm);
    if (shot->registry != NULL) wl_registry_destroy(shot->registry);
    if (shot->display != NULL) wl_display_disconnect(shot->display);
    struct config config = shot->config;
    *shot = (struct shot){.config = config};
    wl_list_init(&shot->outputs);
}

static const char *output_label(const struct output *output)
{
    return output->name != NULL ? output->name : "(unnamed)";
}

/* Choose the output named on the command line, else dwl's focused monitor, else the first. */
static struct output *choose_output(struct shot *shot, const char *name)
{
    struct output *output;
    if (name != NULL) {
        wl_list_for_each(output, &shot->outputs, link) {
            if (output->name != NULL && strcmp(output->name, name) == 0) return output;
        }
        warn("no output named %s (see shot --list)", name);
        return NULL;
    }
    if (wl_list_empty(&shot->outputs)) {
        warn("the compositor reports no outputs");
        return NULL;
    }
    wl_list_for_each(output, &shot->outputs, link) {
        if (output->active) return output;
    }
    output = wl_container_of(shot->outputs.next, output, link);
    if (shot->output_count > 1)
        warn("no focused output reported (dwl IPC unavailable), capturing %s", output_label(output));
    return output;
}

static void list_outputs(struct shot *shot)
{
    struct output *output;
    wl_list_for_each(output, &shot->outputs, link) {
        printf("%s\t%dx%d\t%s%s\n", output_label(output), output->width, output->height,
               output->description != NULL ? output->description : "", output->active ? "\t(focused)" : "");
    }
}

/* Capture */

/* Lower is better. Only 8-bit formats with a byte order shot knows. */
static int format_rank(uint32_t format)
{
    switch (format) {
    case WL_SHM_FORMAT_XRGB8888: return 0;
    case WL_SHM_FORMAT_ARGB8888: return 1;
    case WL_SHM_FORMAT_XBGR8888: return 2;
    case WL_SHM_FORMAT_ABGR8888: return 3;
    default: return -1;
    }
}

static void buffer_free(struct capture *capture)
{
    if (capture->buffer != NULL) wl_buffer_destroy(capture->buffer);
    if (capture->pixels != NULL) munmap(capture->pixels, capture->size);
    capture->buffer = NULL;
    capture->pixels = NULL;
    capture->size = 0;
}

static bool buffer_allocate(struct capture *capture)
{
    struct shot *shot = capture->shot;
    if (capture->buffer != NULL && capture->buffer_width == capture->width &&
        capture->buffer_height == capture->height)
        return true;
    buffer_free(capture);
    uint32_t width = capture->width, height = capture->height;
    if (width == 0 || height == 0 || width > INT32_MAX / 4 || (uint64_t)width * 4 * height > INT32_MAX) {
        capture->error = "the compositor sent an unusable buffer size";
        return false;
    }
    size_t size = (size_t)width * 4 * height;
    int fd = desk_shm_open(size);
    if (fd < 0) {
        capture->error = "can't allocate shared memory";
        return false;
    }
    void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        capture->error = "can't map shared memory";
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shot->shm, fd, (int32_t)size);
    capture->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height, (int32_t)(width * 4),
                                                capture->format);
    wl_shm_pool_destroy(pool);
    close(fd);
    capture->pixels = pixels;
    capture->size = size;
    capture->buffer_width = width;
    capture->buffer_height = height;
    return true;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener;

static void frame_start(struct capture *capture)
{
    capture->want_frame = false;
    if (!capture->have_format) {
        capture->error = "the compositor offers no 8-bit RGB shm format";
        capture->state = CAPTURE_FAILED;
        return;
    }
    if (!buffer_allocate(capture)) {
        capture->state = CAPTURE_FAILED;
        return;
    }
    capture->frame_constraints = capture->constraints;
    capture->transform = WL_OUTPUT_TRANSFORM_NORMAL;
    capture->frame = ext_image_copy_capture_session_v1_create_frame(capture->session);
    ext_image_copy_capture_frame_v1_add_listener(capture->frame, &frame_listener, capture);
    ext_image_copy_capture_frame_v1_attach_buffer(capture->frame, capture->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(capture->frame, 0, 0, (int32_t)capture->buffer_width,
                                                  (int32_t)capture->buffer_height);
    ext_image_copy_capture_frame_v1_capture(capture->frame);
}

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t transform)
{
    (void)frame;
    struct capture *capture = data;
    capture->transform = transform;
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame, int32_t x, int32_t y,
                         int32_t width, int32_t height)
{
    (void)data, (void)frame, (void)x, (void)y, (void)width, (void)height;
}

static void frame_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t sec_hi,
                                    uint32_t sec_lo, uint32_t nsec)
{
    (void)data, (void)frame, (void)sec_hi, (void)sec_lo, (void)nsec;
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame)
{
    struct capture *capture = data;
    ext_image_copy_capture_frame_v1_destroy(frame);
    capture->frame = NULL;
    capture->state = CAPTURE_READY;
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason)
{
    struct capture *capture = data;
    ext_image_copy_capture_frame_v1_destroy(frame);
    capture->frame = NULL;
    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS && capture->retries++ == 0) {
        /* Retry once with the new constraints, now if they already arrived. */
        if (capture->constraints != capture->frame_constraints) frame_start(capture);
        else capture->want_frame = true;
        return;
    }
    capture->state = CAPTURE_FAILED;
    switch (reason) {
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
        capture->error = "the buffer doesn't match the compositor's constraints";
        break;
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
        capture->error = "the output went away during the capture";
        break;
    default:
        capture->error = "the compositor failed to copy the frame";
        break;
    }
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t width,
                                uint32_t height)
{
    (void)session;
    struct capture *capture = data;
    capture->width = width;
    capture->height = height;
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t format)
{
    (void)session;
    struct capture *capture = data;
    int rank = format_rank(format);
    if (rank < 0) return;
    if (!capture->have_format || rank < format_rank(capture->format)) capture->format = format;
    capture->have_format = true;
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
                                  struct wl_array *device)
{
    (void)data, (void)session, (void)device;
}

static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t format,
                                  struct wl_array *modifiers)
{
    (void)data, (void)session, (void)format, (void)modifiers;
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    struct capture *capture = data;
    ++capture->constraints;
    if (capture->want_frame && capture->frame == NULL && capture->state == CAPTURE_WAITING) frame_start(capture);
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    struct capture *capture = data;
    if (capture->state != CAPTURE_WAITING) return;
    capture->state = CAPTURE_FAILED;
    capture->error = "the capture session stopped (output removed?)";
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_buffer_size,
    .shm_format = session_shm_format,
    .dmabuf_device = session_dmabuf_device,
    .dmabuf_format = session_dmabuf_format,
    .done = session_done,
    .stopped = session_stopped,
};

static void capture_finish(struct capture *capture)
{
    if (capture->shot == NULL) return;
    if (capture->frame != NULL) ext_image_copy_capture_frame_v1_destroy(capture->frame);
    if (capture->session != NULL) ext_image_copy_capture_session_v1_destroy(capture->session);
    if (capture->source != NULL) ext_image_capture_source_v1_destroy(capture->source);
    buffer_free(capture);
}

/* Map upright pixel (x, y) of a width × height image to the buffer. The
   transform is what the compositor applied, so applying it again to upright
   coordinates finds the source pixel: flip first, then rotate
   counter-clockwise. */
static void buffer_position(uint32_t transform, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                            uint32_t *bx, uint32_t *by)
{
    if (transform >= WL_OUTPUT_TRANSFORM_FLIPPED) x = width - 1 - x;
    switch (transform & 3) {
    case WL_OUTPUT_TRANSFORM_90:
        *bx = y;
        *by = width - 1 - x;
        break;
    case WL_OUTPUT_TRANSFORM_180:
        *bx = width - 1 - x;
        *by = height - 1 - y;
        break;
    case WL_OUTPUT_TRANSFORM_270:
        *bx = height - 1 - y;
        *by = x;
        break;
    default:
        *bx = x;
        *by = y;
        break;
    }
}

static bool convert(const struct capture *capture, struct image *image)
{
    uint32_t transform = capture->transform & 7;
    bool swap = transform & 1;
    uint32_t width = swap ? capture->buffer_height : capture->buffer_width;
    uint32_t height = swap ? capture->buffer_width : capture->buffer_height;
    uint8_t *rgb = malloc((size_t)width * height * 3);
    if (rgb == NULL) return false;
    /* Words are native-endian 32-bit values, so shifts are byte-order safe. */
    bool bgr = capture->format == WL_SHM_FORMAT_XBGR8888 || capture->format == WL_SHM_FORMAT_ABGR8888;
    int red = bgr ? 0 : 16, blue = bgr ? 16 : 0;
    const uint32_t *pixels = capture->pixels;
    uint32_t stride = capture->buffer_width;
    uint8_t *out = rgb;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t bx = x, by = y;
            if (transform != WL_OUTPUT_TRANSFORM_NORMAL) buffer_position(transform, width, height, x, y, &bx, &by);
            uint32_t word = pixels[(size_t)by * stride + bx];
            *out++ = (uint8_t)(word >> red);
            *out++ = (uint8_t)(word >> 8);
            *out++ = (uint8_t)(word >> blue);
        }
    }
    *image = (struct image){.width = width, .height = height, .rgb = rgb};
    return true;
}

static void capture_begin(struct shot *shot, struct output *output, bool cursor)
{
    struct capture *capture = &output->capture;
    *capture = (struct capture){.shot = shot, .want_frame = true, .state = CAPTURE_WAITING};
    capture->source = ext_output_image_capture_source_manager_v1_create_source(shot->source_manager,
                                                                                output->wl_output);
    uint32_t options = cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
    capture->session = ext_image_copy_capture_manager_v1_create_session(shot->copy_manager, capture->source, options);
    ext_image_copy_capture_session_v1_add_listener(capture->session, &session_listener, capture);
}

static bool captures_waiting(struct shot *shot)
{
    struct output *output;
    wl_list_for_each(output, &shot->outputs, link) {
        if (output->capture.state == CAPTURE_WAITING) return true;
    }
    return false;
}

/* Capture one output, or every output when only is NULL, all at once so the
   frames show the same moment. Each result lands in output->frame. */
static bool capture_outputs(struct shot *shot, struct output *only, bool cursor)
{
    struct output *output;
    wl_list_for_each(output, &shot->outputs, link) {
        if (only == NULL || output == only) capture_begin(shot, output, cursor);
    }
    while (captures_waiting(shot)) {
        if (wl_display_dispatch(shot->display) < 0) {
            warn("lost the Wayland connection: %s", strerror(wl_display_get_error(shot->display)));
            return false;
        }
    }
    bool ok = true;
    wl_list_for_each(output, &shot->outputs, link) {
        struct capture *capture = &output->capture;
        if (capture->state == CAPTURE_IDLE) continue;
        if (capture->state != CAPTURE_READY) {
            warn("can't capture %s: %s", output_label(output), capture->error);
            ok = false;
        } else if (!convert(capture, &output->frame)) {
            warn("out of memory converting the capture of %s", output_label(output));
            ok = false;
        }
        capture_finish(capture);
        *capture = (struct capture){0};
    }
    return ok;
}

/* Region selection */

static void overlay_render(struct shot *shot, struct output *output);

static void overlay_destroy(struct overlay *overlay)
{
    if (overlay->frame_callback != NULL) wl_callback_destroy(overlay->frame_callback);
    if (overlay->viewport != NULL) wp_viewport_destroy(overlay->viewport);
    if (overlay->layer != NULL) zwlr_layer_surface_v1_destroy(overlay->layer);
    if (overlay->surface != NULL) wl_surface_destroy(overlay->surface);
    desk_buffer_pool_finish(&overlay->buffers);
    free(overlay->dimmed);
    free(overlay->bright);
    *overlay = (struct overlay){0};
}

static void selection_cancel(struct shot *shot)
{
    if (shot->selection.state == SELECTION_WAITING) shot->selection.state = SELECTION_CANCELLED;
}

static struct output *output_for_surface(struct shot *shot, struct wl_surface *surface)
{
    struct output *output;
    wl_list_for_each(output, &shot->outputs, link) {
        if (surface != NULL && output->overlay.surface == surface) return output;
    }
    return NULL;
}

static double clamp(double value, double low, double high)
{
    return value < low ? low : value > high ? high : value;
}

/* The selection in the frame's physical pixels, as [x0, x1) × [y0, y1).
   Start edges round down and end edges up, so the crop covers every pixel the
   outline touches. Return false when there's no selection on this output. */
static bool selection_rect(const struct shot *shot, const struct output *output, uint32_t *x0, uint32_t *y0,
                           uint32_t *x1, uint32_t *y1)
{
    const struct selection *selection = &shot->selection;
    const struct overlay *overlay = &output->overlay;
    if (selection->output != output || overlay->width <= 0 || overlay->height <= 0) return false;
    double scale_x = (double)output->frame.width / overlay->width;
    double scale_y = (double)output->frame.height / overlay->height;
    double left = fmin(selection->start_x, selection->end_x), right = fmax(selection->start_x, selection->end_x);
    double top = fmin(selection->start_y, selection->end_y), bottom = fmax(selection->start_y, selection->end_y);
    *x0 = (uint32_t)clamp(floor(left * scale_x), 0, output->frame.width);
    *x1 = (uint32_t)clamp(ceil(right * scale_x), 0, output->frame.width);
    *y0 = (uint32_t)clamp(floor(top * scale_y), 0, output->frame.height);
    *y1 = (uint32_t)clamp(ceil(bottom * scale_y), 0, output->frame.height);
    return true;
}

static void fill_rect(uint32_t *pixels, int width, int height, int x0, int y0, int x1, int y1, uint32_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) pixels[(size_t)y * width + x] = color;
    }
}

static void frame_callback_done(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)time;
    struct output *output = data;
    wl_callback_destroy(callback);
    output->overlay.frame_callback = NULL;
    if (output->overlay.dirty) overlay_render(output->shot, output);
}

static const struct wl_callback_listener frame_callback_listener = {.done = frame_callback_done};

/* Draw the frozen frame dimmed, the selection undimmed, and an outline around
   it. At most once per frame callback: later requests just mark it dirty. */
static void overlay_render(struct shot *shot, struct output *output)
{
    struct overlay *overlay = &output->overlay;
    overlay->dirty = true;
    if (!overlay->configured || overlay->frame_callback != NULL) return;
    int width = (int)output->frame.width, height = (int)output->frame.height;
    DeskBuffer *buffer = desk_buffer_get(&overlay->buffers, shot->shm, width, height, WL_SHM_FORMAT_XRGB8888);
    if (buffer == NULL) return; /* Both busy: the next event redraws. */
    overlay->dirty = false;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool selected = selection_rect(shot, output, &x0, &y0, &x1, &y1) && x1 > x0 && y1 > y0;
    size_t row_bytes = (size_t)width * 4;
    for (int y = 0; y < height; ++y) {
        uint32_t *row = buffer->pixels + (size_t)y * width;
        const uint32_t *dimmed = overlay->dimmed + (size_t)y * width, *bright = overlay->bright + (size_t)y * width;
        if (!selected || (uint32_t)y < y0 || (uint32_t)y >= y1) {
            memcpy(row, dimmed, row_bytes);
            continue;
        }
        memcpy(row, dimmed, (size_t)x0 * 4);
        memcpy(row + x0, bright + x0, (size_t)(x1 - x0) * 4);
        memcpy(row + x1, dimmed + x1, (size_t)(width - (int)x1) * 4);
    }
    if (selected && shot->config.border > 0) {
        /* Outside the selection, so it never hides a pixel that will be saved. */
        int thickness = (int)lround(shot->config.border * (double)width / overlay->width);
        if (thickness < 1) thickness = 1;
        int left = (int)x0, top = (int)y0, right = (int)x1, bottom = (int)y1;
        uint32_t color = shot->border_rgb;
        fill_rect(buffer->pixels, width, height, left - thickness, top - thickness, right + thickness, top, color);
        fill_rect(buffer->pixels, width, height, left - thickness, bottom, right + thickness, bottom + thickness,
                  color);
        fill_rect(buffer->pixels, width, height, left - thickness, top, left, bottom, color);
        fill_rect(buffer->pixels, width, height, right, top, right + thickness, bottom, color);
    }
    wl_surface_attach(overlay->surface, buffer->buffer, 0, 0);
    wl_surface_damage_buffer(overlay->surface, 0, 0, width, height);
    overlay->frame_callback = wl_surface_frame(overlay->surface);
    wl_callback_add_listener(overlay->frame_callback, &frame_callback_listener, output);
    wl_surface_commit(overlay->surface);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial, uint32_t width,
                            uint32_t height)
{
    struct output *output = data;
    struct overlay *overlay = &output->overlay;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (width == 0 || height == 0 || width > INT32_MAX || height > INT32_MAX) return;
    overlay->width = (int)width;
    overlay->height = (int)height;
    /* The buffer is the frame at physical size; the viewport maps it onto the
       logical surface, which is pixel for pixel at any output scale. */
    wp_viewport_set_destination(overlay->viewport, (int32_t)width, (int32_t)height);
    overlay->configured = true;
    overlay_render(output->shot, output);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
    (void)layer;
    struct output *output = data;
    selection_cancel(output->shot);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static bool overlay_create(struct shot *shot, struct output *output)
{
    struct overlay *overlay = &output->overlay;
    size_t count = (size_t)output->frame.width * output->frame.height;
    overlay->dimmed = malloc(count * 4);
    overlay->bright = malloc(count * 4);
    if (overlay->dimmed == NULL || overlay->bright == NULL) return false;
    int keep = (int)lround((1 - shot->config.dim) * 256);
    const uint8_t *rgb = output->frame.rgb;
    for (size_t i = 0; i < count; ++i, rgb += 3) {
        uint32_t r = rgb[0], g = rgb[1], b = rgb[2];
        overlay->bright[i] = r << 16 | g << 8 | b;
        overlay->dimmed[i] = (r * keep >> 8) << 16 | (g * keep >> 8) << 8 | (b * keep >> 8);
    }
    overlay->surface = wl_compositor_create_surface(shot->compositor);
    overlay->viewport = wp_viewporter_get_viewport(shot->viewporter, overlay->surface);
    overlay->layer = zwlr_layer_shell_v1_get_layer_surface(shot->layer_shell, overlay->surface, output->wl_output,
                                                            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "shot");
    zwlr_layer_surface_v1_set_anchor(overlay->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(overlay->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(overlay->layer,
                                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(overlay->layer, &layer_listener, output);
    wl_surface_commit(overlay->surface);
    return true;
}

static void selection_moved(struct shot *shot)
{
    struct selection *selection = &shot->selection;
    if (!selection->dragging || selection->pointer_output != selection->output) return;
    const struct overlay *overlay = &selection->output->overlay;
    /* dwl keeps the pointer on the pressed surface, with coordinates past
       its edges when the drag leaves it, so clamping keeps it on this output. */
    selection->end_x = clamp(selection->x, 0, overlay->width);
    selection->end_y = clamp(selection->y, 0, overlay->height);
    overlay_render(shot, selection->output);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    struct shot *shot = data;
    struct selection *selection = &shot->selection;
    selection->pointer_output = output_for_surface(shot, surface);
    selection->x = wl_fixed_to_double(x);
    selection->y = wl_fixed_to_double(y);
    if (shot->cursor_shape_device != NULL)
        wp_cursor_shape_device_v1_set_shape(shot->cursor_shape_device, serial,
                                            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
    selection_moved(shot);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    (void)pointer, (void)serial, (void)surface;
    struct shot *shot = data;
    shot->selection.pointer_output = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer, (void)time;
    struct shot *shot = data;
    shot->selection.x = wl_fixed_to_double(x);
    shot->selection.y = wl_fixed_to_double(y);
    selection_moved(shot);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    (void)pointer, (void)serial, (void)time;
    struct shot *shot = data;
    struct selection *selection = &shot->selection;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (button == BTN_RIGHT && pressed) {
        selection_cancel(shot);
    } else if (button == BTN_LEFT && pressed && !selection->dragging) {
        struct output *output = selection->pointer_output;
        if (output == NULL || !output->overlay.configured) return;
        struct output *previous = selection->output;
        selection->output = output;
        selection->dragging = true;
        selection->start_x = selection->end_x = clamp(selection->x, 0, output->overlay.width);
        selection->start_y = selection->end_y = clamp(selection->y, 0, output->overlay.height);
        if (previous != NULL && previous != output) overlay_render(shot, previous);
        overlay_render(shot, output);
    } else if (button == BTN_LEFT && !pressed && selection->dragging) {
        selection->dragging = false;
        /* A click without a drag cancels, so a stray click saves nothing. */
        if (fabs(selection->end_x - selection->start_x) < 3 || fabs(selection->end_y - selection->start_y) < 3)
            selection_cancel(shot);
        else if (selection->state == SELECTION_WAITING)
            selection->state = SELECTION_DONE;
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data, (void)pointer, (void)time, (void)axis, (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data, (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)data, (void)pointer, (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
    (void)data, (void)pointer, (void)time, (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)data, (void)pointer, (void)axis, (void)discrete;
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

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data, (void)keyboard, (void)format, (void)size;
    close(fd); /* Escape is matched by evdev code, so no keymap is needed. */
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)data, (void)keyboard, (void)serial, (void)surface, (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
    (void)data, (void)keyboard, (void)serial, (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    (void)keyboard, (void)serial, (void)time;
    if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED) selection_cancel(data);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data, (void)keyboard, (void)serial, (void)depressed, (void)latched, (void)locked, (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)data, (void)keyboard, (void)rate, (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct shot *shot = data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && shot->pointer == NULL) {
        shot->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(shot->pointer, &pointer_listener, shot);
        if (shot->cursor_shape_manager != NULL)
            shot->cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(shot->cursor_shape_manager, shot->pointer);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && shot->keyboard == NULL) {
        shot->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(shot->keyboard, &keyboard_listener, shot);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data, (void)seat, (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static uint32_t border_color(const struct config *config)
{
    if (!config->border_accent) return config->border_rgb;
    uint32_t color = 0xffffff;
    char *path = desk_palette_path();
    DeskPalette palette;
    if (path != NULL && desk_palette_load(path, &palette) == 0) color = palette.rgb[DESK_ACCENT];
    free(path);
    return color;
}

static bool crop(const struct image *frame, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, struct image *image)
{
    uint32_t width = x1 - x0, height = y1 - y0;
    uint8_t *rgb = malloc((size_t)width * height * 3);
    if (rgb == NULL) return false;
    for (uint32_t y = 0; y < height; ++y)
        memcpy(rgb + (size_t)y * width * 3, frame->rgb + ((size_t)(y0 + y) * frame->width + x0) * 3,
               (size_t)width * 3);
    *image = (struct image){.width = width, .height = height, .rgb = rgb};
    return true;
}

/* Freeze every output, let the user drag a rectangle on one of them, and crop
   it from the frozen frame. Return EXIT_SAVED with image set, EXIT_FAILED on
   errors or cancel (cancel is silent). */
static int select_region(struct shot *shot, bool cursor, struct image *image)
{
    const char *missing = shot->compositor == NULL     ? "wl_compositor"
                          : shot->layer_shell == NULL  ? "wlr-layer-shell-unstable-v1"
                          : shot->viewporter == NULL   ? "wp-viewporter"
                          : shot->seat == NULL         ? "wl_seat"
                                                       : NULL;
    if (missing != NULL) {
        warn("the compositor lacks %s, which region capture needs", missing);
        return EXIT_FAILED;
    }
    if (wl_list_empty(&shot->outputs)) {
        warn("the compositor reports no outputs");
        return EXIT_FAILED;
    }
    if (!capture_outputs(shot, NULL, cursor)) return EXIT_FAILED;
    shot->border_rgb = border_color(&shot->config);
    shot->selection = (struct selection){.state = SELECTION_WAITING};
    struct output *output;
    wl_list_for_each(output, &shot->outputs, link) {
        if (!overlay_create(shot, output)) {
            warn("out of memory preparing the overlay");
            return EXIT_FAILED;
        }
    }
    while (shot->selection.state == SELECTION_WAITING) {
        if (wl_display_dispatch(shot->display) < 0) {
            warn("lost the Wayland connection: %s", strerror(wl_display_get_error(shot->display)));
            return EXIT_FAILED;
        }
    }
    if (shot->selection.state != SELECTION_DONE) return EXIT_FAILED;
    uint32_t x0, y0, x1, y1;
    output = shot->selection.output;
    if (!selection_rect(shot, output, &x0, &y0, &x1, &y1) || x1 <= x0 || y1 <= y0) return EXIT_FAILED;
    if (!crop(&output->frame, x0, y0, x1, y1, image)) {
        warn("out of memory cropping the selection");
        return EXIT_FAILED;
    }
    return EXIT_SAVED;
}

/* PNG */

struct png_error_state {
    jmp_buf jump;
    char message[256];
};

static void png_fail(png_structp png, png_const_charp message)
{
    struct png_error_state *state = png_get_error_ptr(png);
    snprintf(state->message, sizeof state->message, "%s", message);
    longjmp(state->jump, 1);
}

static void png_warning_ignore(png_structp png, png_const_charp message)
{
    (void)png, (void)message;
}

/* Return NULL on success or an error message valid until the next call. */
static const char *png_write(FILE *file, const struct image *image, int compression)
{
    static struct png_error_state state;
    state.message[0] = 0;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &state, png_fail, png_warning_ignore);
    png_infop info = png != NULL ? png_create_info_struct(png) : NULL;
    if (info == NULL) {
        png_destroy_write_struct(&png, NULL);
        return "out of memory";
    }
    if (setjmp(state.jump)) {
        png_destroy_write_struct(&png, &info);
        return state.message[0] != 0 ? state.message : "PNG encoding failed";
    }
    png_init_io(png, file);
    png_set_compression_level(png, compression);
    png_set_IHDR(png, info, image->width, image->height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (uint32_t y = 0; y < image->height; ++y) png_write_row(png, image->rgb + (size_t)y * image->width * 3);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    return NULL;
}

/* Encode to memory once, so a file, stdout and the clipboard share the bytes. */
static bool png_encode(const struct image *image, int compression, struct png_data *png)
{
    *png = (struct png_data){0};
    FILE *stream = open_memstream(&png->data, &png->size);
    if (stream == NULL) {
        warn("can't encode the PNG: %s", strerror(errno));
        return false;
    }
    const char *error = png_write(stream, image, compression);
    if (fclose(stream) == EOF && error == NULL) error = strerror(errno);
    if (error != NULL) {
        warn("can't encode the PNG: %s", error);
        free(png->data);
        *png = (struct png_data){0};
        return false;
    }
    return true;
}

static bool write_all(FILE *file, const struct png_data *png)
{
    return fwrite(png->data, 1, png->size, file) == png->size;
}

/* Output files */

static int save_stdout(const struct png_data *png)
{
    if (!write_all(stdout, png) || fflush(stdout) == EOF) {
        warn("can't write to stdout: %s", strerror(errno));
        return EXIT_FAILED;
    }
    return EXIT_SAVED;
}

/* The path for a capture without a FILE argument: the configured directory
   (created when missing) and strftime name, split so a suffix can go before
   a .png extension. */
static bool default_path(const struct config *config, char **stem, char extension[5])
{
    char *directory = desk_expand_home(config->directory);
    if (directory == NULL) {
        warn("can't expand %s (is HOME set?)", config->directory);
        return false;
    }
    int error = desk_mkdirs(directory, 0755);
    if (error != 0) {
        warn("can't create %s: %s", directory, strerror(error));
        free(directory);
        return false;
    }
    time_t now = time(NULL);
    struct tm local;
    char name[256];
    if (localtime_r(&now, &local) == NULL || strftime(name, sizeof name, config->name, &local) == 0 ||
        strchr(name, '/') != NULL) {
        warn("the name format %s gives an empty or invalid name", config->name);
        free(directory);
        return false;
    }
    size_t length = strlen(name);
    extension[0] = 0;
    if (length > 4 && strcasecmp(name + length - 4, ".png") == 0) {
        memcpy(extension, name + length - 4, 5);
        name[length - 4] = 0;
    }
    error = asprintf(stem, "%s/%s", directory, name) < 0;
    free(directory);
    return error == 0;
}

/* Start `view path`, detached: in its own session, not a child of shot, and
   without shot's stdin and stdout, so `xdg-open "$(shot)"` and key bindings
   don't wait for the viewer to close. */
static void open_in_view(const char *path)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        warn("can't start view: %s", strerror(errno));
        return;
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
        return;
    }
    setsid();
    if (fork() != 0) _exit(0); /* The grandchild is reparented, so nothing waits on it. */
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
    }
    execlp("view", "view", "--", path, (char *)NULL);
    warn("can't run view: %s", strerror(errno));
    _exit(127);
}

/* Write path, or a name built from the config when path is NULL. Configured
   names never replace a file: they get -1, -2... instead. */
static int save_file(const struct config *config, const char *path, const struct png_data *png)
{
    char *stem = NULL, *target = NULL;
    char extension[5] = "";
    if (path == NULL) {
        if (!default_path(config, &stem, extension)) return EXIT_FAILED;
        if (asprintf(&target, "%s%s", stem, extension) < 0) {
            free(stem);
            warn("out of memory");
            return EXIT_FAILED;
        }
        path = target;
    }
    DeskAtomic atomic;
    int error = desk_atomic_open(&atomic, path, 0644);
    if (error != 0) {
        warn("can't create a file next to %s: %s", path, strerror(error));
        free(stem);
        free(target);
        return EXIT_FAILED;
    }
    if (!write_all(atomic.file, png)) {
        warn("can't write %s: %s", path, strerror(errno ? errno : EIO));
        desk_atomic_abort(&atomic);
        free(stem);
        free(target);
        return EXIT_FAILED;
    }
    error = desk_atomic_commit(&atomic, stem == NULL);
    for (unsigned suffix = 1; error == EEXIST && suffix < 1000; ++suffix) {
        free(target);
        if (asprintf(&target, "%s-%u%s", stem, suffix, extension) < 0) {
            target = NULL;
            error = ENOMEM;
            break;
        }
        path = target;
        if ((error = desk_atomic_retarget(&atomic, path)) == 0) error = desk_atomic_commit(&atomic, false);
    }
    desk_atomic_abort(&atomic);
    if (error != 0) warn("can't save %s: %s", path != NULL ? path : "the capture", strerror(error));
    else printf("%s\n", path);
    if (error == 0 && config->open) open_in_view(path);
    free(stem);
    free(target);
    return error != 0 ? EXIT_FAILED : EXIT_SAVED;
}

/* Clipboard */

struct clipboard {
    const struct png_data *png;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct ext_data_control_manager_v1 *manager;
    struct ext_data_control_device_v1 *device;
    struct ext_data_control_source_v1 *source;
    bool cancelled;
};

static const char png_mime[] = "image/png";

static void source_send(void *data, struct ext_data_control_source_v1 *source, const char *mime_type, int32_t fd)
{
    (void)source;
    struct clipboard *clipboard = data;
    /* One process per transfer: a reader that stalls can't hold up the others,
       and the copy needs no event loop of its own. SIGCHLD is ignored, so
       finished writers are reaped automatically. */
    if (strcmp(mime_type, png_mime) == 0 && fork() == 0) {
        const char *at = clipboard->png->data;
        size_t left = clipboard->png->size;
        while (left > 0) {
            ssize_t written = write(fd, at, left);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) _exit(1);
            at += written;
            left -= (size_t)written;
        }
        _exit(0);
    }
    close(fd);
}

static void source_cancelled(void *data, struct ext_data_control_source_v1 *source)
{
    (void)source;
    struct clipboard *clipboard = data;
    clipboard->cancelled = true;
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = source_send,
    .cancelled = source_cancelled,
};

static void device_data_offer(void *data, struct ext_data_control_device_v1 *device,
                              struct ext_data_control_offer_v1 *offer)
{
    (void)data, (void)device, (void)offer; /* Destroyed when a selection event names it. */
}

static void device_selection(void *data, struct ext_data_control_device_v1 *device,
                             struct ext_data_control_offer_v1 *offer)
{
    (void)data, (void)device;
    if (offer != NULL) ext_data_control_offer_v1_destroy(offer);
}

static void device_finished(void *data, struct ext_data_control_device_v1 *device)
{
    (void)device;
    struct clipboard *clipboard = data;
    clipboard->cancelled = true;
}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = device_data_offer,
    .selection = device_selection,
    .finished = device_finished,
    .primary_selection = device_selection,
};

static void clipboard_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                             uint32_t version)
{
    (void)version;
    struct clipboard *clipboard = data;
    if (strcmp(interface, wl_seat_interface.name) == 0 && clipboard->seat == NULL)
        clipboard->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
        clipboard->manager = wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1);
}

static void clipboard_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data, (void)registry, (void)name;
}

static const struct wl_registry_listener clipboard_registry_listener = {
    .global = clipboard_global,
    .global_remove = clipboard_global_remove,
};

/* Connect and take the clipboard. Return false after reporting why not. */
static bool clipboard_take(struct clipboard *clipboard)
{
    clipboard->display = wl_display_connect(NULL);
    if (clipboard->display == NULL) {
        warn("can't connect to the Wayland display for the clipboard");
        return false;
    }
    clipboard->registry = wl_display_get_registry(clipboard->display);
    wl_registry_add_listener(clipboard->registry, &clipboard_registry_listener, clipboard);
    if (wl_display_roundtrip(clipboard->display) < 0) {
        warn("lost the Wayland connection");
        return false;
    }
    if (clipboard->manager == NULL || clipboard->seat == NULL) {
        warn("the compositor lacks %s, which the clipboard needs",
             clipboard->manager == NULL ? "ext-data-control-v1" : "wl_seat");
        return false;
    }
    clipboard->device = ext_data_control_manager_v1_get_data_device(clipboard->manager, clipboard->seat);
    ext_data_control_device_v1_add_listener(clipboard->device, &device_listener, clipboard);
    clipboard->source = ext_data_control_manager_v1_create_data_source(clipboard->manager);
    ext_data_control_source_v1_add_listener(clipboard->source, &source_listener, clipboard);
    ext_data_control_source_v1_offer(clipboard->source, png_mime);
    ext_data_control_device_v1_set_selection(clipboard->device, clipboard->source);
    if (wl_display_roundtrip(clipboard->display) < 0 || clipboard->cancelled) {
        warn("the compositor refused the clipboard selection");
        return false;
    }
    return true;
}

/* Put the PNG on the clipboard. A background child owns the selection until
   another client replaces it; the parent returns once the child has it, so
   key bindings don't wait. */
static int clipboard_copy(const struct png_data *png)
{
    int ready[2];
    if (pipe2(ready, O_CLOEXEC) < 0) {
        warn("can't start the clipboard process: %s", strerror(errno));
        return EXIT_FAILED;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        warn("can't start the clipboard process: %s", strerror(errno));
        close(ready[0]);
        close(ready[1]);
        return EXIT_FAILED;
    }
    if (pid > 0) {
        close(ready[1]);
        char ok = 0;
        ssize_t got;
        do got = read(ready[0], &ok, 1);
        while (got < 0 && errno == EINTR);
        close(ready[0]);
        if (got == 1 && ok == 1) return EXIT_SAVED;
        waitpid(pid, NULL, 0); /* It reported its own error. */
        return EXIT_FAILED;
    }

    close(ready[0]);
    setsid();
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    struct clipboard clipboard = {.png = png};
    char ok = clipboard_take(&clipboard);
    if (ok) {
        /* Let go of the terminal and of $(...) pipes before reporting success. */
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            close(null);
        }
        if (chdir("/") < 0) { /* Only so the directory isn't kept busy. */ }
    }
    if (write(ready[1], &ok, 1) < 0) { /* The parent is gone; serve anyway. */ }
    close(ready[1]);
    while (ok && !clipboard.cancelled && wl_display_dispatch(clipboard.display) >= 0) {
    }
    if (clipboard.display != NULL) wl_display_disconnect(clipboard.display);
    _exit(ok ? EXIT_SAVED : EXIT_FAILED);
}

/* Command line */

static void usage(FILE *stream)
{
    fputs("Usage: shot [-r | -o OUTPUT] [-c] [-C] [FILE]\n"
          "       shot -l\n"
          "\n"
          "Capture an output, or a region of one, to a PNG file.\n"
          "\n"
          "  -r, --region       drag a rectangle to capture (Escape or right-click cancels)\n"
          "  -o, --output NAME  capture this output (default: the focused one)\n"
          "  -c, --cursor       include the cursor\n"
          "  -C, --clipboard    copy to the clipboard; without FILE, don't save a file\n"
          "  -l, --list         list outputs and exit\n"
          "  -h, --help         show this help\n"
          "  -v, --version      show the version\n"
          "\n"
          "FILE defaults to [save] directory and name in ~/.config/shot/shot.conf.\n"
          "Use - to write the PNG to stdout.\n",
          stream);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"region", no_argument, NULL, 'r'},
        {"output", required_argument, NULL, 'o'},
        {"cursor", no_argument, NULL, 'c'},
        {"clipboard", no_argument, NULL, 'C'},
        {"list", no_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {0},
    };
    const char *output_name_option = NULL;
    bool cursor = false, list = false, region = false, clipboard = false;
    int option;
    while ((option = getopt_long(argc, argv, "ro:cClhv", options, NULL)) != -1) {
        switch (option) {
        case 'r': region = true; break;
        case 'o': output_name_option = optarg; break;
        case 'c': cursor = true; break;
        case 'C': clipboard = true; break;
        case 'l': list = true; break;
        case 'h': usage(stdout); return EXIT_SAVED;
        case 'v': puts("shot " SHOT_VERSION); return EXIT_SAVED;
        default: usage(stderr); return EXIT_USAGE;
        }
    }
    if (argc - optind > 1 || (region && output_name_option != NULL)) {
        if (region && output_name_option != NULL) warn("-r and -o can't be combined");
        usage(stderr);
        return EXIT_USAGE;
    }
    const char *path = optind < argc ? argv[optind] : NULL;
    bool to_stdout = path != NULL && strcmp(path, "-") == 0;
    if (to_stdout && isatty(STDOUT_FILENO)) {
        warn("refusing to write a PNG to a terminal (redirect stdout or give a file name)");
        return EXIT_USAGE;
    }

    struct shot shot = {0};
    config_load(&shot.config);
    cursor = cursor || shot.config.cursor;
    struct image image = {0};
    int status = EXIT_FAILED;
    if (!shot_connect(&shot)) goto done;
    if (list) {
        list_outputs(&shot);
        status = EXIT_SAVED;
        goto done;
    }
    if (shot.shm == NULL || shot.source_manager == NULL || shot.copy_manager == NULL) {
        warn("the compositor lacks %s", shot.shm == NULL              ? "wl_shm"
                                        : shot.source_manager == NULL ? "ext-output-image-capture-source-v1"
                                                                      : "ext-image-copy-capture-v1");
        goto done;
    }
    if (region) {
        if (select_region(&shot, cursor, &image) != EXIT_SAVED) goto done;
    } else {
        struct output *output = choose_output(&shot, output_name_option);
        if (output == NULL || !capture_outputs(&shot, output, cursor)) goto done;
        image = output->frame;
        output->frame = (struct image){0};
    }
    /* The image is in memory: drop the overlays before the slow PNG encoding. */
    shot_disconnect(&shot);
    struct png_data png;
    if (!png_encode(&image, shot.config.compression, &png)) goto done;
    /* The clipboard process lives on: keep only the encoded bytes. */
    free(image.rgb);
    image.rgb = NULL;
    /* -C alone only copies. A FILE, or no -C, saves; [save] clipboard adds a copy. */
    status = EXIT_SAVED;
    if (!clipboard || path != NULL)
        status = to_stdout ? save_stdout(&png) : save_file(&shot.config, path, &png);
    if (status == EXIT_SAVED && (clipboard || shot.config.clipboard)) status = clipboard_copy(&png);
    free(png.data);

done:
    shot_disconnect(&shot);
    free(image.rgb);
    config_free(&shot.config);
    return status;
}
