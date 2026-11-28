#define _GNU_SOURCE
#include "files.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grp.h>
#include <pwd.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "cursor-shape-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define FONT_POINTS 11.0      /* UI text size, in points at 96 dpi. */
#define ROW_SPACING 1.5       /* Row height as a multiple of the line height. */
#define PADDING 10            /* Logical pixels around the list. */
#define GAP 16                /* Logical pixels between name and details. */
#define SCROLL_MARGIN 3       /* Rows kept visible above and below the cursor. */
#define REFRESH_DELAY_MS 100  /* Changes are gathered this long before rereading. */
#define MAX_REPEATS 8         /* Repeats handled at once after a stall. */
#define SIDEBAR_CELLS 60      /* Narrower than this, the sidebar goes. */
#define SIDEBAR_WIDTH 180     /* Logical pixels, to begin with. */
#define SIDEBAR_MIN 100       /* No narrower than this, and no wider than half the window. */
#define SIDEBAR_STEP 20       /* < and > change the width by this. */
#define DIVIDER_GRAB 5        /* Logical pixels either side of a divider that grab it. */
#define DOUBLE_CLICK_MS 400

/* Zoom levels: the list's text size as a multiple of the window's, and the
   icon view's icon size in logical pixels. */
static const double list_zooms[] = {0.75, 0.875, 1.0, 1.125, 1.25, 1.5, 1.75, 2.0};
static const double icon_zooms[] = {32, 40, 48, 64, 80, 96, 128, 160, 192};
#define LIST_ZOOMS ((int)(sizeof list_zooms / sizeof *list_zooms))
#define ICON_ZOOMS ((int)(sizeof icon_zooms / sizeof *icon_zooms))
static const int default_zoom[VIEW_COUNT] = {2, 3};

/* Nerd Font icons (Font Awesome and Octicons ranges of Symbols Nerd Font). */
#define ICON_FOLDER 0xf07b
#define ICON_FOLDER_LINK 0xf482
#define ICON_FILE 0xf15b
#define ICON_FILE_LINK 0xf481
#define ICON_TEXT 0xf15c
#define ICON_IMAGE 0xf1c5
#define ICON_VIDEO 0xf1c8
#define ICON_AUDIO 0xf1c7
#define ICON_ARCHIVE 0xf1c6
#define ICON_PDF 0xf1c1
#define ICON_CODE 0xf1c9
#define ICON_MARKDOWN 0xf48a
#define ICON_EXECUTABLE 0xf489
#define ICON_HOME 0xf015
#define ICON_DISK 0xf0a0
#define ICON_DRIVE 0xf287

/* Colors without a palette. */
static const struct colors default_colors = {
    .background = 0xff1a1b26,
    .foreground = 0xffc0caf5,
    .directory = 0xff7aa2f7,
    .muted = 0xff565f89,
    .selection_background = 0xff283457,
    .selection_foreground = 0xffc0caf5,
    .urgent = 0xfff7768e,
};

static void update_colors(struct files *files)
{
    DeskPalette palette;
    if (!files->palette_path || desk_palette_load(files->palette_path, &palette) != 0) {
        files->colors = default_colors;
        return;
    }
    files->colors = (struct colors){
        .background = 0xff000000u | palette.rgb[DESK_BACKGROUND],
        .foreground = 0xff000000u | palette.rgb[DESK_FOREGROUND],
        .directory = 0xff000000u | palette.rgb[DESK_ACCENT],
        .muted = 0xff000000u | palette.rgb[DESK_MUTED],
        .selection_background = 0xff000000u | palette.rgb[DESK_SELECTION_BACKGROUND],
        .selection_foreground = 0xff000000u | palette.rgb[DESK_SELECTION_FOREGROUND],
        .urgent = 0xff000000u | palette.rgb[DESK_URGENT],
    };
}

/* Paths */

static char *join_path(const char *directory, const char *name)
{
    char *path;
    bool root = strcmp(directory, "/") == 0;
    return asprintf(&path, "%s%s%s", directory, root ? "" : "/", name) < 0 ? NULL : path;
}

/* The parent of an absolute path, as a new string; "/" for "/". */
static char *parent_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t length = slash && slash != path ? (size_t)(slash - path) : 1;
    return strndup(path, length);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

/* The path with HOME shown as ~, for the title. */
static void display_path(const struct files *files, const char *path, char *out, size_t size)
{
    size_t home = files->home ? strlen(files->home) : 0;
    if (home > 1 && strncmp(path, files->home, home) == 0 && (path[home] == '/' || path[home] == 0))
        snprintf(out, size, "~%s", path + home);
    else
        snprintf(out, size, "%s", path);
}

/* Sorting */

/* Natural order, case-insensitive: "file2" before "file10", "Apple" next to
   "apple". Letters fold by ASCII case only and otherwise compare by code
   point; ties fall back to the bytes so the order is total. */
static int natural_compare(const char *a, const char *b)
{
    const char *a_end = a + strlen(a), *b_end = b + strlen(b);
    const char *x = a, *y = b;
    while (x < a_end && y < b_end) {
        if (isdigit((unsigned char)*x) && isdigit((unsigned char)*y)) {
            while (*x == '0' && isdigit((unsigned char)x[1]))
                ++x;
            while (*y == '0' && isdigit((unsigned char)y[1]))
                ++y;
            const char *x_start = x, *y_start = y;
            while (isdigit((unsigned char)*x))
                ++x;
            while (isdigit((unsigned char)*y))
                ++y;
            ptrdiff_t x_length = x - x_start, y_length = y - y_start;
            if (x_length != y_length)
                return x_length < y_length ? -1 : 1;
            int order = memcmp(x_start, y_start, (size_t)x_length);
            if (order)
                return order;
            continue;
        }
        uint32_t cx = desk_utf8_next(&x, a_end), cy = desk_utf8_next(&y, b_end);
        if (cx < 0x80)
            cx = (uint32_t)tolower((int)cx);
        if (cy < 0x80)
            cy = (uint32_t)tolower((int)cy);
        if (cx != cy)
            return cx < cy ? -1 : 1;
    }
    if (x < a_end || y < b_end)
        return x < a_end ? 1 : -1;
    return strcmp(a, b);
}

static int entry_order(const void *left, const void *right)
{
    const struct entry *a = left, *b = right;
    if (a->directory != b->directory)
        return a->directory ? -1 : 1;
    return natural_compare(a->name, b->name);
}

/* Listing */

static void listing_free(struct listing *listing)
{
    for (size_t i = 0; i < listing->count; ++i)
        free(listing->entries[i].name);
    free(listing->entries);
    free(listing->shown);
    *listing = (struct listing){0};
}

/* Fill an entry from fstatat relative to the directory, so a long path or a
   rename of the directory while reading can't confuse it. */
static void entry_stat(int directory, struct entry *entry, unsigned char type)
{
    struct stat info;
    if (fstatat(directory, entry->name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        /* Readable directory without search permission: only the name and type. */
        entry->directory = type == DT_DIR;
        entry->mode = type == DT_DIR ? S_IFDIR : type == DT_LNK ? S_IFLNK : S_IFREG;
        entry->link = type == DT_LNK;
        entry->target_mode = entry->link ? 0 : entry->mode;
        return;
    }
    entry->mode = info.st_mode;
    entry->target_mode = info.st_mode;
    entry->owner = info.st_uid;
    entry->group = info.st_gid;
    entry->size = info.st_size;
    entry->modified = info.st_mtim;
    if (S_ISLNK(info.st_mode)) {
        entry->link = true;
        struct stat target;
        if (fstatat(directory, entry->name, &target, 0) != 0) {
            entry->broken = true;
            entry->target_mode = 0;
            return;
        }
        info = target;
        entry->target_mode = target.st_mode;
        entry->size = target.st_size;
    }
    entry->directory = S_ISDIR(info.st_mode);
    entry->executable = S_ISREG(info.st_mode) && (info.st_mode & 0111);
}

/* Read a directory. Return 0, or an errno value with an empty listing that
   carries the error, such as for a directory without read permission. */
static int listing_read(struct listing *listing, const char *path)
{
    *listing = (struct listing){0};
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *directory = fd >= 0 ? fdopendir(fd) : NULL;
    if (!directory) {
        listing->error = errno;
        if (fd >= 0)
            close(fd);
        return listing->error;
    }
    struct dirent *item;
    errno = 0;
    while ((item = readdir(directory))) {
        const char *name = item->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (listing->count == listing->capacity) {
            size_t capacity = listing->capacity ? listing->capacity * 2 : 64;
            struct entry *entries = realloc(listing->entries, capacity * sizeof *entries);
            if (!entries) {
                errno = ENOMEM;
                break;
            }
            listing->entries = entries;
            listing->capacity = capacity;
        }
        struct entry *entry = &listing->entries[listing->count];
        *entry = (struct entry){.name = strdup(name), .items = COUNT_UNKNOWN, .hidden = name[0] == '.'};
        if (!entry->name) {
            errno = ENOMEM;
            break;
        }
        entry_stat(dirfd(directory), entry, item->d_type);
        listing->count++;
        errno = 0;
    }
    int error = errno;
    closedir(directory);
    if (error) {
        listing_free(listing);
        listing->error = error;
        return error;
    }
    if (listing->count)
        qsort(listing->entries, listing->count, sizeof *listing->entries, entry_order);
    listing->shown = malloc((listing->count ? listing->count : 1) * sizeof *listing->shown);
    if (!listing->shown) {
        listing_free(listing);
        listing->error = ENOMEM;
        return ENOMEM;
    }
    return 0;
}

/* Show what's not hidden (or everything) whose name contains search,
   ignoring ASCII case. */
static void listing_filter(struct listing *listing, bool show_hidden, const char *search)
{
    listing->shown_count = 0;
    for (size_t i = 0; i < listing->count; ++i) {
        const struct entry *entry = &listing->entries[i];
        if ((show_hidden || !entry->hidden) && (!search || !*search || strcasestr(entry->name, search)))
            listing->shown[listing->shown_count++] = i;
    }
}

/* Count a directory's entries once it's on screen. */
static void count_items(const struct files *files, struct entry *entry)
{
    if (entry->items != COUNT_UNKNOWN)
        return;
    char *path = join_path(files->path, entry->name);
    int fd = path ? open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    free(path);
    DIR *directory = fd >= 0 ? fdopendir(fd) : NULL;
    if (!directory) {
        if (fd >= 0)
            close(fd);
        entry->items = COUNT_UNREADABLE;
        return;
    }
    long items = 0, hidden = 0;
    struct dirent *item;
    while ((item = readdir(directory))) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
            continue;
        ++items;
        hidden += item->d_name[0] == '.';
    }
    closedir(directory);
    entry->items = items;
    entry->hidden_items = hidden;
}

static struct entry *cursor_entry(struct files *files)
{
    struct listing *listing = &files->listing;
    return files->cursor < listing->shown_count ? &listing->entries[listing->shown[files->cursor]] : NULL;
}

/* Find a shown entry by name. */
static bool find_shown(const struct listing *listing, const char *name, size_t *index)
{
    for (size_t i = 0; name && i < listing->shown_count; ++i) {
        if (strcmp(listing->entries[listing->shown[i]].name, name) == 0) {
            *index = i;
            return true;
        }
    }
    return false;
}

/* Put the cursor on the entry with this name, if shown. */
static bool select_name(struct files *files, const char *name)
{
    return find_shown(&files->listing, name, &files->cursor);
}

/* Remembered selections */

static struct memory *find_memory(struct files *files, const char *path)
{
    for (size_t i = 0; i < files->memory_count; ++i) {
        if (strcmp(files->memories[i].path, path) == 0)
            return &files->memories[i];
    }
    return NULL;
}

static void remember(struct files *files)
{
    struct entry *entry = cursor_entry(files);
    if (!entry || !files->path)
        return;
    char *name = strdup(entry->name);
    if (!name)
        return;
    struct memory *memory = find_memory(files, files->path);
    if (memory) {
        free(memory->name);
        memory->name = name;
        return;
    }
    if (files->memory_count == files->memory_capacity) {
        size_t capacity = files->memory_capacity ? files->memory_capacity * 2 : 16;
        struct memory *memories = realloc(files->memories, capacity * sizeof *memories);
        if (!memories) {
            free(name);
            return;
        }
        files->memories = memories;
        files->memory_capacity = capacity;
    }
    char *path = strdup(files->path);
    if (!path) {
        free(name);
        return;
    }
    files->memories[files->memory_count++] = (struct memory){path, name};
}

/* Watching */

static void watch_directory(struct files *files)
{
    if (files->inotify < 0)
        return;
    if (files->watch >= 0)
        inotify_rm_watch(files->inotify, files->watch);
    files->watch = inotify_add_watch(files->inotify, files->path,
                                     IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                                         IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
}

static void arm_refresh(struct files *files)
{
    if (files->refresh < 0 || files->refresh_armed)
        return;
    struct itimerspec spec = {.it_value = {0, REFRESH_DELAY_MS * 1000000L}};
    if (timerfd_settime(files->refresh, 0, &spec, NULL) == 0)
        files->refresh_armed = true;
}

/* Protection */

/* Whether entries in directory may be created, renamed or removed. Root may
   change anything; everyone else only what's really inside their home, with
   symlinks resolved, so a slip can't delete the system. Every file
   operation (0.4) asks this of the directories it changes. */
static bool may_modify(const struct files *files, const char *directory)
{
    if (files->root)
        return true;
    if (!files->home_real || strcmp(files->home_real, "/") == 0)
        return false;
    char *real = realpath(directory, NULL);
    if (!real)
        return false;
    size_t length = strlen(files->home_real);
    bool inside = strncmp(real, files->home_real, length) == 0 && (real[length] == '/' || real[length] == 0);
    free(real);
    return inside;
}

/* State */

/* $XDG_STATE_HOME/files/state, or ~/.local/state/files/state. */
static char *state_path(void)
{
    const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char *path = NULL;
    int length = -1;
    if (state && state[0] == '/')
        length = asprintf(&path, "%s/files/state", state);
    else if (home && home[0])
        length = asprintf(&path, "%s/.local/state/files/state", home);
    return length < 0 ? NULL : path;
}

static void state_entry(void *data, const char *section, const char *key, const char *value, int line)
{
    (void)line;
    struct files *files = data;
    if (!key || strcmp(section, "layout") != 0)
        return;
    if (strcmp(key, "sidebar") == 0) {
        char *end;
        double width = strtod(value, &end);
        if (end != value && !*end && width >= SIDEBAR_MIN && width <= 4000)
            files->sidebar_width = width;
    } else if (strcmp(key, "show_sidebar") == 0) {
        files->show_sidebar = strcmp(value, "no") != 0;
    } else if (strcmp(key, "view") == 0) {
        files->view = strcmp(value, "icons") == 0 ? VIEW_ICONS : VIEW_LIST;
    } else if (strcmp(key, "list_zoom") == 0 || strcmp(key, "icon_size") == 0) {
        bool list = key[0] == 'l';
        const double *levels = list ? list_zooms : icon_zooms;
        int count = list ? LIST_ZOOMS : ICON_ZOOMS;
        double wanted = strtod(value, NULL);
        for (int i = 0; i < count; ++i) {
            if (fabs(levels[i] - wanted) < 0.001)
                files->zoom[list ? VIEW_LIST : VIEW_ICONS] = i;
        }
    }
}

static void load_state(struct files *files)
{
    files->sidebar_width = SIDEBAR_WIDTH;
    files->show_sidebar = true;
    files->view = VIEW_LIST;
    memcpy(files->zoom, default_zoom, sizeof files->zoom);
    if (files->state_path)
        desk_ini_load(files->state_path, state_entry, files);
}

/* Write the state atomically. Failing only loses the layout, so it's quiet. */
static void save_state(struct files *files)
{
    if (!files->state_path)
        return;
    char *directory = parent_path(files->state_path);
    if (!directory || desk_mkdirs(directory, 0700) != 0) {
        free(directory);
        return;
    }
    free(directory);
    DeskAtomic atomic;
    if (desk_atomic_open(&atomic, files->state_path, 0600) != 0)
        return;
    fprintf(atomic.file,
            "# Written by files. The sidebar's width and the icon size in logical pixels.\n[layout]\n"
            "sidebar = %.0f\nshow_sidebar = %s\nview = %s\nlist_zoom = %.3f\nicon_size = %.0f\n",
            files->sidebar_width, files->show_sidebar ? "yes" : "no", files->view == VIEW_ICONS ? "icons" : "list",
            list_zooms[files->zoom[VIEW_LIST]], icon_zooms[files->zoom[VIEW_ICONS]]);
    desk_atomic_commit(&atomic, true);
    desk_atomic_abort(&atomic); /* Frees what's left, after success or failure. */
}

/* Places */

static bool is_directory(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static void places_free(struct place *places, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        free(places[i].name);
        free(places[i].path);
    }
    free(places);
}

/* Append a place, copying name and path; a NULL path makes a heading. */
static void add_place(struct place **places, size_t *count, const char *name, const char *path, uint32_t icon)
{
    struct place *grown = realloc(*places, (*count + 1) * sizeof *grown);
    if (!grown)
        return;
    *places = grown;
    char *name_copy = strdup(name), *path_copy = path ? strdup(path) : NULL;
    if (!name_copy || (path && !path_copy)) {
        free(name_copy);
        free(path_copy);
        return;
    }
    grown[(*count)++] = (struct place){name_copy, path_copy, icon};
}

/* A folder from user-dirs.dirs (XDG_key_DIR="$HOME/..."), else ~/fallback. */
static char *user_dir(const struct files *files, const char *key, const char *fallback)
{
    const char *config = getenv("XDG_CONFIG_HOME");
    char *file = NULL, *found = NULL;
    if (config && config[0] == '/') {
        if (asprintf(&file, "%s/user-dirs.dirs", config) < 0)
            file = NULL;
    } else if (asprintf(&file, "%s/.config/user-dirs.dirs", files->home) < 0) {
        file = NULL;
    }
    FILE *stream = file ? fopen(file, "re") : NULL;
    free(file);
    if (stream) {
        char line[PATH_MAX + 64], prefix[64];
        snprintf(prefix, sizeof prefix, "XDG_%s_DIR=\"", key);
        size_t prefix_length = strlen(prefix);
        while (!found && fgets(line, sizeof line, stream)) {
            if (strncmp(line, prefix, prefix_length) != 0)
                continue;
            char *value = line + prefix_length, *quote = strrchr(value, '"');
            if (!quote)
                continue;
            *quote = 0;
            if (strncmp(value, "$HOME", 5) == 0 && (value[5] == '/' || value[5] == 0)) {
                if (asprintf(&found, "%s%s", files->home, value + 5) < 0)
                    found = NULL;
            } else if (value[0] == '/') {
                found = strdup(value);
            }
        }
        fclose(stream);
    }
    if (!found && asprintf(&found, "%s/%s", files->home, fallback) < 0)
        found = NULL;
    size_t length = found ? strlen(found) : 0;
    while (length > 1 && found[length - 1] == '/')
        found[--length] = 0;
    return found;
}

/* Undo mountinfo's octal escapes (\040 for a space) in place. */
static void unescape_mount(char *text)
{
    char *out = text;
    for (const char *in = text; *in;) {
        if (in[0] == '\\' && in[1] >= '0' && in[1] <= '3' && in[2] >= '0' && in[2] <= '7' && in[3] >= '0' &&
            in[3] <= '7') {
            *out++ = (char)((in[1] - '0') * 64 + (in[2] - '0') * 8 + (in[3] - '0'));
            in += 4;
        } else {
            *out++ = *in++;
        }
    }
    *out = 0;
}

/* Drives mounted where removable ones go: /run/media, /media and /mnt. */
static void add_drives(struct place **places, size_t *count)
{
    FILE *stream = fopen("/proc/self/mountinfo", "re");
    if (!stream)
        return;
    static const char *const roots[] = {"/run/media/", "/media/", "/mnt/"};
    bool heading = false;
    char line[PATH_MAX * 2 + 256];
    while (fgets(line, sizeof line, stream)) {
        /* The mount point is the fifth field. */
        char *save, *field = strtok_r(line, " ", &save);
        for (int i = 1; field && i < 5; ++i)
            field = strtok_r(NULL, " ", &save);
        if (!field)
            continue;
        unescape_mount(field);
        bool removable = false;
        for (size_t i = 0; i < sizeof roots / sizeof *roots; ++i)
            removable |= strncmp(field, roots[i], strlen(roots[i])) == 0;
        if (!removable)
            continue;
        /* /run/media/$USER itself can be a mount: only what's inside it counts. */
        if (strncmp(field, "/run/media/", 11) == 0 && !strchr(field + 11, '/'))
            continue;
        bool seen = false;
        for (size_t i = 0; i < *count && !seen; ++i)
            seen = (*places)[i].path && strcmp((*places)[i].path, field) == 0;
        if (seen)
            continue;
        if (!heading) {
            add_place(places, count, "Drives", NULL, 0);
            heading = true;
        }
        add_place(places, count, base_name(field), field, ICON_DRIVE);
    }
    fclose(stream);
}

/* The sidebar: home and the usual folders in it that exist, the whole file
   system, then mounted drives. Made again on every load, so new folders
   and drives show up. */
static void build_places(struct files *files)
{
    struct place *places = NULL;
    size_t count = 0;
    if (files->home && is_directory(files->home)) {
        add_place(&places, &count, "Home", files->home, ICON_HOME);
        static const struct {
            const char *key, *fallback;
            uint32_t icon;
        } folders[] = {
            {"DESKTOP", "Desktop", 0xf108},  {"DOCUMENTS", "Documents", 0xf0f6},
            {"DOWNLOAD", "Downloads", 0xf019}, {"MUSIC", "Music", 0xf001},
            {"PICTURES", "Pictures", 0xf03e}, {"VIDEOS", "Videos", 0xf03d},
        };
        for (size_t i = 0; i < sizeof folders / sizeof *folders; ++i) {
            char *path = user_dir(files, folders[i].key, folders[i].fallback);
            if (path && strcmp(path, files->home) != 0 && is_directory(path))
                add_place(&places, &count, base_name(path), path, folders[i].icon);
            free(path);
        }
    }
    add_place(&places, &count, "File system", "/", ICON_DISK);
    add_drives(&places, &count);
    places_free(files->places, files->place_count);
    files->places = places;
    files->place_count = count;
}

/* Navigation */

static void update_title(struct files *files)
{
    char title[PATH_MAX + 2];
    display_path(files, files->path, title, sizeof title);
    desk_window_set_title(&files->window, title);
}

static void filter(struct files *files)
{
    listing_filter(&files->listing, files->show_hidden, files->search);
}

/* Replace the listing with path's, keeping the cursor on keep (a name) when
   it's still there, else near the old position. */
static void load(struct files *files, const char *keep, size_t near)
{
    listing_free(&files->listing);
    listing_read(&files->listing, files->path);
    filter(files);
    build_places(files);
    files->writable = may_modify(files, files->path);
    if (!select_name(files, keep))
        files->cursor = near < files->listing.shown_count ? near
                      : files->listing.shown_count ? files->listing.shown_count - 1
                                                   : 0;
}

/* Show directory path (taken over), selecting select or what was selected
   there last time, without touching the history. A search ends there. */
static void enter(struct files *files, char *path, const char *select)
{
    remember(files);
    free(files->path);
    files->path = path;
    files->top = 0;
    files->free_scroll = false;
    files->press.kind = TARGET_NONE; /* A click there and one here aren't a double-click. */
    files->search[0] = 0;
    files->search_cursor = 0;
    files->search_focus = false;
    struct memory *memory = find_memory(files, path);
    load(files, select ? select : memory ? memory->name : NULL, 0);
    watch_directory(files);
    if (files->window.toplevel)
        update_title(files);
    desk_window_redraw(&files->window);
}

static bool history_push(struct history *history, char *path)
{
    if (history->count == HISTORY_LIMIT) {
        free(history->paths[0]);
        memmove(history->paths, history->paths + 1, (history->count - 1) * sizeof *history->paths);
        --history->count;
    }
    if (history->count == history->capacity) {
        size_t capacity = history->capacity ? history->capacity * 2 : 16;
        char **paths = realloc(history->paths, capacity * sizeof *paths);
        if (!paths)
            return false;
        history->paths = paths;
        history->capacity = capacity;
    }
    history->paths[history->count++] = path;
    return true;
}

static void history_clear(struct history *history)
{
    for (size_t i = 0; i < history->count; ++i)
        free(history->paths[i]);
    history->count = 0;
}

static void history_free(struct history *history)
{
    history_clear(history);
    free(history->paths);
    *history = (struct history){0};
}

/* Go to directory path (taken over), remembering where we were for back. */
static void go_to(struct files *files, char *path, const char *select)
{
    if (files->path && strcmp(files->path, path) != 0) {
        char *here = strdup(files->path);
        if (here && !history_push(&files->back, here))
            free(here);
        history_clear(&files->forward);
    }
    enter(files, path, select);
}

/* Back (from back, onto forward) or forward (the other way), skipping
   directories that have gone since. */
static void travel(struct files *files, struct history *from, struct history *to)
{
    while (from->count) {
        char *path = from->paths[--from->count];
        if (!is_directory(path)) {
            free(path);
            continue;
        }
        char *here = strdup(files->path);
        if (here && !history_push(to, here))
            free(here);
        enter(files, path, NULL);
        return;
    }
}

static void go_back(struct files *files)
{
    travel(files, &files->back, &files->forward);
}

static void go_forward(struct files *files)
{
    travel(files, &files->forward, &files->back);
}

static void go_up(struct files *files)
{
    if (strcmp(files->path, "/") == 0)
        return;
    char *parent = parent_path(files->path);
    char *came_from = strdup(base_name(files->path));
    if (parent && came_from)
        go_to(files, parent, came_from);
    else
        free(parent);
    free(came_from);
}

static void go_in(struct files *files)
{
    struct entry *entry = cursor_entry(files);
    if (!entry || !entry->directory)
        return; /* Opening files comes in 0.3. */
    char *path = join_path(files->path, entry->name);
    if (path)
        go_to(files, path, NULL);
}

static void go_home(struct files *files)
{
    char *home = strdup(files->home ? files->home : "/");
    if (home)
        go_to(files, home, NULL);
}

/* The nearest ancestor of path (taken over) that exists, or "/". */
static char *existing(char *path)
{
    while (path && strcmp(path, "/") != 0 && !is_directory(path)) {
        char *parent = parent_path(path);
        free(path);
        path = parent;
    }
    return path ? path : strdup("/");
}

/* After the current directory vanished. */
static void go_to_existing(struct files *files)
{
    char *path = existing(strdup(files->path));
    if (path)
        go_to(files, path, NULL);
}

static void refresh(struct files *files)
{
    struct entry *entry = cursor_entry(files);
    char *keep = entry ? strdup(entry->name) : NULL;
    load(files, keep, files->cursor);
    free(keep);
    desk_window_redraw(&files->window);
}

/* Drain inotify, and refresh or move up when the directory itself went. */
static void watch_events(struct files *files)
{
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool gone = false, changed = false;
    ssize_t length;
    while ((length = read(files->inotify, buffer, sizeof buffer)) > 0) {
        for (char *at = buffer; at < buffer + length;) {
            const struct inotify_event *event = (const struct inotify_event *)at;
            at += sizeof *event + event->len;
            if (event->wd != files->watch)
                continue;
            if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED))
                gone = true;
            else
                changed = true;
        }
    }
    if (gone) {
        files->watch = -1;
        go_to_existing(files);
    } else if (changed) {
        arm_refresh(files);
    }
}

/* Moving and scrolling. The view scrolls by lines: a row of the list, or a
   row of icons. */

static size_t columns(const struct files *files)
{
    return files->view == VIEW_ICONS && files->panes.columns > 0 ? (size_t)files->panes.columns : 1;
}

static size_t line_count(const struct files *files)
{
    size_t per_line = columns(files);
    return (files->listing.shown_count + per_line - 1) / per_line;
}

static void move_cursor(struct files *files, long delta)
{
    size_t count = files->listing.shown_count;
    if (!count)
        return;
    long target = (long)files->cursor + delta;
    if (target < 0)
        target = 0;
    if (target >= (long)count)
        target = (long)count - 1;
    if ((size_t)target != files->cursor || files->free_scroll) {
        files->cursor = (size_t)target;
        files->free_scroll = false;
        desk_window_redraw(&files->window);
    }
}

/* A page of lines, in entries. */
static long page(const struct files *files)
{
    return (files->page_rows > 1 ? files->page_rows - 1 : 1) * (long)columns(files);
}

/* The wheel: move the view by lines without moving the selection. */
static void scroll_lines(struct files *files, long lines)
{
    long last_top = (long)line_count(files) - files->page_rows;
    long top = (long)files->top + lines;
    if (top > last_top)
        top = last_top;
    if (top < 0)
        top = 0;
    if ((size_t)top != files->top) {
        files->top = (size_t)top;
        files->free_scroll = true;
        desk_window_redraw(&files->window);
    }
}

/* Half a page, moving the view with the cursor like a pager. */
static void scroll_half(struct files *files, int direction)
{
    long half = files->page_rows > 1 ? files->page_rows / 2 : 1;
    long top = (long)files->top + direction * half;
    long last_top = (long)line_count(files) - files->page_rows;
    if (top > last_top)
        top = last_top;
    if (top < 0)
        top = 0;
    files->top = (size_t)top;
    move_cursor(files, direction * half * (long)columns(files));
    desk_window_redraw(&files->window);
}

/* Filter again after show_hidden or the search changed, keeping the cursor
   on the same entry or the next one still shown. */
static void refilter(struct files *files)
{
    struct listing *listing = &files->listing;
    size_t old = files->cursor < listing->shown_count ? listing->shown[files->cursor] : 0;
    filter(files);
    files->cursor = 0;
    for (size_t i = 0; i < listing->shown_count; ++i) {
        files->cursor = i;
        if (listing->shown[i] >= old)
            break;
    }
    files->free_scroll = false;
    desk_window_redraw(&files->window);
}

static void toggle_hidden(struct files *files)
{
    files->show_hidden = !files->show_hidden;
    refilter(files);
}

/* The sidebar */

/* Its width in logical pixels, kept between SIDEBAR_MIN and half the window. */
static void set_sidebar_width(struct files *files, double width)
{
    double most = files->window.width / 2.0;
    if (width > most)
        width = most;
    if (width < SIDEBAR_MIN)
        width = SIDEBAR_MIN;
    if (width != files->sidebar_width) {
        files->sidebar_width = width;
        desk_window_redraw(&files->window);
    }
}

/* < and >: narrower or wider by a step. */
static void resize_sidebar(struct files *files, int direction)
{
    if (!files->panes.sidebar)
        return;
    set_sidebar_width(files, files->sidebar_width + direction * SIDEBAR_STEP);
    save_state(files);
}

static void reset_sidebar(struct files *files)
{
    set_sidebar_width(files, SIDEBAR_WIDTH);
    save_state(files);
}

static void toggle_sidebar(struct files *files)
{
    files->show_sidebar = !files->show_sidebar;
    save_state(files);
    desk_window_redraw(&files->window);
}

/* Views and zoom */

static void set_view(struct files *files, enum view view)
{
    if (files->view == view)
        return;
    files->view = view;
    files->free_scroll = false;
    save_state(files);
    desk_window_redraw(&files->window);
}

/* One step bigger (+1) or smaller (-1), or back to the default (0), in the
   current view. */
static void zoom(struct files *files, int direction)
{
    int count = files->view == VIEW_LIST ? LIST_ZOOMS : ICON_ZOOMS;
    int level = direction ? files->zoom[files->view] + direction : default_zoom[files->view];
    if (level < 0)
        level = 0;
    if (level >= count)
        level = count - 1;
    if (level == files->zoom[files->view])
        return;
    files->zoom[files->view] = level;
    files->free_scroll = false;
    save_state(files);
    desk_window_redraw(&files->window);
}

/* Tabs */

static void tab_free(struct tab *tab)
{
    free(tab->path);
    free(tab->selected);
    free(tab->search);
    history_free(&tab->back);
    history_free(&tab->forward);
    *tab = (struct tab){0};
}

/* Put the current tab's state into its slot before leaving it. */
static void tab_store(struct files *files)
{
    struct tab *tab = &files->tabs[files->current_tab];
    tab_free(tab);
    struct entry *entry = cursor_entry(files);
    tab->path = strdup(files->path);
    tab->selected = entry ? strdup(entry->name) : NULL;
    tab->search = files->search[0] ? strdup(files->search) : NULL;
    tab->top = files->top;
    tab->back = files->back;
    tab->forward = files->forward;
    files->back = files->forward = (struct history){0};
}

/* Make tab index the current one, from what its slot kept. */
static void tab_load(struct files *files, size_t index)
{
    struct tab *tab = &files->tabs[index];
    files->current_tab = index;
    history_free(&files->back);
    history_free(&files->forward);
    files->back = tab->back;
    files->forward = tab->forward;
    tab->back = tab->forward = (struct history){0};
    char *path = existing(tab->path ? tab->path : strdup("/"));
    tab->path = NULL;
    if (path)
        enter(files, path, tab->selected);
    files->top = tab->top;
    if (tab->search) {
        snprintf(files->search, sizeof files->search, "%s", tab->search);
        files->search_cursor = strlen(files->search);
        refilter(files);
        select_name(files, tab->selected);
    }
    free(tab->selected);
    free(tab->search);
    tab->selected = tab->search = NULL;
    desk_window_redraw(&files->window);
}

static void switch_tab(struct files *files, size_t index)
{
    if (index >= files->tab_count || index == files->current_tab)
        return;
    tab_store(files);
    tab_load(files, index);
}

/* A tab for path (taken over), after the current one: switched to, or left
   in the background. */
static void new_tab(struct files *files, char *path, bool switch_to)
{
    struct tab *tabs = realloc(files->tabs, (files->tab_count + 1) * sizeof *tabs);
    if (!tabs) {
        free(path);
        return;
    }
    files->tabs = tabs;
    size_t at = files->current_tab + 1;
    memmove(&tabs[at + 1], &tabs[at], (files->tab_count - at) * sizeof *tabs);
    tabs[at] = (struct tab){.path = path};
    ++files->tab_count;
    if (switch_to)
        switch_tab(files, at);
    else
        desk_window_redraw(&files->window);
}

/* Close a tab; closing the last one closes the window. */
static void close_tab(struct files *files, size_t index)
{
    if (index >= files->tab_count)
        return;
    if (files->tab_count == 1) {
        files->running = false;
        return;
    }
    bool current = index == files->current_tab;
    tab_free(&files->tabs[index]);
    memmove(&files->tabs[index], &files->tabs[index + 1], (files->tab_count - index - 1) * sizeof *files->tabs);
    --files->tab_count;
    if (current)
        tab_load(files, index < files->tab_count ? index : files->tab_count - 1);
    else if (index < files->current_tab)
        --files->current_tab;
    desk_window_redraw(&files->window);
}

static void next_tab(struct files *files, int direction)
{
    size_t count = files->tab_count;
    switch_tab(files, (files->current_tab + count + (size_t)(direction > 0 ? 1 : count - 1)) % count);
}

/* What a tab is called: Home, /, or its directory's name. */
static const char *tab_title(const struct files *files, const char *path)
{
    if (files->home && strcmp(path, files->home) == 0)
        return "Home";
    return strcmp(path, "/") == 0 ? "/" : base_name(path);
}

static const char *tab_path(const struct files *files, size_t index)
{
    return index == files->current_tab || !files->tabs[index].path ? files->path : files->tabs[index].path;
}

/* Search */

static void search_focus(struct files *files, bool focus)
{
    if (files->search_focus != focus) {
        files->search_focus = focus;
        desk_window_redraw(&files->window);
    }
}

static void search_changed(struct files *files)
{
    refilter(files);
    files->top = 0;
}

static void search_clear(struct files *files)
{
    if (files->search[0]) {
        files->search[0] = 0;
        files->search_cursor = 0;
        search_changed(files);
    }
}

/* The start of the character before at, or of the word before it. */
static size_t back_char(const char *text, size_t at)
{
    while (at > 0 && ((unsigned char)text[--at] & 0xc0) == 0x80)
        ;
    return at;
}

static size_t forward_char(const char *text, size_t at)
{
    size_t length = strlen(text);
    if (at < length)
        ++at;
    while (at < length && ((unsigned char)text[at] & 0xc0) == 0x80)
        ++at;
    return at;
}

static size_t back_word(const char *text, size_t at)
{
    while (at > 0 && text[at - 1] == ' ')
        --at;
    while (at > 0 && text[at - 1] != ' ')
        --at;
    return at;
}

static size_t forward_word(const char *text, size_t at)
{
    size_t length = strlen(text);
    while (at < length && text[at] == ' ')
        ++at;
    while (at < length && text[at] != ' ')
        ++at;
    return at;
}

static void search_delete(struct files *files, size_t from, size_t to)
{
    if (from >= to)
        return;
    memmove(files->search + from, files->search + to, strlen(files->search + to) + 1);
    files->search_cursor = from;
    search_changed(files);
}

static void search_insert(struct files *files, const char *text, size_t length)
{
    size_t used = strlen(files->search);
    if (used + length >= sizeof files->search)
        return;
    char *at = files->search + files->search_cursor;
    memmove(at + length, at, used - files->search_cursor + 1);
    memcpy(at, text, length);
    files->search_cursor += length;
    search_changed(files);
}

static void search_move(struct files *files, size_t to)
{
    if (to != files->search_cursor) {
        files->search_cursor = to;
        desk_window_redraw(&files->window);
    }
}

/* A key while the search field has the focus. Returns whether it repeats,
   or -1 when it isn't the field's, so the window handles it. */
static int search_key(struct files *files, uint32_t key, xkb_keysym_t sym, unsigned mods)
{
    const char *text = files->search;
    size_t at = files->search_cursor;
    bool word = mods & DESK_MOD_CTRL;
    switch (sym) {
    case XKB_KEY_Escape:
        search_clear(files);
        search_focus(files, false);
        return 0;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_Tab:
    case XKB_KEY_Down:
        search_focus(files, false);
        return 0;
    case XKB_KEY_BackSpace:
        search_delete(files, word ? back_word(text, at) : back_char(text, at), at);
        return 1;
    case XKB_KEY_Delete:
        search_delete(files, at, word ? forward_word(text, at) : forward_char(text, at));
        return 1;
    case XKB_KEY_Left:
        search_move(files, word ? back_word(text, at) : back_char(text, at));
        return 1;
    case XKB_KEY_Right:
        search_move(files, word ? forward_word(text, at) : forward_char(text, at));
        return 1;
    case XKB_KEY_Home:
        search_move(files, 0);
        return 0;
    case XKB_KEY_End:
        search_move(files, strlen(text));
        return 0;
    default:
        break;
    }
    if (mods & (DESK_MOD_CTRL | DESK_MOD_ALT | DESK_MOD_LOGO))
        return -1;
    char typed[16];
    int length = files->keyboard.state ? xkb_state_key_get_utf8(files->keyboard.state, key + 8, typed, sizeof typed)
                                       : 0;
    if (length <= 0 || (unsigned char)typed[0] < 0x20 || typed[0] == 0x7f)
        return -1;
    search_insert(files, typed, (size_t)length);
    return 1;
}

/* Keys */

/* Act on a key. Return whether it repeats while held. */
static bool key_action(struct files *files, xkb_keysym_t sym, unsigned mods)
{
    bool icons = files->view == VIEW_ICONS;
    long line = (long)columns(files);
    if (mods & DESK_MOD_CTRL) {
        switch (sym) {
        case XKB_KEY_d: scroll_half(files, 1); return true;
        case XKB_KEY_u: scroll_half(files, -1); return true;
        case XKB_KEY_q: files->running = false; return false;
        case XKB_KEY_t: new_tab(files, strdup(files->path), true); return false;
        case XKB_KEY_w: close_tab(files, files->current_tab); return false;
        case XKB_KEY_Tab:
        case XKB_KEY_Page_Down: next_tab(files, 1); return false;
        case XKB_KEY_ISO_Left_Tab:
        case XKB_KEY_Page_Up: next_tab(files, -1); return false;
        case XKB_KEY_f: search_focus(files, true); return false;
        case XKB_KEY_1: set_view(files, VIEW_LIST); return false;
        case XKB_KEY_2: set_view(files, VIEW_ICONS); return false;
        case XKB_KEY_plus:
        case XKB_KEY_equal:
        case XKB_KEY_KP_Add: zoom(files, 1); return true;
        case XKB_KEY_minus:
        case XKB_KEY_KP_Subtract: zoom(files, -1); return true;
        case XKB_KEY_0:
        case XKB_KEY_KP_0: zoom(files, 0); return false;
        default: return false;
        }
    }
    if (mods & DESK_MOD_ALT) {
        if (sym >= XKB_KEY_1 && sym <= XKB_KEY_8) {
            switch_tab(files, sym - XKB_KEY_1);
            return false;
        }
        switch (sym) {
        case XKB_KEY_9: switch_tab(files, files->tab_count - 1); return false;
        case XKB_KEY_Left: go_back(files); return false;
        case XKB_KEY_Right: go_forward(files); return false;
        case XKB_KEY_Up: go_up(files); return false;
        default: return false;
        }
    }
    if (mods & DESK_MOD_LOGO)
        return false;
    switch (sym) {
    case XKB_KEY_j:
    case XKB_KEY_Down:
        move_cursor(files, line);
        return true;
    case XKB_KEY_k:
    case XKB_KEY_Up:
        move_cursor(files, -line);
        return true;
    case XKB_KEY_Right:
        if (icons)
            move_cursor(files, 1);
        else
            go_in(files);
        return true;
    case XKB_KEY_Left:
        if (icons)
            move_cursor(files, -1);
        else
            go_up(files);
        return true;
    case XKB_KEY_l:
        go_in(files);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        go_in(files);
        return false;
    case XKB_KEY_h:
    case XKB_KEY_BackSpace:
        go_up(files);
        return true;
    case XKB_KEY_g:
    case XKB_KEY_Home:
        move_cursor(files, -(long)files->listing.shown_count);
        return false;
    case XKB_KEY_G:
    case XKB_KEY_End:
        move_cursor(files, (long)files->listing.shown_count);
        return false;
    case XKB_KEY_Page_Down:
        move_cursor(files, page(files));
        return true;
    case XKB_KEY_Page_Up:
        move_cursor(files, -page(files));
        return true;
    case XKB_KEY_minus:
        go_back(files);
        return false;
    case XKB_KEY_less:
        resize_sidebar(files, -1);
        return true;
    case XKB_KEY_greater:
        resize_sidebar(files, 1);
        return true;
    case XKB_KEY_equal:
        reset_sidebar(files);
        return false;
    case XKB_KEY_b:
    case XKB_KEY_F9:
        toggle_sidebar(files);
        return false;
    case XKB_KEY_period:
        toggle_hidden(files);
        return false;
    case XKB_KEY_asciitilde:
        go_home(files);
        return false;
    case XKB_KEY_Escape:
        search_clear(files);
        return false;
    case XKB_KEY_q:
        files->running = false;
        return false;
    default:
        return false;
    }
}

/* A key press or repeat: to the search field when it has the focus, else
   to the window. Returns whether it repeats. */
static bool handle_key(struct files *files, uint32_t key)
{
    xkb_keysym_t sym = desk_keyboard_sym(&files->keyboard, key);
    unsigned mods = desk_keyboard_mods(&files->keyboard);
    if (files->search_focus) {
        int repeats = search_key(files, key, sym, mods);
        if (repeats >= 0)
            return repeats;
    }
    return key_action(files, sym, mods);
}

/* Drawing */

static double font_pixels(const struct files *files)
{
    return FONT_POINTS * 96.0 / 72.0 * desk_window_scale(&files->window);
}

static void resize_font(DeskFont *font, double size)
{
    if (desk_font_size(font) != size)
        desk_font_resize(font, size);
}

static void fonts_resize(struct files *files)
{
    double size = font_pixels(files), list = size * list_zooms[files->zoom[VIEW_LIST]];
    resize_font(files->font, size);
    resize_font(files->italic, size);
    resize_font(files->mono, size * 0.9);
    resize_font(files->list_font, list);
    resize_font(files->list_italic, list);
    resize_font(files->list_mono, list * 0.9);
    resize_font(files->icon_font, icon_zooms[files->zoom[VIEW_ICONS]] * desk_window_scale(&files->window));
}

static bool has_suffix(const char *name, const char *const *suffixes)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return false;
    for (; *suffixes; ++suffixes) {
        if (strcasecmp(dot + 1, *suffixes) == 0)
            return true;
    }
    return false;
}

static uint32_t entry_icon(const struct entry *entry)
{
    static const char *const images[] = {"png", "jpg", "jpeg", "jpe", "gif", "webp", "svg", "bmp", "avif",
                                         "heic", "tif", "tiff", "ico", "jxl", NULL};
    static const char *const videos[] = {"mp4", "mkv", "webm", "mov", "avi", "m4v", NULL};
    static const char *const audio[] = {"mp3", "flac", "ogg", "opus", "wav", "m4a", "aac", NULL};
    static const char *const archives[] = {"zip", "tar", "gz", "tgz", "xz", "zst", "bz2", "7z", "rar", "lz4",
                                           "deb", "rpm", NULL};
    static const char *const code[] = {"c", "h", "cc", "cpp", "hpp", "py", "js", "ts", "rs", "go", "sh",
                                       "lua", "java", "rb", "zig", "json", "xml", "html", "css", "mk", NULL};
    static const char *const text[] = {"txt", "conf", "ini", "cfg", "log", "toml", "yaml", "yml", "csv", NULL};
    static const char *const markdown[] = {"md", "markdown", NULL};
    static const char *const pdf[] = {"pdf", NULL};
    if (entry->directory)
        return entry->link ? ICON_FOLDER_LINK : ICON_FOLDER;
    if (entry->link)
        return ICON_FILE_LINK;
    if (has_suffix(entry->name, images)) return ICON_IMAGE;
    if (has_suffix(entry->name, videos)) return ICON_VIDEO;
    if (has_suffix(entry->name, audio)) return ICON_AUDIO;
    if (has_suffix(entry->name, archives)) return ICON_ARCHIVE;
    if (has_suffix(entry->name, pdf)) return ICON_PDF;
    if (has_suffix(entry->name, markdown)) return ICON_MARKDOWN;
    if (has_suffix(entry->name, code)) return ICON_CODE;
    if (has_suffix(entry->name, text)) return ICON_TEXT;
    if (entry->executable) return ICON_EXECUTABLE;
    if (strcmp(entry->name, "Makefile") == 0) return ICON_CODE;
    return ICON_FILE;
}

static void format_size(off_t size, char *out, size_t length)
{
    static const char units[] = "BKMGTPE";
    double value = (double)size;
    int unit = 0;
    while (value >= 1024 && units[unit + 1]) {
        value /= 1024;
        ++unit;
    }
    if (unit == 0)
        snprintf(out, length, "%lld B", (long long)size);
    else if (value < 10)
        snprintf(out, length, "%.1f %c", value, units[unit]);
    else
        snprintf(out, length, "%.0f %c", value, units[unit]);
}

/* The date and time, "2026-11-28 14:03". */
static void format_date(const struct timespec *when, char *out, size_t length)
{
    struct tm local;
    if (when->tv_sec == 0 || !localtime_r(&when->tv_sec, &local)) {
        snprintf(out, length, "-");
        return;
    }
    if (!strftime(out, length, "%Y-%m-%d %H:%M", &local))
        snprintf(out, length, "-");
}

/* The name's tail kept when cutting the middle out: a few characters and the
   extension, as in "a-very-long-na…me.tar.gz". */
static size_t tail_start(const char *name, size_t length)
{
    const char *dot = memrchr(name, '.', length);
    if (!dot || dot == name || name + length - dot > 12)
        return length;
    /* Include ".tar" in ".tar.gz" and the like. */
    if (dot - name > 4 && strncasecmp(dot - 4, ".tar", 4) == 0)
        dot -= 4;
    const char *start = dot;
    for (int kept = 0; kept < 2 && start > name; ++kept) {
        do
            --start;
        while (start > name && ((unsigned char)*start & 0xc0) == 0x80);
    }
    return (size_t)(start - name);
}

/* Draw length bytes of a name in width pixels: whole, cut in the middle for
   files, or cut at the end for directories. */
static void draw_name_part(DeskFont *font, DeskCanvas *canvas, int x, int baseline, int width, const char *name,
                           size_t length, bool directory, uint32_t color)
{
    if (desk_text_width(font, name, length) <= width) {
        desk_text_draw(font, canvas, x, baseline, name, length, color);
        return;
    }
    static const char ellipsis[] = "…";
    int ellipsis_width = desk_text_width(font, ellipsis, sizeof ellipsis - 1);
    size_t tail = directory ? length : tail_start(name, length);
    int tail_width = desk_text_width(font, name + tail, length - tail);
    if (tail_width > width / 2) {
        tail = length;
        tail_width = 0;
    }
    int room = width - ellipsis_width - tail_width;
    size_t head = room > 0 ? desk_text_fit(font, name, tail, room) : 0;
    int pen = desk_text_draw(font, canvas, x, baseline, name, head, color);
    pen = desk_text_draw(font, canvas, pen, baseline, ellipsis, sizeof ellipsis - 1, color);
    desk_text_draw(font, canvas, pen, baseline, name + tail, length - tail, color);
}

static void draw_name(DeskFont *font, DeskCanvas *canvas, int x, int baseline, int width, const char *name,
                      bool directory, uint32_t color)
{
    draw_name_part(font, canvas, x, baseline, width, name, strlen(name), directory, color);
}

/* Layout */

/* Sizes in buffer pixels for one frame, for one set of fonts: the window's,
   or the list's at its zoom. */
struct layout {
    DeskFont *font, *italic, *mono;
    int row_height, baseline;   /* baseline: from the top of a row. */
    int padding, gap, icon_width;
    int size_width, date_width;
};

static struct layout make_layout(DeskFont *font, DeskFont *italic, DeskFont *mono, double scale)
{
    DeskFontMetrics metrics = desk_font_metrics(font);
    struct layout layout = {
        .font = font,
        .italic = italic,
        .mono = mono,
        .row_height = (int)lround(metrics.height * ROW_SPACING),
        .padding = (int)lround(PADDING * scale),
        .gap = (int)lround(GAP * scale),
        .icon_width = (int)lround(metrics.height * 1.4),
        .size_width = desk_text_width(mono, "1023 K", 6),
        .date_width = desk_text_width(mono, "2026-11-28 14:03", 16),
    };
    layout.baseline = (layout.row_height - (metrics.ascent + metrics.descent)) / 2 + metrics.ascent;
    return layout;
}

static uint32_t mix(uint32_t a, uint32_t b, unsigned amount)
{
    uint32_t out = 0xff000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        unsigned x = (a >> shift) & 0xff, y = (b >> shift) & 0xff;
        out |= ((x * (255 - amount) + y * amount + 127) / 255) << shift;
    }
    return out;
}

/* Keep the cursor's line on screen with a margin, and the view full. */
static size_t keep_visible(size_t top, size_t cursor, size_t count, int rows)
{
    size_t margin = (size_t)(rows / 4 < SCROLL_MARGIN ? rows / 4 : SCROLL_MARGIN);
    if (cursor < top + margin)
        top = cursor > margin ? cursor - margin : 0;
    if (cursor + margin + 1 > top + (size_t)rows)
        top = cursor + margin + 1 - (size_t)rows;
    if (count <= (size_t)rows)
        top = 0;
    else if (top > count - (size_t)rows)
        top = count - (size_t)rows;
    return top;
}

/* After the wheel: only keep the view full. */
static size_t clamp_top(size_t top, size_t count, int rows)
{
    if (count <= (size_t)rows)
        return 0;
    return top > count - (size_t)rows ? count - (size_t)rows : top;
}

static void encode_utf8(uint32_t code, char *out, size_t *length)
{
    if (code < 0x80) {
        out[0] = (char)code, *length = 1;
    } else if (code < 0x800) {
        out[0] = (char)(0xc0 | code >> 6), out[1] = (char)(0x80 | (code & 0x3f)), *length = 2;
    } else if (code < 0x10000) {
        out[0] = (char)(0xe0 | code >> 12), out[1] = (char)(0x80 | (code >> 6 & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f)), *length = 3;
    } else {
        out[0] = (char)(0xf0 | code >> 18), out[1] = (char)(0x80 | (code >> 12 & 0x3f));
        out[2] = (char)(0x80 | (code >> 6 & 0x3f)), out[3] = (char)(0x80 | (code & 0x3f)), *length = 4;
    }
}

/* A glyph centered between left and right. */
static void draw_icon(DeskFont *font, DeskCanvas *canvas, int left, int right, int baseline, uint32_t code,
                      uint32_t color)
{
    char text[4];
    size_t length;
    encode_utf8(code, text, &length);
    int width = desk_char_width(font, code);
    desk_text_draw(font, canvas, left + (right - left - width) / 2, baseline, text, length, color);
}

/* A message in place of the entries, such as "Empty". */
static void draw_message(DeskCanvas *canvas, const struct layout *layout, int x, int y, const char *message,
                         uint32_t color)
{
    desk_text_draw(layout->italic, canvas, x + layout->padding, y + layout->baseline, message, strlen(message),
                   color);
}

static uint32_t entry_color(const struct files *files, const struct entry *entry, bool selected)
{
    const struct colors *colors = &files->colors;
    if (entry->broken)
        return colors->urgent;
    if (selected)
        return colors->selection_foreground;
    return entry->hidden ? colors->muted : entry->directory ? colors->directory : colors->foreground;
}

/* One row of the list between x0 and x1. details: size and date on the right. */
static void draw_row(struct files *files, DeskCanvas *canvas, const struct layout *layout, struct entry *entry,
                     int x0, int x1, int y, bool selected, bool details)
{
    const struct colors *colors = &files->colors;
    if (selected)
        desk_canvas_fill(canvas, x0, y, x1 - x0, layout->row_height, colors->selection_background);
    uint32_t color = entry_color(files, entry, selected);
    int baseline = y + layout->baseline, inner = layout->padding / 2;
    draw_icon(layout->font, canvas, x0 + inner / 2, x0 + inner / 2 + layout->icon_width, baseline,
              entry_icon(entry), color);

    int name_x = x0 + inner + layout->icon_width;
    int details_width = layout->size_width + layout->gap + layout->date_width;
    int name_right = details ? x1 - details_width - layout->gap - inner : x1 - inner;
    DeskCanvas clipped = *canvas;
    desk_canvas_clip(&clipped, name_x > canvas->clip_left ? name_x : canvas->clip_left, canvas->clip_top,
                     name_right < canvas->clip_right ? name_right : canvas->clip_right, canvas->clip_bottom);
    draw_name(entry->link ? layout->italic : layout->font, &clipped, name_x, baseline, name_right - name_x,
              entry->name, entry->directory, color);
    if (!details)
        return;

    uint32_t detail_color = selected ? colors->selection_foreground : colors->muted;
    char size[32], date[32];
    if (entry->directory) {
        count_items(files, entry);
        long items = files->show_hidden ? entry->items : entry->items - entry->hidden_items;
        if (entry->items == COUNT_UNREADABLE)
            snprintf(size, sizeof size, "?");
        else
            snprintf(size, sizeof size, "%ld", items);
    } else if (entry->broken) {
        snprintf(size, sizeof size, "broken");
    } else {
        format_size(entry->size, size, sizeof size);
    }
    format_date(&entry->modified, date, sizeof date);
    int right = x1 - inner;
    int date_x = right - desk_text_width(layout->mono, date, strlen(date));
    int size_x = right - layout->date_width - layout->gap - desk_text_width(layout->mono, size, strlen(size));
    desk_text_draw(layout->mono, canvas, size_x, baseline, size, strlen(size), detail_color);
    desk_text_draw(layout->mono, canvas, date_x, baseline, date, strlen(date), detail_color);
}

/* Why there's nothing to show, or NULL when there is. */
static const char *empty_message(const struct files *files, uint32_t *color)
{
    const struct listing *listing = &files->listing;
    *color = listing->error ? files->colors.urgent : files->colors.muted;
    if (listing->error)
        return strerror(listing->error);
    if (listing->shown_count)
        return NULL;
    if (files->search[0])
        return "No matches";
    return listing->count ? "Only hidden files (. shows them)" : "Empty";
}

/* The list view: rows from line top. */
static void paint_list(struct files *files, DeskCanvas *canvas, const struct layout *layout, int x0, int x1,
                       int y, int rows)
{
    struct listing *listing = &files->listing;
    int name_room = x1 - x0 - layout->icon_width - layout->padding;
    bool details = name_room > 2 * (layout->size_width + layout->gap + layout->date_width);
    for (int row = 0; row < rows && files->top + (size_t)row < listing->shown_count; ++row) {
        size_t index = files->top + (size_t)row;
        draw_row(files, canvas, layout, &listing->entries[listing->shown[index]], x0, x1,
                 y + row * layout->row_height, index == files->cursor, details);
    }
}

/* Where a name breaks onto a tile's second line: the last space, dot, dash
   or underscore that fits, else as much as fits. */
static size_t tile_break(DeskFont *font, const char *name, size_t length, int width)
{
    size_t fit = desk_text_fit(font, name, length, width);
    if (fit >= length)
        return length;
    for (size_t i = fit; i > fit / 2; --i) {
        if (strchr(" .-_", name[i - 1]))
            return i;
    }
    return fit;
}

/* The icon view: tiles of a big icon over the name in up to two lines. */
static void paint_icons(struct files *files, DeskCanvas *canvas, const struct layout *ui, int x0, int y,
                        int rows)
{
    struct listing *listing = &files->listing;
    const struct panes *panes = &files->panes;
    int tile = panes->tile_width, pad = ui->padding / 2, line = desk_font_metrics(ui->font).height;
    int icon = (int)lround(desk_font_size(files->icon_font));
    DeskFontMetrics icon_metrics = desk_font_metrics(files->icon_font);
    int icon_baseline = pad + (icon - (icon_metrics.ascent + icon_metrics.descent)) / 2 + icon_metrics.ascent;
    size_t per_line = (size_t)panes->columns;
    for (int row = 0; row < rows; ++row) {
        for (size_t column = 0; column < per_line; ++column) {
            size_t index = (files->top + (size_t)row) * per_line + column;
            if (index >= listing->shown_count)
                return;
            struct entry *entry = &listing->entries[listing->shown[index]];
            bool selected = index == files->cursor;
            int left = x0 + (int)column * tile, top = y + row * panes->line_height;
            uint32_t color = entry_color(files, entry, selected);
            if (selected)
                desk_canvas_fill(canvas, left + pad / 2, top, tile - pad, panes->line_height - pad / 2,
                                 files->colors.selection_background);
            draw_icon(files->icon_font, canvas, left, left + tile, top + icon_baseline, entry_icon(entry),
                      selected ? color : entry->broken ? color : entry->hidden ? files->colors.muted
                                                                               : files->colors.directory);
            DeskFont *font = entry->link ? ui->italic : ui->font;
            int text_width = tile - 2 * pad, baseline = top + pad + icon + pad + ui->baseline - (ui->row_height - line) / 2;
            size_t length = strlen(entry->name), cut = tile_break(font, entry->name, length, text_width);
            DeskCanvas clipped = *canvas;
            desk_canvas_clip(&clipped, left + pad > canvas->clip_left ? left + pad : canvas->clip_left,
                             canvas->clip_top,
                             left + tile - pad < canvas->clip_right ? left + tile - pad : canvas->clip_right,
                             canvas->clip_bottom);
            int first = desk_text_width(font, entry->name, cut);
            desk_text_draw(font, &clipped, left + (tile - first) / 2, baseline, entry->name, cut, color);
            if (cut < length) {
                const char *rest = entry->name + cut;
                size_t rest_length = length - cut;
                int second = desk_text_width(font, rest, rest_length);
                int x = second <= text_width ? left + (tile - second) / 2 : left + pad;
                draw_name_part(font, &clipped, x, baseline + line, text_width, rest, rest_length, false, color);
            }
        }
    }
}

/* Toolbar */

#define ICON_SIDEBAR 0xf0c9
#define ICON_BACK 0xf060
#define ICON_FORWARD 0xf061
#define ICON_UP 0xf062
#define ICON_LIST 0xf03a
#define ICON_GRID 0xf00a
#define ICON_SEARCH 0xf422
#define ICON_CLOSE 0xf00d
#define ICON_PLUS 0xf067
#define CRUMB_SEPARATOR "\xe2\x80\xba" /* › */
#define ELLIPSIS "\xe2\x80\xa6"
#define SEARCH_WIDTH 240 /* Logical pixels, at most. */
#define TAB_WIDTH 220    /* Logical pixels, at most. */

static bool hovering(const struct files *files, enum target_kind kind, size_t index)
{
    return !files->drag && files->hover.kind == kind && files->hover.index == index;
}

static bool button_enabled(const struct files *files, int button)
{
    switch (button) {
    case BUTTON_BACK: return files->back.count > 0;
    case BUTTON_FORWARD: return files->forward.count > 0;
    case BUTTON_UP: return strcmp(files->path, "/") != 0;
    default: return true;
    }
}

/* Split the path into crumbs: home (when inside it) or /, then each
   directory. Returns whether the first crumb is home. */
static bool make_crumbs(struct files *files)
{
    struct toolbar *bar = &files->toolbar;
    const char *path = files->path;
    size_t length = strlen(path), home_length = files->home ? strlen(files->home) : 0;
    bool home = home_length > 1 && strncmp(path, files->home, home_length) == 0 &&
                (path[home_length] == 0 || path[home_length] == '/');
    size_t needed = 1;
    for (size_t i = 1; i < length; ++i)
        needed += path[i] == '/';
    needed += 1;
    if (needed > bar->crumb_capacity) {
        struct crumb *crumbs = realloc(bar->crumbs, needed * sizeof *crumbs);
        if (!crumbs) {
            bar->crumb_count = 0;
            return false;
        }
        bar->crumbs = crumbs;
        bar->crumb_capacity = needed;
    }
    size_t count = 0, at = home ? home_length : 1;
    bar->crumbs[count++] = (struct crumb){.left = -1, .right = -1, .start = 0, .end = at};
    while (at < length) {
        if (path[at] == '/')
            ++at;
        size_t end = at;
        while (end < length && path[end] != '/')
            ++end;
        if (end > at)
            bar->crumbs[count++] = (struct crumb){.left = -1, .right = -1, .start = at, .end = end};
        at = end;
    }
    bar->crumb_count = count;
    return home;
}

static void outline(DeskCanvas *canvas, int x, int y, int width, int height, int line, uint32_t color)
{
    desk_canvas_fill(canvas, x, y, width, line, color);
    desk_canvas_fill(canvas, x, y + height - line, width, line, color);
    desk_canvas_fill(canvas, x, y, line, height, color);
    desk_canvas_fill(canvas, x + width - line, y, line, height, color);
}

/* The tab strip: one tab per directory open, each with a ×, then +. */
static void paint_tabs(struct files *files, DeskCanvas *canvas, const struct layout *layout, int top)
{
    const struct colors *colors = &files->colors;
    struct toolbar *bar = &files->toolbar;
    int height = layout->row_height, baseline = top + layout->baseline, space = layout->padding / 2;
    double scale = desk_window_scale(&files->window);
    bar->tabs_top = top;
    bar->tabs_bottom = top + height;
    int left = layout->padding, right = canvas->width - layout->padding - height - space;
    int width = (int)lround(TAB_WIDTH * scale);
    if (files->tab_count && (right - left) / (int)files->tab_count < width)
        width = (right - left) / (int)files->tab_count;
    uint32_t hover_color = mix(colors->background, colors->muted, 80);
    int x = left;
    for (size_t i = 0; i < files->tab_count; ++i) {
        struct tab *tab = &files->tabs[i];
        bool current = i == files->current_tab;
        const char *path = tab_path(files, i);
        tab->left = x;
        tab->right = x + width - space / 2;
        tab->close_right = tab->right - space / 2;
        tab->close_left = tab->close_right - height + space;
        if (current)
            desk_canvas_fill(canvas, tab->left, top, tab->right - tab->left, height,
                             mix(colors->background, colors->muted, 35));
        else if (hovering(files, TARGET_TAB, i) || hovering(files, TARGET_TAB_CLOSE, i))
            desk_canvas_fill(canvas, tab->left, top, tab->right - tab->left, height,
                             mix(colors->background, colors->muted, 20));
        if (current)
            desk_canvas_fill(canvas, tab->left, top + height - (int)fmax(1, lround(2 * scale)),
                             tab->right - tab->left, (int)fmax(1, lround(2 * scale)), colors->directory);
        bool home = files->home && strcmp(path, files->home) == 0;
        int icon_right = tab->left + space + layout->icon_width;
        draw_icon(layout->font, canvas, tab->left + space, icon_right, baseline, home ? ICON_HOME : ICON_FOLDER,
                  colors->directory);
        DeskCanvas clipped = *canvas;
        desk_canvas_clip(&clipped, icon_right, top, tab->close_left, top + height);
        draw_name(layout->font, &clipped, icon_right, baseline, tab->close_left - icon_right - space / 2,
                  tab_title(files, path), true, current ? colors->foreground : colors->muted);
        if (hovering(files, TARGET_TAB_CLOSE, i))
            desk_canvas_fill(canvas, tab->close_left, top + space / 2, tab->close_right - tab->close_left,
                             height - space, hover_color);
        draw_icon(layout->font, canvas, tab->close_left, tab->close_right, baseline, ICON_CLOSE, colors->muted);
        x += width;
    }
    bar->new_tab_left = x + space / 2;
    bar->new_tab_right = bar->new_tab_left + height;
    if (hovering(files, TARGET_NEW_TAB, 0))
        desk_canvas_fill(canvas, bar->new_tab_left, top, height, height, hover_color);
    draw_icon(layout->font, canvas, bar->new_tab_left, bar->new_tab_right, baseline, ICON_PLUS, colors->foreground);
}

/* The search field: magnifier, text or "Search", the cursor when focused,
   and a × that clears it. */
static void paint_search(struct files *files, DeskCanvas *canvas, const struct layout *layout, int top)
{
    const struct colors *colors = &files->colors;
    struct toolbar *bar = &files->toolbar;
    int height = layout->row_height, baseline = top + layout->baseline, space = layout->padding / 2;
    int line = desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1;
    int left = bar->search_left, right = bar->search_right;
    desk_canvas_fill(canvas, left, top, right - left, height, mix(colors->background, colors->muted, 35));
    if (files->search_focus)
        outline(canvas, left, top, right - left, height, line, colors->directory);
    draw_icon(layout->font, canvas, left, left + height, baseline, ICON_SEARCH, colors->muted);

    int text_left = left + height, text_right = right - space;
    bar->search_clear_left = -1;
    if (files->search[0]) {
        bar->search_clear_left = right - height;
        text_right = bar->search_clear_left;
        if (hovering(files, TARGET_SEARCH_CLEAR, 0))
            desk_canvas_fill(canvas, bar->search_clear_left + space / 2, top + space / 2, height - space,
                             height - space, mix(colors->background, colors->muted, 80));
        draw_icon(layout->font, canvas, bar->search_clear_left, right, baseline, ICON_CLOSE, colors->muted);
    }
    DeskCanvas clipped = *canvas;
    desk_canvas_clip(&clipped, text_left, top, text_right, top + height);
    if (!files->search[0] && !files->search_focus) {
        bar->search_scroll = 0;
        bar->search_text_x = text_left;
        desk_text_draw(layout->font, &clipped, text_left, baseline, "Search", 6, colors->muted);
        return;
    }
    /* Scroll so the cursor stays in view. */
    int room = text_right - text_left - line;
    int cursor_x = desk_text_width(layout->font, files->search, files->search_cursor);
    if (cursor_x - bar->search_scroll > room)
        bar->search_scroll = cursor_x - room;
    if (cursor_x < bar->search_scroll)
        bar->search_scroll = cursor_x;
    if (bar->search_scroll < 0)
        bar->search_scroll = 0;
    bar->search_text_x = text_left - bar->search_scroll;
    desk_text_draw(layout->font, &clipped, bar->search_text_x, baseline, files->search, strlen(files->search),
                   colors->foreground);
    if (files->search_focus)
        desk_canvas_fill(&clipped, bar->search_text_x + cursor_x, top + space / 2, line, height - space,
                         colors->foreground);
}

/* Sidebar, back, forward and up buttons; the path as crumbs to click, with a
   lock at its end where nothing may be changed; the search field; the
   list and icon view buttons. */
static void paint_toolbar(struct files *files, DeskCanvas *canvas, const struct layout *layout, int top)
{
    const struct colors *colors = &files->colors;
    struct toolbar *bar = &files->toolbar;
    int height = layout->row_height, baseline = top + layout->baseline, space = layout->padding / 2;
    uint32_t hover_color = mix(colors->background, colors->muted, 80);
    bar->top = top;
    bar->bottom = top + height;

    static const uint32_t icons[BUTTON_COUNT] = {ICON_SIDEBAR, ICON_BACK, ICON_FORWARD,
                                                 ICON_UP,      ICON_LIST, ICON_GRID};
    /* The navigation buttons on the left, the view buttons on the right. */
    int x = layout->padding;
    for (int i = 0; i <= BUTTON_UP; ++i) {
        bar->button_left[i] = x;
        bar->button_right[i] = x + height;
        x += height + space / 2;
    }
    int right = canvas->width - layout->padding;
    for (int i = BUTTON_ICONS; i >= BUTTON_LIST; --i) {
        bar->button_right[i] = right;
        bar->button_left[i] = right - height;
        right -= height + (i == BUTTON_ICONS ? 0 : space);
    }
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        bool enabled = button_enabled(files, i);
        bool chosen = (i == BUTTON_LIST && files->view == VIEW_LIST) || (i == BUTTON_ICONS && files->view == VIEW_ICONS);
        int left = bar->button_left[i];
        if (chosen)
            desk_canvas_fill(canvas, left, top, height, height, mix(colors->background, colors->selection_background, 150));
        else if (enabled && hovering(files, TARGET_BUTTON, (size_t)i))
            desk_canvas_fill(canvas, left, top, height, height, hover_color);
        draw_icon(layout->font, canvas, left, left + height, baseline, icons[i],
                  enabled ? colors->foreground : mix(colors->background, colors->muted, 110));
    }

    bar->path_left = x + space;
    int free_width = right - bar->path_left;
    int search_width = (int)lround(SEARCH_WIDTH * desk_window_scale(&files->window));
    if (search_width > free_width * 2 / 5)
        search_width = free_width * 2 / 5;
    bar->search_right = right;
    bar->search_left = right - search_width;
    bar->path_right = bar->search_left - space;
    paint_search(files, canvas, layout, top);
    desk_canvas_fill(canvas, bar->path_left, top, bar->path_right - bar->path_left, height,
                     mix(colors->background, colors->muted, 35));

    right = bar->path_right - space;
    const char *mark = files->root ? "\xef\x80\xa3 root" : !files->writable ? "\xef\x80\xa3 read-only" : NULL;
    if (mark) {
        right -= desk_text_width(layout->font, mark, strlen(mark));
        desk_text_draw(layout->font, canvas, right, baseline, mark, strlen(mark),
                       files->root ? colors->urgent : colors->muted);
        right -= layout->gap;
    }

    bool home = make_crumbs(files);
    size_t count = bar->crumb_count;
    if (!count)
        return;
    const char *path = files->path;
    int pad = space, separator = desk_text_width(layout->font, CRUMB_SEPARATOR, strlen(CRUMB_SEPARATOR));
    int ellipsis = desk_text_width(layout->font, ELLIPSIS, strlen(ELLIPSIS)) + 2 * pad;
    int widths[count];
    for (size_t i = 0; i < count; ++i) {
        const struct crumb *crumb = &bar->crumbs[i];
        int width = i == 0 && home ? desk_char_width(layout->font, ICON_HOME)
                                   : desk_text_width(layout->font, path + crumb->start, crumb->end - crumb->start);
        widths[i] = width + 2 * pad;
    }
    /* From the right: as many as fit, the rest behind an ellipsis that goes
       to the last one hidden. */
    int room = right - (bar->path_left + space / 2);
    size_t first = count - 1;
    int used = widths[first];
    while (first > 0) {
        int more = separator + widths[first - 1] + (first - 1 > 0 ? separator + ellipsis : 0);
        if (used + more > room)
            break;
        used += separator + widths[--first];
    }

    DeskCanvas clipped = *canvas;
    desk_canvas_clip(&clipped, bar->path_left, top, right, top + height);
    x = bar->path_left + space / 2;
    for (size_t i = first > 0 ? first - 1 : 0; i < count; ++i) {
        struct crumb *crumb = &bar->crumbs[i];
        bool hidden = i < first, last = i == count - 1;
        int width = hidden ? ellipsis : widths[i];
        crumb->left = x;
        crumb->right = x + width;
        if (!last && hovering(files, TARGET_CRUMB, i))
            desk_canvas_fill(&clipped, x, top, width, height, hover_color);
        if (hidden)
            desk_text_draw(layout->font, &clipped, x + pad, baseline, ELLIPSIS, strlen(ELLIPSIS), colors->muted);
        else if (i == 0 && home)
            draw_icon(layout->font, &clipped, x, x + width, baseline, ICON_HOME, colors->foreground);
        else
            desk_text_draw(layout->font, &clipped, x + pad, baseline, path + crumb->start,
                           crumb->end - crumb->start, colors->foreground);
        x += width;
        if (!last) {
            desk_text_draw(layout->font, &clipped, x, baseline, CRUMB_SEPARATOR, strlen(CRUMB_SEPARATOR),
                           colors->muted);
            x += separator;
        }
    }
}

/* Places, one a row, the one we're in highlighted, headings muted. */
static void paint_sidebar(struct files *files, DeskCanvas *canvas, const struct layout *layout, int x0, int x1,
                          int top, int rows)
{
    const struct colors *colors = &files->colors;
    int inner = layout->padding / 2;
    for (size_t i = 0; i < files->place_count && (int)i < rows; ++i) {
        const struct place *place = &files->places[i];
        int y = top + (int)i * layout->row_height, baseline = y + layout->baseline;
        if (!place->path) {
            desk_text_draw(layout->font, canvas, x0 + inner, baseline, place->name, strlen(place->name),
                           colors->muted);
            continue;
        }
        if (strcmp(place->path, files->path) == 0)
            desk_canvas_fill(canvas, x0, y, x1 - x0, layout->row_height,
                             mix(colors->background, colors->selection_background, 150));
        else if (hovering(files, TARGET_PLACE, i))
            desk_canvas_fill(canvas, x0, y, x1 - x0, layout->row_height,
                             mix(colors->background, colors->muted, 80));
        draw_icon(layout->font, canvas, x0 + inner, x0 + inner + layout->icon_width - inner, baseline, place->icon,
                  colors->directory);
        int name_x = x0 + inner + layout->icon_width;
        DeskCanvas clipped = *canvas;
        desk_canvas_clip(&clipped, name_x, canvas->clip_top, x1 - inner < canvas->clip_right ? x1 - inner
                                                                                             : canvas->clip_right,
                         canvas->clip_bottom);
        draw_name(layout->font, &clipped, name_x, baseline, x1 - inner - name_x, place->name, true,
                  colors->foreground);
    }
}

static void paint(struct files *files, DeskCanvas *canvas)
{
    const struct colors *colors = &files->colors;
    double scale = desk_window_scale(&files->window);
    fonts_resize(files);
    struct layout ui = make_layout(files->font, files->italic, files->mono, scale);
    struct layout list = make_layout(files->list_font, files->list_italic, files->list_mono, scale);
    desk_canvas_fill(canvas, 0, 0, canvas->width, canvas->height, colors->background);

    /* Tabs at the top, then the toolbar; the sidebar and the entries below. */
    paint_tabs(files, canvas, &ui, ui.padding / 2);
    paint_toolbar(files, canvas, &ui, files->toolbar.tabs_bottom + ui.padding / 2);
    int top = files->toolbar.bottom + ui.padding, bottom = canvas->height - ui.padding / 2;

    int cell = desk_char_width(files->mono, '0');
    int cells = canvas->width / (cell > 0 ? cell : 1);
    bool sidebar = files->show_sidebar && cells >= SIDEBAR_CELLS;
    int sidebar_width = (int)lround(fmin(files->sidebar_width, canvas->width / scale / 2) * scale);
    struct panes *panes = &files->panes;
    *panes = (struct panes){
        .sidebar = sidebar,
        .sidebar_left = ui.padding,
        .sidebar_right = ui.padding + sidebar_width,
        .divider = sidebar ? ui.padding + sidebar_width + ui.padding / 2 : -1,
        .list_left = sidebar ? 2 * ui.padding + sidebar_width : ui.padding,
        .list_right = canvas->width - ui.padding,
        .top = top,
        .place_height = ui.row_height,
        .columns = 1,
    };
    int list_width = panes->list_right - panes->list_left;
    if (files->view == VIEW_ICONS) {
        int icon = (int)lround(desk_font_size(files->icon_font));
        int least = (int)fmax(icon * 1.6, 7 * font_pixels(files));
        panes->columns = list_width / least > 1 ? list_width / least : 1;
        panes->tile_width = list_width / panes->columns;
        panes->line_height = icon + ui.padding + 2 * desk_font_metrics(ui.font).height + ui.padding;
    } else {
        panes->tile_width = list_width;
        panes->line_height = list.row_height;
    }
    int rows = (bottom - top) / panes->line_height;
    panes->rows = rows < 1 ? 1 : rows;
    files->page_rows = panes->rows;
    files->row_height = panes->line_height;

    if (sidebar) {
        uint32_t rule = mix(colors->background, colors->muted, 70);
        int rule_width = scale >= 2 ? (int)scale : 1;
        /* The divider under the pointer or being dragged stands out. */
        bool active = files->drag || hovering(files, TARGET_DIVIDER, 0);
        int width = active ? rule_width * 3 : rule_width;
        desk_canvas_fill(canvas, panes->divider - (width - rule_width) / 2, top, width, bottom - top,
                         active ? colors->directory : rule);
        DeskCanvas clipped = *canvas;
        desk_canvas_clip(&clipped, panes->sidebar_left, top, panes->sidebar_right, bottom);
        paint_sidebar(files, &clipped, &ui, panes->sidebar_left, panes->sidebar_right, top,
                      (bottom - top) / ui.row_height);
    }

    DeskCanvas area = *canvas;
    desk_canvas_clip(&area, panes->list_left, top, panes->list_right, bottom);
    uint32_t message_color;
    const char *message = empty_message(files, &message_color);
    if (message) {
        draw_message(&area, &list, panes->list_left, top, message, message_color);
        return;
    }
    size_t lines = line_count(files), per_line = columns(files);
    if (files->free_scroll)
        files->top = clamp_top(files->top, lines, panes->rows);
    else
        files->top = keep_visible(files->top, files->cursor / per_line, lines, panes->rows);
    if (files->view == VIEW_ICONS)
        paint_icons(files, &area, &ui, panes->list_left, top, panes->rows + 1);
    else
        paint_list(files, &area, &list, panes->list_left, panes->list_right, top, panes->rows + 1);
}

static void draw(void *data, DeskWindow *window)
{
    struct files *files = data;
    DeskBuffer *buffer = desk_window_buffer(window, WL_SHM_FORMAT_XRGB8888);
    if (!buffer)
        return; /* Both busy: retried after a release. */
    DeskCanvas canvas = desk_canvas(buffer->pixels, buffer->width, buffer->height);
    paint(files, &canvas);
    desk_window_commit(window, buffer);
}

static void window_close(void *data, DeskWindow *window)
{
    (void)window;
    struct files *files = data;
    files->running = false;
}

static const DeskWindowListener window_listener = {
    .draw = draw,
    .close = window_close,
};

/* Keyboard */

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    (void)keyboard;
    struct files *files = data;
    desk_keyboard_keymap(&files->keyboard, format, fd, size);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
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
    struct files *files = data;
    desk_repeat_cancel(&files->repeat);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    struct files *files = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        desk_repeat_release(&files->repeat, key);
        return;
    }
    if (!files->keyboard.state)
        return;
    bool repeats = handle_key(files, key);
    if (repeats && desk_keyboard_repeats(&files->keyboard, key))
        desk_repeat_start(&files->repeat, key);
    else
        desk_repeat_cancel(&files->repeat);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)keyboard;
    (void)serial;
    struct files *files = data;
    desk_keyboard_modifiers(&files->keyboard, depressed, latched, locked, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)keyboard;
    struct files *files = data;
    desk_repeat_info(&files->repeat, rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* Pointer */

#define WHEEL_ROWS 3      /* List rows a wheel notch scrolls; icons scroll a row. */
#define ZOOM_PIXELS 40.0  /* Touchpad distance, logical pixels, per zoom step. */

static uint32_t cursor_shape(const struct files *files)
{
    if (files->drag || files->hover.kind == TARGET_DIVIDER)
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE;
    if (files->hover.kind == TARGET_SEARCH)
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
}

static void set_cursor(struct files *files, bool force)
{
    uint32_t shape = cursor_shape(files);
    if (files->cursor_shape && (force || shape != files->shape))
        wp_cursor_shape_device_v1_set_shape(files->cursor_shape, files->pointer_serial, shape);
    files->shape = shape;
}

/* The pointer in buffer pixels. */
static int pointer_buffer_x(const struct files *files)
{
    return (int)lround(files->pointer_x * desk_window_scale(&files->window));
}

static int pointer_buffer_y(const struct files *files)
{
    return (int)lround(files->pointer_y * desk_window_scale(&files->window));
}

static bool divider_at(const struct files *files)
{
    const struct panes *panes = &files->panes;
    int x = pointer_buffer_x(files), reach = (int)lround(DIVIDER_GRAB * desk_window_scale(&files->window));
    return panes->divider >= 0 && pointer_buffer_y(files) >= panes->top && abs(x - panes->divider) <= reach;
}

/* Whether the pointer is over the entries, for the wheel. */
static bool in_list(const struct files *files)
{
    const struct panes *panes = &files->panes;
    int x = pointer_buffer_x(files);
    return pointer_buffer_y(files) >= panes->top && x >= panes->list_left && x < panes->list_right;
}

static struct target target_at(const struct files *files)
{
    const struct toolbar *bar = &files->toolbar;
    const struct panes *panes = &files->panes;
    int x = pointer_buffer_x(files), y = pointer_buffer_y(files);
    if (y >= bar->tabs_top && y < bar->tabs_bottom) {
        for (size_t i = 0; i < files->tab_count; ++i) {
            const struct tab *tab = &files->tabs[i];
            if (x >= tab->close_left && x < tab->close_right)
                return (struct target){TARGET_TAB_CLOSE, i};
            if (x >= tab->left && x < tab->right)
                return (struct target){TARGET_TAB, i};
        }
        if (x >= bar->new_tab_left && x < bar->new_tab_right)
            return (struct target){TARGET_NEW_TAB, 0};
        return (struct target){TARGET_NONE, 0};
    }
    if (y >= bar->top && y < bar->bottom) {
        for (int i = 0; i < BUTTON_COUNT; ++i) {
            if (x >= bar->button_left[i] && x < bar->button_right[i])
                return (struct target){TARGET_BUTTON, (size_t)i};
        }
        for (size_t i = 0; i < bar->crumb_count; ++i) {
            const struct crumb *crumb = &bar->crumbs[i];
            if (crumb->left >= 0 && x >= crumb->left && x < crumb->right)
                return (struct target){TARGET_CRUMB, i};
        }
        if (bar->search_clear_left >= 0 && x >= bar->search_clear_left && x < bar->search_right)
            return (struct target){TARGET_SEARCH_CLEAR, 0};
        if (x >= bar->search_left && x < bar->search_right)
            return (struct target){TARGET_SEARCH, 0};
        return (struct target){TARGET_NONE, 0};
    }
    if (divider_at(files))
        return (struct target){TARGET_DIVIDER, 0};
    if (y < panes->top)
        return (struct target){TARGET_NONE, 0};
    if (panes->sidebar && x >= panes->sidebar_left && x < panes->sidebar_right) {
        long row = panes->place_height > 0 ? (y - panes->top) / panes->place_height : -1;
        if (row >= 0 && (size_t)row < files->place_count && files->places[row].path)
            return (struct target){TARGET_PLACE, (size_t)row};
    } else if (x >= panes->list_left && x < panes->list_right && panes->line_height > 0 && panes->tile_width > 0) {
        size_t line = files->top + (size_t)((y - panes->top) / panes->line_height);
        size_t column = (size_t)((x - panes->list_left) / panes->tile_width);
        size_t index = line * columns(files) + column;
        if (column < columns(files) && index < files->listing.shown_count && !empty_message(files, &(uint32_t){0}))
            return (struct target){TARGET_ROW, index};
    }
    return (struct target){TARGET_NONE, 0};
}

/* Everything clickable lights up under the pointer except entries. */
static bool shows_hover(enum target_kind kind)
{
    return kind != TARGET_NONE && kind != TARGET_ROW && kind != TARGET_SEARCH;
}

static void update_hover(struct files *files, bool force)
{
    struct target hover = target_at(files);
    if (!force && hover.kind == files->hover.kind && hover.index == files->hover.index)
        return;
    struct target old = files->hover;
    /* Set before redrawing: the redraw may paint right away. */
    files->hover = hover;
    if (shows_hover(hover.kind) || shows_hover(old.kind))
        desk_window_redraw(&files->window);
    set_cursor(files, force);
}

/* A crumb: its directory, with the one we came through selected. */
static void go_to_crumb(struct files *files, size_t index)
{
    const struct toolbar *bar = &files->toolbar;
    if (index + 1 >= bar->crumb_count)
        return; /* The current directory. */
    const struct crumb *next = &bar->crumbs[index + 1];
    char *path = strndup(files->path, bar->crumbs[index].end);
    char *select = strndup(files->path + next->start, next->end - next->start);
    if (path && select)
        go_to(files, path, select);
    else
        free(path);
    free(select);
}

/* A place in the sidebar. */
static void go_to_place(struct files *files, size_t index)
{
    const struct place *place = index < files->place_count ? &files->places[index] : NULL;
    if (!place || !place->path || strcmp(place->path, files->path) == 0)
        return;
    char *path = strdup(place->path);
    if (path)
        go_to(files, path, NULL);
}

/* Put the search cursor where the pointer is in the text. */
static void search_click(struct files *files)
{
    int x = pointer_buffer_x(files) - files->toolbar.search_text_x;
    const char *text = files->search;
    size_t at = 0, length = strlen(text);
    while (at < length) {
        size_t next = forward_char(text, at);
        int left = desk_text_width(files->font, text, at), right = desk_text_width(files->font, text, next);
        if (x < (left + right) / 2)
            break;
        at = next;
    }
    files->search_cursor = at;
    files->search_focus = true;
    desk_window_redraw(&files->window);
}

static void click(struct files *files, uint32_t time)
{
    struct target target = target_at(files);
    bool twice = target.kind != TARGET_NONE && target.kind == files->press.kind &&
                 target.index == files->press.index && time - files->press_time < DOUBLE_CLICK_MS;
    files->press = target;
    files->press_time = time;
    if (target.kind != TARGET_SEARCH && target.kind != TARGET_SEARCH_CLEAR)
        search_focus(files, false);
    switch (target.kind) {
    case TARGET_BUTTON:
        switch (target.index) {
        case BUTTON_SIDEBAR: toggle_sidebar(files); break;
        case BUTTON_BACK: go_back(files); break;
        case BUTTON_FORWARD: go_forward(files); break;
        case BUTTON_UP: go_up(files); break;
        case BUTTON_LIST: set_view(files, VIEW_LIST); break;
        case BUTTON_ICONS: set_view(files, VIEW_ICONS); break;
        }
        break;
    case TARGET_CRUMB:
        go_to_crumb(files, target.index);
        break;
    case TARGET_DIVIDER:
        if (twice) {
            files->press.kind = TARGET_NONE;
            reset_sidebar(files);
        } else {
            files->drag = true;
        }
        break;
    case TARGET_PLACE:
        go_to_place(files, target.index);
        break;
    case TARGET_ROW:
        if (twice) {
            go_in(files);
        } else {
            files->cursor = target.index;
            files->free_scroll = false;
            desk_window_redraw(&files->window);
        }
        break;
    case TARGET_TAB:
        switch_tab(files, target.index);
        break;
    case TARGET_TAB_CLOSE:
        close_tab(files, target.index);
        break;
    case TARGET_NEW_TAB:
        new_tab(files, strdup(files->path), true);
        break;
    case TARGET_SEARCH:
        search_click(files);
        break;
    case TARGET_SEARCH_CLEAR:
        search_clear(files);
        search_focus(files, true);
        break;
    case TARGET_NONE:
        break;
    }
    /* Whatever moved under the pointer. */
    update_hover(files, false);
}

/* Middle-click: a directory or place opens in a new tab behind this one; a
   tab closes. */
static void middle_click(struct files *files)
{
    struct target target = target_at(files);
    char *path = NULL;
    if (target.kind == TARGET_TAB || target.kind == TARGET_TAB_CLOSE) {
        close_tab(files, target.index);
    } else if (target.kind == TARGET_PLACE) {
        path = strdup(files->places[target.index].path);
    } else if (target.kind == TARGET_ROW) {
        const struct entry *entry = &files->listing.entries[files->listing.shown[target.index]];
        if (entry->directory)
            path = join_path(files->path, entry->name);
    } else if (target.kind == TARGET_CRUMB && target.index < files->toolbar.crumb_count) {
        path = strndup(files->path, files->toolbar.crumbs[target.index].end);
    }
    if (path)
        new_tab(files, path, false);
    update_hover(files, false);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)surface;
    struct files *files = data;
    files->pointer_serial = serial;
    files->pointer_x = wl_fixed_to_double(x);
    files->pointer_y = wl_fixed_to_double(y);
    update_hover(files, true);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    struct files *files = data;
    if (files->drag)
        save_state(files);
    files->drag = false;
    files->hover = (struct target){TARGET_NONE, 0};
    files->scroll_pixels = files->zoom_pixels = 0;
    desk_window_redraw(&files->window);
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)time;
    struct files *files = data;
    files->pointer_x = wl_fixed_to_double(x);
    files->pointer_y = wl_fixed_to_double(y);
    if (files->drag)
        set_sidebar_width(files, files->pointer_x - PADDING * 1.5);
    else
        update_hover(files, false);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)serial;
    struct files *files = data;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (button == BTN_SIDE || button == BTN_BACK) {
        if (pressed)
            go_back(files);
    } else if (button == BTN_EXTRA || button == BTN_FORWARD) {
        if (pressed)
            go_forward(files);
    } else if (button == BTN_MIDDLE) {
        if (pressed)
            middle_click(files);
    } else if (button != BTN_LEFT) {
        /* Right-click opens a menu in 0.3. */
    } else if (pressed) {
        click(files, time);
    } else if (files->drag) {
        files->drag = false;
        save_state(files);
        update_hover(files, true);
        desk_window_redraw(&files->window);
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer;
    (void)time;
    struct files *files = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        files->scroll_pixels += wl_fixed_to_double(value);
}

/* Apply the frame's scrolling. With Ctrl held it zooms, a step a notch;
   otherwise it scrolls the entries under the pointer: notches by
   WHEEL_ROWS list rows or one row of icons, touchpads by distance. */
static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)pointer;
    struct files *files = data;
    int steps = files->scroll_steps;
    double pixels = files->scroll_pixels;
    files->scroll_steps = 0;
    if (desk_keyboard_mods(&files->keyboard) & DESK_MOD_CTRL) {
        files->scroll_pixels = 0;
        if (!steps) {
            files->zoom_pixels += pixels;
            steps = (int)(files->zoom_pixels / ZOOM_PIXELS);
            files->zoom_pixels -= steps * ZOOM_PIXELS;
        }
        for (int i = 0; i < abs(steps); ++i)
            zoom(files, steps < 0 ? 1 : -1); /* Up, away from you, zooms in. */
        update_hover(files, false);
        return;
    }
    long lines = 0;
    if (steps) {
        lines = (long)steps * (files->view == VIEW_ICONS ? 1 : WHEEL_ROWS);
        files->scroll_pixels = 0;
    } else if (files->row_height > 0) {
        double line = files->row_height / desk_window_scale(&files->window);
        lines = (long)(pixels / line);
        files->scroll_pixels -= lines * line;
    }
    if (!lines)
        return;
    if (in_list(files))
        scroll_lines(files, lines);
    update_hover(files, false);
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
    struct files *files = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        files->scroll_pixels = files->zoom_pixels = 0;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)pointer;
    struct files *files = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        files->scroll_steps += discrete;
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
    struct files *files = data;
    bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && !files->wl_keyboard) {
        files->wl_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(files->wl_keyboard, &keyboard_listener, files);
    } else if (!has_keyboard && files->wl_keyboard) {
        desk_repeat_cancel(&files->repeat);
        wl_keyboard_release(files->wl_keyboard);
        files->wl_keyboard = NULL;
    }
    bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && !files->pointer) {
        files->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(files->pointer, &pointer_listener, files);
        if (files->cursor_shape_manager)
            files->cursor_shape = wp_cursor_shape_manager_v1_get_pointer(files->cursor_shape_manager, files->pointer);
    } else if (!has_pointer && files->pointer) {
        if (files->cursor_shape)
            wp_cursor_shape_device_v1_destroy(files->cursor_shape);
        files->cursor_shape = NULL;
        wl_pointer_release(files->pointer);
        files->pointer = NULL;
        files->drag = false;
        files->hover = (struct target){TARGET_NONE, 0};
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
    struct files *files = data;
    if (desk_globals_bind(&files->globals, registry, name, interface, version))
        return;
    if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        files->cursor_shape_manager = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !files->seat) {
        /* Version 5 adds wl_keyboard.release. */
        files->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(files->seat, &seat_listener, files);
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

static bool open_window(struct files *files)
{
    char error[256];
    double size = FONT_POINTS * 96.0 / 72.0;
    if (!(files->font = desk_font_open("sans-serif", size, error, sizeof error)) ||
        !(files->italic = desk_font_open("sans-serif:slant=italic", size, error, sizeof error)) ||
        !(files->mono = desk_font_open("monospace", size * 0.9, error, sizeof error)) ||
        !(files->list_font = desk_font_open("sans-serif", size, error, sizeof error)) ||
        !(files->list_italic = desk_font_open("sans-serif:slant=italic", size, error, sizeof error)) ||
        !(files->list_mono = desk_font_open("monospace", size * 0.9, error, sizeof error)) ||
        !(files->icon_font = desk_font_open("sans-serif", 64, error, sizeof error))) {
        fprintf(stderr, "files: %s\n", error);
        return false;
    }
    if (desk_keyboard_open(&files->keyboard) != 0) {
        fprintf(stderr, "files: out of memory\n");
        return false;
    }
    files->display = wl_display_connect(NULL);
    if (!files->display) {
        fprintf(stderr, "files: cannot connect to the Wayland display\n");
        return false;
    }
    files->registry = wl_display_get_registry(files->display);
    wl_registry_add_listener(files->registry, &registry_listener, files);
    if (wl_display_roundtrip(files->display) < 0) {
        fprintf(stderr, "files: lost the Wayland connection\n");
        return false;
    }
    const char *missing = desk_globals_missing(&files->globals);
    if (missing) {
        fprintf(stderr, "files: the compositor doesn't offer %s\n", missing);
        return false;
    }
    /* The seat's capabilities may have arrived before the cursor shape manager. */
    if (files->pointer && files->cursor_shape_manager && !files->cursor_shape)
        files->cursor_shape = wp_cursor_shape_manager_v1_get_pointer(files->cursor_shape_manager, files->pointer);
    if (!desk_window_open(&files->window, &files->globals, "files", &window_listener, files)) {
        fprintf(stderr, "files: out of memory\n");
        return false;
    }
    update_title(files);
    return true;
}

static void close_window(struct files *files)
{
    desk_window_close(&files->window);
    if (files->wl_keyboard)
        wl_keyboard_release(files->wl_keyboard);
    if (files->cursor_shape)
        wp_cursor_shape_device_v1_destroy(files->cursor_shape);
    if (files->pointer)
        wl_pointer_release(files->pointer);
    if (files->cursor_shape_manager)
        wp_cursor_shape_manager_v1_destroy(files->cursor_shape_manager);
    if (files->seat)
        wl_seat_destroy(files->seat);
    desk_globals_finish(&files->globals);
    if (files->registry)
        wl_registry_destroy(files->registry);
    if (files->display)
        wl_display_disconnect(files->display);
    desk_keyboard_close(&files->keyboard);
    desk_font_close(files->font);
    desk_font_close(files->italic);
    desk_font_close(files->mono);
    desk_font_close(files->list_font);
    desk_font_close(files->list_italic);
    desk_font_close(files->list_mono);
    desk_font_close(files->icon_font);
}

/* Dispatch Wayland events, key repeats and directory changes until closed. */
static bool run(struct files *files)
{
    struct wl_display *display = files->display;
    while (files->running) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0)
                return false;
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            return false;
        }
        struct pollfd fds[6] = {
            {.fd = wl_display_get_fd(display), .events = POLLIN},
            {.fd = files->repeat.fd, .events = POLLIN},
            {.fd = files->inotify, .events = POLLIN},
            {.fd = files->refresh, .events = POLLIN},
            {.fd = files->palette_watch.fd, .events = POLLIN},
            {.fd = files->mounts, .events = POLLPRI},
        };
        if (poll(fds, 6, -1) < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR)
                continue;
            return false;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(display) < 0)
                return false;
        } else {
            wl_display_cancel_read(display);
            if (fds[0].revents & (POLLERR | POLLHUP))
                return false;
        }
        if (wl_display_dispatch_pending(display) < 0)
            return false;

        if (files->repeat.fd >= 0 && fds[1].revents & POLLIN) {
            unsigned count = desk_repeat_fired(&files->repeat);
            if (count > MAX_REPEATS)
                count = MAX_REPEATS;
            for (unsigned i = 0; i < count && files->repeat.active && files->running; ++i)
                handle_key(files, files->repeat.key);
        }
        if (files->inotify >= 0 && fds[2].revents & POLLIN)
            watch_events(files);
        if (files->refresh >= 0 && fds[3].revents & POLLIN) {
            uint64_t expirations;
            (void)!read(files->refresh, &expirations, sizeof expirations);
            files->refresh_armed = false;
            refresh(files);
        }
        if (files->palette_watch.fd >= 0 && fds[4].revents &&
            desk_watch_changed(&files->palette_watch, fds[4].revents)) {
            update_colors(files);
            desk_window_redraw(&files->window);
        }
        if (files->mounts >= 0 && fds[5].revents & (POLLPRI | POLLERR)) {
            build_places(files); /* A drive came or went. */
            desk_window_redraw(&files->window);
        }
        desk_window_update(&files->window);
    }
    return true;
}

/* Main */

static void usage(FILE *stream)
{
    fputs("Usage: files [DIR | FILE]\n"
          "\n"
          "Browse a directory, the home directory by default. A FILE opens its\n"
          "directory with it selected.\n"
          "\n"
          "  -h, --help     Show this help\n"
          "  -v, --version  Show the version\n"
          "\n"
          "Keys: arrows move, Enter opens, Backspace goes up, Alt+Left/Right back and\n"
          "forward, Ctrl+T/W new and close tab, Ctrl+Tab next tab, Ctrl+F search,\n"
          "Ctrl+1/2 list or icons, Ctrl+plus/minus/0 zoom, F9 sidebar, . hidden files,\n"
          "Ctrl+Q quits. Ctrl+wheel zooms; middle-click opens a folder in a new tab.\n",
          stream);
}

/* Resolve the argument to an absolute directory and the name to select. */
static int start_at(const char *argument, char **directory, char **select)
{
    *directory = *select = NULL;
    struct stat info;
    if (stat(argument, &info) != 0) {
        fprintf(stderr, "files: %s: %s\n", argument, strerror(errno));
        return EXIT_FAILED;
    }
    if (S_ISDIR(info.st_mode)) {
        *directory = realpath(argument, NULL);
    } else {
        /* Resolve the directory but keep the file's own name, link or not. */
        char *copy = strdup(argument);
        char *slash = copy ? strrchr(copy, '/') : NULL;
        const char *parent = !copy ? NULL : !slash ? "." : slash == copy ? "/" : (*slash = 0, copy);
        if (copy)
            *select = strdup(base_name(argument));
        *directory = parent ? realpath(parent, NULL) : NULL;
        free(copy);
    }
    if (!*directory) {
        fprintf(stderr, "files: %s: %s\n", argument, strerror(errno));
        free(*select);
        *select = NULL;
        return EXIT_FAILED;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {0},
    };
    int option;
    while ((option = getopt_long(argc, argv, "hv", options, NULL)) != -1) {
        switch (option) {
        case 'h': usage(stdout); return EXIT_CLOSED;
        case 'v': puts("files " FILES_VERSION); return EXIT_CLOSED;
        default: usage(stderr); return EXIT_USAGE;
        }
    }
    if (argc - optind > 1) {
        usage(stderr);
        return EXIT_USAGE;
    }

    struct files files = {
        .inotify = -1, .watch = -1, .refresh = -1, .mounts = -1, .running = true, .root = geteuid() == 0,
        .palette_watch = {.fd = -1}, .repeat = {.fd = -1},
    };
    const char *home = getenv("HOME");
    if (home && home[0] == '/') {
        files.home = strdup(home);
        size_t length = files.home ? strlen(files.home) : 0;
        while (length > 1 && files.home[length - 1] == '/')
            files.home[--length] = 0;
    }
    if (files.home)
        files.home_real = realpath(files.home, NULL);
    files.state_path = state_path();
    load_state(&files);
    char *directory, *select = NULL;
    int status = start_at(optind < argc ? argv[optind] : files.home ? files.home : "/", &directory, &select);
    if (status) {
        free(files.home);
        free(files.home_real);
        free(files.state_path);
        return status;
    }

    files.palette_path = desk_palette_path();
    if (files.palette_path)
        desk_watch_open(&files.palette_watch, files.palette_path);
    update_colors(&files);
    files.inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    files.refresh = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    int error = desk_repeat_open(&files.repeat);
    if (error)
        fprintf(stderr, "files: no key repeat: %s\n", strerror(error));

    files.mounts = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    files.tabs = calloc(1, sizeof *files.tabs);
    if (!files.tabs) {
        fprintf(stderr, "files: out of memory\n");
        return EXIT_FAILED;
    }
    files.tab_count = 1;
    go_to(&files, directory, select);
    free(select);
    if (!open_window(&files)) {
        status = EXIT_FAILED;
    } else if (!run(&files)) {
        fprintf(stderr, "files: lost the Wayland connection\n");
        status = EXIT_FAILED;
    }
    close_window(&files);
    if (files.mounts >= 0)
        close(files.mounts);

    desk_repeat_close(&files.repeat);
    desk_watch_close(&files.palette_watch);
    if (files.inotify >= 0)
        close(files.inotify);
    if (files.refresh >= 0)
        close(files.refresh);
    listing_free(&files.listing);
    places_free(files.places, files.place_count);
    free(files.toolbar.crumbs);
    history_free(&files.back);
    history_free(&files.forward);
    for (size_t i = 0; i < files.tab_count; ++i)
        tab_free(&files.tabs[i]);
    free(files.tabs);
    for (size_t i = 0; i < files.memory_count; ++i) {
        free(files.memories[i].path);
        free(files.memories[i].name);
    }
    free(files.memories);
    free(files.palette_path);
    free(files.path);
    free(files.home);
    free(files.home_real);
    free(files.state_path);
    return status;
}
