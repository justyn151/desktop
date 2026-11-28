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

#if defined DESK_FONT || defined DESK_WINDOW
#include <math.h>
#endif

#ifdef DESK_WINDOW
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#endif

#ifdef DESK_FONT
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

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

/* UTF-8 */

uint32_t desk_utf8_next(const char **text, const char *end)
{
    const unsigned char *s = (const unsigned char *)*text;
    size_t left = (size_t)(end - *text);
    unsigned char first = s[0];
    int length;
    uint32_t code, minimum;
    if (first < 0x80) {
        *text += 1;
        return first;
    } else if (first >= 0xc2 && first <= 0xdf) {
        length = 2, code = first & 0x1f, minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        length = 3, code = first & 0x0f, minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        length = 4, code = first & 0x07, minimum = 0x10000;
    } else {
        *text += 1;
        return 0xfffd;
    }
    if (left < (size_t)length) {
        *text += 1;
        return 0xfffd;
    }
    for (int i = 1; i < length; ++i) {
        if ((s[i] & 0xc0) != 0x80) {
            *text += 1;
            return 0xfffd;
        }
        code = code << 6 | (s[i] & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
        *text += 1;
        return 0xfffd;
    }
    *text += length;
    return code;
}

/* Text */

bool desk_text_set(DeskText *text, const char *value)
{
    size_t length = strlen(value);
    if (length + 1 > text->capacity) {
        char *grown = realloc(text->text, length + 1);
        if (!grown)
            return false;
        text->text = grown;
        text->capacity = length + 1;
    }
    memcpy(text->text, value, length + 1);
    text->length = length;
    text->cursor = text->anchor = length;
    return true;
}

void desk_text_free(DeskText *text)
{
    free(text->text);
    *text = (DeskText){0};
}

bool desk_text_selection(const DeskText *text, size_t *start, size_t *end)
{
    *start = text->cursor < text->anchor ? text->cursor : text->anchor;
    *end = text->cursor < text->anchor ? text->anchor : text->cursor;
    return *start != *end;
}

void desk_text_select_all(DeskText *text)
{
    text->anchor = 0;
    text->cursor = text->length;
}

static bool text_continuation(const DeskText *text, size_t at)
{
    return at < text->length && ((unsigned char)text->text[at] & 0xc0) == 0x80;
}

static size_t text_step(const DeskText *text, size_t at, int direction, bool word)
{
    const char *s = text->text;
    if (direction < 0) {
        if (word) {
            while (at > 0 && !isalnum((unsigned char)s[at - 1]) && (unsigned char)s[at - 1] < 0x80)
                --at;
            while (at > 0 && (isalnum((unsigned char)s[at - 1]) || (unsigned char)s[at - 1] >= 0x80))
                --at;
            return at;
        }
        while (at > 0 && text_continuation(text, --at))
            ;
        return at;
    }
    if (word) {
        while (at < text->length && !isalnum((unsigned char)s[at]) && (unsigned char)s[at] < 0x80)
            ++at;
        while (at < text->length && (isalnum((unsigned char)s[at]) || (unsigned char)s[at] >= 0x80))
            ++at;
        return at;
    }
    if (at < text->length)
        ++at;
    while (text_continuation(text, at))
        ++at;
    return at;
}

static void text_remove(DeskText *text, size_t start, size_t end)
{
    memmove(text->text + start, text->text + end, text->length - end + 1);
    text->length -= end - start;
    text->cursor = text->anchor = start;
}

bool desk_text_insert(DeskText *text, const char *insert, size_t length)
{
    if (!text->text && !desk_text_set(text, ""))
        return false;
    size_t start, end;
    if (desk_text_selection(text, &start, &end))
        text_remove(text, start, end);
    if (text->length + length + 1 > text->capacity) {
        size_t capacity = (text->length + length + 1) * 2;
        char *grown = realloc(text->text, capacity);
        if (!grown)
            return false;
        text->text = grown;
        text->capacity = capacity;
    }
    char *at = text->text + text->cursor;
    memmove(at + length, at, text->length - text->cursor + 1);
    memcpy(at, insert, length);
    text->length += length;
    text->cursor = text->anchor = text->cursor + length;
    return true;
}

bool desk_text_delete(DeskText *text, int direction, bool word)
{
    if (!text->text)
        return false;
    size_t start, end;
    if (!desk_text_selection(text, &start, &end)) {
        size_t other = text_step(text, text->cursor, direction, word);
        start = other < text->cursor ? other : text->cursor;
        end = other < text->cursor ? text->cursor : other;
    }
    if (start == end)
        return false;
    text_remove(text, start, end);
    return true;
}

void desk_text_move(DeskText *text, int direction, bool word, bool extend)
{
    size_t start, end;
    if (!extend && desk_text_selection(text, &start, &end)) {
        text->cursor = text->anchor = direction < 0 ? start : end;
        return;
    }
    text->cursor = text_step(text, text->cursor, direction, word);
    if (!extend)
        text->anchor = text->cursor;
}

void desk_text_move_to(DeskText *text, size_t offset, bool extend)
{
    if (offset > text->length)
        offset = text->length;
    while (text_continuation(text, offset))
        --offset;
    text->cursor = offset;
    if (!extend)
        text->anchor = offset;
}

/* Canvas */

DeskCanvas desk_canvas(uint32_t *pixels, int width, int height)
{
    return (DeskCanvas){
        .pixels = pixels, .width = width, .height = height, .stride = width,
        .clip_right = width, .clip_bottom = height,
    };
}

void desk_canvas_clip(DeskCanvas *canvas, int left, int top, int right, int bottom)
{
    canvas->clip_left = left < 0 ? 0 : left;
    canvas->clip_top = top < 0 ? 0 : top;
    canvas->clip_right = right > canvas->width ? canvas->width : right;
    canvas->clip_bottom = bottom > canvas->height ? canvas->height : bottom;
}

/* Blend premultiplied src over dst with coverage 0–255. */
static inline uint32_t blend_over(uint32_t dst, uint32_t color, unsigned coverage)
{
    unsigned alpha = ((color >> 24) * coverage + 127) / 255;
    if (alpha == 0)
        return dst;
    if (alpha == 255)
        return color | 0xff000000u;
    unsigned inverse = 255 - alpha;
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        unsigned source = shift == 24 ? 255 : (color >> shift) & 0xff;
        unsigned value = (source * alpha + ((dst >> shift) & 0xff) * inverse + 127) / 255;
        out |= (uint32_t)value << shift;
    }
    return out;
}

void desk_canvas_fill(DeskCanvas *canvas, int x, int y, int width, int height, uint32_t argb)
{
    int left = x > canvas->clip_left ? x : canvas->clip_left;
    int top = y > canvas->clip_top ? y : canvas->clip_top;
    int right = x + width < canvas->clip_right ? x + width : canvas->clip_right;
    int bottom = y + height < canvas->clip_bottom ? y + height : canvas->clip_bottom;
    if (left >= right || top >= bottom)
        return;
    bool opaque = argb >> 24 == 0xff;
    for (int row = top; row < bottom; ++row) {
        uint32_t *line = canvas->pixels + (size_t)row * (size_t)canvas->stride;
        for (int column = left; column < right; ++column)
            line[column] = opaque ? argb : blend_over(line[column], argb, 255);
    }
}

void desk_canvas_blit(DeskCanvas *canvas, int x, int y, const uint32_t *pixels, int width, int height)
{
    int left = x > canvas->clip_left ? x : canvas->clip_left;
    int top = y > canvas->clip_top ? y : canvas->clip_top;
    int right = x + width < canvas->clip_right ? x + width : canvas->clip_right;
    int bottom = y + height < canvas->clip_bottom ? y + height : canvas->clip_bottom;
    for (int row = top; row < bottom; ++row) {
        uint32_t *line = canvas->pixels + (size_t)row * (size_t)canvas->stride;
        const uint32_t *source = pixels + (size_t)(row - y) * (size_t)width - x;
        for (int column = left; column < right; ++column) {
            uint32_t pixel = source[column];
            unsigned alpha = pixel >> 24;
            if (alpha == 255) {
                line[column] = pixel;
            } else if (alpha) {
                uint32_t dst = line[column], out = 0;
                for (int shift = 0; shift < 32; shift += 8) {
                    unsigned value = ((pixel >> shift) & 0xff) + (((dst >> shift) & 0xff) * (255 - alpha) + 127) / 255;
                    out |= (uint32_t)(value > 255 ? 255 : value) << shift;
                }
                line[column] = out;
            }
        }
    }
}

#ifdef DESK_KEYBOARD
/* Keyboard */

int desk_keyboard_open(DeskKeyboard *keyboard)
{
    *keyboard = (DeskKeyboard){0};
    keyboard->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    return keyboard->context ? 0 : ENOMEM;
}

int desk_keyboard_keymap(DeskKeyboard *keyboard, uint32_t format, int fd, uint32_t size)
{
    if (format != 1 /* WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 */ || size == 0 || !keyboard->context) {
        close(fd);
        return EINVAL;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    int error = errno;
    close(fd);
    if (map == MAP_FAILED)
        return error;
    struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(keyboard->context, map, strnlen(map, size),
                                                           XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!keymap)
        return EINVAL;
    struct xkb_state *state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        return ENOMEM;
    }
    xkb_state_unref(keyboard->state);
    xkb_keymap_unref(keyboard->keymap);
    keyboard->keymap = keymap;
    keyboard->state = state;
    return 0;
}

void desk_keyboard_modifiers(DeskKeyboard *keyboard, uint32_t depressed, uint32_t latched, uint32_t locked,
                             uint32_t group)
{
    if (keyboard->state)
        xkb_state_update_mask(keyboard->state, depressed, latched, locked, 0, 0, group);
}

xkb_keysym_t desk_keyboard_sym(const DeskKeyboard *keyboard, uint32_t key)
{
    return keyboard->state ? xkb_state_key_get_one_sym(keyboard->state, key + 8) : XKB_KEY_NoSymbol;
}

unsigned desk_keyboard_mods(const DeskKeyboard *keyboard)
{
    static const struct { const char *name; unsigned flag; } names[] = {
        { XKB_MOD_NAME_SHIFT, DESK_MOD_SHIFT },
        { XKB_MOD_NAME_CTRL, DESK_MOD_CTRL },
        { XKB_MOD_NAME_ALT, DESK_MOD_ALT },
        { XKB_MOD_NAME_LOGO, DESK_MOD_LOGO },
    };
    unsigned mods = 0;
    if (!keyboard->state)
        return 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (xkb_state_mod_name_is_active(keyboard->state, names[i].name, XKB_STATE_MODS_EFFECTIVE) > 0)
            mods |= names[i].flag;
    }
    return mods;
}

bool desk_keyboard_repeats(const DeskKeyboard *keyboard, uint32_t key)
{
    return keyboard->keymap && xkb_keymap_key_repeats(keyboard->keymap, key + 8);
}

void desk_keyboard_close(DeskKeyboard *keyboard)
{
    xkb_state_unref(keyboard->state);
    xkb_keymap_unref(keyboard->keymap);
    xkb_context_unref(keyboard->context);
    *keyboard = (DeskKeyboard){0};
}

int desk_text_key(DeskText *text, uint32_t sym, unsigned mods, const char *utf8)
{
    bool word = mods & DESK_MOD_CTRL, extend = mods & DESK_MOD_SHIFT;
    switch (sym) {
    case XKB_KEY_Left:
    case XKB_KEY_KP_Left:
        desk_text_move(text, -1, word, extend);
        return DESK_TEXT_MOVED;
    case XKB_KEY_Right:
    case XKB_KEY_KP_Right:
        desk_text_move(text, 1, word, extend);
        return DESK_TEXT_MOVED;
    case XKB_KEY_Home:
    case XKB_KEY_KP_Home:
        desk_text_move_to(text, 0, extend);
        return DESK_TEXT_MOVED;
    case XKB_KEY_End:
    case XKB_KEY_KP_End:
        desk_text_move_to(text, text->length, extend);
        return DESK_TEXT_MOVED;
    case XKB_KEY_BackSpace:
        return desk_text_delete(text, -1, word) ? DESK_TEXT_CHANGED : DESK_TEXT_MOVED;
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        return desk_text_delete(text, 1, word) ? DESK_TEXT_CHANGED : DESK_TEXT_MOVED;
    default:
        break;
    }
    if ((mods & DESK_MOD_CTRL) && (sym == XKB_KEY_a || sym == XKB_KEY_A)) {
        desk_text_select_all(text);
        return DESK_TEXT_MOVED;
    }
    if (mods & (DESK_MOD_CTRL | DESK_MOD_ALT | DESK_MOD_LOGO))
        return DESK_TEXT_IGNORED;
    if (!utf8 || !utf8[0] || (unsigned char)utf8[0] < 0x20 || utf8[0] == 0x7f)
        return DESK_TEXT_IGNORED;
    return desk_text_insert(text, utf8, strlen(utf8)) ? DESK_TEXT_CHANGED : DESK_TEXT_IGNORED;
}
#endif

#ifdef DESK_FONT
/* Fonts */

struct glyph {
    uint32_t code; /* The key; used is separate because U+0000 is valid. */
    bool used;
    int face;      /* Index in the font set, or -1 for nothing drawn. */
    int advance;   /* 26.6 pixels. */
    int left, top; /* Bitmap offset from the pen and baseline. */
    int width, height;
    unsigned char *coverage; /* width × height, or NULL when empty. */
};

struct DeskFont {
    FT_Library library;
    FcFontSet *set;          /* Faces in preference order. */
    FcCharSet **charsets;    /* Borrowed from set, NULL when absent. */
    FT_Face *faces;          /* Loaded on first use. */
    bool *failed;            /* Couldn't load, or not scalable: skip it. */
    int face_count;
    double size;
    DeskFontMetrics metrics;
    struct glyph *glyphs;    /* Open addressing, power-of-two capacity. */
    size_t glyph_count, glyph_capacity;
};

static void font_error(char *error, size_t size, const char *message, const char *pattern)
{
    if (error && size)
        snprintf(error, size, message, pattern);
}

static FT_Face font_face(DeskFont *font, int index)
{
    if (font->faces[index] || font->failed[index])
        return font->faces[index];
    FcPattern *pattern = font->set->fonts[index];
    FcChar8 *file;
    int face_index = 0;
    FcBool scalable = FcTrue;
    FcPatternGetBool(pattern, FC_SCALABLE, 0, &scalable);
    FcPatternGetInteger(pattern, FC_INDEX, 0, &face_index);
    FT_Face face = NULL;
    if (!scalable || FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch ||
        FT_New_Face(font->library, (const char *)file, face_index, &face) ||
        FT_Set_Char_Size(face, 0, (FT_F26Dot6)lround(font->size * 64), 0, 0)) {
        if (face)
            FT_Done_Face(face);
        font->failed[index] = true;
        return NULL;
    }
    font->faces[index] = face;
    return face;
}

static void font_measure(DeskFont *font)
{
    FT_Face face = font->faces[0];
    FT_Size_Metrics *size = &face->size->metrics;
    int ascent = (int)((size->ascender + 63) >> 6);
    int descent = (int)((-size->descender + 63) >> 6);
    int height = (int)((size->height + 63) >> 6);
    font->metrics = (DeskFontMetrics){ascent, descent, height > ascent + descent ? height : ascent + descent};
}

DeskFont *desk_font_open(const char *pattern_text, double pixel_size, char *error, size_t error_size)
{
    if (!(pixel_size > 0) || pixel_size > 4096) {
        font_error(error, error_size, "bad font size for %s", pattern_text);
        return NULL;
    }
    DeskFont *font = calloc(1, sizeof *font);
    if (!font) {
        font_error(error, error_size, "out of memory loading %s", pattern_text);
        return NULL;
    }
    font->size = pixel_size;
    if (FT_Init_FreeType(&font->library)) {
        font_error(error, error_size, "can't start FreeType for %s", pattern_text);
        free(font);
        return NULL;
    }
    FcPattern *pattern = FcNameParse((const FcChar8 *)pattern_text);
    FcResult result;
    if (!FcInit() || !pattern || !FcConfigSubstitute(NULL, pattern, FcMatchPattern)) {
        font_error(error, error_size, "can't match the font %s", pattern_text);
        if (pattern)
            FcPatternDestroy(pattern);
        desk_font_close(font);
        return NULL;
    }
    FcDefaultSubstitute(pattern);
    font->set = FcFontSort(NULL, pattern, FcTrue, NULL, &result);
    FcPatternDestroy(pattern);
    if (!font->set || font->set->nfont == 0) {
        font_error(error, error_size, "no font matches %s", pattern_text);
        desk_font_close(font);
        return NULL;
    }
    font->face_count = font->set->nfont;
    font->charsets = calloc((size_t)font->face_count, sizeof *font->charsets);
    font->faces = calloc((size_t)font->face_count, sizeof *font->faces);
    font->failed = calloc((size_t)font->face_count, sizeof *font->failed);
    if (!font->charsets || !font->faces || !font->failed) {
        font_error(error, error_size, "out of memory loading %s", pattern_text);
        desk_font_close(font);
        return NULL;
    }
    for (int i = 0; i < font->face_count; ++i) {
        if (FcPatternGetCharSet(font->set->fonts[i], FC_CHARSET, 0, &font->charsets[i]) != FcResultMatch)
            font->charsets[i] = NULL;
    }
    /* The first face that loads sets the metrics; usually the first one. */
    int first = -1;
    for (int i = 0; i < font->face_count && first < 0; ++i) {
        if (font_face(font, i))
            first = i;
    }
    if (first < 0) {
        font_error(error, error_size, "can't load a face for %s", pattern_text);
        desk_font_close(font);
        return NULL;
    }
    if (first > 0) {
        /* Move it to the front so fallback order starts with it. */
        FT_Face face = font->faces[first];
        FcCharSet *charset = font->charsets[first];
        FcPattern *swap = font->set->fonts[first];
        font->faces[first] = font->faces[0], font->faces[0] = face;
        font->charsets[first] = font->charsets[0], font->charsets[0] = charset;
        font->set->fonts[first] = font->set->fonts[0], font->set->fonts[0] = swap;
        font->failed[first] = font->failed[0], font->failed[0] = false;
    }
    font_measure(font);
    return font;
}

static void glyphs_clear(DeskFont *font)
{
    for (size_t i = 0; i < font->glyph_capacity; ++i)
        free(font->glyphs[i].coverage);
    free(font->glyphs);
    font->glyphs = NULL;
    font->glyph_count = font->glyph_capacity = 0;
}

int desk_font_resize(DeskFont *font, double pixel_size)
{
    if (!(pixel_size > 0) || pixel_size > 4096)
        return EINVAL;
    if (pixel_size == font->size)
        return 0;
    font->size = pixel_size;
    glyphs_clear(font);
    for (int i = 0; i < font->face_count; ++i) {
        if (font->faces[i] && FT_Set_Char_Size(font->faces[i], 0, (FT_F26Dot6)lround(pixel_size * 64), 0, 0)) {
            if (i == 0)
                return EINVAL;
            FT_Done_Face(font->faces[i]);
            font->faces[i] = NULL;
            font->failed[i] = true;
        }
    }
    font_measure(font);
    return 0;
}

double desk_font_size(const DeskFont *font)
{
    return font->size;
}

DeskFontMetrics desk_font_metrics(const DeskFont *font)
{
    return font->metrics;
}

static size_t glyph_hash(uint32_t code)
{
    uint32_t h = code * 2654435761u;
    return h ^ h >> 16;
}

static bool glyphs_grow(DeskFont *font)
{
    size_t capacity = font->glyph_capacity ? font->glyph_capacity * 2 : 256;
    struct glyph *glyphs = calloc(capacity, sizeof *glyphs);
    if (!glyphs)
        return false;
    for (size_t i = 0; i < font->glyph_capacity; ++i) {
        struct glyph *old = &font->glyphs[i];
        if (!old->used)
            continue;
        size_t slot = glyph_hash(old->code) & (capacity - 1);
        while (glyphs[slot].used)
            slot = (slot + 1) & (capacity - 1);
        glyphs[slot] = *old;
    }
    free(font->glyphs);
    font->glyphs = glyphs;
    font->glyph_capacity = capacity;
    return true;
}

/* Render code into glyph from the first face that has it. */
static void glyph_render(DeskFont *font, uint32_t code, struct glyph *glyph)
{
    *glyph = (struct glyph){.code = code, .used = true, .face = -1};
    FT_Face face = NULL;
    FT_UInt index = 0;
    for (int i = 0; i < font->face_count && !index; ++i) {
        if (font->failed[i] || (font->charsets[i] && !FcCharSetHasChar(font->charsets[i], code)))
            continue;
        face = font_face(font, i);
        if (face && (index = FT_Get_Char_Index(face, code)) != 0)
            glyph->face = i;
    }
    if (!index) {
        /* Nobody has it: the first face's missing-glyph box. */
        face = font->faces[0];
        glyph->face = 0;
    }
    if (FT_Load_Glyph(face, index, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) ||
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LIGHT))
        return;
    FT_GlyphSlot slot = face->glyph;
    glyph->advance = (int)slot->advance.x;
    const FT_Bitmap *bitmap = &slot->bitmap;
    if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY || !bitmap->width || !bitmap->rows)
        return;
    glyph->coverage = malloc((size_t)bitmap->width * bitmap->rows);
    if (!glyph->coverage)
        return;
    for (unsigned row = 0; row < bitmap->rows; ++row)
        memcpy(glyph->coverage + (size_t)row * bitmap->width, bitmap->buffer + (ptrdiff_t)row * bitmap->pitch,
               bitmap->width);
    glyph->width = (int)bitmap->width;
    glyph->height = (int)bitmap->rows;
    glyph->left = slot->bitmap_left;
    glyph->top = slot->bitmap_top;
}

static const struct glyph *font_glyph(DeskFont *font, uint32_t code)
{
    static const struct glyph empty = {.face = -1};
    if (font->glyph_count + 1 > font->glyph_capacity / 2 && !glyphs_grow(font))
        return &empty;
    size_t mask = font->glyph_capacity - 1, slot = glyph_hash(code) & mask;
    while (font->glyphs[slot].used) {
        if (font->glyphs[slot].code == code)
            return &font->glyphs[slot];
        slot = (slot + 1) & mask;
    }
    glyph_render(font, code, &font->glyphs[slot]);
    font->glyph_count++;
    return &font->glyphs[slot];
}

/* Control characters, such as a newline in a file name, show as U+FFFD. */
static uint32_t visible_code(uint32_t code)
{
    return code < 0x20 || code == 0x7f ? 0xfffd : code;
}

int desk_char_width(DeskFont *font, uint32_t code)
{
    return (font_glyph(font, visible_code(code))->advance + 32) >> 6;
}

void desk_char_ink(DeskFont *font, uint32_t code, int *above, int *below)
{
    const struct glyph *glyph = font_glyph(font, visible_code(code));
    *above = glyph->height ? glyph->top : 0;
    *below = glyph->height ? glyph->height - glyph->top : 0;
}

int desk_text_width(DeskFont *font, const char *text, size_t length)
{
    const char *end = text + length;
    long pen = 0;
    while (text < end)
        pen += font_glyph(font, visible_code(desk_utf8_next(&text, end)))->advance;
    return (int)((pen + 32) >> 6);
}

size_t desk_text_fit(DeskFont *font, const char *text, size_t length, int max_width)
{
    const char *start = text, *end = text + length;
    long pen = 0, limit = (long)max_width << 6;
    while (text < end) {
        const char *next = text;
        long advance = font_glyph(font, visible_code(desk_utf8_next(&next, end)))->advance;
        if (pen + advance > limit + 32)
            break;
        pen += advance;
        text = next;
    }
    return (size_t)(text - start);
}

int desk_text_draw(DeskFont *font, DeskCanvas *canvas, int x, int y, const char *text, size_t length,
                   uint32_t argb)
{
    const char *end = text + length;
    long pen = (long)x << 6;
    uint32_t color = argb & 0x00ffffffu;
    unsigned alpha = argb >> 24;
    while (text < end) {
        const struct glyph *glyph = font_glyph(font, visible_code(desk_utf8_next(&text, end)));
        int left = (int)((pen + 32) >> 6) + glyph->left, top = y - glyph->top;
        pen += glyph->advance;
        if (!glyph->coverage || left >= canvas->clip_right || top >= canvas->clip_bottom ||
            left + glyph->width <= canvas->clip_left || top + glyph->height <= canvas->clip_top)
            continue;
        int row0 = top < canvas->clip_top ? canvas->clip_top - top : 0;
        int row1 = top + glyph->height > canvas->clip_bottom ? canvas->clip_bottom - top : glyph->height;
        int col0 = left < canvas->clip_left ? canvas->clip_left - left : 0;
        int col1 = left + glyph->width > canvas->clip_right ? canvas->clip_right - left : glyph->width;
        for (int row = row0; row < row1; ++row) {
            uint32_t *line = canvas->pixels + (size_t)(top + row) * (size_t)canvas->stride + left;
            const unsigned char *coverage = glyph->coverage + (size_t)row * (size_t)glyph->width;
            for (int column = col0; column < col1; ++column) {
                unsigned c = coverage[column];
                if (c)
                    line[column] = blend_over(line[column], color | (uint32_t)alpha << 24, c);
            }
        }
    }
    return (int)((pen + 32) >> 6);
}

void desk_font_close(DeskFont *font)
{
    if (!font)
        return;
    glyphs_clear(font);
    for (int i = 0; i < font->face_count; ++i) {
        if (font->faces && font->faces[i])
            FT_Done_Face(font->faces[i]);
    }
    free(font->faces);
    free(font->failed);
    free(font->charsets);
    if (font->set)
        FcFontSetDestroy(font->set);
    if (font->library)
        FT_Done_FreeType(font->library);
    free(font);
}
#endif

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

#ifdef DESK_WINDOW
/* Window */

static uint32_t min_version(uint32_t offered, uint32_t wanted)
{
    return offered < wanted ? offered : wanted;
}

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

bool desk_globals_bind(DeskGlobals *globals, struct wl_registry *registry, uint32_t name, const char *interface,
                       uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0 && !globals->compositor) {
        /* Version 6 adds preferred_buffer_scale. */
        globals->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, min_version(version, 6));
    } else if (strcmp(interface, wl_shm_interface.name) == 0 && !globals->shm) {
        globals->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0 && !globals->wm_base) {
        /* Version 4 adds configure_bounds and 5 wm_capabilities. */
        globals->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, min_version(version, 5));
        xdg_wm_base_add_listener(globals->wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0 && !globals->viewporter) {
        globals->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0 &&
               !globals->fractional_manager) {
        globals->fractional_manager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else {
        return false;
    }
    return true;
}

const char *desk_globals_missing(const DeskGlobals *globals)
{
    return !globals->compositor ? wl_compositor_interface.name
         : !globals->shm        ? wl_shm_interface.name
         : !globals->wm_base    ? xdg_wm_base_interface.name
                                : NULL;
}

void desk_globals_finish(DeskGlobals *globals)
{
    if (globals->fractional_manager)
        wp_fractional_scale_manager_v1_destroy(globals->fractional_manager);
    if (globals->viewporter)
        wp_viewporter_destroy(globals->viewporter);
    if (globals->wm_base)
        xdg_wm_base_destroy(globals->wm_base);
    if (globals->shm)
        wl_shm_destroy(globals->shm);
    if (globals->compositor)
        wl_compositor_destroy(globals->compositor);
    *globals = (DeskGlobals){0};
}

static void window_draw(DeskWindow *window)
{
    window->listener->draw(window->data, window);
}

static void window_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)time;
    DeskWindow *window = data;
    wl_callback_destroy(callback);
    window->frame_callback = NULL;
    if (window->dirty)
        window_draw(window);
}

static const struct wl_callback_listener window_frame_listener = {.done = window_frame_done};

static void window_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    DeskWindow *window = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    int width = window->pending_width, height = window->pending_height;
    if (width <= 0 || height <= 0) {
        if (window->configured) {
            width = window->width;
            height = window->height;
        } else {
            width = 800, height = 600;
            if (window->listener->initial_size)
                window->listener->initial_size(window->data, window, &width, &height);
        }
    }
    bool changed = !window->configured || width != window->width || height != window->height;
    window->width = width;
    window->height = height;
    window->configured = true;
    if (changed || window->dirty) {
        window->dirty = true;
        window_draw(window); /* Commit now, without waiting for a frame, to answer the configure. */
    }
}

static const struct xdg_surface_listener window_surface_listener = {.configure = window_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)toplevel;
    DeskWindow *window = data;
    window->pending_width = width;
    window->pending_height = height;
    window->activated = false;
    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_ACTIVATED)
            window->activated = true;
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    DeskWindow *window = data;
    if (window->listener->close)
        window->listener->close(window->data, window);
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height)
{
    (void)toplevel;
    DeskWindow *window = data;
    window->bounds_width = width;
    window->bounds_height = height;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel, struct wl_array *capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener window_toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void fractional_preferred_scale(void *data, struct wp_fractional_scale_v1 *fractional, uint32_t scale)
{
    (void)fractional;
    DeskWindow *window = data;
    if (scale == window->fraction)
        return;
    window->fraction = scale;
    desk_window_redraw(window);
}

static const struct wp_fractional_scale_v1_listener window_fractional_listener = {
    .preferred_scale = fractional_preferred_scale,
};

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

static void surface_preferred_buffer_scale(void *data, struct wl_surface *surface, int32_t factor)
{
    (void)surface;
    DeskWindow *window = data;
    if (factor < 1 || factor == window->buffer_scale)
        return;
    window->buffer_scale = factor;
    if (!window->fraction)
        desk_window_redraw(window);
}

static void surface_preferred_buffer_transform(void *data, struct wl_surface *surface, uint32_t transform)
{
    (void)data;
    (void)surface;
    (void)transform;
}

static const struct wl_surface_listener window_surface_scale_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_preferred_buffer_scale,
    .preferred_buffer_transform = surface_preferred_buffer_transform,
};

/* The surface, its scale objects and its xdg_surface, for a window or a popup. */
static bool window_surface(DeskWindow *window, const DeskGlobals *globals, const DeskWindowListener *listener,
                           void *data)
{
    *window = (DeskWindow){.shm = globals->shm, .buffer_scale = 1, .listener = listener, .data = data};
    window->surface = wl_compositor_create_surface(globals->compositor);
    if (!window->surface)
        return false;
    wl_surface_add_listener(window->surface, &window_surface_scale_listener, window);
    if (globals->viewporter) {
        window->viewport = wp_viewporter_get_viewport(globals->viewporter, window->surface);
        if (globals->fractional_manager) {
            window->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(globals->fractional_manager,
                                                                                      window->surface);
            wp_fractional_scale_v1_add_listener(window->fractional, &window_fractional_listener, window);
        }
    }
    window->xdg_surface = xdg_wm_base_get_xdg_surface(globals->wm_base, window->surface);
    xdg_surface_add_listener(window->xdg_surface, &window_surface_listener, window);
    return true;
}

static void popup_configure(void *data, struct xdg_popup *popup, int32_t x, int32_t y, int32_t width,
                            int32_t height)
{
    (void)popup;
    (void)x;
    (void)y;
    DeskWindow *window = data;
    window->pending_width = width;
    window->pending_height = height;
}

static void popup_done(void *data, struct xdg_popup *popup)
{
    (void)popup;
    DeskWindow *window = data;
    if (window->listener->close)
        window->listener->close(window->data, window);
}

static void popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token)
{
    (void)data;
    (void)popup;
    (void)token;
}

static const struct xdg_popup_listener window_popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

bool desk_popup_open(DeskWindow *popup, const DeskGlobals *globals, DeskWindow *parent, int x, int y, int width,
                     int height, struct wl_seat *seat, uint32_t serial, const DeskWindowListener *listener,
                     void *data)
{
    if (!window_surface(popup, globals, listener, data))
        return false;
    /* Start at the parent's scale so the first frame is already sharp. */
    popup->fraction = parent->fraction;
    popup->buffer_scale = parent->buffer_scale;
    struct xdg_positioner *positioner = xdg_wm_base_create_positioner(globals->wm_base);
    xdg_positioner_set_size(positioner, width > 0 ? width : 1, height > 0 ? height : 1);
    xdg_positioner_set_anchor_rect(positioner, x, y, 1, 1);
    xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(positioner, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    popup->popup = xdg_surface_get_popup(popup->xdg_surface, parent->xdg_surface, positioner);
    xdg_positioner_destroy(positioner);
    xdg_popup_add_listener(popup->popup, &window_popup_listener, popup);
    if (seat)
        xdg_popup_grab(popup->popup, seat, serial);
    popup->width = width;
    popup->height = height;
    wl_surface_commit(popup->surface);
    return true;
}

bool desk_window_open(DeskWindow *window, const DeskGlobals *globals, const char *app_id,
                      const DeskWindowListener *listener, void *data)
{
    if (!window_surface(window, globals, listener, data))
        return false;
    window->toplevel = xdg_surface_get_toplevel(window->xdg_surface);
    xdg_toplevel_add_listener(window->toplevel, &window_toplevel_listener, window);
    xdg_toplevel_set_app_id(window->toplevel, app_id);
    wl_surface_commit(window->surface);
    return true;
}

void desk_window_set_title(DeskWindow *window, const char *title)
{
    if (window->title && strcmp(window->title, title) == 0)
        return;
    char *copy = strdup(title);
    if (!copy)
        return;
    free(window->title);
    window->title = copy;
    xdg_toplevel_set_title(window->toplevel, title);
}

double desk_window_scale(const DeskWindow *window)
{
    if (window->viewport && window->fraction)
        return window->fraction / 120.0;
    return window->buffer_scale;
}

void desk_window_buffer_size(const DeskWindow *window, int *width, int *height)
{
    if (window->viewport) {
        double scale = desk_window_scale(window);
        *width = (int)lround(window->width * scale);
        *height = (int)lround(window->height * scale);
    } else {
        *width = window->width * window->buffer_scale;
        *height = window->height * window->buffer_scale;
    }
    if (*width < 1)
        *width = 1;
    if (*height < 1)
        *height = 1;
}

DeskBuffer *desk_window_buffer(DeskWindow *window, uint32_t format)
{
    int width, height;
    desk_window_buffer_size(window, &width, &height);
    return desk_buffer_get(&window->buffers, window->shm, width, height, format);
}

void desk_window_commit(DeskWindow *window, DeskBuffer *buffer)
{
    if (window->viewport)
        wp_viewport_set_destination(window->viewport, window->width, window->height);
    wl_surface_set_buffer_scale(window->surface, window->viewport ? 1 : window->buffer_scale);
    wl_surface_attach(window->surface, buffer->buffer, 0, 0);
    wl_surface_damage_buffer(window->surface, 0, 0, buffer->width, buffer->height);
    if (window->frame_callback)
        wl_callback_destroy(window->frame_callback);
    window->frame_callback = wl_surface_frame(window->surface);
    wl_callback_add_listener(window->frame_callback, &window_frame_listener, window);
    wl_surface_commit(window->surface);
    window->dirty = false;
}

void desk_window_redraw(DeskWindow *window)
{
    window->dirty = true;
    desk_window_update(window);
}

void desk_window_update(DeskWindow *window)
{
    if (window->dirty && window->configured && !window->frame_callback)
        window_draw(window);
}

bool desk_window_idle(const DeskWindow *window)
{
    return window->configured && !window->dirty && !window->frame_callback;
}

void desk_window_close(DeskWindow *window)
{
    if (window->frame_callback)
        wl_callback_destroy(window->frame_callback);
    desk_buffer_pool_finish(&window->buffers);
    if (window->toplevel)
        xdg_toplevel_destroy(window->toplevel);
    if (window->popup)
        xdg_popup_destroy(window->popup);
    if (window->xdg_surface)
        xdg_surface_destroy(window->xdg_surface);
    if (window->fractional)
        wp_fractional_scale_v1_destroy(window->fractional);
    if (window->viewport)
        wp_viewport_destroy(window->viewport);
    if (window->surface)
        wl_surface_destroy(window->surface);
    free(window->title);
    *window = (DeskWindow){0};
}
#endif
#endif

#ifdef DESK_APPS
/* Applications and file types */

#include <dirent.h>
#include <fnmatch.h>
#include <spawn.h>

extern char **environ;

/* Make room for one more item in an array. */
static bool apps_grow(void **items, size_t *capacity, size_t count, size_t size)
{
    if (count < *capacity)
        return true;
    size_t grown = *capacity ? *capacity * 2 : 16;
    void *moved = realloc(*items, grown * size);
    if (!moved)
        return false;
    *items = moved;
    *capacity = grown;
    return true;
}

/* A whole small file as a string, or NULL with errno set. */
static char *apps_read(const char *path)
{
    FILE *file = fopen(path, "re");
    if (!file)
        return NULL;
    char *text = NULL;
    size_t length = 0, capacity = 0;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, file)) > 0) {
        if (length + got + 1 > capacity) {
            size_t grown = (length + got + 1) * 2;
            char *moved = grown <= (1u << 20) * 2 ? realloc(text, grown) : NULL;
            if (!moved) {
                free(text);
                fclose(file);
                errno = ENOMEM;
                return NULL;
            }
            text = moved;
            capacity = grown;
        }
        memcpy(text + length, chunk, got);
        length += got;
    }
    fclose(file);
    if (!text && !(text = calloc(1, 1)))
        return NULL;
    text[length] = 0;
    return text;
}

/* $XDG_DATA_HOME, then each of $XDG_DATA_DIRS, as new strings in a
   NULL-terminated array; config does the same for the config directories. */
static char **apps_base_dirs(bool config)
{
    const char *home_variable = getenv(config ? "XDG_CONFIG_HOME" : "XDG_DATA_HOME");
    const char *dirs = getenv(config ? "XDG_CONFIG_DIRS" : "XDG_DATA_DIRS");
    const char *home = getenv("HOME");
    if (!dirs || !*dirs)
        dirs = config ? "/etc/xdg" : "/usr/local/share:/usr/share";
    size_t count = 2;
    for (const char *at = dirs; *at; ++at)
        count += *at == ':';
    char **list = calloc(count + 1, sizeof *list);
    if (!list)
        return NULL;
    size_t used = 0;
    if (home_variable && home_variable[0] == '/')
        list[used++] = strdup(home_variable);
    else if (home && *home && asprintf(&list[used], "%s/%s", home, config ? ".config" : ".local/share") >= 0)
        ++used;
    for (const char *at = dirs; *at;) {
        size_t length = strcspn(at, ":");
        if (length && at[0] == '/' && (list[used] = strndup(at, length)))
            ++used;
        at += length + (at[length] == ':');
    }
    return list;
}

static void apps_free_strings(char **list)
{
    for (size_t i = 0; list && list[i]; ++i)
        free(list[i]);
    free(list);
}

/* Decode the desktop file escapes \s \n \t \r \\ in place. */
static void apps_unescape(char *value)
{
    char *out = value;
    for (const char *in = value; *in; ++in) {
        if (*in == '\\' && in[1]) {
            char next = *++in;
            switch (next) {
            case 's': *out++ = ' '; break;
            case 'n': *out++ = '\n'; break;
            case 't': *out++ = '\t'; break;
            case 'r': *out++ = '\r'; break;
            case '\\': *out++ = '\\'; break;
            default: /* Kept for the next level, as \" in Exec or \; in lists. */
                *out++ = '\\';
                *out++ = next;
            }
        } else {
            *out++ = *in;
        }
    }
    *out = 0;
}

static bool apps_in_path(const char *program)
{
    if (strchr(program, '/'))
        return access(program, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char candidate[4096];
    for (const char *at = path; *at;) {
        size_t length = strcspn(at, ":");
        if (length && snprintf(candidate, sizeof candidate, "%.*s/%s", (int)length, at, program) <
                          (int)sizeof candidate &&
            access(candidate, X_OK) == 0)
            return true;
        at += length + (at[length] == ':');
    }
    return false;
}

static void app_free(DeskApp *app)
{
    free(app->id);
    free(app->path);
    free(app->name);
    free(app->exec);
    for (size_t i = 0; i < app->mime_count; ++i)
        free(app->mime_types[i]);
    free(app->mime_types);
    *app = (DeskApp){0};
}

/* Read one desktop entry. Return 1 for an application, 0 for something to
   skip (another type, Hidden, a missing TryExec), or -ENOMEM. */
static int apps_parse_entry(const char *path, const char *id, DeskApp *app)
{
    char *text = apps_read(path);
    if (!text)
        return errno == ENOMEM ? -ENOMEM : 0;
    *app = (DeskApp){0};
    bool group = false, application = false, hidden = false, missing = false, failed = false;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\r' || line[length - 1] == ' ' || line[length - 1] == '\t'))
            line[--length] = 0;
        while (*line == ' ' || *line == '\t')
            ++line;
        if (!*line || *line == '#')
            continue;
        if (*line == '[') {
            group = strcmp(line, "[Desktop Entry]") == 0;
            continue;
        }
        char *equals = strchr(line, '=');
        if (!group || !equals)
            continue;
        char *key_end = equals;
        while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t'))
            --key_end;
        *key_end = 0;
        char *value = equals + 1;
        while (*value == ' ' || *value == '\t')
            ++value;
        if (strchr(line, '['))
            continue; /* A translation. */
        if (strcmp(line, "Type") == 0) {
            application = strcmp(value, "Application") == 0;
        } else if (strcmp(line, "Hidden") == 0) {
            desk_parse_bool(value, &hidden);
        } else if (strcmp(line, "NoDisplay") == 0) {
            desk_parse_bool(value, &app->no_display);
        } else if (strcmp(line, "Terminal") == 0) {
            desk_parse_bool(value, &app->terminal);
        } else if (strcmp(line, "TryExec") == 0) {
            apps_unescape(value);
            missing = !apps_in_path(value);
        } else if (strcmp(line, "Name") == 0 || strcmp(line, "Exec") == 0) {
            apps_unescape(value);
            char **field = line[0] == 'N' ? &app->name : &app->exec;
            free(*field);
            failed |= !(*field = strdup(value));
        } else if (strcmp(line, "MimeType") == 0) {
            size_t capacity = 0;
            for (char *type = strtok(value, ";"); type; type = strtok(NULL, ";")) {
                if (!*type)
                    continue;
                if (!apps_grow((void **)&app->mime_types, &capacity, app->mime_count, sizeof *app->mime_types) ||
                    !(app->mime_types[app->mime_count] = strdup(type))) {
                    failed = true;
                    break;
                }
                ++app->mime_count;
            }
        }
    }
    free(text);
    if (!failed && application && !hidden && !missing && app->exec && app->name) {
        app->id = strdup(id);
        app->path = strdup(path);
        if (app->id && app->path)
            return 1;
        failed = true;
    }
    app_free(app);
    return failed ? -ENOMEM : 0;
}

static int apps_compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Every .desktop in directory, in name order. The first entry seen with an
   id wins, so the user's override the system's; ids already in seen are
   skipped, and Hidden ones still claim their id. */
static int apps_scan(DeskApps *apps, size_t *capacity, char ***seen, size_t *seen_count, size_t *seen_capacity,
                     const char *directory)
{
    DIR *dir = opendir(directory);
    if (!dir)
        return 0;
    char **names = NULL;
    size_t count = 0, names_capacity = 0;
    struct dirent *item;
    int error = 0;
    while ((item = readdir(dir))) {
        size_t length = strlen(item->d_name);
        if (length <= 8 || strcmp(item->d_name + length - 8, ".desktop") != 0)
            continue;
        if (!apps_grow((void **)&names, &names_capacity, count, sizeof *names) ||
            !(names[count] = strdup(item->d_name))) {
            error = ENOMEM;
            break;
        }
        ++count;
    }
    closedir(dir);
    if (count)
        qsort(names, count, sizeof *names, apps_compare_names);
    for (size_t i = 0; i < count && !error; ++i) {
        bool taken = false;
        for (size_t j = 0; j < *seen_count && !taken; ++j)
            taken = strcmp((*seen)[j], names[i]) == 0;
        if (taken)
            continue;
        if (!apps_grow((void **)seen, seen_capacity, *seen_count, sizeof **seen)) {
            error = ENOMEM;
            break;
        }
        (*seen)[(*seen_count)++] = names[i];
        char *path;
        if (asprintf(&path, "%s/%s", directory, names[i]) < 0) {
            error = ENOMEM;
            break;
        }
        if (!apps_grow((void **)&apps->apps, capacity, apps->app_count, sizeof *apps->apps)) {
            free(path);
            error = ENOMEM;
            break;
        }
        int result = apps_parse_entry(path, names[i], &apps->apps[apps->app_count]);
        free(path);
        if (result < 0)
            error = ENOMEM;
        else if (result > 0)
            ++apps->app_count;
        names[i] = NULL; /* Owned by seen now. */
    }
    for (size_t i = 0; i < count; ++i)
        free(names[i]);
    free(names);
    return error;
}

struct apps_list_state {
    DeskApps *apps;
    size_t capacity;
    int error;
};

static void apps_list_entry(void *data, const char *section, const char *key, const char *value, int line)
{
    (void)line;
    struct apps_list_state *state = data;
    if (!key || state->error)
        return;
    int kind = strcmp(section, "Default Applications") == 0 ? DESK_DEFAULT
             : strcmp(section, "Added Associations") == 0   ? DESK_ADDED
             : strcmp(section, "Removed Associations") == 0 ? DESK_REMOVED
                                                            : -1;
    if (kind < 0)
        return;
    char *ids = strdup(value);
    if (!ids) {
        state->error = ENOMEM;
        return;
    }
    DeskApps *apps = state->apps;
    for (char *id = strtok(ids, ";"); id; id = strtok(NULL, ";")) {
        while (*id == ' ')
            ++id;
        if (!*id)
            continue;
        if (!apps_grow((void **)&apps->associations, &state->capacity, apps->association_count,
                       sizeof *apps->associations)) {
            state->error = ENOMEM;
            break;
        }
        DeskAssociation *association = &apps->associations[apps->association_count];
        *association = (DeskAssociation){strdup(key), strdup(id), kind};
        if (!association->mime || !association->id) {
            free(association->mime);
            free(association->id);
            state->error = ENOMEM;
            break;
        }
        ++apps->association_count;
    }
    free(ids);
}

static int apps_load_globs(DeskApps *apps, const char *path)
{
    char *text = apps_read(path);
    if (!text)
        return errno == ENOMEM ? ENOMEM : 0;
    size_t capacity = 0;
    int error = 0;
    for (char *line = text, *next; line && *line && !error; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (*line == '#')
            continue;
        char *weight = line, *mime = strchr(weight, ':');
        char *pattern = mime ? strchr(++mime, ':') : NULL;
        if (!pattern)
            continue;
        mime[-1] = 0;
        *pattern++ = 0;
        char *flags = strchr(pattern, ':');
        if (flags)
            *flags++ = 0;
        if (!apps_grow((void **)&apps->globs, &capacity, apps->glob_count, sizeof *apps->globs)) {
            error = ENOMEM;
            break;
        }
        DeskGlob *glob = &apps->globs[apps->glob_count];
        *glob = (DeskGlob){strdup(pattern), strdup(mime), atoi(weight), flags && strstr(flags, "cs")};
        if (!glob->pattern || !glob->mime) {
            free(glob->pattern);
            free(glob->mime);
            error = ENOMEM;
            break;
        }
        ++apps->glob_count;
    }
    free(text);
    return error;
}

int desk_apps_load(DeskApps *apps)
{
    *apps = (DeskApps){0};
    char **data = apps_base_dirs(false), **config = apps_base_dirs(true);
    int error = data && config ? 0 : ENOMEM;

    char **seen = NULL;
    size_t seen_count = 0, seen_capacity = 0, capacity = 0;
    for (size_t i = 0; !error && data[i]; ++i) {
        char *directory;
        if (asprintf(&directory, "%s/applications", data[i]) < 0) {
            error = ENOMEM;
            break;
        }
        error = apps_scan(apps, &capacity, &seen, &seen_count, &seen_capacity, directory);
        free(directory);
    }
    for (size_t i = 0; i < seen_count; ++i)
        free(seen[i]);
    free(seen);

    /* mimeapps.list: the config directories', then the data directories'. */
    struct apps_list_state state = {apps, 0, 0};
    for (int pass = 0; !error && pass < 2; ++pass) {
        char **dirs = pass ? data : config;
        for (size_t i = 0; dirs[i]; ++i) {
            char *path;
            if (asprintf(&path, pass ? "%s/applications/mimeapps.list" : "%s/mimeapps.list", dirs[i]) < 0) {
                error = ENOMEM;
                break;
            }
            desk_ini_load(path, apps_list_entry, &state);
            free(path);
        }
    }
    if (!error)
        error = state.error;

    /* The globs: the first mime directory that has them. */
    for (size_t i = 0; !error && data[i] && !apps->glob_count; ++i) {
        char *path;
        if (asprintf(&path, "%s/mime/globs2", data[i]) < 0) {
            error = ENOMEM;
            break;
        }
        error = apps_load_globs(apps, path);
        free(path);
    }
    apps_free_strings(data);
    apps_free_strings(config);
    if (error)
        desk_apps_free(apps);
    return error;
}

void desk_apps_free(DeskApps *apps)
{
    for (size_t i = 0; i < apps->app_count; ++i)
        app_free(&apps->apps[i]);
    free(apps->apps);
    for (size_t i = 0; i < apps->association_count; ++i) {
        free(apps->associations[i].mime);
        free(apps->associations[i].id);
    }
    free(apps->associations);
    for (size_t i = 0; i < apps->glob_count; ++i) {
        free(apps->globs[i].pattern);
        free(apps->globs[i].mime);
    }
    free(apps->globs);
    *apps = (DeskApps){0};
}

/* Common types by extension, for when shared-mime-info isn't installed. */
static const char *const builtin_types[][2] = {
    {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"jpe", "image/jpeg"},
    {"gif", "image/gif"}, {"webp", "image/webp"}, {"svg", "image/svg+xml"}, {"bmp", "image/bmp"},
    {"avif", "image/avif"}, {"heic", "image/heic"}, {"tif", "image/tiff"}, {"tiff", "image/tiff"},
    {"ico", "image/vnd.microsoft.icon"}, {"jxl", "image/jxl"},
    {"mp4", "video/mp4"}, {"m4v", "video/mp4"}, {"mkv", "video/x-matroska"}, {"webm", "video/webm"},
    {"mov", "video/quicktime"}, {"avi", "video/x-msvideo"},
    {"mp3", "audio/mpeg"}, {"flac", "audio/flac"}, {"ogg", "audio/ogg"}, {"opus", "audio/ogg"},
    {"wav", "audio/x-wav"}, {"m4a", "audio/mp4"}, {"aac", "audio/aac"},
    {"pdf", "application/pdf"}, {"zip", "application/zip"}, {"tar", "application/x-tar"},
    {"gz", "application/gzip"}, {"tgz", "application/x-compressed-tar"}, {"xz", "application/x-xz"},
    {"zst", "application/zstd"}, {"bz2", "application/x-bzip2"}, {"7z", "application/x-7z-compressed"},
    {"rar", "application/vnd.rar"}, {"deb", "application/vnd.debian.binary-package"},
    {"md", "text/markdown"}, {"markdown", "text/markdown"}, {"txt", "text/plain"}, {"log", "text/x-log"},
    {"c", "text/x-csrc"}, {"h", "text/x-chdr"}, {"cc", "text/x-c++src"}, {"cpp", "text/x-c++src"},
    {"hpp", "text/x-c++hdr"}, {"py", "text/x-python3"}, {"sh", "application/x-shellscript"},
    {"js", "text/javascript"}, {"ts", "text/x-typescript"}, {"rs", "text/rust"}, {"go", "text/x-go"},
    {"lua", "text/x-lua"}, {"java", "text/x-java"}, {"rb", "application/x-ruby"}, {"zig", "text/x-zig"},
    {"json", "application/json"}, {"xml", "application/xml"}, {"html", "text/html"}, {"htm", "text/html"},
    {"css", "text/css"}, {"csv", "text/csv"}, {"toml", "application/toml"}, {"yaml", "application/yaml"},
    {"yml", "application/yaml"}, {"ini", "text/plain"}, {"conf", "text/plain"}, {"cfg", "text/plain"},
    {"desktop", "application/x-desktop"}, {"mk", "text/x-makefile"},
};

/* By content: an ELF executable, text (no NUL in the first 1 KiB), or
   anything else. */
static const char *apps_sniff(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return "application/octet-stream";
    unsigned char head[1024];
    ssize_t length = read(fd, head, sizeof head);
    close(fd);
    if (length < 0)
        return "application/octet-stream";
    if (length >= 4 && memcmp(head, "\x7f" "ELF", 4) == 0)
        return "application/x-executable";
    if (memchr(head, 0, (size_t)length))
        return "application/octet-stream";
    return "text/plain";
}

const char *desk_mime_type(const DeskApps *apps, const char *path, const char *name)
{
    struct stat info;
    if (path && stat(path, &info) == 0 && S_ISDIR(info.st_mode))
        return "inode/directory";
    /* Case-sensitive globs first, as the spec says: shared-mime-info lists
       *.C:cs for C++ and *.c:cs for C, plus both again ignoring case. Then
       the highest weight, then the longest pattern. */
    for (int pass = 0; pass < 2; ++pass) {
        const char *best = NULL;
        int best_weight = -1;
        size_t best_length = 0;
        for (size_t i = 0; apps && i < apps->glob_count; ++i) {
            const DeskGlob *glob = &apps->globs[i];
            if (glob->case_sensitive != (pass == 0))
                continue;
            size_t length = strlen(glob->pattern);
            if (glob->weight < best_weight || (glob->weight == best_weight && length <= best_length))
                continue;
            if (fnmatch(glob->pattern, name, pass == 0 ? 0 : FNM_CASEFOLD) == 0) {
                best = glob->mime;
                best_weight = glob->weight;
                best_length = length;
            }
        }
        if (best)
            return best;
    }
    if (!apps || !apps->glob_count) {
        if (strcasecmp(name, "Makefile") == 0 || strcasecmp(name, "GNUmakefile") == 0)
            return "text/x-makefile";
        const char *dot = strrchr(name, '.');
        for (size_t i = 0; dot && dot != name && i < sizeof builtin_types / sizeof *builtin_types; ++i) {
            if (strcasecmp(dot + 1, builtin_types[i][0]) == 0)
                return builtin_types[i][1];
        }
    }
    return path ? apps_sniff(path) : "application/octet-stream";
}

const DeskApp *desk_app_find(const DeskApps *apps, const char *id)
{
    for (size_t i = 0; i < apps->app_count; ++i) {
        if (strcmp(apps->apps[i].id, id) == 0)
            return &apps->apps[i];
    }
    return NULL;
}

static bool apps_removed(const DeskApps *apps, const char *mime, const char *id)
{
    for (size_t i = 0; i < apps->association_count; ++i) {
        const DeskAssociation *association = &apps->associations[i];
        if (association->kind == DESK_REMOVED && strcmp(association->mime, mime) == 0 &&
            strcmp(association->id, id) == 0)
            return true;
    }
    return false;
}

static bool apps_declares(const DeskApp *app, const char *mime)
{
    for (size_t i = 0; i < app->mime_count; ++i) {
        if (strcmp(app->mime_types[i], mime) == 0)
            return true;
    }
    return false;
}

size_t desk_apps_for(const DeskApps *apps, const char *mime, const DeskApp **out, size_t max)
{
    const DeskApp **found = apps->app_count ? calloc(apps->app_count, sizeof *found) : NULL;
    if (!found)
        return 0;
    size_t count = 0;
    const char *types[2] = {mime, strncmp(mime, "text/", 5) == 0 && strcmp(mime, "text/plain") != 0 ? "text/plain"
                                                                                                     : NULL};
    for (int t = 0; t < 2 && types[t]; ++t) {
        const char *type = types[t];
        /* Defaults, then added associations, then whoever declares it. */
        for (int kind = DESK_DEFAULT; kind <= DESK_ADDED + 1; ++kind) {
            size_t total = kind <= DESK_ADDED ? apps->association_count : apps->app_count;
            for (size_t i = 0; i < total; ++i) {
                const DeskApp *app;
                if (kind <= DESK_ADDED) {
                    const DeskAssociation *association = &apps->associations[i];
                    if (association->kind != kind || strcmp(association->mime, type) != 0)
                        continue;
                    app = desk_app_find(apps, association->id);
                } else {
                    app = &apps->apps[i];
                    if (!apps_declares(app, type))
                        continue;
                }
                if (!app || apps_removed(apps, type, app->id))
                    continue;
                bool already = false;
                for (size_t j = 0; j < count && !already; ++j)
                    already = found[j] == app;
                if (!already)
                    found[count++] = app;
            }
        }
    }
    for (size_t i = 0; i < count && i < max; ++i)
        out[i] = found[i];
    free(found);
    return count;
}

struct apps_argv {
    char **argv;
    size_t count, capacity;
};

static bool apps_push(struct apps_argv *list, const char *text, size_t length)
{
    if (!apps_grow((void **)&list->argv, &list->capacity, list->count + 1, sizeof *list->argv))
        return false;
    if (!(list->argv[list->count] = strndup(text, length)))
        return false;
    list->argv[++list->count] = NULL;
    return true;
}

static void apps_argv_free(struct apps_argv *list)
{
    for (size_t i = 0; i < list->count; ++i)
        free(list->argv[i]);
    free(list->argv);
    *list = (struct apps_argv){0};
}

/* Split a command line into words: spaces separate, "double quotes" group
   with \" \` \$ \\ escaped inside. Return false when a quote isn't closed. */
static bool apps_split(const char *line, struct apps_argv *words)
{
    char *word = malloc(strlen(line) + 1);
    if (!word)
        return false;
    const char *at = line;
    bool ok = true;
    while (ok) {
        while (*at == ' ' || *at == '\t')
            ++at;
        if (!*at)
            break;
        size_t length = 0;
        while (*at && *at != ' ' && *at != '\t') {
            if (*at == '"') {
                ++at;
                while (*at && *at != '"') {
                    if (*at == '\\' && at[1] && strchr("\"`$\\", at[1]))
                        ++at;
                    word[length++] = *at++;
                }
                if (*at != '"') {
                    ok = false;
                    break;
                }
                ++at;
            } else {
                word[length++] = *at++;
            }
        }
        if (ok && !apps_push(words, word, length))
            ok = false;
    }
    free(word);
    return ok;
}

static char spawn_mark[64];

void desk_spawn_mark(const char *name)
{
    snprintf(spawn_mark, sizeof spawn_mark, "%s", name ? name : "");
}

/* environ for a child: without an inherited mark, with ours when set.
   NULL when out of memory. */
static char **spawn_environment(char *mark, size_t size)
{
    size_t count = 0;
    while (environ[count])
        ++count;
    char **env = malloc((count + 2) * sizeof *env);
    if (!env)
        return NULL;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        if (strncmp(environ[i], "DESK_LAUNCHED_BY=", 17) != 0)
            env[used++] = environ[i];
    }
    if (spawn_mark[0]) {
        struct timespec now;
        clock_gettime(CLOCK_BOOTTIME, &now);
        snprintf(mark, size, "DESK_LAUNCHED_BY=%s %ld %lld", spawn_mark, (long)getpid(),
                 (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000);
        env[used++] = mark;
    }
    env[used] = NULL;
    return env;
}

int desk_spawn(char *const argv[], const char *terminal, const char *directory)
{
    struct apps_argv full = {0};
    if (terminal && !apps_split(terminal, &full)) {
        apps_argv_free(&full);
        return EINVAL;
    }
    for (size_t i = 0; argv[i]; ++i) {
        if (!apps_push(&full, argv[i], strlen(argv[i]))) {
            apps_argv_free(&full);
            return ENOMEM;
        }
    }
    if (!full.count) {
        apps_argv_free(&full);
        return EINVAL;
    }
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_t actions;
    int error = posix_spawnattr_init(&attributes);
    if (!error) {
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
        if (!error && !(error = posix_spawn_file_actions_init(&actions))) {
            error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
            if (!error && directory)
                error = posix_spawn_file_actions_addchdir_np(&actions, directory);
            pid_t child;
            char mark[128];
            char **env = error ? NULL : spawn_environment(mark, sizeof mark);
            if (!error && !env)
                error = ENOMEM;
            if (!error)
                error = posix_spawnp(&child, full.argv[0], &actions, &attributes, full.argv, env);
            free(env);
            posix_spawn_file_actions_destroy(&actions);
        }
        posix_spawnattr_destroy(&attributes);
    }
    apps_argv_free(&full);
    return error;
}

/* Expand one word of Exec for the paths. Return false when out of memory. */
static bool apps_expand(const DeskApp *app, const char *word, char *const paths[], size_t count,
                        struct apps_argv *argv)
{
    if (strcmp(word, "%F") == 0 || strcmp(word, "%U") == 0) {
        for (size_t i = 0; i < count; ++i) {
            if (!apps_push(argv, paths[i], strlen(paths[i])))
                return false;
        }
        return true;
    }
    char *out = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&out, &length);
    if (!stream)
        return false;
    bool only_codes = true;
    for (const char *at = word; *at; ++at) {
        if (*at != '%' || !at[1]) {
            fputc(*at, stream);
            only_codes = false;
            continue;
        }
        char code = *++at;
        if (code == '%') {
            fputc('%', stream);
            only_codes = false;
        } else if ((code == 'f' || code == 'u') && count) {
            fputs(paths[0], stream);
        } else if (code == 'c') {
            fputs(app->name, stream);
        }
        /* %i, %k and the deprecated codes expand to nothing. */
    }
    fclose(stream);
    bool ok = !out || (only_codes && !length) || apps_push(argv, out, length);
    free(out);
    return out != NULL && ok;
}

int desk_app_launch(const DeskApp *app, char *const paths[], size_t count, const char *terminal,
                    const char *directory)
{
    struct apps_argv words = {0};
    if (!apps_split(app->exec, &words) || !words.count) {
        apps_argv_free(&words);
        return EINVAL;
    }
    bool many = false, one = false;
    for (size_t i = 0; i < words.count; ++i) {
        many |= strcmp(words.argv[i], "%F") == 0 || strcmp(words.argv[i], "%U") == 0;
        one |= strstr(words.argv[i], "%f") || strstr(words.argv[i], "%u");
    }
    int error = 0;
    /* An application that takes one file starts once for each. */
    size_t runs = one && !many && count > 1 ? count : 1;
    for (size_t run = 0; run < runs && !error; ++run) {
        char *const *these = runs > 1 ? paths + run : paths;
        size_t these_count = runs > 1 ? 1 : count;
        struct apps_argv argv = {0};
        for (size_t i = 0; i < words.count && !error; ++i) {
            if (!apps_expand(app, words.argv[i], these, these_count, &argv))
                error = ENOMEM;
        }
        if (!error)
            error = argv.count ? desk_spawn(argv.argv, app->terminal ? terminal : NULL, directory) : EINVAL;
        apps_argv_free(&argv);
    }
    apps_argv_free(&words);
    return error;
}

int desk_mimeapps_set_default(const char *mime, const char *id)
{
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char *path = NULL;
    if (config && config[0] == '/') {
        if (asprintf(&path, "%s/mimeapps.list", config) < 0)
            return ENOMEM;
    } else if (home && *home) {
        if (asprintf(&path, "%s/.config/mimeapps.list", home) < 0)
            return ENOMEM;
    } else {
        return ENOENT;
    }
    char *text = apps_read(path);
    if (!text && errno != ENOENT) {
        free(path);
        return errno;
    }
    char *out = NULL;
    size_t out_length = 0;
    FILE *stream = open_memstream(&out, &out_length);
    if (!stream) {
        free(text);
        free(path);
        return ENOMEM;
    }
    /* Copy the lines, replacing mime's in the two sections, and add them at
       each section's end (or a new section) when they weren't there. */
    static const char *const sections[2] = {"[Default Applications]", "[Added Associations]"};
    bool done[2] = {false, false}, seen[2] = {false, false};
    int section = -1;
    size_t mime_length = strlen(mime);
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (line[0] == '[') {
            if (section >= 0 && !done[section]) {
                fprintf(stream, "%s=%s;\n", mime, id);
                done[section] = true;
            }
            section = -1;
            for (int i = 0; i < 2; ++i) {
                if (strncmp(line, sections[i], strlen(sections[i])) == 0) {
                    section = i;
                    seen[i] = true;
                }
            }
        } else if (section >= 0 && strncmp(line, mime, mime_length) == 0 &&
                   (line[mime_length] == '=' || line[mime_length] == ' ')) {
            if (section == 0) {
                fprintf(stream, "%s=%s;\n", mime, id);
            } else {
                /* Added associations: id first, the others kept after it. */
                const char *list = strchr(line, '=');
                fprintf(stream, "%s=%s;", mime, id);
                char *copy = strdup(list ? list + 1 : "");
                for (char *other = copy ? strtok(copy, ";") : NULL; other; other = strtok(NULL, ";")) {
                    while (*other == ' ')
                        ++other;
                    if (*other && strcmp(other, id) != 0)
                        fprintf(stream, "%s;", other);
                }
                free(copy);
                fputc('\n', stream);
            }
            done[section] = true;
            continue;
        }
        fprintf(stream, "%s\n", line);
    }
    if (section >= 0 && !done[section]) {
        fprintf(stream, "%s=%s;\n", mime, id);
        done[section] = true;
    }
    for (int i = 0; i < 2; ++i) {
        if (!seen[i])
            fprintf(stream, "%s%s\n%s=%s;\n", out_length || ftell(stream) ? "\n" : "", sections[i], mime, id);
    }
    fclose(stream);
    free(text);
    int error = ENOMEM;
    if (out) {
        char *directory = strdup(path);
        char *slash = directory ? strrchr(directory, '/') : NULL;
        if (slash)
            *slash = 0;
        error = directory ? desk_mkdirs(directory, 0700) : ENOMEM;
        free(directory);
        DeskAtomic atomic;
        if (!error && !(error = desk_atomic_open(&atomic, path, 0644))) {
            fwrite(out, 1, out_length, atomic.file);
            error = desk_atomic_commit(&atomic, true);
            desk_atomic_abort(&atomic);
        }
    }
    free(out);
    free(path);
    return error;
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
