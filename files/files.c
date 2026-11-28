#define _GNU_SOURCE
#include "files.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/eventfd.h>
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
#define TYPE_RESET_MS 1000    /* Type to select starts over after this long without a key. */
#define DIVIDER_GRAB 5        /* Logical pixels either side of a divider that grab it. */
#define DOUBLE_CLICK_MS 400
#define NAME_MIN 150          /* Logical pixels the name column keeps before the others narrow. */
#define COLUMN_MIN 40         /* Logical pixels: no column is dragged narrower. */

/* Type, size and modified to begin with, logical pixels at zoom 1. */
static const double default_column_width[COLUMN_COUNT] = {0, 120, 75, 135};
static const char *const column_titles[COLUMN_COUNT] = {"Name", "Type", "Size", "Modified"};

/* Zoom levels: the list's text size as a multiple of the window's, and the
   icon view's icon size in logical pixels. Together they make one scale:
   zooming in past the biggest list goes to the smallest icons, and out past
   the smallest icons back to the list. */
static const double list_zooms[] = {0.75, 0.875, 1.0, 1.125, 1.25, 1.5};
static const double icon_zooms[] = {48, 64, 80, 96, 128, 160, 192};
#define LIST_ZOOMS ((int)(sizeof list_zooms / sizeof *list_zooms))
#define ICON_ZOOMS ((int)(sizeof icon_zooms / sizeof *icon_zooms))
static const int default_zoom[VIEW_COUNT] = {2, 2};

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

static void entry_type(const struct entry *entry, char *out, size_t length);

/* The order qsort sorts by, set by listing_sort. */
static enum column sort_column;
static bool sort_descending;

static int compare_numbers(long long a, long long b)
{
    return a < b ? -1 : a > b;
}

/* Directories first, then by the sorted column, ties by name. Directories
   are sized by their contents; ones still being summed count as smallest. */
static int entry_order(const void *left, const void *right)
{
    const struct entry *a = left, *b = right;
    if (a->directory != b->directory)
        return a->directory ? -1 : 1;
    int order = 0;
    switch (sort_column) {
    case COLUMN_TYPE: {
        char x[48], y[48];
        entry_type(a, x, sizeof x);
        entry_type(b, y, sizeof y);
        order = natural_compare(x, y);
        break;
    }
    case COLUMN_SIZE:
        order = compare_numbers(a->directory ? a->total : (long long)a->size,
                                b->directory ? b->total : (long long)b->size);
        break;
    case COLUMN_MODIFIED:
        order = compare_numbers(a->modified.tv_sec, b->modified.tv_sec);
        if (!order)
            order = compare_numbers(a->modified.tv_nsec, b->modified.tv_nsec);
        break;
    case COLUMN_NAME:
    case COLUMN_COUNT:
        break;
    }
    if (!order) {
        order = natural_compare(a->name, b->name);
        if (sort_column != COLUMN_NAME)
            return order; /* Ties by name stay A to Z either way. */
    }
    return sort_descending ? -order : order;
}

static void listing_sort(struct listing *listing, enum column column, bool descending)
{
    sort_column = column;
    sort_descending = descending;
    if (listing->count)
        qsort(listing->entries, listing->count, sizeof *listing->entries, entry_order);
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
        *entry = (struct entry){.name = strdup(name), .hidden = name[0] == '.', .total = TOTAL_NONE};
        if (!entry->name) {
            errno = ENOMEM;
            break;
        }
        entry_stat(dirfd(directory), entry, item->d_type);
        if (entry->directory && !entry->link)
            entry->total = TOTAL_PENDING;
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
    } else if (strcmp(key, "sort") == 0) {
        for (int c = 0; c < COLUMN_COUNT; ++c) {
            if (strcasecmp(value, column_titles[c]) == 0)
                files->sort = (enum column)c;
        }
    } else if (strcmp(key, "sort_reverse") == 0) {
        files->sort_reverse = strcmp(value, "yes") == 0;
    } else if (strcmp(key, "columns") == 0) {
        const char *at = value;
        for (int c = COLUMN_TYPE; c < COLUMN_COUNT; ++c) {
            char *end;
            double width = strtod(at, &end);
            if (end == at)
                break;
            if (width >= COLUMN_MIN && width <= 2000)
                files->column_width[c] = width;
            at = end;
        }
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
    files->sort = COLUMN_NAME;
    files->sort_reverse = false;
    memcpy(files->column_width, default_column_width, sizeof files->column_width);
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
            "# Written by files. Widths and the icon size in logical pixels.\n[layout]\n"
            "sidebar = %.0f\nshow_sidebar = %s\nview = %s\nlist_zoom = %.3f\nicon_size = %.0f\n"
            "sort = %s\nsort_reverse = %s\ncolumns = %.0f %.0f %.0f\n",
            files->sidebar_width, files->show_sidebar ? "yes" : "no", files->view == VIEW_ICONS ? "icons" : "list",
            list_zooms[files->zoom[VIEW_LIST]], icon_zooms[files->zoom[VIEW_ICONS]], column_titles[files->sort],
            files->sort_reverse ? "yes" : "no", files->column_width[COLUMN_TYPE], files->column_width[COLUMN_SIZE],
            files->column_width[COLUMN_MODIFIED]);
    desk_atomic_commit(&atomic, true);
    desk_atomic_abort(&atomic); /* Frees what's left, after success or failure. */
}

/* Applications marked Terminal=true run in run's terminal (its
   [applications] terminal setting), "deal -e" by default. */
static void terminal_entry(void *data, const char *section, const char *key, const char *value, int line)
{
    (void)line;
    char **terminal = data;
    if (key && strcmp(section, "applications") == 0 && strcmp(key, "terminal") == 0 && *value) {
        char *copy = strdup(value);
        if (copy) {
            free(*terminal);
            *terminal = copy;
        }
    }
}

static char *terminal_command(void)
{
    char *terminal = NULL, *path = desk_config_path("run");
    if (path)
        desk_ini_load(path, terminal_entry, &terminal);
    free(path);
    return terminal ? terminal : strdup("deal -e");
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
    grown[(*count)++] = (struct place){name_copy, path_copy, icon, false};
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
/* Bookmarks: GTK's file, one "file:///path Label" a line, shared with GTK
   file dialogs and other file managers. */

static char *bookmarks_file(void)
{
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char *path = NULL;
    if (config && config[0] == '/')
        return asprintf(&path, "%s/gtk-3.0/bookmarks", config) < 0 ? NULL : path;
    if (home && *home)
        return asprintf(&path, "%s/.config/gtk-3.0/bookmarks", home) < 0 ? NULL : path;
    return NULL;
}

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The path in a file:// URI, decoded, or NULL for other URIs. */
static char *uri_path(const char *uri, size_t length)
{
    if (length < 8 || strncmp(uri, "file://", 7) != 0)
        return NULL;
    uri += 7, length -= 7;
    if (length >= 9 && strncmp(uri, "localhost", 9) == 0)
        uri += 9, length -= 9;
    if (!length || uri[0] != '/')
        return NULL;
    char *path = malloc(length + 1), *out = path;
    for (size_t i = 0; path && i < length; ++i) {
        int high, low;
        if (uri[i] == '%' && i + 2 < length && (high = hex_digit(uri[i + 1])) >= 0 &&
            (low = hex_digit(uri[i + 2])) >= 0) {
            *out++ = (char)(high << 4 | low);
            i += 2;
        } else {
            *out++ = uri[i];
        }
    }
    if (path)
        *out = 0;
    return path;
}

/* file:// and the path with everything but unreserved characters and /
   percent-encoded. */
static char *path_uri(const char *path)
{
    size_t length = strlen(path);
    char *uri = malloc(7 + 3 * length + 1);
    if (!uri)
        return NULL;
    char *out = uri + sprintf(uri, "file://");
    for (const unsigned char *at = (const unsigned char *)path; *at; ++at) {
        if (isalnum(*at) || strchr("/-._~", *at))
            *out++ = (char)*at;
        else
            out += sprintf(out, "%%%02X", *at);
    }
    *out = 0;
    return uri;
}

/* Call each(path, label, data) for each bookmark; label may be NULL. */
static void each_bookmark(const struct files *files, void (*each)(const char *, const char *, void *), void *data)
{
    FILE *file = files->bookmarks_path ? fopen(files->bookmarks_path, "re") : NULL;
    if (!file)
        return;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) > 0) {
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = 0;
        size_t uri_length = strcspn(line, " ");
        char *path = uri_path(line, uri_length);
        if (path)
            each(path, line[uri_length] && line[uri_length + 1] ? line + uri_length + 1 : NULL, data);
        free(path);
    }
    free(line);
    fclose(file);
}

struct bookmark_places {
    struct place **places;
    size_t *count;
    bool heading;
};

static void add_bookmark_place(const char *path, const char *label, void *data)
{
    struct bookmark_places *state = data;
    if (!is_directory(path))
        return;
    if (!state->heading) {
        add_place(state->places, state->count, "Bookmarks", NULL, 0);
        state->heading = true;
    }
    size_t before = *state->count;
    add_place(state->places, state->count, label ? label : strcmp(path, "/") == 0 ? "/" : base_name(path), path,
              ICON_FOLDER);
    if (*state->count > before)
        (*state->places)[before].bookmark = true;
}

struct bookmark_search {
    const char *path;
    bool found;
};

static void find_bookmark(const char *path, const char *label, void *data)
{
    (void)label;
    struct bookmark_search *search = data;
    search->found |= strcmp(path, search->path) == 0;
}

static bool is_bookmarked(const struct files *files, const char *path)
{
    struct bookmark_search search = {path, false};
    each_bookmark(files, find_bookmark, &search);
    return search.found;
}

/* Add path to the bookmarks, or with add false remove it, rewriting the
   file atomically and keeping every other line as it was. */
static int change_bookmark(struct files *files, const char *path, bool add)
{
    if (!files->bookmarks_path)
        return ENOENT;
    FILE *in = fopen(files->bookmarks_path, "re");
    char *directory = parent_path(files->bookmarks_path);
    int error = directory ? desk_mkdirs(directory, 0700) : ENOMEM;
    free(directory);
    DeskAtomic atomic;
    if (!error)
        error = desk_atomic_open(&atomic, files->bookmarks_path, 0644);
    if (error) {
        if (in)
            fclose(in);
        return error;
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while (in && (length = getline(&line, &capacity, in)) > 0) {
        char *path_here = uri_path(line, strcspn(line, " \r\n"));
        bool same = path_here && strcmp(path_here, path) == 0;
        free(path_here);
        if (!same)
            fputs(line, atomic.file);
        if (line[length - 1] != '\n' && !same)
            fputc('\n', atomic.file);
    }
    free(line);
    if (in)
        fclose(in);
    if (add) {
        char *uri = path_uri(path);
        if (uri)
            fprintf(atomic.file, "%s\n", uri);
        free(uri);
    }
    error = desk_atomic_commit(&atomic, true);
    desk_atomic_abort(&atomic);
    return error;
}

static void build_places(struct files *files)
{
    struct place *places = NULL;
    size_t count = 0;
    if (files->home && is_directory(files->home)) {
        add_place(&places, &count, "Places", NULL, 0);
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
    if (!count)
        add_place(&places, &count, "Places", NULL, 0);
    add_place(&places, &count, "File system", "/", ICON_DISK);
    struct bookmark_places bookmarks = {&places, &count, false};
    each_bookmark(files, add_bookmark_place, &bookmarks);
    add_drives(&places, &count);
    places_free(files->places, files->place_count);
    files->places = places;
    files->place_count = count;
}

/* Folder sizes

   A thread sums each directory's contents, apparent sizes of regular files
   without following links or leaving the file system, so the window never
   waits on a big tree. Moving to another directory starts a new job: the
   generation goes up and a walk in progress for the old one stops. */

struct size_result {
    unsigned long generation;
    char *name;
    long long total;
};

struct sizer {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int event;                      /* eventfd: results are waiting. */
    _Atomic unsigned long generation;
    bool quit;
    char *directory;                /* The job: these names in this directory. */
    char **names;
    size_t count, next;
    struct size_result *results;
    size_t result_count, result_capacity;
};

/* Sum what's under the open directory fd (taken over), on this device. */
static long long sum_tree(struct sizer *sizer, unsigned long generation, int fd, dev_t device)
{
    DIR *directory = fdopendir(fd);
    if (!directory) {
        close(fd);
        return 0;
    }
    long long total = 0;
    struct dirent *item;
    while ((item = readdir(directory)) && atomic_load(&sizer->generation) == generation) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
            continue;
        struct stat info;
        if (fstatat(dirfd(directory), item->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        if (S_ISREG(info.st_mode)) {
            total += info.st_size;
        } else if (S_ISDIR(info.st_mode) && info.st_dev == device) {
            int child = openat(dirfd(directory), item->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child >= 0)
                total += sum_tree(sizer, generation, child, device);
        }
    }
    closedir(directory);
    return total;
}

static long long sum_directory(struct sizer *sizer, unsigned long generation, const char *parent, const char *name)
{
    int base = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int fd = base >= 0 ? openat(base, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (base >= 0)
        close(base);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0) {
        if (fd >= 0)
            close(fd);
        return TOTAL_UNREADABLE;
    }
    return sum_tree(sizer, generation, fd, info.st_dev);
}

static void *sizer_main(void *data)
{
    struct sizer *sizer = data;
    pthread_mutex_lock(&sizer->lock);
    while (!sizer->quit) {
        if (sizer->next >= sizer->count) {
            pthread_cond_wait(&sizer->wake, &sizer->lock);
            continue;
        }
        unsigned long generation = atomic_load(&sizer->generation);
        char *directory = strdup(sizer->directory);
        char *name = strdup(sizer->names[sizer->next++]);
        pthread_mutex_unlock(&sizer->lock);
        long long total = directory && name ? sum_directory(sizer, generation, directory, name) : TOTAL_UNREADABLE;
        free(directory);
        pthread_mutex_lock(&sizer->lock);
        if (generation != atomic_load(&sizer->generation) || !name) {
            free(name);
            continue;
        }
        if (sizer->result_count == sizer->result_capacity) {
            size_t capacity = sizer->result_capacity ? sizer->result_capacity * 2 : 32;
            struct size_result *results = realloc(sizer->results, capacity * sizeof *results);
            if (!results) {
                free(name);
                continue;
            }
            sizer->results = results;
            sizer->result_capacity = capacity;
        }
        sizer->results[sizer->result_count++] = (struct size_result){generation, name, total};
        uint64_t one = 1;
        (void)!write(sizer->event, &one, sizeof one);
    }
    pthread_mutex_unlock(&sizer->lock);
    return NULL;
}

static void sizer_clear_job(struct sizer *sizer)
{
    for (size_t i = 0; i < sizer->count; ++i)
        free(sizer->names[i]);
    free(sizer->names);
    free(sizer->directory);
    sizer->names = NULL;
    sizer->directory = NULL;
    sizer->count = sizer->next = 0;
}

static struct sizer *sizer_create(void)
{
    struct sizer *sizer = calloc(1, sizeof *sizer);
    if (!sizer)
        return NULL;
    sizer->event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    pthread_mutex_init(&sizer->lock, NULL);
    pthread_cond_init(&sizer->wake, NULL);
    if (sizer->event < 0 || pthread_create(&sizer->thread, NULL, sizer_main, sizer) != 0) {
        if (sizer->event >= 0)
            close(sizer->event);
        pthread_mutex_destroy(&sizer->lock);
        pthread_cond_destroy(&sizer->wake);
        free(sizer);
        return NULL;
    }
    return sizer;
}

static void sizer_destroy(struct sizer *sizer)
{
    if (!sizer)
        return;
    pthread_mutex_lock(&sizer->lock);
    sizer->quit = true;
    atomic_fetch_add(&sizer->generation, 1); /* Stops a walk in progress. */
    pthread_cond_signal(&sizer->wake);
    pthread_mutex_unlock(&sizer->lock);
    pthread_join(sizer->thread, NULL);
    sizer_clear_job(sizer);
    for (size_t i = 0; i < sizer->result_count; ++i)
        free(sizer->results[i].name);
    free(sizer->results);
    close(sizer->event);
    pthread_mutex_destroy(&sizer->lock);
    pthread_cond_destroy(&sizer->wake);
    free(sizer);
}

/* Start summing the current listing's directories, dropping any earlier job. */
static void sizer_start(struct files *files)
{
    free(files->sized_path);
    files->sized_path = strdup(files->path);
    struct sizer *sizer = files->sizer;
    struct listing *listing = &files->listing;
    size_t count = 0;
    for (size_t i = 0; i < listing->count; ++i)
        count += listing->entries[i].total != TOTAL_NONE;
    char **names = count ? calloc(count, sizeof *names) : NULL;
    size_t named = 0;
    for (size_t i = 0; names && i < listing->count; ++i) {
        if (listing->entries[i].total != TOTAL_NONE && (names[named] = strdup(listing->entries[i].name)))
            ++named;
    }
    if (!sizer || (count && !names)) {
        /* Nothing will sum them: don't leave them waiting. */
        for (size_t i = 0; i < listing->count; ++i) {
            if (listing->entries[i].total == TOTAL_PENDING)
                listing->entries[i].total = TOTAL_NONE;
        }
        for (size_t i = 0; i < named; ++i)
            free(names[i]);
        free(names);
        return;
    }
    pthread_mutex_lock(&sizer->lock);
    atomic_fetch_add(&sizer->generation, 1);
    sizer_clear_job(sizer);
    sizer->directory = strdup(files->path);
    sizer->names = names;
    sizer->count = sizer->directory ? named : 0;
    pthread_cond_signal(&sizer->wake);
    pthread_mutex_unlock(&sizer->lock);
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
    struct listing old = files->listing;
    listing_read(&files->listing, files->path);
    /* Rereading the same directory: show the sizes summed last time until
       the new ones come in. */
    if (files->sized_path && strcmp(files->sized_path, files->path) == 0) {
        for (size_t i = 0; i < files->listing.count; ++i) {
            struct entry *entry = &files->listing.entries[i];
            for (size_t j = 0; entry->total == TOTAL_PENDING && j < old.count; ++j) {
                if (old.entries[j].total >= 0 && strcmp(old.entries[j].name, entry->name) == 0)
                    entry->total = old.entries[j].total;
            }
        }
    }
    listing_free(&old);
    listing_sort(&files->listing, files->sort, files->sort_reverse);
    sizer_start(files);
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
    files->typed[0] = 0;
    files->notice[0] = 0;
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

static void notify(struct files *files, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* A line over the foot of the list, until the next click or key. */
static void notify(struct files *files, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(files->notice, sizeof files->notice, format, arguments);
    va_end(arguments);
    desk_window_redraw(&files->window);
}

static void clear_notice(struct files *files)
{
    if (files->notice[0]) {
        files->notice[0] = 0;
        desk_window_redraw(&files->window);
    }
}

/* Types that are text even though they aren't text/: they open in the
   editor when nothing else claims them. */
static bool text_like(const char *mime)
{
    static const char *const types[] = {"application/json", "application/xml", "application/toml",
                                        "application/yaml", "application/x-shellscript", "application/x-desktop",
                                        "application/javascript", "application/x-ruby", "application/x-perl",
                                        "application/sql", NULL};
    if (strncmp(mime, "text/", 5) == 0)
        return true;
    size_t length = strlen(mime);
    if (length > 4 && (strcmp(mime + length - 4, "+xml") == 0 || strcmp(mime + length - 5, "+json") == 0))
        return true;
    for (int i = 0; types[i]; ++i) {
        if (strcmp(mime, types[i]) == 0)
            return true;
    }
    return false;
}

/* Open a file with its type's default application, as other desktops do:
   mimeapps.list, then the applications that declare the type. Text nothing
   claims goes to editor in the terminal. Programs aren't run. */
static void open_file(struct files *files, const char *path_in)
{
    char *path = strdup(path_in);
    if (!path)
        return;
    const char *name = base_name(path);
    DeskApps apps;
    if (desk_apps_load(&apps) != 0) {
        notify(files, "Out of memory");
        free(path);
        return;
    }
    const char *mime = desk_mime_type(&apps, path, name);
    const DeskApp *app = NULL;
    desk_apps_for(&apps, mime, &app, 1);
    int error = 0;
    if (app) {
        error = desk_app_launch(app, &path, 1, files->terminal, files->path);
        if (error)
            notify(files, "Couldn't start %s: %s", app->name, strerror(error));
    } else if (text_like(mime)) {
        char *argv[] = {"editor", path, NULL};
        if ((error = desk_spawn(argv, files->terminal, files->path)))
            notify(files, "Couldn't start editor: %s", strerror(error));
    } else {
        notify(files, "No application is set up to open %s (%s)", name, mime);
    }
    desk_apps_free(&apps);
    free(path);
}

/* Enter or double-click: into a directory, or open a file. */
static void go_in(struct files *files)
{
    struct entry *entry = cursor_entry(files);
    if (!entry)
        return;
    char *path = join_path(files->path, entry->name);
    if (!path)
        return;
    if (entry->directory) {
        go_to(files, path, NULL);
        return;
    }
    if (!entry->broken)
        open_file(files, path);
    else
        notify(files, "%s is a broken link", entry->name);
    free(path);
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

/* Sort again, keeping the cursor on the same entry. */
static void resort(struct files *files)
{
    struct entry *entry = cursor_entry(files);
    char *keep = entry ? strdup(entry->name) : NULL;
    listing_sort(&files->listing, files->sort, files->sort_reverse);
    filter(files);
    if (!select_name(files, keep))
        files->cursor = 0;
    free(keep);
    desk_window_redraw(&files->window);
}

/* A heading clicked: sort by it, or the other way when it's already sorted
   by it. Size and modified start with the biggest and newest. */
static void sort_by(struct files *files, enum column column)
{
    if (files->sort == column) {
        files->sort_reverse = !files->sort_reverse;
    } else {
        files->sort = column;
        files->sort_reverse = column == COLUMN_SIZE || column == COLUMN_MODIFIED;
    }
    files->free_scroll = false;
    save_state(files);
    resort(files);
}

/* Take the sizes the thread has summed. */
static void sizes_arrived(struct files *files)
{
    struct sizer *sizer = files->sizer;
    uint64_t count;
    (void)!read(sizer->event, &count, sizeof count);
    pthread_mutex_lock(&sizer->lock);
    struct size_result *results = sizer->results;
    size_t result_count = sizer->result_count;
    sizer->results = NULL;
    sizer->result_count = sizer->result_capacity = 0;
    unsigned long generation = atomic_load(&sizer->generation);
    pthread_mutex_unlock(&sizer->lock);
    bool changed = false;
    struct listing *listing = &files->listing;
    for (size_t i = 0; i < result_count; ++i) {
        for (size_t j = 0; results[i].generation == generation && j < listing->count; ++j) {
            struct entry *entry = &listing->entries[j];
            if (entry->directory && strcmp(entry->name, results[i].name) == 0) {
                changed |= entry->total != results[i].total;
                entry->total = results[i].total;
                break;
            }
        }
        free(results[i].name);
    }
    free(results);
    if (!changed)
        return;
    if (files->sort == COLUMN_SIZE)
        resort(files);
    else
        desk_window_redraw(&files->window);
}

/* Drag column's left edge to the pointer: its right edge stays put. */
static void resize_column(struct files *files, int column)
{
    const struct panes *panes = &files->panes;
    double unit = desk_window_scale(&files->window) * list_zooms[files->zoom[VIEW_LIST]] *
                  (panes->squeeze > 0.05 ? panes->squeeze : 0.05);
    double width = (panes->column_right[column] - files->pointer_x * desk_window_scale(&files->window)) / unit;
    if (width < COLUMN_MIN)
        width = COLUMN_MIN;
    if (width > 2000)
        width = 2000;
    if (width != files->column_width[column]) {
        files->column_width[column] = width;
        desk_window_redraw(&files->window);
    }
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
   current view; past the list's biggest or the icons' smallest it changes
   view. */
static void zoom(struct files *files, int direction)
{
    enum view view = files->view;
    int level = direction ? files->zoom[view] + direction : default_zoom[view];
    if (view == VIEW_LIST && level >= LIST_ZOOMS) {
        view = VIEW_ICONS;
        level = 0;
    } else if (view == VIEW_ICONS && level < 0) {
        view = VIEW_LIST;
        level = LIST_ZOOMS - 1;
    }
    int count = view == VIEW_LIST ? LIST_ZOOMS : ICON_ZOOMS;
    if (level < 0)
        level = 0;
    if (level >= count)
        level = count - 1;
    if (view == files->view && level == files->zoom[view])
        return;
    files->view = view;
    files->zoom[view] = level;
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

static void menu_key(struct files *files, xkb_keysym_t sym);
static void context_key(struct files *files);

/* Act on a key. Return whether it repeats while held. */
static bool key_action(struct files *files, xkb_keysym_t sym, unsigned mods)
{
    bool icons = files->view == VIEW_ICONS;
    long line = (long)columns(files);
    if (mods & DESK_MOD_CTRL) {
        switch (sym) {
        case XKB_KEY_h: toggle_hidden(files); return false;
        case XKB_KEY_r: refresh(files); return false;
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
        case XKB_KEY_Home: go_home(files); return false;
        default: return false;
        }
    }
    if (mods & DESK_MOD_LOGO)
        return false;
    switch (sym) {
    case XKB_KEY_Down:
        move_cursor(files, line);
        return true;
    case XKB_KEY_Up:
        move_cursor(files, -line);
        return true;
    case XKB_KEY_Right:
        if (icons)
            move_cursor(files, 1);
        return icons;
    case XKB_KEY_Left:
        if (icons)
            move_cursor(files, -1);
        return icons;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        go_in(files);
        return false;
    case XKB_KEY_BackSpace:
        go_up(files);
        return true;
    case XKB_KEY_Home:
        move_cursor(files, -(long)files->listing.shown_count);
        return false;
    case XKB_KEY_End:
        move_cursor(files, (long)files->listing.shown_count);
        return false;
    case XKB_KEY_Page_Down:
        move_cursor(files, page(files));
        return true;
    case XKB_KEY_Page_Up:
        move_cursor(files, -page(files));
        return true;
    case XKB_KEY_F5:
        refresh(files);
        return false;
    case XKB_KEY_F9:
        toggle_sidebar(files);
        return false;
    case XKB_KEY_Escape:
        search_clear(files);
        return false;
    default:
        return false;
    }
}

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

/* Whether name starts with prefix, ignoring ASCII case. */
static bool starts_with(const char *name, const char *prefix)
{
    return strncasecmp(name, prefix, strlen(prefix)) == 0;
}

/* Type to select: jump to the first name starting with what's been typed
   in the last second. The same letter again goes to the next name starting
   with it. */
static void type_to_select(struct files *files, const char *text, size_t length)
{
    struct listing *listing = &files->listing;
    uint64_t now = now_ms();
    if (now - files->typed_at > TYPE_RESET_MS)
        files->typed[0] = 0;
    files->typed_at = now;
    size_t used = strlen(files->typed); /* Bytes; letters are compared as bytes too. */
    if (used + length >= sizeof files->typed)
        return;
    memcpy(files->typed + used, text, length);
    files->typed[used + length] = 0;
    /* One letter over and over cycles through the names starting with it. */
    bool cycling = true;
    for (size_t i = 0; files->typed[i]; ++i)
        cycling &= strncasecmp(files->typed + i, files->typed, length) == 0 && (i % length) == 0;
    const char *prefix = files->typed;
    size_t start = files->cursor;
    if (cycling) {
        /* A first letter, or the same one again: the next name after this. */
        files->typed[length] = 0;
        start = files->cursor + 1;
    }
    size_t count = listing->shown_count;
    for (size_t step = 0; step < count; ++step) {
        size_t index = (start + step) % count;
        if (starts_with(listing->entries[listing->shown[index]].name, prefix)) {
            files->cursor = index;
            files->free_scroll = false;
            desk_window_redraw(&files->window);
            return;
        }
    }
}

/* A key press or repeat: to the search field when it has the focus, else
   to the window. Returns whether it repeats. */
static bool handle_key(struct files *files, uint32_t key)
{
    xkb_keysym_t sym = desk_keyboard_sym(&files->keyboard, key);
    unsigned mods = desk_keyboard_mods(&files->keyboard);
    clear_notice(files);
    if (files->menus[0].open) {
        menu_key(files, sym);
        return false;
    }
    if (sym == XKB_KEY_Menu || (sym == XKB_KEY_F10 && (mods & DESK_MOD_SHIFT))) {
        context_key(files);
        return false;
    }
    if (files->search_focus) {
        int repeats = search_key(files, key, sym, mods);
        if (repeats >= 0)
            return repeats;
    }
    if (!(mods & (DESK_MOD_CTRL | DESK_MOD_ALT | DESK_MOD_LOGO)) && files->keyboard.state) {
        char typed[16];
        int length = xkb_state_key_get_utf8(files->keyboard.state, key + 8, typed, sizeof typed);
        if (length > 0 && (unsigned char)typed[0] > 0x20 && typed[0] != 0x7f) {
            type_to_select(files, typed, (size_t)length);
            return false;
        }
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

/* What kind of file an entry is, for the Type column: "Folder", "PNG image",
   "C source", "Text file" and so on, from the extension. */
static void entry_type(const struct entry *entry, char *out, size_t length)
{
    if (entry->broken) {
        snprintf(out, length, "Broken link");
        return;
    }
    if (entry->directory) {
        snprintf(out, length, entry->link ? "Folder link" : "Folder");
        return;
    }
    const char *dot = strrchr(entry->name, '.');
    char extension[9] = "";
    if (dot && dot != entry->name && strlen(dot + 1) < sizeof extension) {
        for (size_t i = 0; dot[1 + i]; ++i)
            extension[i] = (char)toupper((unsigned char)dot[1 + i]);
        extension[strlen(dot + 1)] = 0;
    }
    const char *kind;
    switch (entry_icon(entry)) {
    case ICON_IMAGE: kind = "image"; break;
    case ICON_VIDEO: kind = "video"; break;
    case ICON_AUDIO: kind = "audio"; break;
    case ICON_ARCHIVE: kind = "archive"; break;
    case ICON_PDF: snprintf(out, length, "PDF document"); return;
    case ICON_MARKDOWN: snprintf(out, length, "Markdown file"); return;
    case ICON_CODE: kind = extension[0] ? "source" : "Makefile"; break;
    case ICON_TEXT: kind = "file"; break;
    case ICON_EXECUTABLE: snprintf(out, length, "Program"); return;
    default: kind = "file"; break;
    }
    if (!extension[0])
        snprintf(out, length, "%c%s", toupper((unsigned char)kind[0]), kind + 1);
    else
        snprintf(out, length, "%s %s", extension, kind);
}

static bool hovering(const struct files *files, enum target_kind kind, size_t index)
{
    return !files->drag && !files->resizing && files->hover.kind == kind && files->hover.index == index;
}

/* Lay out the list's columns for this frame: type, size and modified at
   their widths from the right, the name taking the rest. When the window
   is too narrow for them and a name, all three are narrowed in proportion
   rather than dropped. */
static void layout_columns(struct files *files, const struct layout *layout)
{
    struct panes *panes = &files->panes;
    double unit = desk_window_scale(&files->window) * list_zooms[files->zoom[VIEW_LIST]];
    int inner = layout->padding / 2, gap = layout->gap;
    int left = panes->list_left + inner, right = panes->list_right - inner;
    int wanted = 0;
    for (int c = COLUMN_TYPE; c < COLUMN_COUNT; ++c)
        wanted += (int)lround(files->column_width[c] * unit) + gap;
    int room = right - left - (int)lround(NAME_MIN * unit);
    panes->squeeze = wanted > room && wanted > 0 ? fmax(room, 0) / wanted : 1;
    int x = right;
    for (int c = COLUMN_COUNT - 1; c > COLUMN_NAME; --c) {
        int width = (int)lround(files->column_width[c] * unit * panes->squeeze);
        int space = (int)lround(gap * panes->squeeze);
        panes->column_right[c] = x;
        panes->column_left[c] = x - width;
        panes->edge[c] = x - width - space / 2;
        x -= width + space;
    }
    panes->column_left[COLUMN_NAME] = left;
    panes->column_right[COLUMN_NAME] = x;
    panes->edge[COLUMN_NAME] = panes->list_left;
    panes->edge[COLUMN_COUNT] = panes->list_right;
}

/* Text in [left, right), cut at the end with … when it doesn't fit. */
static void draw_clipped(DeskFont *font, DeskCanvas *canvas, int left, int right, int baseline, const char *text,
                         uint32_t color)
{
    if (right - left < 2)
        return;
    DeskCanvas clipped = *canvas;
    desk_canvas_clip(&clipped, left > canvas->clip_left ? left : canvas->clip_left, canvas->clip_top,
                     right < canvas->clip_right ? right : canvas->clip_right, canvas->clip_bottom);
    draw_name(font, &clipped, left, baseline, right - left, text, true, color);
}

/* Text right-aligned in [left, right), or from left, cut, when it's too long. */
static void draw_right(DeskFont *font, DeskCanvas *canvas, int left, int right, int baseline, const char *text,
                       uint32_t color)
{
    int width = desk_text_width(font, text, strlen(text));
    draw_clipped(font, canvas, width <= right - left ? right - width : left, right, baseline, text, color);
}

/* A small solid triangle pointing up or down, centered on x, y. */
static void draw_arrow(DeskCanvas *canvas, int x, int y, int half, bool up, uint32_t color)
{
    for (int row = 0; row <= half; ++row) {
        int reach = up ? row : half - row;
        desk_canvas_fill(canvas, x - reach, y - half / 2 + row, 2 * reach + 1, 1, color);
    }
}

/* The column header over the list, in the window's font, lined up with the
   list's columns at its zoom: each heading sorts by it, the sorted one with
   an arrow, and the edges between them resize the columns. */
static void paint_header(struct files *files, DeskCanvas *canvas, const struct layout *ui, int y)
{
    const struct colors *colors = &files->colors;
    const struct panes *panes = &files->panes;
    int baseline = y + ui->baseline, height = ui->row_height;
    int line = desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1;
    int half = (int)lround(desk_font_metrics(ui->font).height * 0.18);
    uint32_t rule = mix(colors->background, colors->muted, 70);
    for (int c = 0; c < COLUMN_COUNT; ++c) {
        if (hovering(files, TARGET_HEADING, (size_t)c))
            desk_canvas_fill(canvas, panes->edge[c], y, panes->edge[c + 1] - panes->edge[c], height - line,
                             mix(colors->background, colors->muted, 35));
        bool sorted = files->sort == (enum column)c;
        uint32_t color = sorted ? colors->foreground : colors->muted;
        int left = panes->column_left[c], right = panes->column_right[c];
        int arrow = sorted ? 2 * half + ui->gap / 4 : 0; /* Room for the arrow. */
        const char *title = column_titles[c];
        int width = desk_text_width(ui->font, title, strlen(title));
        if (c == COLUMN_SIZE) {
            /* Right-aligned like the sizes, the arrow before it. */
            draw_right(ui->font, canvas, left + arrow, right, baseline, title, color);
            if (sorted && right - left > width + arrow)
                draw_arrow(canvas, right - width - arrow / 2 - ui->gap / 8, y + height / 2, half,
                           !files->sort_reverse, color);
        } else {
            draw_clipped(ui->font, canvas, left, right - arrow, baseline, title, color);
            if (sorted && right - left > width + arrow)
                draw_arrow(canvas, left + width + arrow / 2 + ui->gap / 8, y + height / 2, half,
                           !files->sort_reverse, color);
        }
        if (c > COLUMN_NAME) {
            bool active = files->resizing == c || hovering(files, TARGET_COLUMN_EDGE, (size_t)c);
            int width_line = active ? 3 * line : line;
            desk_canvas_fill(canvas, panes->edge[c] - width_line / 2, y + height / 5, width_line, height * 3 / 5,
                             active ? colors->directory : rule);
        }
    }
    desk_canvas_fill(canvas, panes->list_left, y + height - line, panes->list_right - panes->list_left, line, rule);
}

/* One row of the list between x0 and x1, in the columns laid out. */
static void draw_row(struct files *files, DeskCanvas *canvas, const struct layout *layout, struct entry *entry,
                     int x0, int x1, int y, bool selected)
{
    const struct colors *colors = &files->colors;
    const struct panes *panes = &files->panes;
    if (selected)
        desk_canvas_fill(canvas, x0, y, x1 - x0, layout->row_height, colors->selection_background);
    uint32_t color = entry_color(files, entry, selected);
    int baseline = y + layout->baseline, inner = layout->padding / 2;
    draw_icon(layout->font, canvas, x0 + inner / 2, x0 + inner / 2 + layout->icon_width, baseline,
              entry_icon(entry), color);

    int name_x = x0 + inner + layout->icon_width, name_right = panes->column_right[COLUMN_NAME];
    if (name_right > name_x) {
        DeskCanvas clipped = *canvas;
        desk_canvas_clip(&clipped, name_x > canvas->clip_left ? name_x : canvas->clip_left, canvas->clip_top,
                         name_right < canvas->clip_right ? name_right : canvas->clip_right, canvas->clip_bottom);
        draw_name(entry->link ? layout->italic : layout->font, &clipped, name_x, baseline, name_right - name_x,
                  entry->name, entry->directory, color);
    }

    uint32_t detail_color = selected ? colors->selection_foreground : colors->muted;
    char type[48], size[32] = "", date[32];
    entry_type(entry, type, sizeof type);
    if (entry->directory && entry->total >= 0)
        format_size((off_t)entry->total, size, sizeof size);
    else if (entry->directory && entry->total == TOTAL_PENDING)
        snprintf(size, sizeof size, "…");
    else if (entry->directory && entry->total == TOTAL_UNREADABLE)
        snprintf(size, sizeof size, "?");
    else if (!entry->directory && !entry->broken)
        format_size(entry->size, size, sizeof size);
    format_date(&entry->modified, date, sizeof date);
    draw_clipped(layout->font, canvas, panes->column_left[COLUMN_TYPE], panes->column_right[COLUMN_TYPE], baseline,
                 type, detail_color);
    draw_right(layout->mono, canvas, panes->column_left[COLUMN_SIZE], panes->column_right[COLUMN_SIZE], baseline,
               size, detail_color);
    draw_clipped(layout->mono, canvas, panes->column_left[COLUMN_MODIFIED], panes->column_right[COLUMN_MODIFIED],
                 baseline, date, detail_color);
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
    for (int row = 0; row < rows && files->top + (size_t)row < listing->shown_count; ++row) {
        size_t index = files->top + (size_t)row;
        draw_row(files, canvas, layout, &listing->entries[listing->shown[index]], x0, x1,
                 y + row * layout->row_height, index == files->cursor);
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
#define ICON_CLOSE 0xf00d
#define ICON_PLUS 0xf067
#define CRUMB_SEPARATOR "\xe2\x80\xba" /* › */
#define ELLIPSIS "\xe2\x80\xa6"
#define SEARCH_WIDTH 240 /* Logical pixels, at most. */
#define TAB_WIDTH 220    /* Logical pixels, at most. */

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

/* A magnifying glass in a size×size square at x, y: a ring and a handle,
   drawn here rather than taken from a font, where fallback fonts disagree
   on the glyph. Edges are smoothed by sampling each pixel 4×4. */
static void draw_magnifier(DeskCanvas *canvas, int x, int y, int size, uint32_t color, uint32_t background)
{
    double glyph = size * 0.5, left = x + (size - glyph) / 2, top = y + (size - glyph) / 2;
    double radius = glyph * 0.34, stroke = fmax(1.2, glyph * 0.13);
    double cx = left + radius + stroke / 2, cy = top + radius + stroke / 2;
    double hx = left + glyph - stroke / 2, hy = top + glyph - stroke / 2; /* The handle's end. */
    double reach = radius * M_SQRT1_2;
    double sx = cx + reach, sy = cy + reach; /* Where it leaves the ring. */
    for (int py = (int)top - 1; py <= (int)(top + glyph) + 1; ++py) {
        for (int px = (int)left - 1; px <= (int)(left + glyph) + 1; ++px) {
            int hits = 0;
            for (int i = 0; i < 16; ++i) {
                double qx = px + (i % 4 + 0.5) / 4, qy = py + (i / 4 + 0.5) / 4;
                double ring = fabs(hypot(qx - cx, qy - cy) - radius);
                /* Distance to the handle's segment. */
                double dx = hx - sx, dy = hy - sy;
                double t = ((qx - sx) * dx + (qy - sy) * dy) / (dx * dx + dy * dy);
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                double handle = hypot(qx - (sx + t * dx), qy - (sy + t * dy));
                hits += ring <= stroke / 2 || handle <= stroke * 0.6;
            }
            if (hits)
                desk_canvas_fill(canvas, px, py, 1, 1, mix(background, color, (unsigned)(hits * 255 / 16)));
        }
    }
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
    draw_magnifier(canvas, left, top, height, colors->muted, mix(colors->background, colors->muted, 35));

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

/* The notice, if any, in a bar across the foot of the list. */
static void paint_notice(struct files *files, DeskCanvas *canvas, const struct layout *ui, int bottom)
{
    if (!files->notice[0])
        return;
    const struct colors *colors = &files->colors;
    const struct panes *panes = &files->panes;
    int height = ui->row_height, top = bottom - height;
    desk_canvas_fill(canvas, panes->list_left, top, panes->list_right - panes->list_left, height,
                     mix(colors->background, colors->muted, 60));
    draw_clipped(ui->font, canvas, panes->list_left + ui->padding, panes->list_right - ui->padding,
                 top + ui->baseline, files->notice, colors->foreground);
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
    /* The list view has a column header over the entries. */
    if (files->view == VIEW_LIST)
        layout_columns(files, &list);
    panes->entries_top = files->view == VIEW_LIST ? top + ui.row_height + ui.padding / 4 : top;
    int rows = (bottom - panes->entries_top) / panes->line_height;
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

    if (files->view == VIEW_LIST) {
        DeskCanvas header = *canvas;
        desk_canvas_clip(&header, panes->list_left, top, panes->list_right, panes->entries_top);
        paint_header(files, &header, &ui, top);
    }
    top = panes->entries_top;
    DeskCanvas area = *canvas;
    desk_canvas_clip(&area, panes->list_left, top, panes->list_right, bottom);
    uint32_t message_color;
    const char *message = empty_message(files, &message_color);
    if (message) {
        draw_message(&area, &list, panes->list_left, top, message, message_color);
        paint_notice(files, &area, &ui, bottom);
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
    paint_notice(files, &area, &ui, bottom);
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
    (void)time;
    struct files *files = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        desk_repeat_release(&files->repeat, key);
        return;
    }
    files->input_serial = serial;
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
    if (files->drag || files->resizing || files->hover.kind == TARGET_DIVIDER ||
        files->hover.kind == TARGET_COLUMN_EDGE)
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
        if (y < panes->entries_top) {
            /* The column header: an edge to drag, or a heading to sort by. */
            int reach = (int)lround(DIVIDER_GRAB * desk_window_scale(&files->window));
            for (int c = COLUMN_TYPE; c < COLUMN_COUNT; ++c) {
                if (abs(x - panes->edge[c]) <= reach)
                    return (struct target){TARGET_COLUMN_EDGE, (size_t)c};
            }
            for (int c = 0; c < COLUMN_COUNT; ++c) {
                if (x >= panes->edge[c] && x < panes->edge[c + 1])
                    return (struct target){TARGET_HEADING, (size_t)c};
            }
            return (struct target){TARGET_NONE, 0};
        }
        size_t line = files->top + (size_t)((y - panes->entries_top) / panes->line_height);
        size_t column = (size_t)((x - panes->list_left) / panes->tile_width);
        size_t index = line * columns(files) + column;
        if (column < columns(files) && index < files->listing.shown_count && !empty_message(files, &(uint32_t){0}))
            return (struct target){TARGET_ROW, index};
    }
    return (struct target){TARGET_NONE, 0};
}

/* Clipboard: Copy path offers text until something else takes the
   clipboard. */

static void clip_target(void *data, struct wl_data_source *source, const char *mime)
{
    (void)data;
    (void)source;
    (void)mime;
}

static void clip_send(void *data, struct wl_data_source *source, const char *mime, int32_t fd)
{
    (void)source;
    (void)mime;
    struct files *files = data;
    const char *text = files->clip_text ? files->clip_text : "";
    size_t left = strlen(text);
    while (left) {
        ssize_t written = write(fd, text, left);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        text += written;
        left -= (size_t)written;
    }
    close(fd);
}

static void clip_cancelled(void *data, struct wl_data_source *source)
{
    struct files *files = data;
    if (files->clip_source == source) {
        files->clip_source = NULL;
        free(files->clip_text);
        files->clip_text = NULL;
    }
    wl_data_source_destroy(source);
}

static void clip_dnd_drop_performed(void *data, struct wl_data_source *source)
{
    (void)data;
    (void)source;
}

static void clip_dnd_finished(void *data, struct wl_data_source *source)
{
    (void)data;
    (void)source;
}

static void clip_action(void *data, struct wl_data_source *source, uint32_t action)
{
    (void)data;
    (void)source;
    (void)action;
}

static const struct wl_data_source_listener clip_listener = {
    .target = clip_target,
    .send = clip_send,
    .cancelled = clip_cancelled,
    .dnd_drop_performed = clip_dnd_drop_performed,
    .dnd_finished = clip_dnd_finished,
    .action = clip_action,
};

static void copy_text(struct files *files, const char *text)
{
    if (!files->data_device) {
        notify(files, "The compositor has no clipboard to copy to");
        return;
    }
    char *copy = strdup(text);
    if (!copy)
        return;
    struct wl_data_source *source = wl_data_device_manager_create_data_source(files->data_manager);
    wl_data_source_add_listener(source, &clip_listener, files);
    static const char *const types[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"};
    for (size_t i = 0; i < sizeof types / sizeof *types; ++i)
        wl_data_source_offer(source, types[i]);
    wl_data_device_set_selection(files->data_device, source, files->input_serial);
    if (files->clip_source)
        wl_data_source_destroy(files->clip_source);
    free(files->clip_text);
    files->clip_source = source;
    files->clip_text = copy;
}

/* Other clients' clipboard and drags: nothing is read from them yet, but
   their offers are released. */
static void device_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
    (void)data;
    (void)device;
    (void)offer;
}

static void device_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface,
                         wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer)
{
    (void)data;
    (void)device;
    (void)serial;
    (void)surface;
    (void)x;
    (void)y;
    if (offer)
        wl_data_offer_destroy(offer); /* Drops onto files come in 0.4. */
}

static void device_leave(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
}

static void device_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)device;
    (void)time;
    (void)x;
    (void)y;
}

static void device_drop(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
}

static void device_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
    (void)device;
    struct files *files = data;
    if (files->selection_offer)
        wl_data_offer_destroy(files->selection_offer);
    files->selection_offer = offer;
}

static const struct wl_data_device_listener device_listener = {
    .data_offer = device_data_offer,
    .enter = device_enter,
    .leave = device_leave,
    .motion = device_motion,
    .drop = device_drop,
    .selection = device_selection,
};

/* Menus */

enum {
    MENU_OPEN,
    MENU_OPEN_WITH,       /* index: into menu_apps */
    MENU_OPEN_WITH_LIST,  /* The submenu of applications. */
    MENU_RUN,
    MENU_NEW_TAB,
    MENU_COPY_PATH,
    MENU_ADD_BOOKMARK,
    MENU_REMOVE_BOOKMARK,
    MENU_HIDDEN,
    MENU_SORT_LIST,       /* The submenu of columns. */
    MENU_SORT,            /* index: COLUMN_ */
    MENU_DESCENDING,
    MENU_REFRESH,
    MENU_NOTHING,         /* A disabled line such as "No applications". */
};

static struct menu_item *menu_add(struct menu *menu, int action, int index, const char *label)
{
    static struct menu_item spare;
    if (menu->count == MENU_ITEMS)
        return &spare;
    struct menu_item *item = &menu->items[menu->count++];
    *item = (struct menu_item){.action = action, .index = index, .enabled = true};
    snprintf(item->label, sizeof item->label, "%s", label);
    return item;
}

static void menu_separator(struct menu *menu)
{
    if (menu->count && !menu->items[menu->count - 1].separator && menu->count < MENU_ITEMS)
        menu->items[menu->count++] = (struct menu_item){.separator = true};
}

/* Sizes in logical pixels, from the longest label. */
static void menu_measure(struct files *files, struct menu *menu)
{
    double scale = desk_window_scale(&files->window);
    double font = font_pixels(files) / scale;
    menu->item_height = (int)lround(font * 1.9);
    menu->separator_height = (int)lround(font * 0.7);
    menu->padding = 4;
    int widest = 0;
    for (size_t i = 0; i < menu->count; ++i) {
        int width = desk_text_width(files->font, menu->items[i].label, strlen(menu->items[i].label));
        if (width > widest)
            widest = width;
    }
    /* A check column, the label, a gap and the submenu arrow. */
    int text = (int)lround(widest / scale);
    menu->width = text + 2 * menu->item_height + 24;
    if (menu->width < 180)
        menu->width = 180;
    menu->height = 2 * menu->padding;
    for (size_t i = 0; i < menu->count; ++i)
        menu->height += menu->items[i].separator ? menu->separator_height : menu->item_height;
}

static int menu_item_top(const struct menu *menu, int index)
{
    int y = menu->padding;
    for (int i = 0; i < index; ++i)
        y += menu->items[i].separator ? menu->separator_height : menu->item_height;
    return y;
}

/* The item at logical y, or -1. */
static int menu_item_at(const struct menu *menu, double y)
{
    int top = menu->padding;
    for (size_t i = 0; i < menu->count; ++i) {
        int height = menu->items[i].separator ? menu->separator_height : menu->item_height;
        if (y >= top && y < top + height)
            return menu->items[i].separator ? -1 : (int)i;
        top += height;
    }
    return -1;
}

static bool menu_choosable(const struct menu *menu, int index)
{
    return index >= 0 && (size_t)index < menu->count && !menu->items[index].separator &&
           menu->items[index].enabled;
}

/* A check mark: two strokes, smoothed like the magnifier. */
static void draw_check(DeskCanvas *canvas, int x, int y, int size, uint32_t color, uint32_t background)
{
    double points[3][2] = {{x + size * 0.15, y + size * 0.55}, {x + size * 0.4, y + size * 0.8},
                           {x + size * 0.85, y + size * 0.2}};
    double stroke = fmax(1.2, size * 0.12);
    for (int py = y; py < y + size; ++py) {
        for (int px = x; px < x + size; ++px) {
            int hits = 0;
            for (int i = 0; i < 16; ++i) {
                double qx = px + (i % 4 + 0.5) / 4, qy = py + (i / 4 + 0.5) / 4;
                for (int s = 0; s < 2; ++s) {
                    double ax = points[s][0], ay = points[s][1], dx = points[s + 1][0] - ax,
                           dy = points[s + 1][1] - ay;
                    double t = ((qx - ax) * dx + (qy - ay) * dy) / (dx * dx + dy * dy);
                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    if (hypot(qx - (ax + t * dx), qy - (ay + t * dy)) <= stroke / 2) {
                        ++hits;
                        break;
                    }
                }
            }
            if (hits)
                desk_canvas_fill(canvas, px, py, 1, 1, mix(background, color, (unsigned)(hits * 255 / 16)));
        }
    }
}

static void paint_menu(struct files *files, struct menu *menu, DeskCanvas *canvas)
{
    const struct colors *colors = &files->colors;
    double scale = desk_window_scale(&menu->window);
    int line = scale >= 2 ? (int)scale : 1;
    uint32_t background = mix(colors->background, colors->muted, 25);
    desk_canvas_fill(canvas, 0, 0, canvas->width, canvas->height, background);
    uint32_t border = mix(colors->background, colors->muted, 110);
    desk_canvas_fill(canvas, 0, 0, canvas->width, line, border);
    desk_canvas_fill(canvas, 0, canvas->height - line, canvas->width, line, border);
    desk_canvas_fill(canvas, 0, 0, line, canvas->height, border);
    desk_canvas_fill(canvas, canvas->width - line, 0, line, canvas->height, border);
    DeskFontMetrics metrics = desk_font_metrics(files->font);
    int item_height = (int)lround(menu->item_height * scale);
    int column = item_height; /* The check column's width. */
    for (size_t i = 0; i < menu->count; ++i) {
        const struct menu_item *item = &menu->items[i];
        int top = (int)lround(menu_item_top(menu, (int)i) * scale);
        if (item->separator) {
            int height = (int)lround(menu->separator_height * scale);
            desk_canvas_fill(canvas, column / 2, top + height / 2, canvas->width - column, line, border);
            continue;
        }
        bool lit = (int)i == menu->hover && item->enabled;
        if (lit)
            desk_canvas_fill(canvas, line * 2, top, canvas->width - line * 4, item_height,
                             colors->selection_background);
        uint32_t color = !item->enabled ? colors->muted : lit ? colors->selection_foreground : colors->foreground;
        uint32_t behind = lit ? colors->selection_background : background;
        int baseline = top + (item_height - (metrics.ascent + metrics.descent)) / 2 + metrics.ascent;
        if (item->checked) {
            int size = item_height / 2;
            draw_check(canvas, (column - size) / 2 + line * 2, top + (item_height - size) / 2, size, color, behind);
        }
        draw_clipped(files->font, canvas, column, canvas->width - column, baseline, item->label, color);
        if (item->submenu) {
            int half = item_height / 6, x = canvas->width - column / 2, y = top + item_height / 2;
            for (int row = -half; row <= half; ++row) {
                int reach = half - abs(row);
                desk_canvas_fill(canvas, x - half / 2, y + row, reach + 1, 1, color);
            }
        }
    }
}

static struct menu *menu_of(struct files *files, const DeskWindow *window)
{
    for (int i = 0; i < 2; ++i) {
        if (files->menus[i].open && window == &files->menus[i].window)
            return &files->menus[i];
    }
    return NULL;
}

/* The menu a pointer surface belongs to, or NULL for the window. */
static struct menu *menu_at_surface(struct files *files, const struct wl_surface *surface)
{
    for (int i = 0; i < 2; ++i) {
        if (files->menus[i].open && surface && files->menus[i].window.surface == surface)
            return &files->menus[i];
    }
    return NULL;
}

static void menu_draw(void *data, DeskWindow *window)
{
    struct files *files = data;
    struct menu *menu = menu_of(files, window);
    DeskBuffer *buffer = desk_window_buffer(window, WL_SHM_FORMAT_XRGB8888);
    if (!menu || !buffer)
        return;
    DeskCanvas canvas = desk_canvas(buffer->pixels, buffer->width, buffer->height);
    paint_menu(files, menu, &canvas);
    desk_window_commit(window, buffer);
}

static void close_menus(struct files *files);

static void menu_dismissed(void *data, DeskWindow *window)
{
    (void)window;
    close_menus(data);
}

static const DeskWindowListener menu_listener = {
    .draw = menu_draw,
    .close = menu_dismissed,
};

static void close_submenu(struct files *files)
{
    struct menu *sub = &files->menus[1];
    if (sub->open) {
        desk_window_close(&sub->window);
        sub->open = false;
    }
}

static void close_menus(struct files *files)
{
    close_submenu(files);
    if (files->menus[0].open) {
        desk_window_close(&files->menus[0].window);
        files->menus[0].open = false;
    }
    free(files->menu_path);
    files->menu_path = NULL;
    for (size_t i = 0; i < files->menu_app_count; ++i)
        free(files->menu_apps[i]);
    files->menu_app_count = 0;
}

static bool menu_show(struct files *files, struct menu *menu, DeskWindow *parent, int x, int y)
{
    menu_measure(files, menu);
    if (!desk_popup_open(&menu->window, &files->globals, parent, x, y, menu->width, menu->height, files->seat,
                         files->input_serial, &menu_listener, files))
        return false;
    menu->open = true;
    return true;
}

static void menu_first(struct menu *menu)
{
    menu->hover = -1;
    for (size_t i = 0; i < menu->count && menu->hover < 0; ++i) {
        if (menu_choosable(menu, (int)i))
            menu->hover = (int)i;
    }
}

/* Open with's applications for the file at path: the default first. */
static void fill_open_with(struct files *files, struct menu *menu)
{
    DeskApps apps;
    if (desk_apps_load(&apps) != 0)
        return;
    const char *mime = desk_mime_type(&apps, files->menu_path, base_name(files->menu_path));
    const DeskApp *found[MENU_APPS];
    size_t count = desk_apps_for(&apps, mime, found, MENU_APPS);
    if (count > MENU_APPS)
        count = MENU_APPS;
    for (size_t i = 0; i < count; ++i) {
        char *id = strdup(found[i]->id);
        if (!id)
            break;
        files->menu_apps[files->menu_app_count] = id;
        char label[160];
        snprintf(label, sizeof label, i == 0 ? "%s (default)" : "%s", found[i]->name);
        menu_add(menu, MENU_OPEN_WITH, (int)files->menu_app_count++, label);
    }
    if (!files->menu_app_count)
        menu_add(menu, MENU_NOTHING, 0, "No applications for this type")->enabled = false;
    desk_apps_free(&apps);
}

static void build_submenu(struct files *files, int item_index)
{
    struct menu *sub = &files->menus[1];
    const struct menu_item *item = &files->menus[0].items[item_index];
    *sub = (struct menu){.hover = -1, .parent_item = item_index};
    if (item->action == MENU_OPEN_WITH_LIST) {
        for (size_t i = 0; i < files->menu_app_count; ++i)
            free(files->menu_apps[i]);
        files->menu_app_count = 0;
        fill_open_with(files, sub);
    } else if (item->action == MENU_SORT_LIST) {
        for (int c = 0; c < COLUMN_COUNT; ++c)
            menu_add(sub, MENU_SORT, c, column_titles[c])->checked = files->sort == (enum column)c;
        menu_separator(sub);
        menu_add(sub, MENU_DESCENDING, 0, "Descending")->checked = files->sort_reverse;
    }
}

static void open_submenu(struct files *files, int item_index, bool keyboard)
{
    struct menu *parent = &files->menus[0], *sub = &files->menus[1];
    if (sub->open && sub->parent_item == item_index)
        return;
    close_submenu(files);
    build_submenu(files, item_index);
    if (keyboard)
        menu_first(sub);
    menu_show(files, sub, &parent->window, parent->width - 4, menu_item_top(parent, item_index) - sub->padding);
}

/* Fill the right-click menu for context. Return false when out of memory. */
static bool build_menu(struct files *files, enum menu_context context, const char *path, bool directory,
                       bool executable)
{
    struct menu *menu = &files->menus[0];
    *menu = (struct menu){.hover = -1};
    files->menu_context = context;
    files->menu_path = strdup(path);
    files->menu_directory = directory;
    if (!files->menu_path)
        return false;
    if (context == CONTEXT_BACKGROUND) {
        menu_add(menu, MENU_HIDDEN, 0, "Show hidden files")->checked = files->show_hidden;
        menu_add(menu, MENU_SORT_LIST, 0, "Sort by")->submenu = true;
        menu_separator(menu);
        menu_add(menu, MENU_REFRESH, 0, "Refresh");
        menu_add(menu, MENU_COPY_PATH, 0, "Copy path");
    } else {
        menu_add(menu, MENU_OPEN, 0, "Open");
        if (directory) {
            menu_add(menu, MENU_NEW_TAB, 0, "Open in new tab");
        } else {
            menu_add(menu, MENU_OPEN_WITH_LIST, 0, "Open with")->submenu = true;
            if (executable)
                menu_add(menu, MENU_RUN, 0, "Run");
        }
        menu_separator(menu);
        menu_add(menu, MENU_COPY_PATH, 0, "Copy path");
        if (directory) {
            bool bookmarked = is_bookmarked(files, path);
            if (!bookmarked)
                menu_add(menu, MENU_ADD_BOOKMARK, 0, "Add to sidebar");
            else if (context == CONTEXT_PLACE)
                menu_add(menu, MENU_REMOVE_BOOKMARK, 0, "Remove from sidebar");
        }
    }
    return true;
}

/* Build and open the right-click menu for context at x, y in the window. */
static void open_menu(struct files *files, enum menu_context context, const char *path, bool directory,
                      bool executable, int x, int y, bool keyboard)
{
    close_menus(files);
    if (!build_menu(files, context, path, directory, executable))
        return;
    if (keyboard)
        menu_first(&files->menus[0]);
    menu_show(files, &files->menus[0], &files->window, x, y);
}

/* Run an executable: a script in the terminal, so its output can be
   seen; a program on its own. */
static void run_file(struct files *files, const char *path)
{
    DeskApps none = {0};
    bool script = strcmp(desk_mime_type(&none, path, ""), "text/plain") == 0;
    char *argv[] = {(char *)path, NULL};
    char *directory = parent_path(path);
    int error = desk_spawn(argv, script ? files->terminal : NULL, directory ? directory : files->path);
    free(directory);
    if (error)
        notify(files, "Couldn't run %s: %s", base_name(path), strerror(error));
}

static void open_with(struct files *files, const char *path, const char *id)
{
    DeskApps apps;
    if (desk_apps_load(&apps) != 0)
        return;
    const DeskApp *app = desk_app_find(&apps, id);
    char *paths[] = {(char *)path};
    char *directory = parent_path(path);
    int error = app ? desk_app_launch(app, paths, 1, files->terminal, directory ? directory : files->path) : ENOENT;
    free(directory);
    if (error)
        notify(files, "Couldn't start %s: %s", app ? app->name : id, strerror(error));
    desk_apps_free(&apps);
}

static void menu_choose(struct files *files, struct menu *menu, int index, bool keyboard)
{
    if (!menu_choosable(menu, index))
        return;
    const struct menu_item *item = &menu->items[index];
    if (item->submenu) {
        open_submenu(files, index, keyboard);
        return;
    }
    int action = item->action, which = item->index;
    char *path = files->menu_path ? strdup(files->menu_path) : NULL;
    char *app = action == MENU_OPEN_WITH && (size_t)which < files->menu_app_count ? strdup(files->menu_apps[which])
                                                                                  : NULL;
    bool directory = files->menu_directory;
    close_menus(files);
    if (!path)
        return;
    switch (action) {
    case MENU_OPEN:
        if (directory)
            go_to(files, strdup(path), NULL);
        else
            open_file(files, path);
        break;
    case MENU_OPEN_WITH:
        if (app)
            open_with(files, path, app);
        break;
    case MENU_RUN:
        run_file(files, path);
        break;
    case MENU_NEW_TAB:
        new_tab(files, strdup(path), true);
        break;
    case MENU_COPY_PATH:
        copy_text(files, path);
        break;
    case MENU_ADD_BOOKMARK:
    case MENU_REMOVE_BOOKMARK: {
        int error = change_bookmark(files, path, action == MENU_ADD_BOOKMARK);
        if (error)
            notify(files, "Couldn't change the bookmarks: %s", strerror(error));
        build_places(files);
        desk_window_redraw(&files->window);
        break;
    }
    case MENU_HIDDEN:
        toggle_hidden(files);
        break;
    case MENU_SORT:
        if (files->sort != (enum column)which)
            sort_by(files, (enum column)which);
        break;
    case MENU_DESCENDING:
        files->sort_reverse = !files->sort_reverse;
        save_state(files);
        resort(files);
        break;
    case MENU_REFRESH:
        refresh(files);
        break;
    default:
        break;
    }
    free(app);
    free(path);
}

/* The pointer moved over a menu: light the item, and open or close the
   submenu to follow it. */
static void menu_motion(struct files *files, struct menu *menu)
{
    int index = menu_item_at(menu, files->pointer_y);
    if (index != menu->hover) {
        menu->hover = index;
        desk_window_redraw(&menu->window);
    }
    if (menu == &files->menus[0] && index >= 0) {
        if (menu_choosable(menu, index) && menu->items[index].submenu)
            open_submenu(files, index, false);
        else
            close_submenu(files);
    }
}

static void menu_leave(struct files *files, struct menu *menu)
{
    /* The parent keeps its item lit while its submenu is open. */
    if (menu == &files->menus[0] && files->menus[1].open)
        return;
    if (menu->hover >= 0) {
        menu->hover = -1;
        desk_window_redraw(&menu->window);
    }
}

/* Keys while a menu is open. */
static void menu_key(struct files *files, xkb_keysym_t sym)
{
    struct menu *main = &files->menus[0], *sub = &files->menus[1];
    /* Keys go to the submenu once it has an item chosen. */
    struct menu *menu = sub->open && sub->hover >= 0 ? sub : main;
    switch (sym) {
    case XKB_KEY_Up:
    case XKB_KEY_Down: {
        int step = sym == XKB_KEY_Down ? 1 : -1, count = (int)menu->count;
        if (!count)
            return;
        int index = menu->hover;
        for (int tries = 0; tries < count; ++tries) {
            index = index < 0 ? (step > 0 ? 0 : count - 1) : (index + step + count) % count;
            if (menu_choosable(menu, index))
                break;
        }
        if (menu_choosable(menu, index)) {
            menu->hover = index;
            desk_window_redraw(&menu->window);
            if (menu == main && sub->open && sub->parent_item != index)
                close_submenu(files);
        }
        return;
    }
    case XKB_KEY_Right:
        if (menu == main && menu_choosable(main, main->hover) && main->items[main->hover].submenu)
            open_submenu(files, main->hover, true);
        return;
    case XKB_KEY_Left:
        if (menu == sub)
            close_submenu(files);
        return;
    case XKB_KEY_Escape:
        if (menu == sub)
            close_submenu(files);
        else
            close_menus(files);
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        menu_choose(files, menu, menu->hover, true);
        return;
    default:
        return;
    }
}

/* The right-click menu for what's at the pointer in the window. */
static void context_click(struct files *files, struct target target)
{
    int x = (int)lround(files->pointer_x), y = (int)lround(files->pointer_y);
    if (target.kind == TARGET_ROW) {
        files->cursor = target.index;
        files->free_scroll = false;
        desk_window_redraw(&files->window);
        struct entry *entry = cursor_entry(files);
        char *path = entry ? join_path(files->path, entry->name) : NULL;
        if (path)
            open_menu(files, CONTEXT_ENTRY, path, entry->directory, entry->executable && !entry->directory, x, y,
                      false);
        free(path);
    } else if (target.kind == TARGET_PLACE) {
        const struct place *place = &files->places[target.index];
        open_menu(files, CONTEXT_PLACE, place->path, true, false, x, y, false);
    } else if (target.kind == TARGET_NONE && in_list(files) && pointer_buffer_y(files) >= files->panes.entries_top) {
        open_menu(files, CONTEXT_BACKGROUND, files->path, true, false, x, y, false);
    }
}

/* Menu or Shift+F10: the menu for the selected entry, at its row. */
static void context_key(struct files *files)
{
    const struct panes *panes = &files->panes;
    double scale = desk_window_scale(&files->window);
    struct entry *entry = cursor_entry(files);
    size_t per_line = columns(files);
    if (!entry) {
        open_menu(files, CONTEXT_BACKGROUND, files->path, true, false, (int)lround(panes->list_left / scale),
                  (int)lround(panes->entries_top / scale), true);
        return;
    }
    long line = (long)(files->cursor / per_line) - (long)files->top;
    if (line < 0 || line >= panes->rows)
        line = 0;
    int x = panes->list_left + (int)(files->cursor % per_line) * panes->tile_width + panes->line_height;
    int y = panes->entries_top + (int)(line + 1) * panes->line_height;
    char *path = join_path(files->path, entry->name);
    if (path)
        open_menu(files, CONTEXT_ENTRY, path, entry->directory, entry->executable && !entry->directory,
                  (int)lround(x / scale), (int)lround(y / scale), true);
    free(path);
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
    clear_notice(files);
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
    case TARGET_HEADING:
        sort_by(files, (enum column)target.index);
        break;
    case TARGET_COLUMN_EDGE:
        if (twice) {
            files->press.kind = TARGET_NONE;
            files->column_width[target.index] = default_column_width[target.index];
            save_state(files);
            desk_window_redraw(&files->window);
        } else {
            files->resizing = (int)target.index;
        }
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
    struct files *files = data;
    files->pointer_serial = serial;
    files->pointer_surface = surface;
    files->pointer_x = wl_fixed_to_double(x);
    files->pointer_y = wl_fixed_to_double(y);
    struct menu *menu = menu_at_surface(files, surface);
    if (menu) {
        if (files->cursor_shape)
            wp_cursor_shape_device_v1_set_shape(files->cursor_shape, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
        menu_motion(files, menu);
        return;
    }
    update_hover(files, true);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    struct files *files = data;
    struct menu *menu = menu_at_surface(files, surface);
    files->pointer_surface = NULL;
    if (menu) {
        menu_leave(files, menu);
        return;
    }
    if (surface != files->window.surface)
        return; /* A menu that has closed. */
    if (files->drag || files->resizing)
        save_state(files);
    files->drag = false;
    files->resizing = 0;
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
    struct menu *menu = menu_at_surface(files, files->pointer_surface);
    if (menu)
        menu_motion(files, menu);
    else if (files->drag)
        set_sidebar_width(files, files->pointer_x - PADDING * 1.5);
    else if (files->resizing)
        resize_column(files, files->resizing);
    else
        update_hover(files, false);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer;
    struct files *files = data;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (pressed)
        files->input_serial = serial;
    struct menu *menu = menu_at_surface(files, files->pointer_surface);
    if (menu) {
        if (pressed && (button == BTN_LEFT || button == BTN_RIGHT))
            menu_choose(files, menu, menu_item_at(menu, files->pointer_y), false);
        return;
    }
    /* The compositor closes menus for clicks on other clients; a click on
       this window closes them here, and does nothing else. */
    if (files->menus[0].open) {
        if (pressed)
            close_menus(files);
        return;
    }
    if (button == BTN_SIDE || button == BTN_BACK) {
        if (pressed)
            go_back(files);
    } else if (button == BTN_EXTRA || button == BTN_FORWARD) {
        if (pressed)
            go_forward(files);
    } else if (button == BTN_MIDDLE) {
        if (pressed)
            middle_click(files);
    } else if (button == BTN_RIGHT) {
        if (pressed)
            context_click(files, target_at(files));
    } else if (button != BTN_LEFT) {
        /* Other buttons do nothing. */
    } else if (pressed) {
        click(files, time);
    } else if (files->drag || files->resizing) {
        files->drag = false;
        files->resizing = 0;
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
    if (menu_at_surface(files, files->pointer_surface)) {
        files->scroll_pixels = 0; /* Menus don't scroll. */
        return;
    }
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
    if (strcmp(interface, wl_data_device_manager_interface.name) == 0 && !files->data_manager) {
        files->data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface,
                                               version < 3 ? version : 3);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
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
    if (files->data_manager && files->seat) {
        files->data_device = wl_data_device_manager_get_data_device(files->data_manager, files->seat);
        wl_data_device_add_listener(files->data_device, &device_listener, files);
    }
    if (!desk_window_open(&files->window, &files->globals, "files", &window_listener, files)) {
        fprintf(stderr, "files: out of memory\n");
        return false;
    }
    update_title(files);
    return true;
}

static void close_window(struct files *files)
{
    close_menus(files);
    desk_window_close(&files->window);
    if (files->clip_source)
        wl_data_source_destroy(files->clip_source);
    free(files->clip_text);
    if (files->selection_offer)
        wl_data_offer_destroy(files->selection_offer);
    if (files->data_device)
        wl_data_device_release(files->data_device);
    if (files->data_manager)
        wl_data_device_manager_destroy(files->data_manager);
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
        struct pollfd fds[7] = {
            {.fd = wl_display_get_fd(display), .events = POLLIN},
            {.fd = files->repeat.fd, .events = POLLIN},
            {.fd = files->inotify, .events = POLLIN},
            {.fd = files->refresh, .events = POLLIN},
            {.fd = files->palette_watch.fd, .events = POLLIN},
            {.fd = files->mounts, .events = POLLPRI},
            {.fd = files->sizer ? files->sizer->event : -1, .events = POLLIN},
        };
        if (poll(fds, 7, -1) < 0) {
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
        if (files->sizer && fds[6].revents & POLLIN)
            sizes_arrived(files);
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
          "forward, Alt+Home home, typing a name selects it, Ctrl+T/W new and close\n"
          "tab, Ctrl+Tab next tab, Ctrl+F search, Ctrl+H hidden files, F5 refresh,\n"
          "F9 sidebar, Ctrl+1/2 list or icons, Ctrl+plus/minus/0 zoom, Ctrl+Q quits.\n"
          "Double-click opens; middle-click opens a folder in a new tab.\n",
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
    files.sizer = sizer_create(); /* Without it, directories just show no size. */
    files.terminal = terminal_command();
    files.bookmarks_path = bookmarks_file();
    signal(SIGCHLD, SIG_IGN); /* Applications started are left to themselves. */
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
    sizer_destroy(files.sizer);
    free(files.sized_path);
    free(files.terminal);
    free(files.bookmarks_path);
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
