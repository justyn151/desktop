#define _GNU_SOURCE
#include "desk.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
