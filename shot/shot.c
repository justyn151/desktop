#define _GNU_SOURCE
#include "shot.h"

#include <errno.h>
#include <stdarg.h>
#include <getopt.h>
#include <png.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "desk.h"

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

static void output_destroy(struct output *output)
{
    wl_list_remove(&output->link);
    if (output->ipc != NULL) zdwl_ipc_output_v2_release(output->ipc);
    if (wl_output_get_version(output->wl_output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
        wl_output_release(output->wl_output);
    else
        wl_output_destroy(output->wl_output);
    free(output->name);
    free(output->description);
    free(output);
}

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
    *shot = (struct shot){0};
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
    if (shot->ipc_manager != NULL) zdwl_ipc_manager_v2_release(shot->ipc_manager);
    if (shot->copy_manager != NULL) ext_image_copy_capture_manager_v1_destroy(shot->copy_manager);
    if (shot->source_manager != NULL) ext_output_image_capture_source_manager_v1_destroy(shot->source_manager);
    if (shot->shm != NULL) wl_shm_destroy(shot->shm);
    if (shot->registry != NULL) wl_registry_destroy(shot->registry);
    if (shot->display != NULL) wl_display_disconnect(shot->display);
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

static bool capture_output(struct shot *shot, struct output *output, bool cursor, struct image *image)
{
    struct capture capture = {.shot = shot, .want_frame = true};
    capture.source = ext_output_image_capture_source_manager_v1_create_source(shot->source_manager,
                                                                               output->wl_output);
    uint32_t options = cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
    capture.session = ext_image_copy_capture_manager_v1_create_session(shot->copy_manager, capture.source, options);
    ext_image_copy_capture_session_v1_add_listener(capture.session, &session_listener, &capture);
    while (capture.state == CAPTURE_WAITING) {
        if (wl_display_dispatch(shot->display) < 0) {
            warn("lost the Wayland connection: %s", strerror(wl_display_get_error(shot->display)));
            capture_finish(&capture);
            return false;
        }
    }
    bool ok = capture.state == CAPTURE_READY;
    if (!ok) warn("can't capture %s: %s", output_label(output), capture.error);
    else if (!(ok = convert(&capture, image))) warn("out of memory converting the capture");
    capture_finish(&capture);
    return ok;
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
static const char *png_write(FILE *file, const struct image *image)
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
    png_set_IHDR(png, info, image->width, image->height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (uint32_t y = 0; y < image->height; ++y) png_write_row(png, image->rgb + (size_t)y * image->width * 3);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    return NULL;
}

/* Output files */

static char *default_name(void)
{
    time_t now = time(NULL);
    struct tm local;
    char name[64];
    if (localtime_r(&now, &local) == NULL || strftime(name, sizeof name, "%Y-%m-%d_%H-%M-%S", &local) == 0)
        return NULL;
    return strdup(name);
}

static int save_stdout(const struct image *image)
{
    const char *error = png_write(stdout, image);
    if (error == NULL && fflush(stdout) == EOF) error = strerror(errno);
    if (error != NULL) {
        warn("can't write to stdout: %s", error);
        return EXIT_FAILED;
    }
    return EXIT_SAVED;
}

/* Write path, or a timestamped name in the current directory when path is
   NULL. Timestamped names never replace a file: they get -1, -2... instead. */
static int save_file(const char *path, const struct image *image)
{
    char *stem = NULL, *target = NULL;
    if (path == NULL) {
        if ((stem = default_name()) == NULL || asprintf(&target, "%s.png", stem) < 0) {
            free(stem);
            warn("can't build the file name");
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
    const char *png_error = png_write(atomic.file, image);
    if (png_error != NULL) {
        warn("can't write %s: %s", path, png_error);
        desk_atomic_abort(&atomic);
        free(stem);
        free(target);
        return EXIT_FAILED;
    }
    error = desk_atomic_commit(&atomic, stem == NULL);
    for (unsigned suffix = 1; error == EEXIST && suffix < 1000; ++suffix) {
        free(target);
        target = NULL;
        if (asprintf(&target, "%s-%u.png", stem, suffix) < 0) {
            target = NULL;
            error = ENOMEM;
            break;
        }
        path = target;
        if ((error = desk_atomic_retarget(&atomic, path)) == 0) error = desk_atomic_commit(&atomic, false);
    }
    desk_atomic_abort(&atomic);
    if (error != 0) warn("can't save %s: %s", path, strerror(error));
    else printf("%s\n", path);
    free(stem);
    free(target);
    return error != 0 ? EXIT_FAILED : EXIT_SAVED;
}

/* Command line */

static void usage(FILE *stream)
{
    fputs("Usage: shot [-o OUTPUT] [-c] [FILE]\n"
          "       shot -l\n"
          "\n"
          "Capture a whole output to a PNG file.\n"
          "\n"
          "  -o, --output NAME  capture this output (default: the focused one)\n"
          "  -c, --cursor       include the cursor\n"
          "  -l, --list         list outputs and exit\n"
          "  -h, --help         show this help\n"
          "  -v, --version      show the version\n"
          "\n"
          "FILE defaults to YYYY-MM-DD_HH-MM-SS.png in the current directory.\n"
          "Use - to write the PNG to stdout.\n",
          stream);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"output", required_argument, NULL, 'o'},
        {"cursor", no_argument, NULL, 'c'},
        {"list", no_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {0},
    };
    const char *output_name_option = NULL;
    bool cursor = false, list = false;
    int option;
    while ((option = getopt_long(argc, argv, "o:clhv", options, NULL)) != -1) {
        switch (option) {
        case 'o': output_name_option = optarg; break;
        case 'c': cursor = true; break;
        case 'l': list = true; break;
        case 'h': usage(stdout); return EXIT_SAVED;
        case 'v': puts("shot " SHOT_VERSION); return EXIT_SAVED;
        default: usage(stderr); return EXIT_USAGE;
        }
    }
    if (argc - optind > 1) {
        usage(stderr);
        return EXIT_USAGE;
    }
    const char *path = optind < argc ? argv[optind] : NULL;
    bool to_stdout = path != NULL && strcmp(path, "-") == 0;
    if (to_stdout && isatty(STDOUT_FILENO)) {
        warn("refusing to write a PNG to a terminal (redirect stdout or give a file name)");
        return EXIT_USAGE;
    }

    struct shot shot;
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
    struct output *output = choose_output(&shot, output_name_option);
    if (output == NULL) goto done;
    struct image image;
    if (!capture_output(&shot, output, cursor, &image)) goto done;
    /* The image is in memory, so the compositor isn't needed while encoding. */
    shot_disconnect(&shot);
    status = to_stdout ? save_stdout(&image) : save_file(path, &image);
    free(image.rgb);
    return status;

done:
    shot_disconnect(&shot);
    return status;
}
