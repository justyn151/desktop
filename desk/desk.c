#define _GNU_SOURCE
#include "desk.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* Palette */

static const char *const role_names[DESK_ROLE_COUNT] = {
    "background", "foreground", "accent", "highlight", "muted",
    "selection_background", "selection_foreground", "border_active", "border_inactive", "urgent"
};

const char *desk_role_name(enum desk_role role)
{
    return role < DESK_ROLE_COUNT ? role_names[role] : NULL;
}

char *desk_palette_path(void)
{
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    char *path = NULL;
    int length;
    if (cache != NULL && cache[0] == '/') length = asprintf(&path, "%s/wall/colors.conf", cache);
    else if (home != NULL && home[0] != 0) length = asprintf(&path, "%s/.cache/wall/colors.conf", home);
    else return NULL;
    return length < 0 ? NULL : path;
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = 0;
    return text;
}

static bool parse_rgb(const char *text, uint32_t *rgb)
{
    if (strlen(text) != 7 || text[0] != '#') return false;
    uint32_t value = 0;
    for (int i = 1; i < 7; ++i) {
        unsigned char digit = (unsigned char)text[i];
        if (!isxdigit(digit)) return false;
        value = value * 16 + (uint32_t)(isdigit(digit) ? digit - '0' : tolower(digit) - 'a' + 10);
    }
    *rgb = value;
    return true;
}

int desk_palette_load(const char *path, DeskPalette *palette)
{
    if (path == NULL) return EINVAL;
    FILE *file = fopen(path, "re");
    if (file == NULL) return errno;
    DeskPalette loaded = {0};
    unsigned int seen = 0;
    bool in_colors = false, valid = true;
    char line[256];
    while (valid && fgets(line, sizeof line, file) != NULL) {
        if (strchr(line, '\n') == NULL && !feof(file)) {
            valid = false;
            break;
        }
        char *text = trim(line);
        if (*text == 0 || *text == '#' || *text == ';') continue;
        if (*text == '[') {
            in_colors = strcmp(text, "[colors]") == 0;
            continue;
        }
        char *equals = strchr(text, '=');
        if (!in_colors || equals == NULL) continue;
        *equals = 0;
        const char *key = trim(text), *value = trim(equals + 1);
        for (int role = 0; role < DESK_ROLE_COUNT; ++role) {
            if (strcmp(key, role_names[role]) != 0) continue;
            if ((seen & (1u << role)) || !parse_rgb(value, &loaded.rgb[role])) valid = false;
            seen |= 1u << role;
            break;
        }
    }
    bool failed = ferror(file);
    fclose(file);
    if (failed) return EIO;
    if (!valid || seen != (1u << DESK_ROLE_COUNT) - 1) return EINVAL;
    *palette = loaded;
    return 0;
}

/* File watching */

static void watch_arm(DeskWatch *watch)
{
    char *directory = strdup(watch->path);
    if (directory == NULL) return;
    watch->direct = true;
    for (;;) {
        char *slash = strrchr(directory, '/');
        if (slash == NULL) break;
        if (slash == directory) slash[1] = 0;
        else *slash = 0;
        uint32_t mask = IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE | IN_MOVE_SELF |
                        IN_DELETE_SELF | IN_ONLYDIR;
        if (inotify_add_watch(watch->fd, directory, mask) >= 0) break;
        if (errno != ENOENT && errno != ENOTDIR) break;
        if (strcmp(directory, "/") == 0) break;
        watch->direct = false;
    }
    free(directory);
}

bool desk_watch_open(DeskWatch *watch, const char *path)
{
    *watch = (DeskWatch){.fd = -1};
    if (path == NULL || path[0] != '/' || (watch->path = strdup(path)) == NULL) return false;
    watch->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (watch->fd < 0) {
        desk_watch_close(watch);
        return false;
    }
    watch_arm(watch);
    return true;
}

static void watch_rearm(DeskWatch *watch)
{
    close(watch->fd);
    watch->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (watch->fd >= 0) watch_arm(watch);
}

bool desk_watch_changed(DeskWatch *watch, short revents)
{
    if (watch->fd < 0) return false;
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        watch_rearm(watch);
        return true;
    }
    if (!(revents & POLLIN)) return false;
    const char *name = strrchr(watch->path, '/') + 1;
    bool changed = false, rearm = false;
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t length = read(watch->fd, buffer, sizeof buffer);
        if (length <= 0) {
            if (length < 0 && errno == EINTR) continue;
            break;
        }
        for (char *at = buffer; at < buffer + length;) {
            const struct inotify_event *event = (const struct inotify_event *)at;
            at += sizeof *event + event->len;
            if (event->mask & (IN_Q_OVERFLOW | IN_IGNORED | IN_MOVE_SELF | IN_DELETE_SELF)) {
                rearm = changed = true;
            } else if (!watch->direct) {
                /* An ancestor gained a child, which may be the missing directory. */
                rearm = changed = true;
            } else if (event->len != 0 && strcmp(event->name, name) == 0) {
                changed = true;
            }
        }
    }
    if (rearm) watch_rearm(watch);
    return changed;
}

void desk_watch_close(DeskWatch *watch)
{
    if (watch->fd >= 0) close(watch->fd);
    free(watch->path);
    *watch = (DeskWatch){.fd = -1};
}

/* Atomic files */

int desk_atomic_open(DeskAtomic *atomic, const char *path, mode_t mode)
{
    *atomic = (DeskAtomic){0};
    if (path == NULL || path[0] == 0) return EINVAL;
    const char *name = strrchr(path, '/');
    int length;
    if (name == NULL) {
        length = asprintf(&atomic->temporary, ".%s-XXXXXX", path);
    } else {
        /* Keep the temporary file in the target's directory so rename is atomic. */
        length = asprintf(&atomic->temporary, "%.*s/.%s-XXXXXX", (int)(name - path), path, name + 1);
    }
    if (length < 0) atomic->temporary = NULL; /* asprintf leaves it undefined on failure. */
    if (length < 0 || (atomic->path = strdup(path)) == NULL) {
        desk_atomic_abort(atomic);
        return ENOMEM;
    }
    int fd = mkostemp(atomic->temporary, O_CLOEXEC);
    if (fd < 0) {
        int error = errno;
        free(atomic->temporary);
        atomic->temporary = NULL;
        desk_atomic_abort(atomic);
        return error;
    }
    mode_t mask = umask(0);
    umask(mask);
    if (fchmod(fd, mode & ~mask) < 0 || (atomic->file = fdopen(fd, "wb")) == NULL) {
        int error = errno;
        close(fd);
        desk_atomic_abort(atomic);
        return error;
    }
    return 0;
}

int desk_atomic_retarget(DeskAtomic *atomic, const char *path)
{
    char *copy = strdup(path);
    if (copy == NULL) return ENOMEM;
    free(atomic->path);
    atomic->path = copy;
    return 0;
}

int desk_atomic_commit(DeskAtomic *atomic, bool replace)
{
    if (atomic->temporary == NULL) return EINVAL;
    int error = 0;
    if (atomic->file != NULL) {
        if (ferror(atomic->file) || fflush(atomic->file) == EOF || fsync(fileno(atomic->file)) < 0)
            error = errno ? errno : EIO;
        if (fclose(atomic->file) == EOF && error == 0) error = errno;
        atomic->file = NULL;
    }
    if (error == 0) {
        if (replace) {
            if (rename(atomic->temporary, atomic->path) < 0) error = errno;
        } else if (renameat2(AT_FDCWD, atomic->temporary, AT_FDCWD, atomic->path, RENAME_NOREPLACE) < 0) {
            error = errno;
            if (error == EINVAL || error == ENOSYS) {
                /* The filesystem can't rename without replacing: link refuses existing names too. */
                error = link(atomic->temporary, atomic->path) < 0 ? errno : 0;
                if (error == 0) unlink(atomic->temporary);
            }
            if (error == EEXIST) return EEXIST;
        }
    }
    if (error == 0) {
        free(atomic->temporary);
        atomic->temporary = NULL;
    }
    desk_atomic_abort(atomic);
    return error;
}

void desk_atomic_abort(DeskAtomic *atomic)
{
    if (atomic->file != NULL) fclose(atomic->file);
    if (atomic->temporary != NULL) unlink(atomic->temporary);
    free(atomic->temporary);
    free(atomic->path);
    *atomic = (DeskAtomic){0};
}

/* Shared memory */

int desk_shm_open(size_t size)
{
    if (size == 0 || size > (size_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    int fd = memfd_create("desk-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    int error;
    do error = ftruncate(fd, (off_t)size) < 0 ? errno : 0;
    while (error == EINTR);
    if (error != 0) {
        close(fd);
        errno = error;
        return -1;
    }
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
    return fd;
}

/* Paths */

char *desk_config_path(const char *app)
{
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char *path = NULL;
    int length;
    if (config != NULL && config[0] == '/') length = asprintf(&path, "%s/%s/%s.conf", config, app, app);
    else if (home != NULL && home[0] != 0) length = asprintf(&path, "%s/.config/%s/%s.conf", home, app, app);
    else return NULL;
    return length < 0 ? NULL : path;
}

char *desk_expand_home(const char *path)
{
    if (path[0] != '~' || (path[1] != '/' && path[1] != 0)) return strdup(path);
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == 0) return NULL;
    char *expanded;
    return asprintf(&expanded, "%s%s", home, path + 1) < 0 ? NULL : expanded;
}

int desk_mkdirs(const char *path, mode_t mode)
{
    char *copy = strdup(path);
    if (copy == NULL) return ENOMEM;
    int error = 0;
    for (char *at = copy + 1;; ++at) {
        if (*at != '/' && *at != 0) continue;
        char saved = *at;
        *at = 0;
        if (mkdir(copy, mode) < 0 && errno != EEXIST) {
            error = errno;
            break;
        }
        *at = saved;
        if (saved == 0) break;
    }
    struct stat info;
    if (error == 0 && (stat(path, &info) < 0 || !S_ISDIR(info.st_mode))) error = errno ? errno : ENOTDIR;
    free(copy);
    return error;
}

int desk_write_default(const char *path, const void *data, size_t length)
{
    if (access(path, F_OK) == 0) return EEXIST;
    const char *slash = strrchr(path, '/');
    if (slash != NULL && slash != path) {
        char *directory = strndup(path, (size_t)(slash - path));
        if (directory == NULL) return ENOMEM;
        int error = desk_mkdirs(directory, 0755);
        free(directory);
        if (error != 0) return error;
    }
    DeskAtomic atomic;
    int error = desk_atomic_open(&atomic, path, 0644);
    if (error != 0) return error;
    if (length != 0 && fwrite(data, 1, length, atomic.file) != length) {
        error = errno ? errno : EIO;
        desk_atomic_abort(&atomic);
        return error;
    }
    error = desk_atomic_commit(&atomic, false);
    desk_atomic_abort(&atomic);
    return error;
}

/* INI */

void desk_ini_parse(const char *text, size_t length, DeskIniHandler handler, void *data)
{
    char section[128] = "";
    char *line = NULL;
    size_t capacity = 0;
    int number = 0;
    for (size_t start = 0; start < length;) {
        const char *end = memchr(text + start, '\n', length - start);
        size_t size = end != NULL ? (size_t)(end - (text + start)) : length - start;
        ++number;
        if (size + 1 > capacity) {
            char *grown = realloc(line, size + 1);
            if (grown == NULL) break;
            line = grown;
            capacity = size + 1;
        }
        memcpy(line, text + start, size);
        line[size] = 0;
        start += size + 1;

        char *trimmed = trim(line);
        if (*trimmed == 0 || *trimmed == '#' || *trimmed == ';') continue;
        size_t trimmed_length = strlen(trimmed);
        if (*trimmed == '[') {
            if (trimmed[trimmed_length - 1] != ']' || trimmed_length - 2 >= sizeof section) {
                handler(data, section, NULL, NULL, number);
                continue;
            }
            trimmed[trimmed_length - 1] = 0;
            snprintf(section, sizeof section, "%s", trim(trimmed + 1));
            continue;
        }
        char *equals = strchr(trimmed, '=');
        if (equals == NULL) {
            handler(data, section, NULL, NULL, number);
            continue;
        }
        *equals = 0;
        char *key = trim(trimmed), *value = trim(equals + 1);
        size_t value_length = strlen(value);
        if (value_length >= 2 && value[0] == '"' && value[value_length - 1] == '"') {
            value[value_length - 1] = 0;
            ++value;
        }
        handler(data, section, key, value, number);
    }
    free(line);
}

int desk_ini_load(const char *path, DeskIniHandler handler, void *data)
{
    FILE *file = fopen(path, "re");
    if (file == NULL) return errno;
    size_t limit = 1 << 20, length = 0;
    char *text = malloc(limit);
    if (text == NULL) {
        fclose(file);
        return ENOMEM;
    }
    length = fread(text, 1, limit, file);
    int error = ferror(file) ? EIO : (length == limit ? EFBIG : 0);
    fclose(file);
    if (error == 0) desk_ini_parse(text, length, handler, data);
    free(text);
    return error;
}

bool desk_parse_bool(const char *text, bool *value)
{
    static const char *const yes[] = {"true", "yes", "on", "1"}, *const no[] = {"false", "no", "off", "0"};
    for (size_t i = 0; i < sizeof yes / sizeof *yes; ++i) {
        if (strcasecmp(text, yes[i]) == 0) return *value = true, true;
        if (strcasecmp(text, no[i]) == 0) return *value = false, true;
    }
    return false;
}

bool desk_parse_rgb(const char *text, uint32_t *rgb)
{
    return parse_rgb(text, rgb);
}

/* Key repeat */

static void repeat_arm(DeskRepeat *repeat, long first_ms, long interval_ms)
{
    struct itimerspec spec = {
        .it_value = {first_ms / 1000, first_ms % 1000 * 1000000L},
        .it_interval = {interval_ms / 1000, interval_ms % 1000 * 1000000L},
    };
    if (repeat->fd >= 0)
        timerfd_settime(repeat->fd, 0, &spec, NULL);
}

int desk_repeat_open(DeskRepeat *repeat)
{
    *repeat = (DeskRepeat){ .fd = -1, .rate = 25, .delay = 600 };
    repeat->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    return repeat->fd < 0 ? errno : 0;
}

void desk_repeat_info(DeskRepeat *repeat, int32_t rate, int32_t delay)
{
    repeat->rate = rate > 0 ? rate : 0;
    repeat->delay = delay > 0 ? delay : 0;
    if (!repeat->rate)
        desk_repeat_cancel(repeat);
}

void desk_repeat_start(DeskRepeat *repeat, uint32_t key)
{
    if (!repeat->rate) {
        desk_repeat_cancel(repeat);
        return;
    }
    repeat->key = key;
    repeat->active = true;
    long interval = 1000 / repeat->rate;
    /* A zero it_value would disarm the timer. */
    repeat_arm(repeat, repeat->delay > 0 ? repeat->delay : 1, interval > 0 ? interval : 1);
}

void desk_repeat_release(DeskRepeat *repeat, uint32_t key)
{
    if (repeat->active && repeat->key == key)
        desk_repeat_cancel(repeat);
}

void desk_repeat_cancel(DeskRepeat *repeat)
{
    repeat->active = false;
    repeat_arm(repeat, 0, 0);
}

unsigned desk_repeat_fired(DeskRepeat *repeat)
{
    uint64_t count = 0;
    if (repeat->fd < 0 || read(repeat->fd, &count, sizeof count) != sizeof count || !repeat->active)
        return 0;
    return count > UINT32_MAX ? UINT32_MAX : (unsigned)count;
}

void desk_repeat_close(DeskRepeat *repeat)
{
    if (repeat->fd >= 0)
        close(repeat->fd);
    *repeat = (DeskRepeat){ .fd = -1 };
}

#ifdef DESK_WAYLAND
/* shm buffers */

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    DeskBuffer *slot = data;
    slot->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static void buffer_slot_free(DeskBuffer *slot)
{
    if (slot->buffer != NULL) wl_buffer_destroy(slot->buffer);
    if (slot->pixels != NULL) munmap(slot->pixels, slot->size);
    *slot = (DeskBuffer){0};
}

DeskBuffer *desk_buffer_get(DeskBufferPool *pool, struct wl_shm *shm, int width, int height, uint32_t format)
{
    DeskBuffer *slot = NULL;
    for (int i = 0; i < 2 && slot == NULL; ++i) {
        if (!pool->slots[i].busy) slot = &pool->slots[i];
    }
    if (slot == NULL) return NULL;
    if (slot->buffer != NULL && slot->width == width && slot->height == height) {
        slot->busy = true;
        return slot;
    }
    buffer_slot_free(slot);
    if (width <= 0 || height <= 0 || width > INT32_MAX / 4 / height) return NULL;
    size_t size = (size_t)width * 4 * (size_t)height;
    int fd = desk_shm_open(size);
    if (fd < 0) return NULL;
    void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    struct wl_shm_pool *shm_pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    slot->buffer = wl_shm_pool_create_buffer(shm_pool, 0, width, height, width * 4, format);
    wl_shm_pool_destroy(shm_pool);
    close(fd);
    wl_buffer_add_listener(slot->buffer, &buffer_listener, slot);
    slot->pixels = pixels;
    slot->size = size;
    slot->width = width;
    slot->height = height;
    slot->busy = true;
    return slot;
}

void desk_buffer_pool_finish(DeskBufferPool *pool)
{
    for (int i = 0; i < 2; ++i) buffer_slot_free(&pool->slots[i]);
}
#endif

#ifdef DESK_IMAGE
/* Image decoding. Moved from wall, which printed these same messages. */

#include <jpeglib.h>
#include <png.h>
#include <setjmp.h>
#include <stdarg.h>
#ifdef DESK_IMAGE_WEBP
#include <webp/decode.h>
#endif

struct image_error {
    char *text;
    size_t size;
};

static void image_error(char *error, size_t size, const char *format, ...)
{
    if (!size)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static bool image_allocate(DeskImage *image, uint32_t width, uint32_t height, size_t max_bytes)
{
    if (!width || !height || width > max_bytes / 4 ||
        height > max_bytes / ((size_t)width * 4)) {
        errno = EFBIG;
        return false;
    }
    image->pixels = malloc((size_t)width * height * 4);
    if (!image->pixels)
        return false;
    image->width = width;
    image->height = height;
    return true;
}

static void png_fail(png_structp png, png_const_charp message)
{
    struct image_error *error = png_get_error_ptr(png);
    image_error(error->text, error->size, "PNG: %s", message);
    png_longjmp(png, 1);
}

static bool decode_png(FILE *file, DeskImage *image, size_t max_bytes, struct image_error *error)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, error, png_fail, png_fail);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!info) {
        if (png)
            png_destroy_read_struct(&png, NULL, NULL);
        image_error(error->text, error->size, "could not initialize the PNG decoder");
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }
    png_init_io(png, file);
    png_read_info(png, info);
    if (!image_allocate(image, png_get_image_width(png, info), png_get_image_height(png, info), max_bytes))
        png_error(png, strerror(errno));

    int color = png_get_color_type(png, info);
    png_set_expand(png);
    if (png_get_bit_depth(png, info) == 16)
        png_set_scale_16(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_add_alpha(png, 255, PNG_FILLER_AFTER);
    png_set_alpha_mode(png, PNG_ALPHA_PNG, PNG_DEFAULT_sRGB);
    int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);

    size_t stride = (size_t)image->width * 4;
    size_t bytes = stride * image->height;
    if (png_get_rowbytes(png, info) != stride)
        png_error(png, "unexpected RGBA row size");
    if (passes > 1)
        memset(image->pixels, 0, bytes);
    for (int pass = 0; pass < passes; ++pass) {
        for (uint32_t y = 0; y < image->height; ++y)
            png_read_row(png, image->pixels + y * stride, NULL);
    }
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);

    for (size_t i = 0; i < bytes; i += 4) {
        unsigned alpha = image->pixels[i + 3];
        for (size_t channel = 0; channel < 3; ++channel) {
            unsigned value = image->pixels[i + channel] * alpha;
            image->pixels[i + channel] = (unsigned char)((value + 127) / 255);
        }
    }
    return true;
}

struct jpeg_decoder {
    struct jpeg_error_mgr errors;
    struct jpeg_decompress_struct info;
    jmp_buf failure;
    char message[JMSG_LENGTH_MAX];
};

static void jpeg_fail(j_common_ptr info)
{
    struct jpeg_decoder *decoder = (struct jpeg_decoder *)info->err;
    info->err->format_message(info, decoder->message);
    longjmp(decoder->failure, 1);
}

static void jpeg_message(j_common_ptr info, int level)
{
    /* libjpeg otherwise recovers some damaged files, including truncated ones. */
    if (level < 0)
        jpeg_fail(info);
}

static uint32_t exif_read(const unsigned char *at, int bytes, bool big_endian)
{
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i)
        value |= (uint32_t)at[big_endian ? i : bytes - 1 - i] << (8 * (bytes - 1 - i));
    return value;
}

/* The Orientation tag (0x0112) from the first IFD of an EXIF TIFF block, or 1. */
static int exif_orientation(const unsigned char *tiff, size_t length)
{
    if (length < 8)
        return 1;
    bool big_endian;
    if (memcmp(tiff, "II*\0", 4) == 0)
        big_endian = false;
    else if (memcmp(tiff, "MM\0*", 4) == 0)
        big_endian = true;
    else
        return 1;
    uint32_t ifd = exif_read(tiff + 4, 4, big_endian);
    if (ifd > length - 2)
        return 1;
    uint32_t count = exif_read(tiff + ifd, 2, big_endian);
    for (uint32_t i = 0; i < count; ++i) {
        size_t entry = ifd + 2 + (size_t)i * 12;
        if (entry + 12 > length)
            break;
        if (exif_read(tiff + entry, 2, big_endian) != 0x0112)
            continue;
        /* A SHORT, stored in the first two bytes of the value field. */
        if (exif_read(tiff + entry + 2, 2, big_endian) != 3)
            return 1;
        uint32_t value = exif_read(tiff + entry + 8, 2, big_endian);
        return value >= 1 && value <= 8 ? (int)value : 1;
    }
    return 1;
}

static bool jpeg_fits(const struct jpeg_decompress_struct *info, size_t max_bytes)
{
    return info->output_width && info->output_width <= max_bytes / 4 &&
           info->output_height <= max_bytes / ((size_t)info->output_width * 4);
}

static bool decode_jpeg(FILE *file, DeskImage *image, size_t max_bytes, unsigned flags,
                        struct image_error *error)
{
    /* Keep the decoder state outside automatic storage across longjmp. */
    struct jpeg_decoder *decoder = calloc(1, sizeof *decoder);
    if (!decoder) {
        image_error(error->text, error->size, "JPEG decoder: %s", strerror(errno));
        return false;
    }
    struct jpeg_decompress_struct *info = &decoder->info;
    info->err = jpeg_std_error(&decoder->errors);
    decoder->errors.error_exit = jpeg_fail;
    decoder->errors.emit_message = jpeg_message;

    if (setjmp(decoder->failure)) {
        image_error(error->text, error->size, "JPEG: %s", decoder->message);
        jpeg_destroy_decompress(info);
        free(decoder);
        return false;
    }

    jpeg_create_decompress(info);
    jpeg_stdio_src(info, file);
    jpeg_save_markers(info, JPEG_APP0 + 1, 0xffff);
    jpeg_read_header(info, TRUE);
    int orientation = 1;
    for (jpeg_saved_marker_ptr marker = info->marker_list; marker; marker = marker->next) {
        if (marker->marker == JPEG_APP0 + 1 && marker->data_length > 6 &&
            memcmp(marker->data, "Exif\0\0", 6) == 0) {
            orientation = exif_orientation(marker->data + 6, marker->data_length - 6);
            break;
        }
    }
    bool cmyk = info->jpeg_color_space == JCS_CMYK || info->jpeg_color_space == JCS_YCCK;
    info->out_color_space = cmyk ? JCS_CMYK : JCS_EXT_RGBA;
    jpeg_calc_output_dimensions(info);
    if (flags & DESK_IMAGE_SHRINK) {
        while (!jpeg_fits(info, max_bytes) && info->scale_denom < 8) {
            info->scale_num = 1;
            info->scale_denom *= 2;
            jpeg_calc_output_dimensions(info);
        }
    }

    if (!image_allocate(image, info->output_width, info->output_height, max_bytes)) {
        image_error(error->text, error->size, "JPEG pixel storage: %s", strerror(errno));
        jpeg_destroy_decompress(info);
        free(decoder);
        return false;
    }

    jpeg_start_decompress(info);
    while (info->output_scanline < info->output_height) {
        JSAMPROW row = image->pixels + (size_t)info->output_scanline * image->width * 4;
        jpeg_read_scanlines(info, &row, 1);
        if (cmyk) {
            for (uint32_t x = 0; x < image->width; ++x) {
                unsigned char *pixel = row + (size_t)x * 4;
                unsigned black = info->saw_Adobe_marker ? pixel[3] : 255 - pixel[3];
                for (size_t channel = 0; channel < 3; ++channel) {
                    unsigned ink = info->saw_Adobe_marker ? pixel[channel] : 255 - pixel[channel];
                    pixel[channel] = (unsigned char)((ink * black + 127) / 255);
                }
                pixel[3] = 255;
            }
        }
    }
    jpeg_finish_decompress(info);
    jpeg_destroy_decompress(info);
    free(decoder);
    image->orientation = orientation;
    return true;
}

#if defined DESK_IMAGE_GIF || defined DESK_IMAGE_WEBP
/* The whole file, for decoders that work on memory. */
static unsigned char *read_whole(FILE *file, size_t limit, size_t *size, struct image_error *error)
{
    size_t length = 0, capacity = 1 << 16;
    unsigned char *data = malloc(capacity);
    for (;;) {
        if (!data) {
            image_error(error->text, error->size, "%s", strerror(ENOMEM));
            return NULL;
        }
        length += fread(data + length, 1, capacity - length, file);
        if (ferror(file)) {
            image_error(error->text, error->size, "%s", strerror(errno));
            free(data);
            return NULL;
        }
        if (length < capacity)
            break;
        if (capacity >= limit) {
            image_error(error->text, error->size, "%s", strerror(EFBIG));
            free(data);
            return NULL;
        }
        capacity *= 2;
        unsigned char *grown = realloc(data, capacity);
        if (!grown)
            free(data);
        data = grown;
    }
    *size = length;
    return data;
}

static void premultiply(DeskImage *image)
{
    size_t bytes = (size_t)image->width * image->height * 4;
    for (size_t i = 0; i < bytes; i += 4) {
        unsigned alpha = image->pixels[i + 3];
        for (size_t channel = 0; channel < 3; ++channel)
            image->pixels[i + channel] = (unsigned char)((image->pixels[i + channel] * alpha + 127) / 255);
    }
}
#endif

#ifdef DESK_IMAGE_GIF
/* GIF, first frame only. The canvas is the logical screen, transparent where
   the frame doesn't cover it; the background color is ignored, as browsers do. */

struct gif_reader {
    const unsigned char *data;
    size_t size, at;
};

static int gif_byte(struct gif_reader *reader)
{
    return reader->at < reader->size ? reader->data[reader->at++] : -1;
}

static int gif_word(struct gif_reader *reader)
{
    int low = gif_byte(reader), high = gif_byte(reader);
    return low < 0 || high < 0 ? -1 : low | high << 8;
}

/* Skip data sub-blocks up to and including the terminator. */
static bool gif_skip_blocks(struct gif_reader *reader)
{
    for (;;) {
        int length = gif_byte(reader);
        if (length < 0)
            return false;
        if (length == 0)
            return true;
        reader->at += (size_t)length;
    }
}

/* Reads LZW codes across the image's data sub-blocks. */
struct gif_bits {
    struct gif_reader *reader;
    int block_left;
    uint32_t buffer;
    int count;
    bool ended;
};

static int gif_code(struct gif_bits *bits, int size)
{
    while (bits->count < size) {
        if (bits->ended)
            return -1;
        if (bits->block_left == 0) {
            int length = gif_byte(bits->reader);
            if (length <= 0) {
                bits->ended = true;
                return -1;
            }
            bits->block_left = length;
        }
        int byte = gif_byte(bits->reader);
        if (byte < 0) {
            bits->ended = true;
            return -1;
        }
        --bits->block_left;
        bits->buffer |= (uint32_t)byte << bits->count;
        bits->count += 8;
    }
    int code = (int)(bits->buffer & ((1u << size) - 1));
    bits->buffer >>= size;
    bits->count -= size;
    return code;
}

/* The row of the n-th decoded row, for interlaced frames. */
static uint32_t gif_row(uint32_t n, uint32_t height, bool interlaced)
{
    if (!interlaced)
        return n;
    static const uint32_t start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
    for (int pass = 0; pass < 4; ++pass) {
        uint32_t rows = start[pass] < height ? (height - start[pass] + step[pass] - 1) / step[pass] : 0;
        if (n < rows)
            return start[pass] + n * step[pass];
        n -= rows;
    }
    return height;
}

static bool decode_gif(FILE *file, DeskImage *image, size_t max_bytes, struct image_error *error)
{
    size_t size;
    unsigned char *data = read_whole(file, max_bytes, &size, error);
    if (!data)
        return false;
    struct gif_reader reader = { data, size, 6 };
    const char *problem = NULL;
    uint16_t *prefix = NULL;
    unsigned char *suffix = NULL, *first = NULL, *stack = NULL;

    int screen_width = gif_word(&reader), screen_height = gif_word(&reader);
    int flags = gif_byte(&reader);
    reader.at += 2; /* Background color and aspect ratio. */
    if (screen_width <= 0 || screen_height <= 0 || flags < 0) {
        problem = "bad header";
        goto done;
    }
    const unsigned char *global = NULL;
    int global_count = 0;
    if (flags & 0x80) {
        global_count = 2 << (flags & 7);
        global = data + reader.at;
        reader.at += (size_t)global_count * 3;
    }
    if (!image_allocate(image, (uint32_t)screen_width, (uint32_t)screen_height, max_bytes)) {
        image_error(error->text, error->size, "GIF: %s", strerror(errno));
        free(data);
        return false;
    }
    memset(image->pixels, 0, (size_t)image->width * image->height * 4);

    int transparent = -1;
    for (;;) {
        int block = gif_byte(&reader);
        if (block == 0x21) {
            int label = gif_byte(&reader);
            if (label == 0xf9) {
                /* Graphic control: only the transparent index matters for one frame. */
                size_t start = reader.at;
                int length = gif_byte(&reader);
                if (length >= 4 && start + 5 < size) {
                    if (data[start + 1] & 1)
                        transparent = data[start + 4];
                }
                reader.at = start;
            }
            if (label < 0 || !gif_skip_blocks(&reader)) {
                problem = "truncated";
                goto done;
            }
        } else if (block == 0x2c) {
            break;
        } else {
            problem = block == 0x3b ? "no image" : block < 0 ? "truncated" : "bad block";
            goto done;
        }
    }

    int left = gif_word(&reader), top = gif_word(&reader);
    int width = gif_word(&reader), height = gif_word(&reader);
    int frame_flags = gif_byte(&reader);
    if (left < 0 || top < 0 || width < 0 || height < 0 || frame_flags < 0) {
        problem = "truncated";
        goto done;
    }
    const unsigned char *colors = global;
    int color_count = global_count;
    if (frame_flags & 0x80) {
        color_count = 2 << (frame_flags & 7);
        colors = data + reader.at;
        reader.at += (size_t)color_count * 3;
    }
    if (reader.at > size) {
        problem = "truncated";
        goto done;
    }
    if (!colors) {
        problem = "no color table";
        goto done;
    }
    bool interlaced = frame_flags & 0x40;
    int minimum = gif_byte(&reader);
    if (minimum < 1 || minimum > 11) {
        problem = "bad LZW code size";
        goto done;
    }

    enum { CODES = 4096 };
    prefix = malloc(CODES * sizeof *prefix);
    suffix = malloc(CODES);
    first = malloc(CODES);
    stack = malloc(CODES + 2); /* A string, plus the KwKwK case's extra byte. */
    if (!prefix || !suffix || !first || !stack) {
        problem = "out of memory";
        goto done;
    }
    int clear = 1 << minimum, end = clear + 1;
    for (int i = 0; i < clear; ++i) {
        suffix[i] = (unsigned char)i;
        first[i] = (unsigned char)i;
    }
    int next = clear + 2, code_size = minimum + 1, previous = -1;
    struct gif_bits bits = { .reader = &reader };
    size_t total = (size_t)width * height, written = 0;

    while (written < total) {
        int code = gif_code(&bits, code_size);
        if (code < 0)
            break;
        if (code == clear) {
            next = clear + 2;
            code_size = minimum + 1;
            previous = -1;
            continue;
        }
        if (code == end)
            break;

        int depth = 0, walk;
        if (previous < 0) {
            if (code >= clear) {
                problem = "bad LZW code";
                goto done;
            }
            stack[depth++] = (unsigned char)code;
        } else {
            if (code > next || (code == next && next >= CODES)) {
                problem = "bad LZW code";
                goto done;
            }
            /* code == next: the previous string plus its own first byte. */
            walk = code == next ? previous : code;
            if (code == next)
                stack[depth++] = first[previous];
            while (walk >= clear) {
                if (depth >= CODES) {
                    problem = "bad LZW code";
                    goto done;
                }
                stack[depth++] = suffix[walk];
                walk = prefix[walk];
            }
            stack[depth++] = (unsigned char)walk;
            if (next < CODES) {
                prefix[next] = (uint16_t)previous;
                suffix[next] = (unsigned char)walk;
                first[next] = first[previous];
                ++next;
                if (next == 1 << code_size && code_size < 12)
                    ++code_size;
            }
        }
        previous = code;

        /* The stack holds the string backwards. */
        while (depth > 0 && written < total) {
            int index = stack[--depth];
            uint32_t x = (uint32_t)(written % (size_t)width);
            uint32_t y = gif_row((uint32_t)(written / (size_t)width), (uint32_t)height, interlaced);
            ++written;
            uint32_t canvas_x = (uint32_t)left + x, canvas_y = (uint32_t)top + y;
            if (canvas_x >= image->width || canvas_y >= image->height || index == transparent)
                continue;
            unsigned char *pixel = image->pixels + ((size_t)canvas_y * image->width + canvas_x) * 4;
            if (index < color_count) {
                memcpy(pixel, colors + index * 3, 3);
            } else {
                pixel[0] = pixel[1] = pixel[2] = 0;
            }
            pixel[3] = 255;
        }
    }
    if (written < total)
        problem = "truncated";

done:
    free(prefix);
    free(suffix);
    free(first);
    free(stack);
    free(data);
    if (problem) {
        image_error(error->text, error->size, "GIF: %s", problem);
        return false;
    }
    image->orientation = 1;
    return true;
}
#endif

#ifdef DESK_IMAGE_WEBP
static bool decode_webp(FILE *file, DeskImage *image, size_t max_bytes, struct image_error *error)
{
    size_t size;
    unsigned char *data = read_whole(file, max_bytes, &size, error);
    if (!data)
        return false;
    WebPBitstreamFeatures features;
    bool ok = false;
    if (WebPGetFeatures(data, size, &features) != VP8_STATUS_OK) {
        image_error(error->text, error->size, "WebP: not a valid file");
    } else if (features.has_animation) {
        image_error(error->text, error->size, "WebP: animated images aren't supported");
    } else if (!image_allocate(image, (uint32_t)features.width, (uint32_t)features.height, max_bytes)) {
        image_error(error->text, error->size, "WebP: %s", strerror(errno));
    } else {
        size_t stride = (size_t)image->width * 4;
        ok = WebPDecodeRGBAInto(data, size, image->pixels, stride * image->height, (int)stride) != NULL;
        if (ok)
            premultiply(image);
        else
            image_error(error->text, error->size, "WebP: decoding failed");
    }
    free(data);
    image->orientation = 1;
    return ok;
}
#endif

/* A format recognised by its signature but not decoded here, or NULL. */
static const char *unsupported_format(const unsigned char *s, size_t length)
{
#ifndef DESK_IMAGE_GIF
    if (length >= 6 && (memcmp(s, "GIF87a", 6) == 0 || memcmp(s, "GIF89a", 6) == 0))
        return "GIF";
#endif
#ifndef DESK_IMAGE_WEBP
    if (length >= 12 && memcmp(s, "RIFF", 4) == 0 && memcmp(s + 8, "WEBP", 4) == 0)
        return "WebP";
#endif
    if (length >= 12 && memcmp(s + 4, "ftyp", 4) == 0) {
        if (memcmp(s + 8, "avif", 4) == 0 || memcmp(s + 8, "avis", 4) == 0)
            return "AVIF";
        if (memcmp(s + 8, "heic", 4) == 0 || memcmp(s + 8, "heix", 4) == 0 || memcmp(s + 8, "mif1", 4) == 0 ||
            memcmp(s + 8, "msf1", 4) == 0)
            return "HEIC";
    }
    if ((length >= 2 && s[0] == 0xff && s[1] == 0x0a) ||
        (length >= 12 && memcmp(s, "\0\0\0\x0cJXL \r\n\x87\n", 12) == 0))
        return "JPEG XL";
    if (length >= 2 && memcmp(s, "BM", 2) == 0)
        return "BMP";
    if (length >= 4 && (memcmp(s, "II*\0", 4) == 0 || memcmp(s, "MM\0*", 4) == 0))
        return "TIFF";
    if (length >= 4 && memcmp(s, "qoif", 4) == 0)
        return "QOI";
    if (length >= 4 && memcmp(s, "8BPS", 4) == 0)
        return "Photoshop";
    if (length >= 1 && s[0] == '<')
        return "SVG";
    return NULL;
}

#if defined DESK_IMAGE_GIF && defined DESK_IMAGE_WEBP
#define DESK_IMAGE_FORMATS "PNG, JPEG, GIF or WebP"
#elif defined DESK_IMAGE_GIF
#define DESK_IMAGE_FORMATS "PNG, JPEG or GIF"
#elif defined DESK_IMAGE_WEBP
#define DESK_IMAGE_FORMATS "PNG, JPEG or WebP"
#else
#define DESK_IMAGE_FORMATS "PNG or JPEG"
#endif

bool desk_image_load(FILE *file, DeskImage *image, size_t max_bytes, unsigned flags,
                     char *error, size_t error_size)
{
    struct image_error failure = { error, error_size };
    unsigned char signature[16];
    *image = (DeskImage){ .orientation = 1 };
    clearerr(file);
    if (fseek(file, 0, SEEK_SET) != 0) {
        image_error(error, error_size, "%s", strerror(errno));
        return false;
    }
    size_t length = fread(signature, 1, sizeof signature, file);
    if (ferror(file) || fseek(file, 0, SEEK_SET) != 0) {
        image_error(error, error_size, "%s", strerror(errno));
        return false;
    }

    bool ok;
    const char *unsupported;
    if (length >= 8 && png_sig_cmp(signature, 0, 8) == 0) {
        ok = decode_png(file, image, max_bytes, &failure);
    } else if (length >= 2 && signature[0] == 0xff && signature[1] == 0xd8) {
        ok = decode_jpeg(file, image, max_bytes, flags, &failure);
#ifdef DESK_IMAGE_GIF
    } else if (length >= 6 && (memcmp(signature, "GIF87a", 6) == 0 || memcmp(signature, "GIF89a", 6) == 0)) {
        ok = decode_gif(file, image, max_bytes, &failure);
#endif
#ifdef DESK_IMAGE_WEBP
    } else if (length >= 12 && memcmp(signature, "RIFF", 4) == 0 && memcmp(signature + 8, "WEBP", 4) == 0) {
        ok = decode_webp(file, image, max_bytes, &failure);
#endif
    } else if ((unsupported = unsupported_format(signature, length))) {
        image_error(error, error_size, "%s images aren't supported", unsupported);
        return false;
    } else {
        image_error(error, error_size, "expected a " DESK_IMAGE_FORMATS " image");
        return false;
    }
    if (!ok) {
        free(image->pixels);
        *image = (DeskImage){ .orientation = 1 };
    }
    return ok;
}
#endif
