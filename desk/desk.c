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

bool desk_window_open(DeskWindow *window, const DeskGlobals *globals, const char *app_id,
                      const DeskWindowListener *listener, void *data)
{
    *window = (DeskWindow){.shm = globals->shm, .buffer_scale = 1, .listener = listener, .data = data};
    window->surface = wl_compositor_create_surface(globals->compositor);
    if (!window->surface)
        return false;
    wl_surface_add_listener(window->surface, &window_surface_scale_listener, window);
    if (globals->viewporter) {
        window->viewport = wp_viewporter_get_viewport(globals->viewporter, window->surface);
        /* Fractional scale is only usable through a viewport. */
        if (globals->fractional_manager) {
            window->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(globals->fractional_manager,
                                                                                      window->surface);
            wp_fractional_scale_v1_add_listener(window->fractional, &window_fractional_listener, window);
        }
    }
    window->xdg_surface = xdg_wm_base_get_xdg_surface(globals->wm_base, window->surface);
    xdg_surface_add_listener(window->xdg_surface, &window_surface_listener, window);
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
