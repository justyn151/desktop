#define _GNU_SOURCE
#include "files.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <png.h>
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
#include "viewporter-client-protocol.h"

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

/* The selection */

static struct entry *shown_entry(struct files *files, size_t index)
{
    return &files->listing.entries[files->listing.shown[index]];
}

static size_t selected_count(struct files *files)
{
    size_t count = 0;
    for (size_t i = 0; i < files->listing.shown_count; ++i)
        count += shown_entry(files, i)->selected;
    return count;
}

static void clear_selection(struct files *files)
{
    for (size_t i = 0; i < files->listing.count; ++i)
        files->listing.entries[i].selected = false;
}

/* The cursor on index, and only it selected. */
static void select_only(struct files *files, size_t index)
{
    clear_selection(files);
    if (index < files->listing.shown_count) {
        files->cursor = index;
        shown_entry(files, index)->selected = true;
    }
    files->anchor = files->cursor;
}

/* Select from the anchor to index, adding to what's selected or not. */
static void select_range(struct files *files, size_t index, bool add)
{
    if (!add)
        clear_selection(files);
    size_t from = files->anchor < index ? files->anchor : index, to = files->anchor < index ? index : files->anchor;
    for (size_t i = from; i <= to && i < files->listing.shown_count; ++i)
        shown_entry(files, i)->selected = true;
    files->cursor = index;
}

static int compare_strings(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The names selected in old, sorted, for carrying the selection over a
   reread; NULL when none are. */
static char **selected_names(const struct listing *old, size_t *count)
{
    *count = 0;
    for (size_t i = 0; i < old->count; ++i)
        *count += old->entries[i].selected;
    if (!*count)
        return NULL;
    char **names = malloc(*count * sizeof *names);
    if (!names) {
        *count = 0;
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; i < old->count; ++i) {
        if (old->entries[i].selected)
            names[used++] = old->entries[i].name;
    }
    qsort(names, used, sizeof *names, compare_strings);
    return names;
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
    /* Your own trash, wherever $XDG_DATA_HOME puts it. */
    size_t trash = files->trash_real ? strlen(files->trash_real) : 0;
    if (trash && strncmp(real, files->trash_real, trash) == 0 && (real[trash] == '/' || real[trash] == 0))
        inside = true;
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
        /* What GLib leaves as is, so the URI (and a thumbnail's name, its
           MD5) is the same as GTK's. */
        if (isalnum(*at) || strchr("!$&'()*+,-./:=@_~", *at))
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
    if (files->trash_files && is_directory(files->trash_files))
        add_place(&places, &count, "Trash", files->trash_files, 0xf1f8);
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

static bool has_suffix(const char *name, const char *const *suffixes);

/* The freedesktop thumbnail cache: $XDG_CACHE_HOME/thumbnails. */
static char *thumbnails_directory(const char *home)
{
    const char *cache = getenv("XDG_CACHE_HOME");
    char *path = NULL;
    if (cache && cache[0] == '/') {
        if (asprintf(&path, "%s/thumbnails", cache) < 0)
            path = NULL;
    } else if (home && asprintf(&path, "%s/.cache/thumbnails", home) < 0) {
        path = NULL;
    }
    return path;
}

/* Thumbnails

   Images in the icon view show a thumbnail, made on a worker thread. They
   follow the freedesktop thumbnail spec: a PNG named by the MD5 of the
   file's URI in ~/.cache/thumbnails/normal (128 pixels), large (256),
   x-large (512) or xx-large (1024), holding Thumb::URI and Thumb::MTime, so
   other applications make and use the same ones. */

#define THUMB_MAX_BYTES (256u << 20) /* The most an image may decode to. */

static const struct {
    const char *name;
    int size;
} thumb_classes[] = {{"normal", 128}, {"large", 256}, {"x-large", 512}, {"xx-large", 1024}};

/* Whether files makes a thumbnail of this entry: an image it can decode. */
static bool thumbnailable(const struct entry *entry)
{
    static const char *const types[] = {"png", "jpg", "jpeg", "jpe", "gif", "webp", NULL};
    return !entry->directory && !entry->broken && S_ISREG(entry->target_mode) && entry->size > 0 &&
           has_suffix(entry->name, types);
}

/* MD5 (RFC 1321), for the cache's file names. */
static void md5_hex(const char *text, char out[33])
{
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const unsigned char shifts[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    size_t length = strlen(text), total = (length + 8) / 64 * 64 + 64;
    unsigned char *message = calloc(total, 1);
    if (!message) {
        out[0] = 0;
        return;
    }
    memcpy(message, text, length);
    message[length] = 0x80;
    uint64_t bits = (uint64_t)length * 8;
    for (int i = 0; i < 8; ++i)
        message[total - 8 + i] = (unsigned char)(bits >> (8 * i));
    for (size_t chunk = 0; chunk < total; chunk += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) {
            const unsigned char *b = message + chunk + 4 * i;
            w[i] = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            uint32_t sum = a + f + k[i] + w[g];
            int s = shifts[(i / 16) * 4 + i % 4];
            a = d;
            d = c;
            c = b;
            b += (sum << s) | (sum >> (32 - s));
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    free(message);
    for (int i = 0; i < 16; ++i)
        sprintf(out + 2 * i, "%02x", (h[i / 4] >> (8 * (i % 4))) & 0xff);
}

/* An RGBA8 image, premultiplied. */
struct rgba {
    int width, height;
    unsigned char *pixels;
};

/* Shrink to fit a box of size, never enlarging, averaging the source
   pixels each covers. */
static bool rgba_fit(const struct rgba *source, int size, struct rgba *out)
{
    double scale = fmin(1, fmin((double)size / source->width, (double)size / source->height));
    int width = (int)fmax(1, lround(source->width * scale)), height = (int)fmax(1, lround(source->height * scale));
    out->width = width;
    out->height = height;
    out->pixels = malloc((size_t)width * (size_t)height * 4);
    if (!out->pixels)
        return false;
    for (int y = 0; y < height; ++y) {
        int y0 = (int)((long long)y * source->height / height), y1 = (int)((long long)(y + 1) * source->height / height);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < width; ++x) {
            int x0 = (int)((long long)x * source->width / width), x1 = (int)((long long)(x + 1) * source->width / width);
            if (x1 <= x0)
                x1 = x0 + 1;
            unsigned long sum[4] = {0};
            for (int sy = y0; sy < y1; ++sy) {
                const unsigned char *p = source->pixels + ((size_t)sy * (size_t)source->width + (size_t)x0) * 4;
                for (int sx = x0; sx < x1; ++sx, p += 4) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                }
            }
            unsigned long count = (unsigned long)(y1 - y0) * (unsigned long)(x1 - x0);
            unsigned char *o = out->pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
            for (int c = 0; c < 4; ++c)
                o[c] = (unsigned char)((sum[c] + count / 2) / count);
        }
    }
    return true;
}

/* Turn an image stored as EXIF orientation (2–8) says into upright. */
static bool rgba_orient(struct rgba *image, int orientation)
{
    if (orientation < 2 || orientation > 8)
        return true;
    int w = image->width, h = image->height;
    bool swap = orientation >= 5;
    int ow = swap ? h : w, oh = swap ? w : h;
    unsigned char *pixels = malloc((size_t)ow * (size_t)oh * 4);
    if (!pixels)
        return false;
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            int sx, sy;
            switch (orientation) {
            case 2: sx = w - 1 - x; sy = y; break;
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;
            case 4: sx = x; sy = h - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = h - 1 - x; break;
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;
            default: sx = w - 1 - y; sy = x; break;
            }
            memcpy(pixels + ((size_t)y * (size_t)ow + (size_t)x) * 4,
                   image->pixels + ((size_t)sy * (size_t)w + (size_t)sx) * 4, 4);
        }
    }
    free(image->pixels);
    image->pixels = pixels;
    image->width = ow;
    image->height = oh;
    return true;
}

/* A cached thumbnail, if it's there and still matches the file (its URI and
   modification time). */
static bool read_cached(const char *cache, const char *uri, time_t mtime, struct rgba *out)
{
    FILE *file = fopen(cache, "rbe");
    if (!file)
        return false;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    png_bytep *rows = NULL;
    out->pixels = NULL;
    bool ok = false;
    if (!info || setjmp(png_jmpbuf(png)))
        goto done;
    png_init_io(png, file);
    png_read_info(png, info);
    /* The keys come before the image in practice; check them before
       decoding it. */
    png_textp text;
    int count = 0;
    png_get_text(png, info, &text, &count);
    bool uri_ok = false, mtime_ok = false;
    for (int i = 0; i < count; ++i) {
        if (strcmp(text[i].key, "Thumb::URI") == 0)
            uri_ok = strcmp(text[i].text, uri) == 0;
        else if (strcmp(text[i].key, "Thumb::MTime") == 0)
            mtime_ok = strtoll(text[i].text, NULL, 10) == (long long)mtime;
    }
    if (!uri_ok || !mtime_ok)
        goto done;
    png_uint_32 width = png_get_image_width(png, info), height = png_get_image_height(png, info);
    if (!width || !height || width > 4096 || height > 4096)
        goto done;
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    out->width = (int)width;
    out->height = (int)height;
    out->pixels = malloc((size_t)width * height * 4);
    rows = malloc(height * sizeof *rows);
    if (!out->pixels || !rows)
        goto done;
    for (png_uint_32 y = 0; y < height; ++y)
        rows[y] = out->pixels + (size_t)y * width * 4;
    png_read_image(png, rows);
    /* Premultiply. */
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        unsigned char *p = out->pixels + i * 4;
        for (int c = 0; c < 3; ++c)
            p[c] = (unsigned char)((p[c] * p[3] + 127) / 255);
    }
    ok = true;
done:
    if (!ok) {
        free(out->pixels);
        out->pixels = NULL;
    }
    free(rows);
    png_destroy_read_struct(png ? &png : NULL, info ? &info : NULL, NULL);
    fclose(file);
    return ok;
}

/* Save a thumbnail to the cache: written to a temporary file beside it,
   then renamed, so no one reads half of one. */
static void write_cached(const char *cache, const char *uri, time_t mtime, off_t size, const struct rgba *image,
                         int source_width, int source_height)
{
    char *temporary = NULL;
    if (asprintf(&temporary, "%s.files-%ld", cache, (long)getpid()) < 0)
        return;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *file = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!file) {
        if (fd >= 0)
            close(fd);
        free(temporary);
        return;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    unsigned char *straight = malloc((size_t)image->width * (size_t)image->height * 4);
    png_bytep *rows = malloc((size_t)image->height * sizeof *rows);
    bool ok = false;
    if (!info || !straight || !rows || setjmp(png_jmpbuf(png)))
        goto done;
    /* PNG holds straight alpha. */
    for (size_t i = 0; i < (size_t)image->width * (size_t)image->height; ++i) {
        const unsigned char *p = image->pixels + i * 4;
        unsigned char *o = straight + i * 4;
        for (int c = 0; c < 3; ++c)
            o[c] = p[3] ? (unsigned char)((p[c] * 255 + p[3] / 2) / p[3]) : 0;
        o[3] = p[3];
    }
    for (int y = 0; y < image->height; ++y)
        rows[y] = straight + (size_t)y * (size_t)image->width * 4;
    char mtime_text[32], size_text[32], width_text[16], height_text[16];
    snprintf(mtime_text, sizeof mtime_text, "%lld", (long long)mtime);
    snprintf(size_text, sizeof size_text, "%lld", (long long)size);
    snprintf(width_text, sizeof width_text, "%d", source_width);
    snprintf(height_text, sizeof height_text, "%d", source_height);
    png_text text[] = {
        {.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Thumb::URI", .text = (char *)uri},
        {.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Thumb::MTime", .text = mtime_text},
        {.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Thumb::Size", .text = size_text},
        {.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Thumb::Image::Width", .text = width_text},
        {.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Thumb::Image::Height", .text = height_text},
        {.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Software", .text = "files " FILES_VERSION},
    };
    png_init_io(png, file);
    png_set_IHDR(png, info, (png_uint_32)image->width, (png_uint_32)image->height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_text(png, info, text, (int)(sizeof text / sizeof *text));
    png_write_info(png, info);
    png_write_image(png, rows);
    png_write_end(png, NULL);
    ok = true;
done:
    png_destroy_write_struct(png ? &png : NULL, info ? &info : NULL);
    free(rows);
    free(straight);
    if (fclose(file) != 0)
        ok = false;
    if (!ok || rename(temporary, cache) != 0)
        unlink(temporary);
    free(temporary);
}

/* mkdir -p, private: the cache shows what you've looked at. */
static void make_private_directories(char *path)
{
    for (char *at = path + 1; *at; ++at) {
        if (*at == '/') {
            *at = 0;
            mkdir(path, 0700);
            *at = '/';
        }
    }
    mkdir(path, 0700);
}

/* Make a thumbnail of path for a box of size: from the cache when it's
   there and current, else from the image, saving it to the cache. */
static bool make_thumbnail(const char *thumbs_dir, const char *path, time_t mtime, off_t file_size, int size,
                           struct rgba *out)
{
    size_t class = 0;
    while (class + 1 < sizeof thumb_classes / sizeof *thumb_classes && thumb_classes[class].size < size)
        ++class;
    char *uri = path_uri(path), *cache = NULL;
    char hash[33];
    if (!uri)
        return false;
    md5_hex(uri, hash);
    /* Never thumbnails of thumbnails, as the spec asks. */
    size_t dir_length = thumbs_dir ? strlen(thumbs_dir) : 0;
    bool cacheable = thumbs_dir && hash[0] && !(strncmp(path, thumbs_dir, dir_length) == 0 && path[dir_length] == '/');
    if (cacheable && asprintf(&cache, "%s/%s/%s.png", thumbs_dir, thumb_classes[class].name, hash) < 0)
        cache = NULL;

    struct rgba stored = {0};
    bool ok = cache && read_cached(cache, uri, mtime, &stored);
    if (!ok) {
        FILE *file = fopen(path, "rbe");
        DeskImage image;
        char error[128];
        if (file && desk_image_load(file, &image, THUMB_MAX_BYTES, DESK_IMAGE_SHRINK, error, sizeof error)) {
            struct rgba full = {(int)image.width, (int)image.height, image.pixels};
            ok = rgba_fit(&full, thumb_classes[class].size, &stored) && rgba_orient(&stored, image.orientation);
            bool turned = image.orientation >= 5 && image.orientation <= 8;
            free(image.pixels);
            if (ok && cache) {
                char *directory = strdup(cache);
                char *slash = directory ? strrchr(directory, '/') : NULL;
                if (slash) {
                    *slash = 0;
                    make_private_directories(directory);
                }
                free(directory);
                write_cached(cache, uri, mtime, file_size, &stored, turned ? (int)image.height : (int)image.width,
                             turned ? (int)image.width : (int)image.height);
            }
        }
        if (file)
            fclose(file);
    }
    free(uri);
    free(cache);
    if (!ok) {
        free(stored.pixels);
        return false;
    }
    ok = rgba_fit(&stored, size, out);
    free(stored.pixels);
    return ok;
}

struct thumb_job {
    unsigned long generation;
    char *path;
    time_t mtime;
    off_t file_size;
    int size;
    /* The result. */
    bool ok;
    int width, height;
    uint32_t *pixels;
};

struct thumbnailer {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int event;                     /* eventfd: results are waiting. */
    unsigned long generation;      /* Jobs from before a change of directory are dropped. */
    bool quit;
    char *thumbs_dir;
    struct thumb_job *jobs;        /* A stack: the latest asked for, on screen, first. */
    size_t job_count, job_capacity;
    struct thumb_job *results;
    size_t result_count, result_capacity;
};

static bool push_job(struct thumb_job **list, size_t *count, size_t *capacity, struct thumb_job job)
{
    if (*count == *capacity) {
        size_t grown = *capacity ? *capacity * 2 : 64;
        struct thumb_job *bigger = realloc(*list, grown * sizeof *bigger);
        if (!bigger)
            return false;
        *list = bigger;
        *capacity = grown;
    }
    (*list)[(*count)++] = job;
    return true;
}

static void *thumbnailer_main(void *data)
{
    struct thumbnailer *thumbnailer = data;
    pthread_mutex_lock(&thumbnailer->lock);
    while (!thumbnailer->quit) {
        if (!thumbnailer->job_count) {
            pthread_cond_wait(&thumbnailer->wake, &thumbnailer->lock);
            continue;
        }
        struct thumb_job job = thumbnailer->jobs[--thumbnailer->job_count];
        pthread_mutex_unlock(&thumbnailer->lock);
        struct rgba image = {0};
        job.ok = make_thumbnail(thumbnailer->thumbs_dir, job.path, job.mtime, job.file_size, job.size, &image);
        if (job.ok) {
            /* To the canvas's premultiplied ARGB. */
            size_t count = (size_t)image.width * (size_t)image.height;
            job.pixels = malloc(count * sizeof *job.pixels);
            for (size_t i = 0; job.pixels && i < count; ++i) {
                const unsigned char *p = image.pixels + i * 4;
                job.pixels[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            }
            job.ok = job.pixels != NULL;
            job.width = image.width;
            job.height = image.height;
        }
        free(image.pixels);
        pthread_mutex_lock(&thumbnailer->lock);
        if (job.generation != thumbnailer->generation ||
            !push_job(&thumbnailer->results, &thumbnailer->result_count, &thumbnailer->result_capacity, job)) {
            free(job.path);
            free(job.pixels);
            continue;
        }
        uint64_t one = 1;
        (void)!write(thumbnailer->event, &one, sizeof one);
    }
    pthread_mutex_unlock(&thumbnailer->lock);
    return NULL;
}

static struct thumbnailer *thumbnailer_create(const char *thumbs_dir)
{
    struct thumbnailer *thumbnailer = calloc(1, sizeof *thumbnailer);
    if (!thumbnailer)
        return NULL;
    thumbnailer->thumbs_dir = thumbs_dir ? strdup(thumbs_dir) : NULL;
    thumbnailer->event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    pthread_mutex_init(&thumbnailer->lock, NULL);
    pthread_cond_init(&thumbnailer->wake, NULL);
    if (thumbnailer->event < 0 || pthread_create(&thumbnailer->thread, NULL, thumbnailer_main, thumbnailer) != 0) {
        if (thumbnailer->event >= 0)
            close(thumbnailer->event);
        pthread_mutex_destroy(&thumbnailer->lock);
        pthread_cond_destroy(&thumbnailer->wake);
        free(thumbnailer->thumbs_dir);
        free(thumbnailer);
        return NULL;
    }
    return thumbnailer;
}

static void free_jobs(struct thumb_job *jobs, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        free(jobs[i].path);
        free(jobs[i].pixels);
    }
}

static void thumbnailer_destroy(struct thumbnailer *thumbnailer)
{
    if (!thumbnailer)
        return;
    pthread_mutex_lock(&thumbnailer->lock);
    thumbnailer->quit = true;
    pthread_cond_signal(&thumbnailer->wake);
    pthread_mutex_unlock(&thumbnailer->lock);
    pthread_join(thumbnailer->thread, NULL);
    free_jobs(thumbnailer->jobs, thumbnailer->job_count);
    free_jobs(thumbnailer->results, thumbnailer->result_count);
    free(thumbnailer->jobs);
    free(thumbnailer->results);
    close(thumbnailer->event);
    pthread_mutex_destroy(&thumbnailer->lock);
    pthread_cond_destroy(&thumbnailer->wake);
    free(thumbnailer->thumbs_dir);
    free(thumbnailer);
}

static size_t thumb_bucket(const char *path)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *at = (const unsigned char *)path; *at; ++at)
        hash = (hash ^ *at) * 16777619u;
    return hash % THUMB_BUCKETS;
}

/* Forget every thumbnail, and every job not yet started. */
static void thumbs_clear(struct files *files)
{
    for (size_t i = 0; i < THUMB_BUCKETS; ++i) {
        for (struct thumb *thumb = files->thumbs[i], *next; thumb; thumb = next) {
            next = thumb->next;
            free(thumb->path);
            free(thumb->pixels);
            free(thumb);
        }
        files->thumbs[i] = NULL;
    }
    files->thumb_count = 0;
    struct thumbnailer *thumbnailer = files->thumbnailer;
    if (thumbnailer) {
        pthread_mutex_lock(&thumbnailer->lock);
        ++thumbnailer->generation;
        free_jobs(thumbnailer->jobs, thumbnailer->job_count);
        thumbnailer->job_count = 0;
        pthread_mutex_unlock(&thumbnailer->lock);
    }
}

/* The thumbnail of path for a box of size, asked for when there's none or
   it's out of date. NULL when it can't be had at all. */
static struct thumb *thumb_for(struct files *files, const char *path, const struct entry *entry, int size)
{
    if (!files->thumbnailer)
        return NULL;
    size_t bucket = thumb_bucket(path);
    struct thumb *thumb = files->thumbs[bucket];
    while (thumb && strcmp(thumb->path, path) != 0)
        thumb = thumb->next;
    if (thumb && thumb->mtime == entry->modified.tv_sec && thumb->size == size)
        return thumb;
    if (!thumb) {
        if (files->thumb_count >= THUMBS_KEPT)
            thumbs_clear(files);
        thumb = calloc(1, sizeof *thumb);
        if (!thumb || !(thumb->path = strdup(path))) {
            free(thumb);
            return NULL;
        }
        bucket = thumb_bucket(path);
        thumb->next = files->thumbs[bucket];
        files->thumbs[bucket] = thumb;
        ++files->thumb_count;
    }
    /* Keep showing the old one, if any, until the new one comes. */
    thumb->mtime = entry->modified.tv_sec;
    thumb->size = size;
    if (thumb->state == THUMB_FAILED)
        thumb->state = THUMB_PENDING;
    struct thumbnailer *thumbnailer = files->thumbnailer;
    struct thumb_job job = {.path = strdup(path), .mtime = thumb->mtime, .file_size = entry->size, .size = size};
    pthread_mutex_lock(&thumbnailer->lock);
    job.generation = thumbnailer->generation;
    if (job.path && push_job(&thumbnailer->jobs, &thumbnailer->job_count, &thumbnailer->job_capacity, job))
        pthread_cond_signal(&thumbnailer->wake);
    else
        free(job.path);
    pthread_mutex_unlock(&thumbnailer->lock);
    return thumb;
}

/* Take the thumbnails the thread has made. */
static void thumbs_arrived(struct files *files)
{
    struct thumbnailer *thumbnailer = files->thumbnailer;
    uint64_t count;
    (void)!read(thumbnailer->event, &count, sizeof count);
    pthread_mutex_lock(&thumbnailer->lock);
    struct thumb_job *results = thumbnailer->results;
    size_t result_count = thumbnailer->result_count;
    thumbnailer->results = NULL;
    thumbnailer->result_count = thumbnailer->result_capacity = 0;
    pthread_mutex_unlock(&thumbnailer->lock);
    for (size_t i = 0; i < result_count; ++i) {
        struct thumb_job *job = &results[i];
        struct thumb *thumb = files->thumbs[thumb_bucket(job->path)];
        while (thumb && strcmp(thumb->path, job->path) != 0)
            thumb = thumb->next;
        if (thumb && thumb->mtime == job->mtime && thumb->size == job->size) {
            free(thumb->pixels);
            thumb->pixels = job->pixels;
            thumb->width = job->width;
            thumb->height = job->height;
            thumb->state = job->ok ? THUMB_READY : THUMB_FAILED;
            job->pixels = NULL;
        }
        free(job->path);
        free(job->pixels);
    }
    free(results);
    desk_window_redraw(&files->window);
}

/* Navigation */

static const char *field_text(const struct field *field);
static void stop_path_edit(struct files *files);
static void stop_rename(struct files *files);
static int pointer_buffer_x(const struct files *files);

static void update_title(struct files *files)
{
    char title[PATH_MAX + 2];
    display_path(files, files->path, title, sizeof title);
    desk_window_set_title(&files->window, title);
}

static void filter(struct files *files)
{
    listing_filter(&files->listing, files->show_hidden, field_text(&files->search));
}

/* Replace the listing with path's, keeping the cursor on keep (a name) when
   it's still there, else near the old position. */
static void load(struct files *files, const char *keep, size_t near, bool same)
{
    struct listing old = files->listing;
    listing_read(&files->listing, files->path);
    /* Rereading: what was selected stays selected. */
    size_t selected = 0;
    char **names = same ? selected_names(&old, &selected) : NULL;
    for (size_t i = 0; names && i < files->listing.count; ++i) {
        const char *name = files->listing.entries[i].name;
        files->listing.entries[i].selected = bsearch(&name, names, selected, sizeof *names, compare_strings) != NULL;
    }
    free(names);
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
    files->anchor = files->cursor;
    files->band_pending = files->banding = false;
    if (!same)
        select_only(files, files->cursor);
}

/* Show directory path (taken over), selecting select or what was selected
   there last time, without touching the history. A search ends there. */
static void enter(struct files *files, char *path, const char *select)
{
    remember(files);
    if (files->renaming)
        stop_rename(files);
    if (!files->path || strcmp(files->path, path) != 0)
        thumbs_clear(files);
    free(files->path);
    files->path = path;
    files->top = 0;
    files->free_scroll = false;
    files->press.kind = TARGET_NONE; /* A click there and one here aren't a double-click. */
    desk_text_set(&files->search.text, "");
    files->search_focus = false;
    if (files->path_editing)
        stop_path_edit(files);
    files->typed[0] = 0;
    files->notice[0] = 0;
    struct memory *memory = find_memory(files, path);
    load(files, select ? select : memory ? memory->name : NULL, 0, false);
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
    load(files, keep, files->cursor, true);
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

/* Move the cursor: alone it selects just that entry, with Shift it
   selects from the anchor, with Ctrl it leaves the selection be. */
static void move_cursor(struct files *files, long delta, unsigned mods)
{
    size_t count = files->listing.shown_count;
    if (!count)
        return;
    long target = (long)files->cursor + delta;
    if (target < 0)
        target = 0;
    if (target >= (long)count)
        target = (long)count - 1;
    if (mods & DESK_MOD_SHIFT)
        select_range(files, (size_t)target, mods & DESK_MOD_CTRL);
    else if (mods & DESK_MOD_CTRL)
        files->cursor = (size_t)target;
    else
        select_only(files, (size_t)target);
    files->free_scroll = false;
    desk_window_redraw(&files->window);
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
    /* What's no longer shown leaves the selection. */
    for (size_t i = 0; i < listing->count; ++i)
        listing->entries[i].was_selected = listing->entries[i].selected;
    clear_selection(files);
    for (size_t i = 0; i < listing->shown_count; ++i)
        shown_entry(files, i)->selected = shown_entry(files, i)->was_selected;
    if (!selected_count(files))
        select_only(files, files->cursor);
    files->anchor = files->cursor;
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
    files->anchor = files->cursor;
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
    tab->search = field_text(&files->search)[0] ? strdup(field_text(&files->search)) : NULL;
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
        desk_text_set(&files->search.text, tab->search);
        refilter(files);
        size_t index;
        if (find_shown(&files->listing, tab->selected, &index))
            select_only(files, index);
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

static const char *field_text(const struct field *field)
{
    return field->text.text ? field->text.text : "";
}

static void search_changed(struct files *files)
{
    refilter(files);
    files->top = 0;
}

static void search_clear(struct files *files)
{
    if (field_text(&files->search)[0]) {
        desk_text_set(&files->search.text, "");
        search_changed(files);
    }
}

static void copy_text(struct files *files, const char *text);
static char *paste_text(struct files *files);

/* Keys a focused field handles itself: editing, and the clipboard. Return
   -1 when the key isn't the field's, else whether it repeats; *changed
   says whether the text changed. */
static int field_key(struct files *files, struct field *field, uint32_t key, xkb_keysym_t sym, unsigned mods,
                     bool *changed)
{
    DeskText *text = &field->text;
    *changed = false;
    if (!text->text && !desk_text_set(text, ""))
        return -1;
    size_t start, end;
    bool selected = desk_text_selection(text, &start, &end);
    if ((mods & DESK_MOD_CTRL) && !(mods & DESK_MOD_ALT)) {
        if ((sym == XKB_KEY_c || sym == XKB_KEY_x) && selected) {
            char *copy = strndup(text->text + start, end - start);
            if (copy)
                copy_text(files, copy);
            free(copy);
            if (sym == XKB_KEY_x)
                *changed = desk_text_delete(text, 1, false);
            desk_window_redraw(&files->window);
            return 0;
        }
        if (sym == XKB_KEY_v) {
            char *pasted = paste_text(files);
            if (pasted) {
                /* One line: line breaks become spaces, a trailing one goes. */
                size_t length = strlen(pasted);
                while (length && (pasted[length - 1] == '\n' || pasted[length - 1] == '\r'))
                    pasted[--length] = 0;
                for (char *at = pasted; *at; ++at) {
                    if (*at == '\n' || *at == '\r' || *at == '\t')
                        *at = ' ';
                }
                *changed = desk_text_insert(text, pasted, length);
                free(pasted);
            }
            desk_window_redraw(&files->window);
            return 0;
        }
    }
    char typed[16] = "";
    if (files->keyboard.state) {
        int length = xkb_state_key_get_utf8(files->keyboard.state, key + 8, typed, sizeof typed);
        if (length <= 0)
            typed[0] = 0;
    }
    int result = desk_text_key(text, sym, mods, typed);
    if (result == DESK_TEXT_IGNORED)
        return -1;
    *changed = result == DESK_TEXT_CHANGED;
    desk_window_redraw(&files->window);
    return sym != XKB_KEY_Home && sym != XKB_KEY_End;
}

/* The byte offset in the field nearest buffer x. */
static size_t field_offset_at(struct files *files, const struct field *field, int x)
{
    const char *text = field_text(field);
    size_t length = strlen(text), at = 0;
    x -= field->text_x;
    while (at < length) {
        size_t next = at + 1;
        while (next < length && ((unsigned char)text[next] & 0xc0) == 0x80)
            ++next;
        int left = desk_text_width(files->font, text, at), right = desk_text_width(files->font, text, next);
        if (x < (left + right) / 2)
            break;
        at = next;
    }
    return at;
}

/* A press in the field: the cursor goes there (extending the selection with
   Shift), a drag selects, a double-click selects it all. */
static void field_press(struct files *files, struct field *field, bool twice)
{
    if (!field->text.text && !desk_text_set(&field->text, ""))
        return;
    if (twice) {
        desk_text_select_all(&field->text);
    } else {
        bool extend = desk_keyboard_mods(&files->keyboard) & DESK_MOD_SHIFT;
        desk_text_move_to(&field->text, field_offset_at(files, field, pointer_buffer_x(files)), extend);
        files->selecting = field;
    }
    desk_window_redraw(&files->window);
}

static void field_drag(struct files *files)
{
    struct field *field = files->selecting;
    desk_text_move_to(&field->text, field_offset_at(files, field, pointer_buffer_x(files)), true);
    desk_window_redraw(&files->window);
}

/* A key while the search field has the focus. Returns whether it repeats,
   or -1 when it isn't the field's, so the window handles it. */
static int search_key(struct files *files, uint32_t key, xkb_keysym_t sym, unsigned mods)
{
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
    default:
        break;
    }
    bool changed;
    int repeats = field_key(files, &files->search, key, sym, mods, &changed);
    if (changed)
        search_changed(files);
    return repeats;
}

/* Typing a path */

static void free_suggestions(struct files *files)
{
    for (size_t i = 0; i < files->suggestion_count; ++i)
        free(files->suggestions[i]);
    free(files->suggestions);
    files->suggestions = NULL;
    files->suggestion_count = 0;
    files->suggestion = -1;
}

/* What's typed as an absolute path: ~ expanded, a relative path taken from
   the current directory. */
static char *typed_path(const struct files *files, const char *typed)
{
    char *path = NULL;
    if (typed[0] == '~' && (typed[1] == '/' || !typed[1]) && files->home) {
        if (asprintf(&path, "%s%s", files->home, typed + 1) < 0)
            return NULL;
    } else if (typed[0] == '/') {
        path = strdup(typed);
    } else if (asprintf(&path, "%s/%s", strcmp(files->path, "/") == 0 ? "" : files->path, typed) < 0) {
        return NULL;
    }
    return path;
}

/* Clean up a path by its text alone: no //, ., .. or trailing /. Links
   stay as named, as everywhere else in files. */
static char *normalize_path(const char *path)
{
    size_t length = strlen(path);
    char *out = malloc(length + 2);
    if (!out)
        return NULL;
    size_t used = 0;
    for (const char *at = path; *at;) {
        while (*at == '/')
            ++at;
        size_t part = strcspn(at, "/");
        if (!part)
            break;
        if (part == 1 && at[0] == '.') {
            /* Nothing. */
        } else if (part == 2 && at[0] == '.' && at[1] == '.') {
            while (used > 0 && out[used - 1] != '/')
                --used;
            if (used > 0)
                --used;
        } else {
            out[used++] = '/';
            memcpy(out + used, at, part);
            used += part;
        }
        at += part;
    }
    if (!used)
        out[used++] = '/';
    out[used] = 0;
    return out;
}

static int compare_names(const void *a, const void *b)
{
    return natural_compare(*(char *const *)a, *(char *const *)b);
}

/* The directories in what's typed so far whose names start with its last
   part, ignoring case; dot directories only when that starts with a dot. */
static void update_suggestions(struct files *files)
{
    free_suggestions(files);
    const char *typed = field_text(&files->path_field);
    const char *slash = strrchr(typed, '/');
    char *directory_text = slash ? strndup(typed, (size_t)(slash - typed) + 1) : strdup("");
    const char *prefix = slash ? slash + 1 : typed;
    if (!directory_text)
        return;
    if (!slash && typed[0] == '~') {
        free(directory_text);
        return; /* "~" alone: completes once it's "~/". */
    }
    char *directory = typed_path(files, directory_text[0] ? directory_text : ".");
    free(directory_text);
    DIR *dir = directory ? opendir(directory) : NULL;
    size_t capacity = 0, prefix_length = strlen(prefix);
    struct dirent *item;
    while (dir && (item = readdir(dir))) {
        const char *name = item->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || (name[0] == '.' && prefix[0] != '.') ||
            strncasecmp(name, prefix, prefix_length) != 0)
            continue;
        bool is_dir = item->d_type == DT_DIR;
        if (item->d_type == DT_LNK || item->d_type == DT_UNKNOWN) {
            struct stat info;
            is_dir = fstatat(dirfd(dir), name, &info, 0) == 0 && S_ISDIR(info.st_mode);
        }
        if (!is_dir)
            continue;
        if (files->suggestion_count == capacity) {
            size_t grown = capacity ? capacity * 2 : 16;
            char **list = realloc(files->suggestions, grown * sizeof *list);
            if (!list)
                break;
            files->suggestions = list;
            capacity = grown;
        }
        if (!(files->suggestions[files->suggestion_count] = strdup(name)))
            break;
        ++files->suggestion_count;
    }
    if (dir)
        closedir(dir);
    free(directory);
    if (files->suggestion_count)
        qsort(files->suggestions, files->suggestion_count, sizeof *files->suggestions, compare_names);
    /* Exactly what's typed already: nothing to suggest. */
    if (files->suggestion_count == 1 && strcmp(files->suggestions[0], prefix) == 0)
        free_suggestions(files);
}

static void start_path_edit(struct files *files)
{
    search_focus(files, false);
    if (!desk_text_set(&files->path_field.text, files->path))
        return;
    desk_text_select_all(&files->path_field.text);
    files->path_field.scroll = 0;
    files->path_editing = true;
    update_suggestions(files);
    free_suggestions(files); /* Not until something's typed. */
    desk_window_redraw(&files->window);
}

static void stop_path_edit(struct files *files)
{
    if (!files->path_editing)
        return;
    files->path_editing = false;
    if (files->selecting == &files->path_field)
        files->selecting = NULL;
    free_suggestions(files);
    desk_window_redraw(&files->window);
}

/* Replace the last part of what's typed with a suggestion and a /. */
static void use_suggestion(struct files *files, const char *name)
{
    const char *typed = field_text(&files->path_field);
    const char *slash = strrchr(typed, '/');
    char *completed = NULL;
    if (asprintf(&completed, "%.*s%s/", slash ? (int)(slash - typed) + 1 : 0, typed, name) < 0)
        return;
    desk_text_set(&files->path_field.text, completed);
    free(completed);
    update_suggestions(files);
    desk_window_redraw(&files->window);
}

/* Tab: the chosen or only suggestion, else as much as they all share. */
static void complete_path(struct files *files)
{
    if (files->suggestion >= 0) {
        use_suggestion(files, files->suggestions[files->suggestion]);
        return;
    }
    if (!files->suggestion_count) {
        /* A whole directory name: on into it. */
        const char *typed = field_text(&files->path_field);
        size_t length = strlen(typed);
        char *path = length && typed[length - 1] != '/' ? typed_path(files, typed) : NULL;
        struct stat info;
        if (path && stat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
            desk_text_move_to(&files->path_field.text, length, false);
            desk_text_insert(&files->path_field.text, "/", 1);
            update_suggestions(files);
            desk_window_redraw(&files->window);
        }
        free(path);
        return;
    }
    if (files->suggestion_count == 1) {
        use_suggestion(files, files->suggestions[0]);
        return;
    }
    const char *first = files->suggestions[0];
    size_t shared = strlen(first);
    for (size_t i = 1; i < files->suggestion_count; ++i) {
        size_t same = 0;
        while (same < shared && files->suggestions[i][same] == first[same])
            ++same;
        shared = same;
    }
    const char *typed = field_text(&files->path_field);
    const char *slash = strrchr(typed, '/');
    const char *prefix = slash ? slash + 1 : typed;
    if (shared <= strlen(prefix))
        return;
    char *completed = NULL;
    if (asprintf(&completed, "%.*s%.*s", slash ? (int)(slash - typed) + 1 : 0, typed, (int)shared, first) < 0)
        return;
    desk_text_set(&files->path_field.text, completed);
    free(completed);
    update_suggestions(files);
    desk_window_redraw(&files->window);
}

/* Enter: go to what's typed. A file opens its directory with it selected;
   somewhere that doesn't exist says so and keeps the field. */
static void go_to_typed(struct files *files)
{
    char *full = typed_path(files, field_text(&files->path_field));
    char *path = full ? normalize_path(full) : NULL;
    free(full);
    if (!path)
        return;
    struct stat info;
    if (stat(path, &info) != 0) {
        notify(files, "%s: %s", path, strerror(errno));
        free(path);
        return;
    }
    stop_path_edit(files);
    if (S_ISDIR(info.st_mode)) {
        go_to(files, path, NULL);
        return;
    }
    char *parent = parent_path(path);
    if (parent)
        go_to(files, parent, base_name(path));
    free(path);
}

static int path_key(struct files *files, uint32_t key, xkb_keysym_t sym, unsigned mods)
{
    switch (sym) {
    case XKB_KEY_Escape:
        stop_path_edit(files);
        return 0;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (files->suggestion >= 0)
            use_suggestion(files, files->suggestions[files->suggestion]);
        go_to_typed(files);
        return 0;
    case XKB_KEY_Tab:
        complete_path(files);
        return 0;
    case XKB_KEY_Down:
    case XKB_KEY_Up:
        if (files->suggestion_count) {
            int count = (int)(files->suggestion_count < SUGGESTIONS_SHOWN ? files->suggestion_count
                                                                          : SUGGESTIONS_SHOWN);
            int step = sym == XKB_KEY_Down ? 1 : -1;
            files->suggestion = files->suggestion < 0 ? (step > 0 ? 0 : count - 1)
                                                      : (files->suggestion + step + count) % count;
            desk_window_redraw(&files->window);
        }
        return 1;
    default:
        break;
    }
    bool changed;
    int repeats = field_key(files, &files->path_field, key, sym, mods, &changed);
    if (changed) {
        update_suggestions(files);
        desk_window_redraw(&files->window);
    }
    return repeats;
}

/* Changing files

   Everything here changes only what may_modify allows: inside home, unless
   files runs as root. */

static void close_menus(struct files *files);

static void protected_notice(struct files *files, const char *directory)
{
    char shown[PATH_MAX];
    display_path(files, directory, shown, sizeof shown);
    notify(files, "%s is protected: files only changes things inside your home directory", shown);
}

static bool in_trash(const struct files *files)
{
    return files->trash_files && strcmp(files->path, files->trash_files) == 0;
}

static void free_paths(char **paths, size_t count)
{
    for (size_t i = 0; paths && i < count; ++i)
        free(paths[i]);
    free(paths);
}

/* The selected entries' paths, in order. */
static char **selected_paths(struct files *files, size_t *count)
{
    *count = 0;
    size_t total = selected_count(files);
    char **paths = total ? calloc(total, sizeof *paths) : NULL;
    for (size_t i = 0; paths && i < files->listing.shown_count; ++i) {
        const struct entry *entry = shown_entry(files, i);
        if (entry->selected && (paths[*count] = join_path(files->path, entry->name)))
            ++*count;
    }
    return paths;
}

/* Undo */

static void undo_forget(struct files *files)
{
    struct undo *undo = &files->undo;
    free_paths(undo->from, undo->count);
    free_paths(undo->to, undo->count);
    *undo = (struct undo){0};
}

/* Remember a change (taking over the arrays) for Ctrl+Z. */
static void undo_set(struct files *files, enum undo_kind kind, char **from, char **to, size_t count)
{
    undo_forget(files);
    files->undo = (struct undo){kind, from, to, count};
}

static char **one_path(const char *path)
{
    char **paths = calloc(1, sizeof *paths);
    if (paths && !(paths[0] = strdup(path))) {
        free(paths);
        paths = NULL;
    }
    return paths;
}

/* rename, refusing to replace anything. */
static int move_no_replace(const char *from, const char *to)
{
    if (renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS)
        return errno;
    /* A file system without RENAME_NOREPLACE: check, then rename. */
    struct stat info;
    if (lstat(to, &info) == 0)
        return EEXIST;
    return rename(from, to) == 0 ? 0 : errno;
}

/* Reread the directory with name, if there, alone selected. */
static void refresh_selecting(struct files *files, const char *name)
{
    load(files, name, files->cursor, true);
    size_t index;
    if (name && find_shown(&files->listing, name, &index))
        select_only(files, index);
    files->free_scroll = false;
    desk_window_redraw(&files->window);
}

/* Renaming */

/* Edit the name of the entry at index in place: all of it for a directory,
   the part before the extension for a file. */
static void start_rename(struct files *files, size_t index, bool created)
{
    if (index >= files->listing.shown_count)
        return;
    if (!files->writable) {
        protected_notice(files, files->path);
        return;
    }
    const struct entry *entry = shown_entry(files, index);
    free(files->rename_name);
    files->rename_name = strdup(entry->name);
    if (!files->rename_name || !desk_text_set(&files->rename_field.text, entry->name))
        return;
    const char *dot = strrchr(entry->name, '.');
    files->rename_field.text.anchor = 0;
    files->rename_field.text.cursor = !entry->directory && dot && dot != entry->name ? (size_t)(dot - entry->name)
                                                                                    : strlen(entry->name);
    files->rename_field.scroll = 0;
    files->renaming = true;
    files->rename_created = created;
    search_focus(files, false);
    stop_path_edit(files);
    select_only(files, index);
    files->free_scroll = false;
    desk_window_redraw(&files->window);
}

static void stop_rename(struct files *files)
{
    files->renaming = false;
    if (files->selecting == &files->rename_field)
        files->selecting = NULL;
    free(files->rename_name);
    files->rename_name = NULL;
    desk_window_redraw(&files->window);
}

/* Enter, Tab or a click elsewhere: rename to what's typed. A name that
   can't be used says why and keeps the field open. */
static void finish_rename(struct files *files)
{
    if (!files->renaming)
        return;
    const char *name = field_text(&files->rename_field);
    if (!name[0] || strcmp(name, files->rename_name) == 0) {
        stop_rename(files);
        return;
    }
    if (strchr(name, '/')) {
        notify(files, "A name can't contain /");
        return;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        notify(files, "%s can't be used as a name", name);
        return;
    }
    char *from = join_path(files->path, files->rename_name), *to = join_path(files->path, name);
    int error = from && to ? move_no_replace(from, to) : ENOMEM;
    if (error) {
        if (error == EEXIST)
            notify(files, "%s already exists", name);
        else
            notify(files, "Couldn't rename %s: %s", files->rename_name, strerror(error));
        free(from);
        free(to);
        return;
    }
    if (files->rename_created) {
        /* Undoing a new folder named on creation removes it by its new name. */
        free(from);
        undo_set(files, UNDO_CREATE, NULL, one_path(to), 1);
        free(to);
    } else {
        char **froms = calloc(1, sizeof *froms), **tos = calloc(1, sizeof *tos);
        if (froms && tos) {
            froms[0] = from;
            tos[0] = to;
            undo_set(files, UNDO_RENAME, froms, tos, 1);
        } else {
            free(from);
            free(to);
            free(froms);
            free(tos);
        }
    }
    char *kept = strdup(name);
    stop_rename(files);
    refresh_selecting(files, kept);
    free(kept);
}

/* Keys while a name is edited. */
static int rename_key(struct files *files, uint32_t key, xkb_keysym_t sym, unsigned mods)
{
    switch (sym) {
    case XKB_KEY_Escape:
        stop_rename(files);
        return 0;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_Tab:
        finish_rename(files);
        return 0;
    case XKB_KEY_Up:
    case XKB_KEY_Down:
    case XKB_KEY_Page_Up:
    case XKB_KEY_Page_Down:
        return 0;
    default:
        break;
    }
    bool changed;
    return field_key(files, &files->rename_field, key, sym, mods, &changed);
}

/* New folder, New file: made as "New folder" (or "New folder 2"...), then
   named in place. */
static void create_item(struct files *files, bool folder)
{
    if (!files->writable) {
        protected_notice(files, files->path);
        return;
    }
    const char *base = folder ? "New folder" : "New file";
    char name[64];
    int error = EEXIST;
    for (int n = 1; n < 1000 && error == EEXIST; ++n) {
        if (n == 1)
            snprintf(name, sizeof name, "%s", base);
        else
            snprintf(name, sizeof name, "%s %d", base, n);
        char *path = join_path(files->path, name);
        if (!path)
            return;
        if (folder) {
            error = mkdir(path, 0777) == 0 ? 0 : errno;
        } else {
            int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
            error = fd >= 0 ? 0 : errno;
            if (fd >= 0)
                close(fd);
        }
        if (!error)
            undo_set(files, UNDO_CREATE, NULL, one_path(path), 1);
        free(path);
    }
    if (error) {
        notify(files, "Couldn't make %s: %s", folder ? "a folder" : "a file", strerror(error));
        return;
    }
    search_clear(files); /* Or it might not show. */
    refresh_selecting(files, name);
    size_t index;
    if (find_shown(&files->listing, name, &index))
        start_rename(files, index, true);
}

/* The trash */

static char *trash_directory(const char *home)
{
    const char *data = getenv("XDG_DATA_HOME");
    char *path = NULL;
    if (data && data[0] == '/') {
        if (asprintf(&path, "%s/Trash", data) < 0)
            path = NULL;
    } else if (home && asprintf(&path, "%s/.local/share/Trash", home) < 0) {
        path = NULL;
    }
    return path;
}

/* The .trashinfo file for a name in the trash. */
static char *trash_info_path(const struct files *files, const char *name)
{
    char *path = NULL;
    if (asprintf(&path, "%s/info/%s.trashinfo", files->trash_dir, name) < 0)
        return NULL;
    return path;
}

/* Move path into the trash, as the freedesktop spec says: an info file
   with where it came from and when, then the file itself, under a name
   not yet used there. Sets *trashed to where it went. */
static int trash_one(struct files *files, const char *path, char **trashed)
{
    *trashed = NULL;
    if (!files->trash_dir)
        return ENOENT;
    struct stat item, trash;
    if (lstat(path, &item) != 0)
        return errno;
    char *info_dir = NULL;
    if (asprintf(&info_dir, "%s/info", files->trash_dir) < 0)
        return ENOMEM;
    char *files_dir = strdup(files->trash_files);
    if (!files_dir) {
        free(info_dir);
        return ENOMEM;
    }
    make_private_directories(info_dir);
    make_private_directories(files_dir);
    free(info_dir);
    free(files_dir);
    if (stat(files->trash_files, &trash) != 0)
        return errno;
    if (item.st_dev != trash.st_dev)
        return EXDEV; /* Only the home trash, on home's drive, for now. */

    char *uri = path_uri(path);
    if (!uri)
        return ENOMEM;
    char date[32];
    time_t now = time(NULL);
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", localtime(&now));
    const char *name = base_name(path), *dot = strrchr(name, '.');
    size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    int error = EEXIST;
    for (int n = 1; n < 10000 && error == EEXIST; ++n) {
        char *candidate = NULL;
        if (n == 1 ? asprintf(&candidate, "%s", name) < 0
                   : asprintf(&candidate, "%.*s.%d%s", (int)stem, name, n, name + stem) < 0) {
            error = ENOMEM;
            break;
        }
        char *info = trash_info_path(files, candidate), *target = join_path(files->trash_files, candidate);
        free(candidate);
        int fd = info && target ? open(info, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600) : -1;
        struct stat taken;
        if (fd < 0) {
            error = info && target ? errno : ENOMEM;
        } else if (lstat(target, &taken) == 0) {
            close(fd);
            unlink(info);
            error = EEXIST;
        } else {
            dprintf(fd, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", uri + strlen("file://"), date);
            error = close(fd) == 0 ? 0 : errno;
            if (!error && rename(path, target) != 0)
                error = errno;
            if (error)
                unlink(info);
            else
                *trashed = target, target = NULL;
        }
        free(info);
        free(target);
    }
    free(uri);
    return error;
}

/* Where an item in the trash came from, from its info file. */
static char *trash_origin(const struct files *files, const char *name)
{
    char *info = trash_info_path(files, name);
    FILE *file = info ? fopen(info, "re") : NULL;
    free(info);
    if (!file)
        return NULL;
    char *line = NULL, *origin = NULL;
    size_t capacity = 0;
    ssize_t length;
    while (!origin && (length = getline(&line, &capacity, file)) > 0) {
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = 0;
        if (strncmp(line, "Path=", 5) == 0 && line[5] == '/') {
            char *uri = NULL;
            if (asprintf(&uri, "file://%s", line + 5) >= 0)
                origin = uri_path(uri, strlen(uri));
            free(uri);
        }
    }
    free(line);
    fclose(file);
    return origin;
}

static void trash_paths(struct files *files, char **paths, size_t count);

/* Delete, outside the trash: move the selection there. */
static void trash_selection(struct files *files)
{
    size_t count;
    char **paths = selected_paths(files, &count);
    if (!count) {
        free(paths);
        return;
    }
    if (!files->writable) {
        protected_notice(files, files->path);
        free_paths(paths, count);
        return;
    }
    trash_paths(files, paths, count);
}

/* Move paths to the trash (taking them over), with undo. */
static void trash_paths(struct files *files, char **paths, size_t count)
{
    char **trashed = calloc(count, sizeof *trashed);
    char **from = calloc(count, sizeof *from);
    size_t moved = 0;
    int first_error = 0;
    const char *failed = NULL;
    for (size_t i = 0; trashed && from && i < count; ++i) {
        char *to, *parent = parent_path(paths[i]);
        bool allowed = parent && may_modify(files, parent);
        free(parent);
        if (!allowed) {
            protected_notice(files, paths[i]);
            continue;
        }
        int error = trash_one(files, paths[i], &to);
        if (error) {
            if (!first_error) {
                first_error = error;
                failed = paths[i];
            }
            continue;
        }
        from[moved] = paths[i];
        trashed[moved++] = to;
        paths[i] = NULL;
    }
    if (moved) {
        if (moved == 1)
            notify(files, "Moved %s to the trash. Ctrl+Z puts it back", base_name(from[0]));
        else
            notify(files, "Moved %zu items to the trash. Ctrl+Z puts them back", moved);
        undo_set(files, UNDO_TRASH, from, trashed, moved);
    } else {
        free(from);
        free(trashed);
    }
    if (first_error == EXDEV)
        notify(files, "%s is on another drive, which has no trash yet; Shift+Delete deletes it", base_name(failed));
    else if (first_error)
        notify(files, "Couldn't move %s to the trash: %s", base_name(failed), strerror(first_error));
    free_paths(paths, count);
    refresh(files);
}

/* Put items from the trash back where they came from. */
static void restore_selection(struct files *files)
{
    size_t count;
    char **paths = selected_paths(files, &count);
    size_t restored = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *name = base_name(paths[i]);
        char *origin = trash_origin(files, name);
        if (!origin) {
            notify(files, "Where %s came from isn't known", name);
            continue;
        }
        char *parent = parent_path(origin);
        char *ancestor = parent ? existing(strdup(parent)) : NULL;
        if (!ancestor || !may_modify(files, ancestor)) {
            protected_notice(files, parent ? parent : origin);
        } else {
            if (strcmp(ancestor, parent) != 0) {
                /* Its folder went too: make it again. */
                char *made = strdup(parent);
                if (made) {
                    for (char *at = made + strlen(ancestor) + 1; *at; ++at) {
                        if (*at == '/') {
                            *at = 0;
                            mkdir(made, 0777);
                            *at = '/';
                        }
                    }
                    mkdir(made, 0777);
                }
                free(made);
            }
            int error = move_no_replace(paths[i], origin);
            if (error == EEXIST) {
                notify(files, "%s already exists", origin);
            } else if (error) {
                notify(files, "Couldn't restore %s: %s", name, strerror(error));
            } else {
                char *info = trash_info_path(files, name);
                if (info)
                    unlink(info);
                free(info);
                ++restored;
            }
        }
        free(ancestor);
        free(parent);
        free(origin);
    }
    if (restored)
        notify(files, restored == 1 ? "Restored %zu item" : "Restored %zu items", restored);
    free_paths(paths, count);
    undo_forget(files);
    refresh(files);
}

/* Jobs

   Copying, moving and deleting for good run on a worker thread, one job
   at a time, with a progress bar along the window's foot and a × that
   cancels. A copy is written to a hidden file beside its target and only
   renamed into place when whole, so cancelling or failing never leaves
   half a file. Nothing is replaced without asking: a name that's taken
   stops the job with a question (Skip, Keep both, Replace or Merge).
   Recursive work never follows symlinks (they're copied as links) and
   never goes into another mounted file system. */

enum job_kind { JOB_COPY, JOB_MOVE, JOB_DELETE };
enum answer { ANSWER_NONE, ANSWER_SKIP, ANSWER_BOTH, ANSWER_REPLACE, ANSWER_CANCEL };

struct job {
    enum job_kind kind;
    char **sources;
    size_t count;
    char *destination;              /* Copy and move: the directory they go into. */
    bool trash_info;                /* Delete: they're in the trash, so their info files go too. */
    _Atomic long long bytes_done, bytes_total;
    _Atomic long items_done, items_total;
    _Atomic bool cancel;
    bool counted;                   /* The totals are known. */
    char current[256];              /* The name being worked on. Under the lock. */
    /* A question for the window: target is taken. Under the lock. */
    bool asking;
    char *ask_target;
    bool ask_directories;           /* Both are directories: Merge instead of Replace. */
    enum answer answer, answer_all; /* The reply, and the one for every conflict when ticked. */
    /* Results, for the message and undo. */
    int error;                      /* The first, or 0. */
    char *error_name;
    size_t failed, skipped, merged;
    char **made_from, **made;       /* Each top-level item done: where it was and where it is. */
    size_t made_count;
};

struct worker {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int event;                      /* eventfd: progress, a question or a job done. */
    bool quit;
    struct job **queue;             /* queue[0] is the one running. */
    size_t queue_count;
    struct job **finished;
    size_t finished_count;
    uint64_t signalled_ms;
};

static uint64_t now_ms(void);

static void job_free(struct job *job)
{
    if (!job)
        return;
    free_paths(job->sources, job->count);
    free_paths(job->made_from, job->made_count);
    free_paths(job->made, job->made_count);
    free(job->destination);
    free(job->ask_target);
    free(job->error_name);
    free(job);
}

/* Tell the window, at most ten times a second unless it matters. */
static void worker_signal(struct worker *worker, bool now)
{
    uint64_t time = now_ms();
    if (!now && time - worker->signalled_ms < 100)
        return;
    worker->signalled_ms = time;
    uint64_t one = 1;
    (void)!write(worker->event, &one, sizeof one);
}

static void job_status(struct worker *worker, struct job *job, const char *name)
{
    pthread_mutex_lock(&worker->lock);
    snprintf(job->current, sizeof job->current, "%s", name);
    worker_signal(worker, false);
    pthread_mutex_unlock(&worker->lock);
}

static void job_failed(struct job *job, int error, const char *name)
{
    if (error == ECANCELED)
        return;
    ++job->failed;
    if (!job->error) {
        job->error = error;
        job->error_name = strdup(name);
    }
}

static bool cancelled(struct job *job)
{
    return atomic_load(&job->cancel);
}

/* Count what's under path, as the job will go through it. */
static void job_count(struct job *job, int directory, const char *name, dev_t device)
{
    struct stat info;
    if (cancelled(job) || fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
        return;
    atomic_fetch_add(&job->items_total, 1);
    if (S_ISREG(info.st_mode) && job->kind != JOB_DELETE)
        atomic_fetch_add(&job->bytes_total, (long long)info.st_size);
    if (!S_ISDIR(info.st_mode) || info.st_dev != device)
        return;
    int fd = openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;
    if (!dir) {
        if (fd >= 0)
            close(fd);
        return;
    }
    struct dirent *item;
    while ((item = readdir(dir))) {
        if (strcmp(item->d_name, ".") != 0 && strcmp(item->d_name, "..") != 0)
            job_count(job, dirfd(dir), item->d_name, device);
    }
    closedir(dir);
}

/* Remove name in directory and everything under it, never following a
   link and never leaving the file system it's on, like rm -r
   --one-file-system. Returns the first error. */
static int remove_at(struct job *job, int directory, const char *name, dev_t device)
{
    if (job && cancelled(job))
        return ECANCELED;
    struct stat info;
    if (fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? 0 : errno;
    if (job)
        atomic_fetch_add(&job->items_done, 1);
    if (!S_ISDIR(info.st_mode))
        return unlinkat(directory, name, 0) == 0 ? 0 : errno;
    if (info.st_dev != device)
        return EXDEV; /* A mount point: what's mounted there stays. */
    int fd = openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno;
    DIR *dir = fdopendir(fd);
    if (!dir) {
        int error = errno;
        close(fd);
        return error;
    }
    int first = 0;
    struct dirent *item;
    while ((item = readdir(dir))) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
            continue;
        int error = remove_at(job, fd, item->d_name, device);
        if (error && !first)
            first = error;
    }
    closedir(dir);
    if (!first && unlinkat(directory, name, AT_REMOVEDIR) != 0)
        first = errno;
    return first;
}

static int remove_path(struct job *job, const char *path)
{
    char *parent = parent_path(path);
    int fd = parent ? open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    free(parent);
    if (fd < 0)
        return errno;
    struct stat info;
    int error = fstatat(fd, base_name(path), &info, AT_SYMLINK_NOFOLLOW) == 0
                    ? remove_at(job, fd, base_name(path), info.st_dev)
                    : errno;
    close(fd);
    return error;
}

/* "name (2).ext" and so on: the first not taken in directory. */
static char *unused_name(const char *directory, const char *name)
{
    const char *dot = strrchr(name, '.');
    size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    for (int n = 2; n < 10000; ++n) {
        char *candidate = NULL, *path;
        if (asprintf(&candidate, "%.*s (%d)%s", (int)stem, name, n, name + stem) < 0)
            return NULL;
        path = join_path(directory, candidate);
        free(candidate);
        struct stat info;
        if (!path || lstat(path, &info) != 0)
            return path;
        free(path);
    }
    return NULL;
}

/* Ask the window what to do about target, taken; waits for the reply. */
static enum answer job_ask(struct worker *worker, struct job *job, const char *target, bool directories)
{
    pthread_mutex_lock(&worker->lock);
    if (job->answer_all != ANSWER_NONE) {
        enum answer all = job->answer_all;
        pthread_mutex_unlock(&worker->lock);
        return all;
    }
    free(job->ask_target);
    job->ask_target = strdup(target);
    job->ask_directories = directories;
    job->answer = ANSWER_NONE;
    job->asking = true;
    worker_signal(worker, true);
    while (job->answer == ANSWER_NONE && !cancelled(job) && !worker->quit)
        pthread_cond_wait(&worker->wake, &worker->lock);
    enum answer answer = cancelled(job) || worker->quit ? ANSWER_CANCEL : job->answer;
    job->asking = false;
    pthread_mutex_unlock(&worker->lock);
    return answer;
}

/* Where source goes in directory: *target, or NULL to skip it. *merge when
   both are directories and the reply was Merge; *replace when what's there
   gives way. */
static int resolve(struct worker *worker, struct job *job, const char *source, bool source_directory,
                   const char *directory, char **target, bool *merge, bool *replace)
{
    *merge = *replace = false;
    *target = join_path(directory, base_name(source));
    if (!*target)
        return ENOMEM;
    struct stat there;
    if (lstat(*target, &there) != 0)
        return 0;
    if (strcmp(*target, source) == 0) {
        free(*target);
        *target = NULL;
        if (job->kind == JOB_MOVE)
            return 0; /* Already there. */
        *target = unused_name(directory, base_name(source)); /* A copy beside it. */
        return *target ? 0 : ENOMEM;
    }
    bool directories = source_directory && S_ISDIR(there.st_mode);
    switch (job_ask(worker, job, *target, directories)) {
    case ANSWER_SKIP:
        ++job->skipped;
        free(*target);
        *target = NULL;
        return 0;
    case ANSWER_BOTH:
        free(*target);
        *target = unused_name(directory, base_name(source));
        return *target ? 0 : ENOMEM;
    case ANSWER_REPLACE:
        *merge = directories;
        *replace = !directories;
        return 0;
    default:
        free(*target);
        *target = NULL;
        return ECANCELED;
    }
}

/* Copy a regular file's bytes, mode and times to a hidden file beside
   target, then rename it into place. */
static int copy_file(struct worker *worker, struct job *job, const char *source, const struct stat *info,
                     const char *target, bool replace)
{
    job_status(worker, job, base_name(source));
    int in = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0)
        return errno;
    char *directory = parent_path(target), *partial = NULL;
    if (!directory || asprintf(&partial, "%s/.%s.files-partial-%ld", directory, base_name(target), (long)getpid()) < 0) {
        free(directory);
        close(in);
        return ENOMEM;
    }
    free(directory);
    int out = open(partial, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    int error = out < 0 ? errno : 0;
    bool plain = false;
    char *buffer = NULL;
    while (!error) {
        if (cancelled(job)) {
            error = ECANCELED;
            break;
        }
        ssize_t moved;
        if (!plain) {
            moved = copy_file_range(in, NULL, out, NULL, 4 << 20, 0);
            if (moved < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)) {
                plain = true;
                continue;
            }
        } else {
            if (!buffer && !(buffer = malloc(1 << 20))) {
                error = ENOMEM;
                break;
            }
            moved = read(in, buffer, 1 << 20);
            for (ssize_t written = 0; moved > 0 && written < moved;) {
                ssize_t step = write(out, buffer + written, (size_t)(moved - written));
                if (step < 0) {
                    if (errno == EINTR)
                        continue;
                    moved = -1;
                    break;
                }
                written += step;
            }
        }
        if (moved < 0) {
            if (errno == EINTR)
                continue;
            error = errno;
        } else if (moved == 0) {
            break;
        } else {
            atomic_fetch_add(&job->bytes_done, (long long)moved);
            pthread_mutex_lock(&worker->lock);
            worker_signal(worker, false);
            pthread_mutex_unlock(&worker->lock);
        }
    }
    free(buffer);
    close(in);
    if (!error) {
        struct timespec times[2] = {info->st_atim, info->st_mtim};
        fchmod(out, info->st_mode & 07777);
        futimens(out, times);
    }
    if (out >= 0 && close(out) != 0 && !error)
        error = errno;
    if (!error) {
        if (replace)
            error = rename(partial, target) == 0 ? 0 : errno;
        else
            error = move_no_replace(partial, target);
    }
    if (error && out >= 0)
        unlink(partial);
    free(partial);
    return error;
}

/* Copy source (described by info) to target, the whole tree. merge: target
   is a directory already, so go into it. */
static int copy_tree(struct worker *worker, struct job *job, const char *source, const struct stat *info,
                     const char *target, bool merge, bool replace, dev_t device)
{
    if (cancelled(job))
        return ECANCELED;
    atomic_fetch_add(&job->items_done, 1);
    if (replace) {
        /* A file over a file is renamed over it when whole; anything else
           takes the old one's place once it's gone. */
        struct stat there;
        if (lstat(target, &there) == 0 && (S_ISDIR(there.st_mode) || !S_ISREG(info->st_mode))) {
            int error = remove_path(NULL, target);
            if (error)
                return error;
            replace = false;
        }
    }
    if (S_ISREG(info->st_mode))
        return copy_file(worker, job, source, info, target, replace);
    if (S_ISLNK(info->st_mode)) {
        char link[PATH_MAX];
        ssize_t length = readlink(source, link, sizeof link - 1);
        if (length < 0)
            return errno;
        link[length] = 0;
        return symlink(link, target) == 0 ? 0 : errno;
    }
    if (!S_ISDIR(info->st_mode))
        return ENOTSUP; /* Devices, pipes, sockets. */
    job_status(worker, job, base_name(source));
    if (!merge && mkdir(target, (info->st_mode & 07777) | 0700) != 0)
        return errno;
    if (info->st_dev != device)
        return 0; /* A mount point: its folder, not what's mounted. */
    DIR *dir = opendir(source);
    if (!dir)
        return errno;
    int first = 0;
    struct dirent *item;
    while ((item = readdir(dir))) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
            continue;
        char *child = join_path(source, item->d_name);
        struct stat child_info;
        int error = child && lstat(child, &child_info) == 0 ? 0 : errno ? errno : ENOMEM;
        char *child_target = NULL;
        bool child_merge = false, child_replace = false;
        if (!error && merge)
            error = resolve(worker, job, child, S_ISDIR(child_info.st_mode), target, &child_target, &child_merge,
                            &child_replace);
        else if (!error)
            child_target = join_path(target, item->d_name);
        if (!error && child_target)
            error = copy_tree(worker, job, child, &child_info, child_target, child_merge, child_replace, device);
        if (error == ECANCELED) {
            free(child);
            free(child_target);
            first = ECANCELED;
            break;
        }
        if (error)
            job_failed(job, error, item->d_name);
        if (error && !first)
            first = error;
        free(child);
        free(child_target);
    }
    closedir(dir);
    if (!merge) {
        struct timespec times[2] = {info->st_atim, info->st_mtim};
        chmod(target, info->st_mode & 07777);
        utimensat(AT_FDCWD, target, times, AT_SYMLINK_NOFOLLOW);
    }
    return first == ECANCELED ? ECANCELED : first ? -1 : 0; /* -1: already counted. */
}

/* Move source into target: a rename on the same file system (merging a
   directory into one that's there by moving what's inside), else a copy
   and then removing the original once all of it was copied. */
static int move_tree(struct worker *worker, struct job *job, const char *source, const struct stat *info,
                     const char *target, bool merge, bool replace)
{
    if (cancelled(job))
        return ECANCELED;
    job_status(worker, job, base_name(source));
    if (!merge) {
        int error = replace ? (rename(source, target) == 0 ? 0 : errno) : move_no_replace(source, target);
        if (error == EISDIR || error == ENOTDIR || error == ENOTEMPTY || error == EEXIST) {
            /* Replacing across kinds: the old one goes first. */
            if (replace && (error = remove_path(NULL, target)) == 0)
                error = move_no_replace(source, target);
        }
        if (error != EXDEV) {
            if (!error)
                atomic_fetch_add(&job->items_done, 1);
            return error;
        }
        error = copy_tree(worker, job, source, info, target, false, replace, info->st_dev);
        return error ? error : remove_path(NULL, source);
    }
    /* Both directories: move each thing inside, then the emptied one goes. */
    DIR *dir = opendir(source);
    if (!dir)
        return errno;
    int first = 0;
    struct dirent *item;
    while ((item = readdir(dir))) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
            continue;
        char *child = join_path(source, item->d_name), *child_target = NULL;
        struct stat child_info;
        bool child_merge, child_replace;
        int error = child && lstat(child, &child_info) == 0 ? 0 : errno ? errno : ENOMEM;
        if (!error)
            error = resolve(worker, job, child, S_ISDIR(child_info.st_mode), target, &child_target, &child_merge,
                            &child_replace);
        if (!error && child_target)
            error = move_tree(worker, job, child, &child_info, child_target, child_merge, child_replace);
        free(child);
        free(child_target);
        if (error == ECANCELED) {
            first = ECANCELED;
            break;
        }
        if (error > 0) {
            job_failed(job, error, item->d_name);
            first = first ? first : -1;
        }
    }
    closedir(dir);
    if (!first)
        rmdir(source); /* Skipped things keep it. */
    atomic_fetch_add(&job->items_done, 1);
    return first;
}

static void job_made(struct job *job, const char *from, const char *to)
{
    char **froms = realloc(job->made_from, (job->made_count + 1) * sizeof *froms);
    if (froms)
        job->made_from = froms;
    char **tos = realloc(job->made, (job->made_count + 1) * sizeof *tos);
    if (tos)
        job->made = tos;
    if (!froms || !tos)
        return;
    job->made_from[job->made_count] = strdup(from);
    job->made[job->made_count++] = strdup(to);
}

static void job_run(struct worker *worker, struct job *job)
{
    /* Count first, for the bar; a move within a file system is one rename
       a piece. */
    struct stat destination;
    bool have_destination = job->destination && stat(job->destination, &destination) == 0;
    for (size_t i = 0; i < job->count && !cancelled(job); ++i) {
        struct stat info;
        if (lstat(job->sources[i], &info) != 0)
            continue;
        if (job->kind == JOB_MOVE && have_destination && info.st_dev == destination.st_dev) {
            atomic_fetch_add(&job->items_total, 1);
            continue;
        }
        char *parent = parent_path(job->sources[i]);
        int fd = parent ? open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
        if (fd >= 0) {
            job_count(job, fd, base_name(job->sources[i]), info.st_dev);
            close(fd);
        }
        free(parent);
    }
    pthread_mutex_lock(&worker->lock);
    job->counted = true;
    worker_signal(worker, true);
    pthread_mutex_unlock(&worker->lock);

    for (size_t i = 0; i < job->count && !cancelled(job); ++i) {
        const char *source = job->sources[i];
        struct stat info;
        if (lstat(source, &info) != 0) {
            job_failed(job, errno, base_name(source));
            continue;
        }
        int error = 0;
        if (job->kind == JOB_DELETE) {
            job_status(worker, job, base_name(source));
            error = remove_path(job, source);
            if (!error) {
                job_made(job, source, source);
                if (job->trash_info && job->destination) {
                    char *info_path = NULL;
                    if (asprintf(&info_path, "%s/info/%s.trashinfo", job->destination, base_name(source)) >= 0)
                        unlink(info_path);
                    free(info_path);
                }
            }
        } else {
            size_t length = strlen(source);
            if (S_ISDIR(info.st_mode) && strncmp(job->destination, source, length) == 0 &&
                (job->destination[length] == '/' || job->destination[length] == 0) &&
                !(job->kind == JOB_MOVE && strcmp(job->destination, source) == 0)) {
                job_failed(job, ELOOP, base_name(source)); /* Into itself. */
                continue;
            }
            char *target = NULL;
            bool merge, replace;
            error = resolve(worker, job, source, S_ISDIR(info.st_mode), job->destination, &target, &merge, &replace);
            if (!error && target) {
                error = job->kind == JOB_COPY ? copy_tree(worker, job, source, &info, target, merge, replace, info.st_dev)
                                              : move_tree(worker, job, source, &info, target, merge, replace);
                if (!error && !merge)
                    job_made(job, source, target);
                else if (!error)
                    ++job->merged;
            }
            free(target);
        }
        if (error == ECANCELED)
            break;
        if (error > 0)
            job_failed(job, error == ELOOP ? ELOOP : error, base_name(source));
    }
}

static void *worker_main(void *data)
{
    struct worker *worker = data;
    pthread_mutex_lock(&worker->lock);
    while (!worker->quit) {
        if (!worker->queue_count) {
            pthread_cond_wait(&worker->wake, &worker->lock);
            continue;
        }
        struct job *job = worker->queue[0];
        pthread_mutex_unlock(&worker->lock);
        job_run(worker, job);
        pthread_mutex_lock(&worker->lock);
        memmove(worker->queue, worker->queue + 1, --worker->queue_count * sizeof *worker->queue);
        struct job **finished = realloc(worker->finished, (worker->finished_count + 1) * sizeof *finished);
        if (finished) {
            worker->finished = finished;
            worker->finished[worker->finished_count++] = job;
        } else {
            job_free(job);
        }
        worker_signal(worker, true);
    }
    pthread_mutex_unlock(&worker->lock);
    return NULL;
}

static struct worker *worker_create(void)
{
    struct worker *worker = calloc(1, sizeof *worker);
    if (!worker)
        return NULL;
    worker->event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    pthread_mutex_init(&worker->lock, NULL);
    pthread_cond_init(&worker->wake, NULL);
    if (worker->event < 0 || pthread_create(&worker->thread, NULL, worker_main, worker) != 0) {
        if (worker->event >= 0)
            close(worker->event);
        pthread_mutex_destroy(&worker->lock);
        pthread_cond_destroy(&worker->wake);
        free(worker);
        return NULL;
    }
    return worker;
}

/* Closing the window cancels what's running; nothing is left half done. */
static void worker_destroy(struct worker *worker)
{
    if (!worker)
        return;
    pthread_mutex_lock(&worker->lock);
    worker->quit = true;
    for (size_t i = 0; i < worker->queue_count; ++i)
        atomic_store(&worker->queue[i]->cancel, true);
    pthread_cond_broadcast(&worker->wake);
    pthread_mutex_unlock(&worker->lock);
    pthread_join(worker->thread, NULL);
    for (size_t i = 0; i < worker->queue_count; ++i)
        job_free(worker->queue[i]);
    for (size_t i = 0; i < worker->finished_count; ++i)
        job_free(worker->finished[i]);
    free(worker->queue);
    free(worker->finished);
    close(worker->event);
    pthread_mutex_destroy(&worker->lock);
    pthread_cond_destroy(&worker->wake);
    free(worker);
}

/* Queue a job (taking over sources). Copying and moving need the
   destination to be changeable, moving and deleting the sources' folders. */
static void start_job(struct files *files, enum job_kind kind, char **sources, size_t count, const char *destination)
{
    if (!count) {
        free(sources);
        return;
    }
    if (kind != JOB_DELETE && !may_modify(files, destination)) {
        protected_notice(files, destination);
        free_paths(sources, count);
        return;
    }
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        char *parent = parent_path(sources[i]);
        bool allowed = kind == JOB_COPY || (parent && may_modify(files, parent));
        if (!allowed) {
            protected_notice(files, parent ? parent : sources[i]);
            free(sources[i]);
        } else {
            sources[kept++] = sources[i];
        }
        free(parent);
    }
    struct job *job = kept ? calloc(1, sizeof *job) : NULL;
    if (!job || !files->worker) {
        if (kept && !files->worker)
            notify(files, "Couldn't start a thread for this");
        free_paths(sources, kept);
        free(job);
        return;
    }
    job->kind = kind;
    job->sources = sources;
    job->count = kept;
    job->destination = destination ? strdup(destination) : NULL;
    /* Deleting from the trash: their info files go too. */
    char *first_parent = parent_path(sources[0]);
    job->trash_info = kind == JOB_DELETE && files->trash_files && first_parent &&
                      strcmp(first_parent, files->trash_files) == 0;
    free(first_parent);
    if (job->trash_info)
        job->destination = strdup(files->trash_dir);
    struct worker *worker = files->worker;
    pthread_mutex_lock(&worker->lock);
    struct job **queue = realloc(worker->queue, (worker->queue_count + 1) * sizeof *queue);
    if (queue) {
        worker->queue = queue;
        worker->queue[worker->queue_count++] = job;
        pthread_cond_broadcast(&worker->wake);
    }
    pthread_mutex_unlock(&worker->lock);
    if (!queue)
        job_free(job);
    desk_window_redraw(&files->window);
}

/* The progress bar's ×. */
static void cancel_job(struct files *files)
{
    struct worker *worker = files->worker;
    if (!worker)
        return;
    pthread_mutex_lock(&worker->lock);
    if (worker->queue_count) {
        atomic_store(&worker->queue[0]->cancel, true);
        pthread_cond_broadcast(&worker->wake);
    }
    pthread_mutex_unlock(&worker->lock);
}

/* The dialog */

enum { DIALOG_DELETE, DIALOG_EMPTY_TRASH, DIALOG_CONFLICT, DIALOG_PROPERTIES };

/* labels left to right; danger is drawn red (or -1); focus has Enter. */
static void open_dialog(struct files *files, int action, char **paths, size_t count, const char *title,
                        const char *message, const char *const *labels, int buttons, int danger, int focus)
{
    struct dialog *dialog = &files->dialog;
    free_paths(dialog->paths, dialog->count);
    *dialog = (struct dialog){.open = true, .action = action, .paths = paths, .count = count,
                              .button_count = buttons, .danger = danger, .focus = focus};
    snprintf(dialog->title, sizeof dialog->title, "%s", title);
    snprintf(dialog->message, sizeof dialog->message, "%s", message);
    for (int i = 0; i < buttons && i < DIALOG_BUTTONS; ++i)
        snprintf(dialog->labels[i], sizeof dialog->labels[i], "%s", labels[i]);
    close_menus(files);
    if (files->renaming)
        stop_rename(files);
    desk_window_redraw(&files->window);
}

static void counter_stop(struct files *files);

static void close_dialog(struct files *files)
{
    struct dialog *dialog = &files->dialog;
    if (dialog->action == DIALOG_PROPERTIES)
        counter_stop(files);
    free_paths(dialog->paths, dialog->count);
    *dialog = (struct dialog){0};
    /* A job may have a question that waited for this one. */
    if (files->worker) {
        uint64_t one = 1;
        (void)!write(files->worker->event, &one, sizeof one);
    }
    desk_window_redraw(&files->window);
}

/* Shift+Delete, or Delete in the trash: ask, then delete for good. */
static void ask_delete(struct files *files)
{
    size_t count;
    char **paths = selected_paths(files, &count);
    if (!count) {
        free(paths);
        return;
    }
    if (!files->writable) {
        protected_notice(files, files->path);
        free_paths(paths, count);
        return;
    }
    char title[300];
    if (count == 1)
        snprintf(title, sizeof title, "Delete %s permanently?", base_name(paths[0]));
    else
        snprintf(title, sizeof title, "Delete %zu items permanently?", count);
    static const char *const labels[] = {"Cancel", "Delete"};
    open_dialog(files, DIALOG_DELETE, paths, count, title,
                count == 1 && cursor_entry(files) && cursor_entry(files)->directory
                    ? "It and everything in it will be gone. This can't be undone."
                    : "This can't be undone.",
                labels, 2, 1, 0);
}

static void ask_empty_trash(struct files *files)
{
    DIR *dir = files->trash_files ? opendir(files->trash_files) : NULL;
    size_t count = 0;
    struct dirent *item;
    while (dir && (item = readdir(dir)))
        count += strcmp(item->d_name, ".") != 0 && strcmp(item->d_name, "..") != 0;
    if (dir)
        closedir(dir);
    if (!count) {
        notify(files, "The trash is empty");
        return;
    }
    char message[100];
    snprintf(message, sizeof message,
             count == 1 ? "%zu item will be deleted permanently." : "%zu items will be deleted permanently.", count);
    static const char *const labels[] = {"Cancel", "Empty trash"};
    open_dialog(files, DIALOG_EMPTY_TRASH, NULL, 0, "Empty the trash?", message, labels, 2, 1, 0);
}

/* The running job asks about a name that's taken. */
static void ask_conflict(struct files *files, const char *target, bool directories, bool several)
{
    char title[300], message[300], shown[PATH_MAX];
    char *parent = parent_path(target);
    display_path(files, parent ? parent : target, shown, sizeof shown);
    free(parent);
    snprintf(title, sizeof title, directories ? "Merge %s?" : "Replace %s?", base_name(target));
    snprintf(message, sizeof message,
             directories ? "A folder with this name is already in %.200s." : "Something with this name is already in %.200s.",
             shown);
    const char *labels[] = {"Skip", "Keep both", directories ? "Merge" : "Replace"};
    open_dialog(files, DIALOG_CONFLICT, NULL, 0, title, message, labels, 3, -1, 0);
    files->dialog.has_check = several;
}

/* Reply to the running job's question. */
static void answer_job(struct files *files, enum answer answer, bool all)
{
    struct worker *worker = files->worker;
    pthread_mutex_lock(&worker->lock);
    if (worker->queue_count && worker->queue[0]->asking) {
        struct job *job = worker->queue[0];
        if (answer == ANSWER_CANCEL)
            atomic_store(&job->cancel, true);
        job->answer = answer;
        if (all && answer != ANSWER_CANCEL)
            job->answer_all = answer;
        pthread_cond_broadcast(&worker->wake);
    }
    pthread_mutex_unlock(&worker->lock);
}

/* A button of the dialog: do what it says. */
static void dialog_choose(struct files *files, int button)
{
    struct dialog *dialog = &files->dialog;
    int action = dialog->action;
    bool all = dialog->has_check && dialog->checked;
    char **paths = dialog->paths;
    size_t count = dialog->count;
    dialog->paths = NULL;
    dialog->count = 0;
    close_dialog(files);
    if (action == DIALOG_CONFLICT) {
        static const enum answer answers[] = {ANSWER_SKIP, ANSWER_BOTH, ANSWER_REPLACE};
        answer_job(files, button >= 0 && button < 3 ? answers[button] : ANSWER_CANCEL, all);
    } else if (button == 1 && action == DIALOG_DELETE) {
        start_job(files, JOB_DELETE, paths, count, NULL);
        paths = NULL;
    } else if (button == 1 && action == DIALOG_EMPTY_TRASH && files->trash_files) {
        /* Everything in the trash, with its info files. */
        DIR *dir = opendir(files->trash_files);
        size_t used = 0, capacity = 0;
        char **items = NULL;
        struct dirent *item;
        while (dir && (item = readdir(dir))) {
            if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
                continue;
            if (used == capacity) {
                capacity = capacity ? capacity * 2 : 32;
                char **grown = realloc(items, capacity * sizeof *grown);
                if (!grown)
                    break;
                items = grown;
            }
            if ((items[used] = join_path(files->trash_files, item->d_name)))
                ++used;
        }
        if (dir)
            closedir(dir);
        start_job(files, JOB_DELETE, items, used, NULL);
    }
    free_paths(paths, count);
}

/* Properties

   Alt+Enter, or Properties in the menu: what the selection is, over the
   window like the other dialogs. A folder's size, and how many files and
   folders are in it, are counted on a thread while it's open; the
   numbers grow as they come. */

struct counter {
    pthread_t thread;
    int event;                      /* eventfd: the numbers moved, or it's done. */
    char **paths;
    size_t count;
    _Atomic long long bytes;
    _Atomic long files, folders;
    _Atomic bool done, cancel;
    uint64_t signalled_ms;
};

static void count_tree(struct counter *counter, int directory, const char *name, dev_t device, bool top)
{
    struct stat info;
    if (atomic_load(&counter->cancel) || fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
        return;
    if (!S_ISDIR(info.st_mode)) {
        atomic_fetch_add(&counter->files, 1);
        if (S_ISREG(info.st_mode))
            atomic_fetch_add(&counter->bytes, (long long)info.st_size);
    } else {
        if (!top)
            atomic_fetch_add(&counter->folders, 1);
        int fd = info.st_dev == device ? openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
        DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;
        if (!dir && fd >= 0)
            close(fd);
        struct dirent *item;
        while (dir && (item = readdir(dir))) {
            if (strcmp(item->d_name, ".") != 0 && strcmp(item->d_name, "..") != 0)
                count_tree(counter, dirfd(dir), item->d_name, device, false);
        }
        if (dir)
            closedir(dir);
    }
    uint64_t now = now_ms();
    if (now - counter->signalled_ms >= 150) {
        counter->signalled_ms = now;
        uint64_t one = 1;
        (void)!write(counter->event, &one, sizeof one);
    }
}

static void *counter_main(void *data)
{
    struct counter *counter = data;
    for (size_t i = 0; i < counter->count && !atomic_load(&counter->cancel); ++i) {
        char *parent = parent_path(counter->paths[i]);
        int fd = parent ? open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
        struct stat info;
        if (fd >= 0 && fstatat(fd, base_name(counter->paths[i]), &info, AT_SYMLINK_NOFOLLOW) == 0) {
            /* A selected folder counts as one; what's in it is counted too. */
            if (S_ISDIR(info.st_mode) && counter->count > 1)
                atomic_fetch_add(&counter->folders, 1);
            count_tree(counter, fd, base_name(counter->paths[i]), info.st_dev, true);
        }
        if (fd >= 0)
            close(fd);
        free(parent);
    }
    atomic_store(&counter->done, true);
    uint64_t one = 1;
    (void)!write(counter->event, &one, sizeof one);
    return NULL;
}

static void counter_stop(struct files *files)
{
    struct counter *counter = files->counter;
    if (!counter)
        return;
    atomic_store(&counter->cancel, true);
    pthread_join(counter->thread, NULL);
    close(counter->event);
    free_paths(counter->paths, counter->count);
    free(counter);
    files->counter = NULL;
}

static void counter_start(struct files *files, char *const *paths, size_t count)
{
    counter_stop(files);
    struct counter *counter = calloc(1, sizeof *counter);
    if (!counter)
        return;
    counter->paths = calloc(count, sizeof *counter->paths);
    for (size_t i = 0; counter->paths && i < count; ++i) {
        if ((counter->paths[counter->count] = strdup(paths[i])))
            ++counter->count;
    }
    counter->event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (!counter->paths || counter->event < 0 || pthread_create(&counter->thread, NULL, counter_main, counter) != 0) {
        if (counter->event >= 0)
            close(counter->event);
        free_paths(counter->paths, counter->count);
        free(counter);
        return;
    }
    files->counter = counter;
}

static void counter_event(struct files *files)
{
    uint64_t count;
    (void)!read(files->counter->event, &count, sizeof count);
    desk_window_redraw(&files->window);
}

static void format_size(off_t size, char *out, size_t length);

/* "1.2 MB (1,234,567 bytes)". */
static void format_bytes(long long bytes, char *out, size_t length)
{
    char size[32], digits[32], grouped[48];
    format_size((off_t)bytes, size, sizeof size);
    snprintf(digits, sizeof digits, "%lld", bytes);
    size_t n = strlen(digits), used = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i && (n - i) % 3 == 0)
            grouped[used++] = ',';
        grouped[used++] = digits[i];
    }
    grouped[used] = 0;
    if (bytes < 1000)
        snprintf(out, length, "%s", grouped[0] ? grouped : "0");
    else
        snprintf(out, length, "%s (%s bytes)", size, grouped);
    if (bytes < 1000)
        snprintf(out + strlen(out), length - strlen(out), bytes == 1 ? " byte" : " bytes");
}

static void format_full_date(const struct timespec *when, char *out, size_t length)
{
    struct tm local;
    time_t seconds = when->tv_sec;
    if (!localtime_r(&seconds, &local) || !strftime(out, length, "%A, %d %B %Y, %H:%M:%S", &local))
        snprintf(out, length, "?");
}

/* "rwxr-x--- (750)". */
static void format_mode(mode_t mode, char *out, size_t length)
{
    char bits[10];
    const char *letters = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        bits[i] = mode & (0400 >> i) ? letters[i] : '-';
    if (mode & S_ISUID)
        bits[2] = bits[2] == 'x' ? 's' : 'S';
    if (mode & S_ISGID)
        bits[5] = bits[5] == 'x' ? 's' : 'S';
    if (mode & S_ISVTX)
        bits[8] = bits[8] == 'x' ? 't' : 'T';
    snprintf(out, length, "%.9s (%o)", bits, (unsigned)(mode & 07777));
}

#define PROPERTY_ROWS 12

/* The dialog's rows, label and value, for what it was opened on. */
static size_t property_rows(struct files *files, char labels[][24], char values[][PATH_MAX])
{
    const struct dialog *dialog = &files->dialog;
    const struct counter *counter = files->counter;
    size_t rows = 0;
#define ROW(label, ...)                                                                                                \
    do {                                                                                                               \
        if (rows < PROPERTY_ROWS) {                                                                                    \
            snprintf(labels[rows], 24, "%s", label);                                                                   \
            snprintf(values[rows], PATH_MAX, __VA_ARGS__);                                                             \
            ++rows;                                                                                                    \
        }                                                                                                              \
    } while (0)
    char text[PATH_MAX], more[160];
    bool counting = counter && !atomic_load(&counter->done);
    long long bytes = counter ? atomic_load(&counter->bytes) : 0;
    long nfiles = counter ? atomic_load(&counter->files) : 0, nfolders = counter ? atomic_load(&counter->folders) : 0;
    snprintf(more, sizeof more, "%ld %s, %ld %s%s", nfiles, nfiles == 1 ? "file" : "files", nfolders,
             nfolders == 1 ? "folder" : "folders", counting ? " — counting…" : "");

    if (dialog->count != 1) {
        size_t folders = 0;
        bool same_place = true;
        char *first = dialog->count ? parent_path(dialog->paths[0]) : NULL;
        for (size_t i = 0; i < dialog->count; ++i) {
            struct stat info;
            folders += lstat(dialog->paths[i], &info) == 0 && S_ISDIR(info.st_mode);
            char *parent = parent_path(dialog->paths[i]);
            same_place = same_place && parent && first && strcmp(parent, first) == 0;
            free(parent);
        }
        ROW("Selected", "%zu items: %zu %s, %zu %s", dialog->count, folders, folders == 1 ? "folder" : "folders",
            dialog->count - folders, dialog->count - folders == 1 ? "other" : "others");
        if (same_place && first) {
            display_path(files, first, text, sizeof text);
            ROW("Location", "%s", text);
        }
        free(first);
        format_bytes(bytes, text, sizeof text);
        ROW("Size", "%s", text);
        ROW("Contents", "%s", more);
        return rows;
    }

    const char *path = dialog->paths[0];
    char *parent = parent_path(path);
    int fd = parent ? open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    struct entry entry = {.name = (char *)base_name(path), .total = TOTAL_NONE};
    struct stat info;
    struct statx extra;
    bool have = fd >= 0 && fstatat(fd, entry.name, &info, AT_SYMLINK_NOFOLLOW) == 0;
    bool born = fd >= 0 && statx(fd, entry.name, AT_SYMLINK_NOFOLLOW, STATX_BTIME, &extra) == 0 &&
                (extra.stx_mask & STATX_BTIME);
    if (have)
        entry_stat(fd, &entry, DT_UNKNOWN);
    if (!have) {
        ROW("Error", "%s", strerror(errno));
    } else {
        char type[64];
        entry_type(&entry, type, sizeof type);
        ROW("Type", "%s%s", type, entry.link && !entry.directory && !entry.broken ? " (a link)" : "");
        if (parent && strcmp(path, "/") != 0) {
            display_path(files, parent, text, sizeof text);
            ROW("Location", "%s", text);
        }
        if (entry.link) {
            ssize_t length = readlinkat(fd, entry.name, text, sizeof text - 1);
            text[length > 0 ? length : 0] = 0;
            ROW("Link to", "%.4000s%s", text, entry.broken ? " (missing)" : "");
        }
        if (entry.directory && !entry.link) {
            format_bytes(bytes, text, sizeof text);
            ROW("Size", "%s", text);
            ROW("Contents", "%s", more);
        } else if (!entry.broken) {
            format_bytes((long long)entry.size, text, sizeof text);
            ROW("Size", "%s", text);
        }
        format_full_date(&info.st_mtim, text, sizeof text);
        ROW("Modified", "%s", text);
        format_full_date(&info.st_atim, text, sizeof text);
        ROW("Accessed", "%s", text);
        if (born) {
            struct timespec birth = {.tv_sec = extra.stx_btime.tv_sec, .tv_nsec = extra.stx_btime.tv_nsec};
            format_full_date(&birth, text, sizeof text);
            ROW("Created", "%s", text);
        }
        struct passwd *user = getpwuid(info.st_uid);
        struct group *group = getgrgid(info.st_gid);
        char owner[64], group_name[64];
        if (user)
            snprintf(owner, sizeof owner, "%s", user->pw_name);
        else
            snprintf(owner, sizeof owner, "%ld", (long)info.st_uid);
        if (group)
            snprintf(group_name, sizeof group_name, "%s", group->gr_name);
        else
            snprintf(group_name, sizeof group_name, "%ld", (long)info.st_gid);
        ROW("Owner", "%s, group %s", owner, group_name);
        format_mode(entry.link ? entry.target_mode : info.st_mode, text, sizeof text);
        ROW("Permissions", "%.100s%s", text,
            access(path, W_OK) == 0 ? "" : may_modify(files, parent ? parent : path) ? " — read-only for you" : "");
    }
#undef ROW
    if (fd >= 0)
        close(fd);
    free(parent);
    return rows;
}

/* Alt+Enter: properties of the selection, or of path when given. */
static void show_properties(struct files *files, const char *path)
{
    size_t count = 0;
    char **paths = NULL;
    if (path) {
        paths = calloc(1, sizeof *paths);
        if (paths && (paths[0] = strdup(path)))
            count = 1;
    } else {
        paths = selected_paths(files, &count);
    }
    if (!count) {
        free(paths);
        return;
    }
    char title[300];
    if (count == 1)
        snprintf(title, sizeof title, "%s", base_name(paths[0])[0] ? base_name(paths[0]) : "/");
    else
        snprintf(title, sizeof title, "%zu items", count);
    struct stat info;
    bool counts = count > 1 || (lstat(paths[0], &info) == 0 && S_ISDIR(info.st_mode));
    static const char *const labels[] = {"Close"};
    open_dialog(files, DIALOG_PROPERTIES, paths, count, title, "", labels, 1, -1, 0);
    if (counts)
        counter_start(files, paths, count);
}

/* What a job did, once it's done: a line about it, and undo. */
static void job_finished(struct files *files, struct job *job)
{
    static const char *const verbs[] = {"copy", "move", "delete"};
    static const char *const done[] = {"Copied", "Moved", "Deleted"};
    if (job->failed) {
        const char *why = job->error == ELOOP ? "a folder can't go inside itself"
                        : job->error == EXDEV ? "another drive is mounted in it, and was left alone"
                        : job->error == ENOTSUP ? "it's a special file"
                                                : strerror(job->error);
        if (job->failed > 1)
            notify(files, "Couldn't %s %s and %zu more: %s", verbs[job->kind], job->error_name ? job->error_name : "?",
                   job->failed - 1, why);
        else
            notify(files, "Couldn't %s %s: %s", verbs[job->kind], job->error_name ? job->error_name : "?", why);
    } else if (atomic_load(&job->cancel)) {
        notify(files, "Cancelled; what was done stays");
    } else if (job->made_count == 1) {
        notify(files, "%s %s%s", done[job->kind], base_name(job->made[0]),
               job->kind == JOB_DELETE ? "" : ". Ctrl+Z undoes it");
    } else if (job->made_count) {
        notify(files, "%s %zu items%s", done[job->kind], job->made_count,
               job->kind == JOB_DELETE ? "" : ". Ctrl+Z undoes it");
    } else if (job->merged) {
        notify(files, job->merged == 1 ? "Merged into the folder that was there" : "Merged into the folders that were there");
    } else if (job->skipped) {
        notify(files, "Nothing to do: all skipped");
    }
    bool recorded = false;
    if (job->kind == JOB_DELETE) {
        undo_forget(files);
    } else if (job->made_count && !atomic_load(&job->cancel)) {
        undo_set(files, job->kind == JOB_COPY ? UNDO_COPY : UNDO_MOVE, job->made_from, job->made, job->made_count);
        job->made_from = job->made = NULL;
        job->made_count = 0;
        recorded = true;
    } else if (job->merged) {
        undo_forget(files); /* A merge can't be taken apart. */
    }
    /* Show what came, selected, when it came here. */
    if (job->kind != JOB_DELETE && job->destination && strcmp(job->destination, files->path) == 0 &&
        recorded) {
        load(files, NULL, files->cursor, true);
        clear_selection(files);
        for (size_t i = 0; i < files->undo.count; ++i) {
            size_t index;
            if (find_shown(&files->listing, base_name(files->undo.to[i]), &index)) {
                shown_entry(files, index)->selected = true;
                files->cursor = files->anchor = index;
            }
        }
        files->free_scroll = false;
        desk_window_redraw(&files->window);
    } else {
        refresh(files);
    }
}

/* The worker said something: progress, a question, or a job done. */
static void jobs_event(struct files *files)
{
    struct worker *worker = files->worker;
    uint64_t count;
    (void)!read(worker->event, &count, sizeof count);
    pthread_mutex_lock(&worker->lock);
    struct job **finished = worker->finished;
    size_t finished_count = worker->finished_count;
    worker->finished = NULL;
    worker->finished_count = 0;
    char *target = NULL;
    bool directories = false, several = false;
    if (worker->queue_count && worker->queue[0]->asking && worker->queue[0]->answer == ANSWER_NONE &&
        !(files->dialog.open && files->dialog.action == DIALOG_CONFLICT)) {
        struct job *job = worker->queue[0];
        target = job->ask_target ? strdup(job->ask_target) : NULL;
        directories = job->ask_directories;
        several = job->count > 1 || atomic_load(&job->items_total) > 1;
    }
    pthread_mutex_unlock(&worker->lock);
    if (target && !files->dialog.open)
        ask_conflict(files, target, directories, several);
    free(target);
    for (size_t i = 0; i < finished_count; ++i) {
        job_finished(files, finished[i]);
        job_free(finished[i]);
    }
    free(finished);
    desk_window_redraw(&files->window);
}

/* Undo */

/* Ctrl+Z: undo the last rename, new folder or file, move to the trash,
   copy (the copies go to the trash) or move (back where they were). */
static void undo(struct files *files)
{
    struct undo *undo = &files->undo;
    int error = 0;
    const char *what = NULL;
    switch (undo->kind) {
    case UNDO_NONE:
        notify(files, "Nothing to undo");
        return;
    case UNDO_RENAME:
        error = move_no_replace(undo->to[0], undo->from[0]);
        what = "Rename undone";
        break;
    case UNDO_CREATE: {
        struct stat info;
        if (lstat(undo->to[0], &info) != 0)
            error = errno;
        else if (S_ISDIR(info.st_mode) ? rmdir(undo->to[0]) != 0 : info.st_size ? (errno = ENOTEMPTY, 1)
                                                                              : unlink(undo->to[0]) != 0)
            error = errno;
        what = !error && S_ISDIR(info.st_mode) ? "New folder removed" : "New file removed";
        if (error == ENOTEMPTY || error == EEXIST) {
            notify(files, "%s has something in it now, so it stays", base_name(undo->to[0]));
            undo_forget(files);
            return;
        }
        break;
    }
    case UNDO_TRASH:
        for (size_t i = 0; i < undo->count; ++i) {
            int failed = move_no_replace(undo->to[i], undo->from[i]);
            if (failed) {
                error = error ? error : failed;
                continue;
            }
            char *info = trash_info_path(files, base_name(undo->to[i]));
            if (info)
                unlink(info);
            free(info);
        }
        what = "Put back from the trash";
        break;
    case UNDO_COPY: {
        size_t moved = 0;
        for (size_t i = 0; i < undo->count; ++i) {
            char *trashed;
            int failed = trash_one(files, undo->to[i], &trashed);
            free(trashed);
            if (failed)
                error = error ? error : failed;
            else
                ++moved;
        }
        what = moved == 1 ? "The copy went to the trash" : "The copies went to the trash";
        break;
    }
    case UNDO_MOVE: {
        /* Back where each came from: a rename when it can be, else a move job. */
        char **again = NULL;
        size_t again_count = 0;
        char *again_to = NULL;
        for (size_t i = 0; i < undo->count; ++i) {
            int failed = move_no_replace(undo->to[i], undo->from[i]);
            if (failed == EXDEV && strcmp(base_name(undo->to[i]), base_name(undo->from[i])) == 0) {
                char *parent = parent_path(undo->from[i]);
                if (!again_to || (parent && strcmp(parent, again_to) == 0)) {
                    char **grown = realloc(again, (again_count + 1) * sizeof *grown);
                    if (grown && (grown[again_count] = strdup(undo->to[i]))) {
                        again = grown;
                        ++again_count;
                        if (!again_to)
                            again_to = parent, parent = NULL;
                    }
                }
                free(parent);
            } else if (failed) {
                error = error ? error : failed;
            }
        }
        if (again_count) {
            char *to = again_to;
            undo->kind = UNDO_NONE; /* The job sets its own. */
            start_job(files, JOB_MOVE, again, again_count, to);
            free(to);
        } else {
            free(again);
        }
        what = "Moved back";
        break;
    }
    }
    char *select = undo->kind == UNDO_RENAME || undo->kind == UNDO_TRASH || undo->kind == UNDO_MOVE
                       ? strdup(base_name(undo->from[0]))
                       : NULL;
    if (error)
        notify(files, "Couldn't undo all of it: %s", error == EEXIST ? "something has taken its name" : strerror(error));
    else
        notify(files, "%s", what);
    undo_forget(files);
    refresh_selecting(files, select);
    free(select);
}

/* Keys while the dialog is open: nothing else gets them. */
static void dialog_key(struct files *files, xkb_keysym_t sym)
{
    struct dialog *dialog = &files->dialog;
    switch (sym) {
    case XKB_KEY_Escape:
        if (dialog->action == DIALOG_CONFLICT) {
            close_dialog(files);
            answer_job(files, ANSWER_CANCEL, false);
        } else {
            close_dialog(files);
        }
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        dialog_choose(files, dialog->focus);
        break;
    case XKB_KEY_space:
        if (dialog->has_check) {
            dialog->checked = !dialog->checked;
            desk_window_redraw(&files->window);
        } else {
            dialog_choose(files, dialog->focus);
        }
        break;
    case XKB_KEY_Tab:
    case XKB_KEY_Right:
        dialog->focus = (dialog->focus + 1) % dialog->button_count;
        desk_window_redraw(&files->window);
        break;
    case XKB_KEY_ISO_Left_Tab:
    case XKB_KEY_Left:
        dialog->focus = (dialog->focus + dialog->button_count - 1) % dialog->button_count;
        desk_window_redraw(&files->window);
        break;
    default:
        break;
    }
}

static void copy_files(struct files *files, bool cut);
static void paste_files(struct files *files, const char *destination);
static bool clipboard_has_files(const struct files *files);

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
        case XKB_KEY_f: stop_path_edit(files); search_focus(files, true); return false;
        case XKB_KEY_l: start_path_edit(files); return false;
        case XKB_KEY_1: set_view(files, VIEW_LIST); return false;
        case XKB_KEY_2: set_view(files, VIEW_ICONS); return false;
        case XKB_KEY_plus:
        case XKB_KEY_equal:
        case XKB_KEY_KP_Add: zoom(files, 1); return true;
        case XKB_KEY_minus:
        case XKB_KEY_KP_Subtract: zoom(files, -1); return true;
        case XKB_KEY_0:
        case XKB_KEY_KP_0: zoom(files, 0); return false;
        case XKB_KEY_z: undo(files); return false;
        case XKB_KEY_c: copy_files(files, false); return false;
        case XKB_KEY_x: copy_files(files, true); return false;
        case XKB_KEY_v: paste_files(files, files->path); return false;
        case XKB_KEY_N: create_item(files, true); return false;
        case XKB_KEY_n:
            if (mods & DESK_MOD_SHIFT)
                create_item(files, true);
            return false;
        case XKB_KEY_a:
        case XKB_KEY_A:
            for (size_t i = 0; i < files->listing.shown_count; ++i)
                shown_entry(files, i)->selected = true;
            desk_window_redraw(&files->window);
            return false;
        case XKB_KEY_space:
            if (cursor_entry(files)) {
                cursor_entry(files)->selected = !cursor_entry(files)->selected;
                files->anchor = files->cursor;
                desk_window_redraw(&files->window);
            }
            return false;
        default: break;
        }
        /* Ctrl with the moving keys moves the cursor without selecting. */
        switch (sym) {
        case XKB_KEY_Down: move_cursor(files, line, mods); return true;
        case XKB_KEY_Up: move_cursor(files, -line, mods); return true;
        case XKB_KEY_Right: if (icons) move_cursor(files, 1, mods); return icons;
        case XKB_KEY_Left: if (icons) move_cursor(files, -1, mods); return icons;
        case XKB_KEY_Home: move_cursor(files, -(long)files->listing.shown_count, mods); return false;
        case XKB_KEY_End: move_cursor(files, (long)files->listing.shown_count, mods); return false;
        case XKB_KEY_Page_Down: move_cursor(files, page(files), mods); return true;
        case XKB_KEY_Page_Up: move_cursor(files, -page(files), mods); return true;
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
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (selected_count(files))
                show_properties(files, NULL);
            else
                show_properties(files, files->path);
            return false;
        default: return false;
        }
    }
    if (mods & DESK_MOD_LOGO)
        return false;
    switch (sym) {
    case XKB_KEY_Down:
        move_cursor(files, line, mods);
        return true;
    case XKB_KEY_Up:
        move_cursor(files, -line, mods);
        return true;
    case XKB_KEY_Right:
        if (icons)
            move_cursor(files, 1, mods);
        return icons;
    case XKB_KEY_Left:
        if (icons)
            move_cursor(files, -1, mods);
        return icons;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        go_in(files);
        return false;
    case XKB_KEY_BackSpace:
        go_up(files);
        return true;
    case XKB_KEY_Home:
        move_cursor(files, -(long)files->listing.shown_count, mods);
        return false;
    case XKB_KEY_End:
        move_cursor(files, (long)files->listing.shown_count, mods);
        return false;
    case XKB_KEY_Page_Down:
        move_cursor(files, page(files), mods);
        return true;
    case XKB_KEY_Page_Up:
        move_cursor(files, -page(files), mods);
        return true;
    case XKB_KEY_F5:
        refresh(files);
        return false;
    case XKB_KEY_F2:
        if (selected_count(files) <= 1 && cursor_entry(files))
            start_rename(files, files->cursor, false);
        return false;
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        if ((mods & DESK_MOD_SHIFT) || in_trash(files))
            ask_delete(files);
        else
            trash_selection(files);
        return false;
    case XKB_KEY_F9:
        toggle_sidebar(files);
        return false;
    case XKB_KEY_Escape:
        /* First the search, then the selection (the cursor stays). */
        if (field_text(&files->search)[0]) {
            search_clear(files);
        } else {
            clear_selection(files);
            desk_window_redraw(&files->window);
        }
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
            select_only(files, index);
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
    if (files->dialog.open) {
        dialog_key(files, sym);
        return false;
    }
    if (files->menus[0].open) {
        menu_key(files, sym);
        return false;
    }
    if (files->renaming) {
        int repeats = rename_key(files, key, sym, mods);
        if (repeats >= 0)
            return repeats;
        if (!(mods & (DESK_MOD_CTRL | DESK_MOD_ALT | DESK_MOD_LOGO)))
            return false; /* Typing goes to the name, not type to select. */
    }
    if (sym == XKB_KEY_Menu || (sym == XKB_KEY_F10 && (mods & DESK_MOD_SHIFT))) {
        context_key(files);
        return false;
    }
    if (files->path_editing) {
        int repeats = path_key(files, key, sym, mods);
        if (repeats >= 0)
            return repeats;
    } else if (files->search_focus) {
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
static void outline(DeskCanvas *canvas, int x, int y, int width, int height, int line, uint32_t color);

/* How an entry is marked: selected, under the pointer, and whether the
   cursor's outline shows (when it isn't simply the one selected). */
struct marks {
    bool selected, hovered, focused;
    bool drop;              /* Something dragged over would go into it. */
    bool renaming;          /* Its name is being edited. */
};

static struct marks entry_marks(struct files *files, size_t index, size_t selected)
{
    const struct entry *entry = shown_entry(files, index);
    return (struct marks){
        .selected = entry->selected,
        .hovered = hovering(files, TARGET_ROW, index) && !files->banding,
        .focused = index == files->cursor && (!entry->selected || selected > 1),
        .renaming = files->renaming && files->rename_name && strcmp(entry->name, files->rename_name) == 0,
        .drop = files->drop_into && files->drop_target.kind == TARGET_ROW && files->drop_target.index == index,
    };
}

/* Behind an entry: the selection, or a lighter shade under the pointer, and
   the cursor's outline. */
static void draw_marks(struct files *files, DeskCanvas *canvas, struct marks marks, int x, int y, int w, int h)
{
    const struct colors *colors = &files->colors;
    if (marks.selected)
        desk_canvas_fill(canvas, x, y, w, h,
                         marks.hovered ? mix(colors->selection_background, colors->foreground, 18)
                                       : colors->selection_background);
    else if (marks.hovered)
        desk_canvas_fill(canvas, x, y, w, h, mix(colors->background, colors->muted, 45));
    int line = desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1;
    if (marks.drop) {
        desk_canvas_fill(canvas, x, y, w, h, mix(colors->background, colors->directory, 60));
        outline(canvas, x, y, w, h, 2 * line, colors->directory);
    } else if (marks.focused) {
        outline(canvas, x, y, w, h, line, mix(colors->background, colors->directory, 150));
    }
}

static void draw_field(struct files *files, DeskCanvas *canvas, const struct layout *layout, struct field *field,
                       int left, int right, int top, bool focused, const char *placeholder);

/* The field a name is edited in, between left and right on the line at
   top. */
static void paint_rename(struct files *files, DeskCanvas *canvas, const struct layout *layout, int left, int right,
                         int top)
{
    int line = desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1;
    int inner = layout->padding / 4;
    desk_canvas_fill(canvas, left, top, right - left, layout->row_height, files->colors.background);
    outline(canvas, left, top, right - left, layout->row_height, line, files->colors.directory);
    draw_field(files, canvas, layout, &files->rename_field, left + inner + line, right - inner - line, top, true, NULL);
    files->rename_top = top;
    files->rename_bottom = top + layout->row_height;
}

static void draw_row(struct files *files, DeskCanvas *canvas, const struct layout *layout, struct entry *entry,
                     int x0, int x1, int y, struct marks marks)
{
    const struct colors *colors = &files->colors;
    const struct panes *panes = &files->panes;
    bool selected = marks.selected;
    draw_marks(files, canvas, marks, x0, y, x1 - x0, layout->row_height);
    uint32_t color = entry_color(files, entry, selected);
    int baseline = y + layout->baseline, inner = layout->padding / 2;
    draw_icon(layout->font, canvas, x0 + inner / 2, x0 + inner / 2 + layout->icon_width, baseline,
              entry_icon(entry), color);

    int name_x = x0 + inner + layout->icon_width, name_right = panes->column_right[COLUMN_NAME];
    if (marks.renaming && name_right > name_x) {
        paint_rename(files, canvas, layout, name_x - inner / 2, name_right, y);
    } else if (name_right > name_x) {
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
    if (field_text(&files->search)[0])
        return "No matches";
    return listing->count ? "Only hidden files (. shows them)" : "Empty";
}

/* The list view: rows from line top. */
static void paint_list(struct files *files, DeskCanvas *canvas, const struct layout *layout, int x0, int x1,
                       int y, int rows)
{
    struct listing *listing = &files->listing;
    size_t selected = selected_count(files);
    for (int row = 0; row < rows && files->top + (size_t)row < listing->shown_count; ++row) {
        size_t index = files->top + (size_t)row;
        draw_row(files, canvas, layout, &listing->entries[listing->shown[index]], x0, x1,
                 y + row * layout->row_height, entry_marks(files, index, selected));
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

/* Between an icon and its name: more as icons grow, since file glyphs
   fill their whole square. */
static int icon_gap(int icon, int pad)
{
    return icon / 12 > pad ? icon / 12 : pad;
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
    size_t per_line = (size_t)panes->columns, selected_total = selected_count(files);
    for (int row = 0; row < rows; ++row) {
        for (size_t column = 0; column < per_line; ++column) {
            size_t index = (files->top + (size_t)row) * per_line + column;
            if (index >= listing->shown_count)
                return;
            struct entry *entry = &listing->entries[listing->shown[index]];
            struct marks marks = entry_marks(files, index, selected_total);
            bool selected = marks.selected;
            int left = x0 + (int)column * tile, top = y + row * panes->line_height;
            uint32_t color = entry_color(files, entry, selected);
            draw_marks(files, canvas, marks, left + pad / 2, top, tile - pad, panes->line_height - pad / 2);
            /* An image's thumbnail, centered in the icon's square; its icon
               until the thumbnail comes, or when there can't be one. */
            struct thumb *thumb = NULL;
            if (thumbnailable(entry)) {
                char *path = join_path(files->path, entry->name);
                thumb = path ? thumb_for(files, path, entry, icon) : NULL;
                free(path);
            }
            if (thumb && thumb->pixels) {
                desk_canvas_blit(canvas, left + (tile - thumb->width) / 2, top + pad + (icon - thumb->height) / 2,
                                 thumb->pixels, thumb->width, thumb->height);
            } else {
                /* Centered by its ink: icon fonts sit glyphs differently on
                   the baseline than the text font's metrics suggest. */
                int above, below, baseline = icon_baseline;
                desk_char_ink(files->icon_font, entry_icon(entry), &above, &below);
                if (above + below > 0)
                    baseline = pad + (icon - (above + below)) / 2 + above;
                draw_icon(files->icon_font, canvas, left, left + tile, top + baseline, entry_icon(entry),
                          selected ? color : entry->broken ? color : entry->hidden ? files->colors.muted
                                                                                   : files->colors.directory);
            }
            DeskFont *font = entry->link ? ui->italic : ui->font;
            int text_width = tile - 2 * pad, baseline = top + pad + icon + icon_gap(icon, pad) + ui->baseline -
                                                        (ui->row_height - line) / 2;
            size_t length = strlen(entry->name), cut = tile_break(font, entry->name, length, text_width);
            DeskCanvas clipped = *canvas;
            desk_canvas_clip(&clipped, left + pad > canvas->clip_left ? left + pad : canvas->clip_left,
                             canvas->clip_top,
                             left + tile - pad < canvas->clip_right ? left + tile - pad : canvas->clip_right,
                             canvas->clip_bottom);
            if (marks.renaming) {
                paint_rename(files, canvas, ui, left + pad / 2, left + tile - pad / 2, baseline - ui->baseline);
                continue;
            }
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

/* A field's text between left and right: the selection behind it and the
   cursor when focused, scrolled to keep the cursor in view; placeholder in
   muted text when it's empty and not focused. */
static void draw_field(struct files *files, DeskCanvas *canvas, const struct layout *layout, struct field *field,
                       int left, int right, int top, bool focused, const char *placeholder)
{
    const struct colors *colors = &files->colors;
    int height = layout->row_height, baseline = top + layout->baseline, space = layout->padding / 2;
    int line = desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1;
    const char *text = field_text(field);
    field->left = left;
    field->right = right;
    DeskCanvas clipped = *canvas;
    desk_canvas_clip(&clipped, left, top, right, top + height);
    if (!text[0] && !focused) {
        field->scroll = 0;
        field->text_x = left;
        if (placeholder)
            desk_text_draw(layout->font, &clipped, left, baseline, placeholder, strlen(placeholder), colors->muted);
        return;
    }
    int room = right - left - line;
    int cursor_x = desk_text_width(layout->font, text, field->text.cursor);
    if (cursor_x - field->scroll > room)
        field->scroll = cursor_x - room;
    if (cursor_x < field->scroll)
        field->scroll = cursor_x;
    int total = desk_text_width(layout->font, text, strlen(text));
    if (field->scroll > total - room)
        field->scroll = total - room; /* No empty space after the end while it fits. */
    if (field->scroll < 0)
        field->scroll = 0;
    field->text_x = left - field->scroll;
    size_t start, end;
    if (focused && desk_text_selection(&field->text, &start, &end)) {
        int from = desk_text_width(layout->font, text, start), to = desk_text_width(layout->font, text, end);
        desk_canvas_fill(&clipped, field->text_x + from, top + space / 2, to - from, height - space,
                         mix(colors->selection_background, colors->directory, 90));
    }
    desk_text_draw(layout->font, &clipped, field->text_x, baseline, text, strlen(text), colors->foreground);
    if (focused)
        desk_canvas_fill(&clipped, field->text_x + cursor_x, top + space / 2, line, height - space,
                         colors->foreground);
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
    if (field_text(&files->search)[0]) {
        bar->search_clear_left = right - height;
        text_right = bar->search_clear_left;
        if (hovering(files, TARGET_SEARCH_CLEAR, 0))
            desk_canvas_fill(canvas, bar->search_clear_left + space / 2, top + space / 2, height - space,
                             height - space, mix(colors->background, colors->muted, 80));
        draw_icon(layout->font, canvas, bar->search_clear_left, right, baseline, ICON_CLOSE, colors->muted);
    }
    draw_field(files, canvas, layout, &files->search, text_left, text_right, top, files->search_focus, "Search");
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

    if (files->path_editing) {
        outline(canvas, bar->path_left, top, bar->path_right - bar->path_left, height,
                desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1,
                colors->directory);
        bar->crumb_count = 0;
        draw_field(files, canvas, layout, &files->path_field, bar->path_left + space, bar->path_right - space, top,
                   true, NULL);
        return;
    }
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
        if (files->drop_into && files->drop_target.kind == TARGET_PLACE && files->drop_target.index == i) {
            desk_canvas_fill(canvas, x0, y, x1 - x0, layout->row_height, mix(colors->background, colors->directory, 60));
            outline(canvas, x0, y, x1 - x0, layout->row_height,
                    2 * (desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1),
                    colors->directory);
        } else if (strcmp(place->path, files->path) == 0)
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

/* Whether a job is queued or running, so the progress bar shows. */
static bool jobs_running(struct files *files)
{
    if (!files->worker)
        return false;
    pthread_mutex_lock(&files->worker->lock);
    bool running = files->worker->queue_count > 0;
    pthread_mutex_unlock(&files->worker->lock);
    return running;
}

/* The progress bar along the window's foot: what's being done, how far,
   and a × that cancels it. */
static void paint_progress(struct files *files, DeskCanvas *canvas, const struct layout *ui, int top)
{
    struct worker *worker = files->worker;
    files->job_top = -1;
    if (!worker)
        return;
    pthread_mutex_lock(&worker->lock);
    if (!worker->queue_count) {
        pthread_mutex_unlock(&worker->lock);
        return;
    }
    struct job *job = worker->queue[0];
    enum job_kind kind = job->kind;
    char current[256];
    snprintf(current, sizeof current, "%s", job->current);
    bool counted = job->counted, asking = job->asking;
    long done = atomic_load(&job->items_done), total = atomic_load(&job->items_total);
    long long bytes = atomic_load(&job->bytes_done), bytes_total = atomic_load(&job->bytes_total);
    size_t waiting = worker->queue_count - 1;
    pthread_mutex_unlock(&worker->lock);

    const struct colors *colors = &files->colors;
    int line = desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1;
    int pad = ui->padding, row = ui->row_height, left = pad, right = canvas->width - pad;
    desk_canvas_fill(canvas, 0, top, canvas->width, canvas->height - top, mix(colors->background, colors->muted, 25));
    desk_canvas_fill(canvas, 0, top, canvas->width, line, mix(colors->background, colors->muted, 70));
    /* The × on the right. */
    files->job_top = top;
    files->job_cancel_left = right - row;
    files->job_cancel_right = right;
    if (hovering(files, TARGET_JOB_CANCEL, 0))
        desk_canvas_fill(canvas, right - row, top + pad / 2, row, row, mix(colors->background, colors->muted, 80));
    draw_icon(ui->font, canvas, right - row, right, top + pad / 2 + ui->baseline, ICON_CLOSE, colors->foreground);
    right -= row + pad;

    static const char *const verbs[] = {"Copying", "Moving", "Deleting"};
    char text[400], amount[80] = "";
    if (asking)
        snprintf(text, sizeof text, "%s: waiting for an answer", verbs[kind]);
    else if (!counted)
        snprintf(text, sizeof text, "%s: counting…", verbs[kind]);
    else
        snprintf(text, sizeof text, "%s %ld of %ld%s%s", verbs[kind], done < 1 ? 1 : done > total ? total : done, total,
                 current[0] ? " — " : "", current);
    if (waiting)
        snprintf(text + strlen(text), sizeof text - strlen(text), " (%zu more waiting)", waiting);
    if (counted && bytes_total > 0) {
        char a[32], b[32];
        format_size((off_t)bytes, a, sizeof a);
        format_size((off_t)bytes_total, b, sizeof b);
        snprintf(amount, sizeof amount, "%s of %s", a, b);
    }
    int amount_width = amount[0] ? desk_text_width(ui->mono, amount, strlen(amount)) + pad : 0;
    draw_clipped(ui->font, canvas, left, right - amount_width, top + pad / 2 + ui->baseline, text, colors->foreground);
    if (amount[0])
        draw_right(ui->mono, canvas, right - amount_width, right, top + pad / 2 + ui->baseline, amount, colors->muted);
    /* The bar: bytes when there are any, else items. */
    double fraction = bytes_total > 0 ? (double)bytes / (double)bytes_total : total > 0 ? (double)done / total : 0;
    if (fraction > 1)
        fraction = 1;
    int bar_top = top + pad / 2 + row, bar_height = 3 * line, width = right - left;
    desk_canvas_fill(canvas, left, bar_top, width, bar_height, mix(colors->background, colors->muted, 60));
    desk_canvas_fill(canvas, left, bar_top, (int)(width * fraction), bar_height, colors->directory);
}

/* Dropping into the folder shown: a frame around the list. */
static void paint_drop_frame(struct files *files, DeskCanvas *canvas, int top, int bottom)
{
    if (!files->drop_into || files->drop_target.kind != TARGET_NONE)
        return;
    int line = desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1;
    outline(canvas, files->panes.list_left, top, files->panes.list_right - files->panes.list_left, bottom - top, 2 * line,
            files->colors.directory);
}

static void paint(struct files *files, DeskCanvas *canvas)
{
    files->rename_top = files->rename_bottom = -1;
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
    int strip = jobs_running(files) ? ui.row_height + ui.padding + 3 * (scale >= 2 ? (int)scale : 1) + ui.padding / 2 : 0;
    bottom -= strip;
    if (strip)
        paint_progress(files, canvas, &ui, canvas->height - strip);
    else
        files->job_top = -1;

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
        panes->line_height = ui.padding / 2 + icon + icon_gap(icon, ui.padding / 2) +
                             2 * desk_font_metrics(ui.font).height + ui.padding;
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
        paint_drop_frame(files, &area, top, bottom);
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
    if (files->banding) {
        /* The rubber band, from content pixels to the view's. */
        int shift = panes->entries_top - (int)files->top * panes->line_height;
        int x0 = files->band_x0 < files->band_x1 ? files->band_x0 : files->band_x1;
        int x1 = files->band_x0 < files->band_x1 ? files->band_x1 : files->band_x0;
        int y0 = (files->band_y0 < files->band_y1 ? files->band_y0 : files->band_y1) + shift;
        int y1 = (files->band_y0 < files->band_y1 ? files->band_y1 : files->band_y0) + shift;
        int line = desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1;
        DeskCanvas band = area;
        desk_canvas_clip(&band, panes->list_left, panes->entries_top, panes->list_right, bottom);
        outline(&band, x0, y0, x1 - x0 + 1, y1 - y0 + 1, line, files->colors.directory);
    }
    paint_drop_frame(files, &area, top, bottom);
    paint_notice(files, &area, &ui, bottom);
}

/* While a path is typed: the directories it could go on to, in a box
   under the path over the list, the chosen one lit. */
static void paint_suggestions(struct files *files, DeskCanvas *canvas)
{
    struct toolbar *bar = &files->toolbar;
    bar->suggest_row = 0;
    if (!files->path_editing || !files->suggestion_count)
        return;
    const struct colors *colors = &files->colors;
    struct layout ui = make_layout(files->font, files->italic, files->mono, desk_window_scale(&files->window));
    size_t shown = files->suggestion_count < SUGGESTIONS_SHOWN ? files->suggestion_count : SUGGESTIONS_SHOWN;
    int line = desk_window_scale(&files->window) >= 2 ? (int)desk_window_scale(&files->window) : 1;
    int left = bar->path_left, right = bar->path_right, top = bar->bottom + line, row = ui.row_height;
    int height = (int)shown * row + (files->suggestion_count > shown ? row : 0);
    bar->suggest_left = left;
    bar->suggest_right = right;
    bar->suggest_top = top;
    bar->suggest_row = row;
    uint32_t background = mix(colors->background, colors->muted, 25);
    desk_canvas_fill(canvas, left, top, right - left, height, background);
    outline(canvas, left, top, right - left, height, line, mix(colors->background, colors->muted, 110));
    for (size_t i = 0; i < shown; ++i) {
        int y = top + (int)i * row;
        bool chosen = (int)i == files->suggestion, lit = chosen || hovering(files, TARGET_SUGGESTION, i);
        if (lit)
            desk_canvas_fill(canvas, left + line, y, right - left - 2 * line, row,
                             chosen ? colors->selection_background : mix(background, colors->muted, 60));
        uint32_t color = chosen ? colors->selection_foreground : colors->foreground;
        draw_icon(ui.font, canvas, left + ui.padding / 2, left + ui.padding / 2 + ui.icon_width, y + ui.baseline,
                  ICON_FOLDER, chosen ? color : colors->directory);
        draw_clipped(ui.font, canvas, left + ui.padding / 2 + ui.icon_width, right - ui.padding, y + ui.baseline,
                     files->suggestions[i], color);
    }
    if (files->suggestion_count > shown) {
        char more[48];
        snprintf(more, sizeof more, "%zu more", files->suggestion_count - shown);
        draw_clipped(ui.italic, canvas, left + ui.padding / 2 + ui.icon_width, right - ui.padding,
                     top + (int)shown * row + ui.baseline, more, colors->muted);
    }
}

static void draw_check(DeskCanvas *canvas, int x, int y, int size, uint32_t color, uint32_t background);

/* The dialog: the window dimmed, a box with the question, a "Do this for
   all" box when it has one, and its buttons on the right, the dangerous
   one red. */
static void paint_dialog(struct files *files, DeskCanvas *canvas)
{
    struct dialog *dialog = &files->dialog;
    if (!dialog->open)
        return;
    const struct colors *colors = &files->colors;
    double scale = desk_window_scale(&files->window);
    struct layout ui = make_layout(files->font, files->italic, files->mono, scale);
    int line = scale >= 2 ? (int)scale : 1, pad = ui.padding, row = ui.row_height;
    desk_canvas_fill(canvas, 0, 0, canvas->width, canvas->height, 0x80000000);
    int width = (int)lround(460 * scale);
    if (width > canvas->width - 2 * pad)
        width = canvas->width - 2 * pad;
    static char labels[PROPERTY_ROWS][24], values[PROPERTY_ROWS][PATH_MAX];
    size_t rows = dialog->action == DIALOG_PROPERTIES ? property_rows(files, labels, values) : 0;
    if (rows)
        width = (int)lround(560 * scale) < canvas->width - 2 * pad ? (int)lround(560 * scale) : canvas->width - 2 * pad;
    int height = pad + 2 * row + pad + (dialog->has_check ? row + pad / 2 : 0) + row + pad;
    if (rows)
        height += (int)rows * row - row + pad / 2;
    int left = (canvas->width - width) / 2, top = (canvas->height - height) / 3;
    if (top + height > canvas->height)
        top = canvas->height - height > 0 ? canvas->height - height : 0;
    uint32_t background = mix(colors->background, colors->muted, 25);
    desk_canvas_fill(canvas, left, top, width, height, background);
    outline(canvas, left, top, width, height, line, mix(colors->background, colors->muted, 110));
    draw_clipped(ui.font, canvas, left + pad, left + width - pad, top + pad + ui.baseline, dialog->title,
                 colors->foreground);
    draw_clipped(ui.font, canvas, left + pad, left + width - pad, top + pad + row + ui.baseline, dialog->message,
                 colors->muted);
    int y = top + pad + 2 * row + pad;
    if (rows) {
        /* Label and value pairs, under the name. */
        y = top + pad + row + pad / 2;
        int label_width = 0;
        for (size_t i = 0; i < rows; ++i) {
            int w = desk_text_width(ui.font, labels[i], strlen(labels[i]));
            label_width = w > label_width ? w : label_width;
        }
        for (size_t i = 0; i < rows; ++i, y += row) {
            draw_clipped(ui.font, canvas, left + pad, left + pad + label_width, y + ui.baseline, labels[i], colors->muted);
            draw_clipped(ui.font, canvas, left + 2 * pad + label_width, left + width - pad, y + ui.baseline, values[i],
                         colors->foreground);
        }
        y += pad;
    }
    dialog->check_top = dialog->check_bottom = -1;
    if (dialog->has_check) {
        int box = row / 2, box_top = y + (row - box) / 2;
        bool lit = hovering(files, TARGET_DIALOG_CHECK, 0);
        desk_canvas_fill(canvas, left + pad, box_top, box, box, mix(colors->background, colors->muted, lit ? 90 : 50));
        if (dialog->checked)
            draw_check(canvas, left + pad, box_top, box, colors->foreground, mix(colors->background, colors->muted, 50));
        const char *label = "Do this for all conflicts";
        draw_clipped(ui.font, canvas, left + pad + box + pad / 2, left + width - pad, y + ui.baseline, label,
                     colors->foreground);
        dialog->check_left = left + pad;
        dialog->check_right = left + pad + box + pad / 2 + desk_text_width(ui.font, label, strlen(label));
        dialog->check_top = y;
        dialog->check_bottom = y + row;
        y += row + pad / 2;
    }
    int right = left + width - pad;
    dialog->button_top = y;
    dialog->button_bottom = y + row;
    for (int i = dialog->button_count - 1; i >= 0; --i) {
        const char *label = dialog->labels[i];
        int text = desk_text_width(ui.font, label, strlen(label));
        int button = text + 2 * pad > (int)lround(90 * scale) ? text + 2 * pad : (int)lround(90 * scale);
        dialog->button_left[i] = right - button;
        dialog->button_right[i] = right;
        bool lit = hovering(files, TARGET_DIALOG_BUTTON, (size_t)i), danger = i == dialog->danger;
        uint32_t fill = danger ? colors->urgent : mix(colors->background, colors->muted, 60);
        if (lit)
            fill = mix(fill, colors->foreground, 40);
        desk_canvas_fill(canvas, right - button, y, button, row, fill);
        if (dialog->focus == i)
            outline(canvas, right - button - 2 * line, y - 2 * line, button + 4 * line, row + 4 * line, line,
                    colors->foreground);
        desk_text_draw(ui.font, canvas, right - button + (button - text) / 2, y + ui.baseline, label, strlen(label),
                       danger ? colors->background : colors->foreground);
        right -= button + pad;
    }
}

static void draw(void *data, DeskWindow *window)
{
    struct files *files = data;
    DeskBuffer *buffer = desk_window_buffer(window, WL_SHM_FORMAT_XRGB8888);
    if (!buffer)
        return; /* Both busy: retried after a release. */
    DeskCanvas canvas = desk_canvas(buffer->pixels, buffer->width, buffer->height);
    paint(files, &canvas);
    paint_suggestions(files, &canvas);
    paint_dialog(files, &canvas);
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
    if (files->hover.kind == TARGET_SEARCH || files->selecting ||
        (files->hover.kind == TARGET_PATH && files->path_editing))
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
    if (files->dialog.open) {
        /* Modal: only its buttons. */
        const struct dialog *dialog = &files->dialog;
        for (int i = 0; i < dialog->button_count; ++i) {
            if (y >= dialog->button_top && y < dialog->button_bottom && x >= dialog->button_left[i] &&
                x < dialog->button_right[i])
                return (struct target){TARGET_DIALOG_BUTTON, (size_t)i};
        }
        if (dialog->has_check && y >= dialog->check_top && y < dialog->check_bottom && x >= dialog->check_left &&
            x < dialog->check_right)
            return (struct target){TARGET_DIALOG_CHECK, 0};
        return (struct target){TARGET_NONE, 0};
    }
    if (files->job_top >= 0 && y >= files->job_top && x >= files->job_cancel_left && x < files->job_cancel_right) {
        return (struct target){TARGET_JOB_CANCEL, 0};
    }
    if (files->renaming && files->rename_bottom >= 0 && y >= files->rename_top && y < files->rename_bottom &&
        x >= files->rename_field.left - files->panes.line_height / 4 && x < files->rename_field.right)
        return (struct target){TARGET_RENAME, 0};
    if (files->path_editing && files->suggestion_count && bar->suggest_row > 0 && x >= bar->suggest_left &&
        x < bar->suggest_right && y >= bar->suggest_top) {
        size_t row = (size_t)((y - bar->suggest_top) / bar->suggest_row);
        if (row < files->suggestion_count && row < SUGGESTIONS_SHOWN)
            return (struct target){TARGET_SUGGESTION, row};
    }
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
        if (x >= bar->path_left && x < bar->path_right)
            return (struct target){TARGET_PATH, 0}; /* Not on a crumb, or typing a path. */
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

/* Copied files as other file managers read them: GNOME's list ("copy" or
   "cut", then a URI a line) or a plain URI list. */
static char *uri_text(char *const *paths, size_t count, bool cut, bool uri_list)
{
    size_t length = 8;
    char **uris = calloc(count ? count : 1, sizeof *uris);
    for (size_t i = 0; uris && i < count; ++i) {
        uris[i] = path_uri(paths[i]);
        length += (uris[i] ? strlen(uris[i]) : 0) + 2;
    }
    char *text = uris ? malloc(length) : NULL;
    if (text) {
        size_t used = 0;
        if (!uri_list)
            used = (size_t)sprintf(text, "%s", cut ? "cut" : "copy");
        for (size_t i = 0; i < count; ++i) {
            if (!uris[i])
                continue;
            used += (size_t)sprintf(text + used, uri_list ? "%s\r\n" : "\n%s", uris[i]);
        }
        text[used] = 0;
    }
    for (size_t i = 0; uris && i < count; ++i)
        free(uris[i]);
    free(uris);
    return text;
}

static char *clip_uri_text(const struct files *files, bool uri_list)
{
    return uri_text(files->clip_paths, files->clip_count, files->clip_cut, uri_list);
}

static void clip_send(void *data, struct wl_data_source *source, const char *mime, int32_t fd)
{
    (void)source;
    struct files *files = data;
    const char *text = files->clip_text ? files->clip_text : "";
    char *owned = NULL;
    bool gnome = strcmp(mime, "x-special/gnome-copied-files") == 0, uris = strcmp(mime, "text/uri-list") == 0;
    if (files->clip_paths && (gnome || uris)) {
        owned = clip_uri_text(files, uris);
        text = owned ? owned : "";
    }
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
    free(owned);
}

static void clip_forget_files(struct files *files)
{
    free_paths(files->clip_paths, files->clip_count);
    files->clip_paths = NULL;
    files->clip_count = 0;
    files->clip_cut = false;
}

static void clip_cancelled(void *data, struct wl_data_source *source)
{
    struct files *files = data;
    if (files->clip_source == source) {
        files->clip_source = NULL;
        free(files->clip_text);
        files->clip_text = NULL;
        clip_forget_files(files);
        desk_window_redraw(&files->window);
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
    clip_forget_files(files);
}

/* Ctrl+C, Ctrl+X: the selection on the clipboard as files, for files and
   other file managers (and as their paths, for text). */
static void copy_files(struct files *files, bool cut)
{
    size_t count;
    char **paths = selected_paths(files, &count);
    if (!count) {
        free(paths);
        return;
    }
    if (cut && in_trash(files)) {
        notify(files, "Restore takes things out of the trash");
        free_paths(paths, count);
        return;
    }
    if (cut && !files->writable) {
        protected_notice(files, files->path);
        free_paths(paths, count);
        return;
    }
    if (!files->data_device) {
        notify(files, "The compositor has no clipboard to copy to");
        free_paths(paths, count);
        return;
    }
    size_t length = 1;
    for (size_t i = 0; i < count; ++i)
        length += strlen(paths[i]) + 1;
    char *text = malloc(length);
    if (!text) {
        free_paths(paths, count);
        return;
    }
    text[0] = 0;
    for (size_t i = 0; i < count; ++i) {
        strcat(text, paths[i]);
        if (i + 1 < count)
            strcat(text, "\n");
    }
    struct wl_data_source *source = wl_data_device_manager_create_data_source(files->data_manager);
    wl_data_source_add_listener(source, &clip_listener, files);
    static const char *const types[] = {"x-special/gnome-copied-files", "text/uri-list", "text/plain;charset=utf-8",
                                        "text/plain", "UTF8_STRING", "TEXT", "STRING"};
    for (size_t i = 0; i < sizeof types / sizeof *types; ++i)
        wl_data_source_offer(source, types[i]);
    wl_data_device_set_selection(files->data_device, source, files->input_serial);
    if (files->clip_source)
        wl_data_source_destroy(files->clip_source);
    free(files->clip_text);
    clip_forget_files(files);
    files->clip_source = source;
    files->clip_text = text;
    files->clip_paths = paths;
    files->clip_count = count;
    files->clip_cut = cut;
    if (count == 1)
        notify(files, cut ? "Cut %s: Ctrl+V moves it" : "Copied %s: Ctrl+V pastes it", base_name(paths[0]));
    else
        notify(files, cut ? "Cut %zu items: Ctrl+V moves them" : "Copied %zu items: Ctrl+V pastes them", count);
    desk_window_redraw(&files->window);
}

/* Other clients' clipboard: each new offer lists its types before it's
   known whether it's the clipboard or a drag; the best text type is kept. */
static int text_type_rank(const char *mime)
{
    static const char *const types[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"};
    for (size_t i = 0; i < sizeof types / sizeof *types; ++i) {
        if (strcmp(mime, types[i]) == 0)
            return (int)i;
    }
    return -1;
}

static void offer_offer(void *data, struct wl_data_offer *offer, const char *mime)
{
    struct files *files = data;
    if (offer != files->pending_offer)
        return;
    /* Files: GNOME's list (which says copy or cut) before a plain URI list. */
    if (strcmp(mime, "x-special/gnome-copied-files") == 0 ||
        (strcmp(mime, "text/uri-list") == 0 && !files->pending_files_type)) {
        free(files->pending_files_type);
        files->pending_files_type = strdup(mime);
    }
    int rank = text_type_rank(mime);
    if (rank < 0 || (files->pending_type && text_type_rank(files->pending_type) <= rank))
        return;
    free(files->pending_type);
    files->pending_type = strdup(mime);
}

static void offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t actions)
{
    (void)data;
    (void)offer;
    (void)actions;
}

static void offer_action(void *data, struct wl_data_offer *offer, uint32_t action)
{
    struct files *files = data;
    if (offer == files->drop_offer)
        files->drop_action = action;
}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_offer,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

static void device_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
    (void)device;
    struct files *files = data;
    free(files->pending_type);
    files->pending_type = NULL;
    free(files->pending_files_type);
    files->pending_files_type = NULL;
    files->pending_offer = offer;
    wl_data_offer_add_listener(offer, &offer_listener, files);
}

/* The clipboard's text, or NULL when it has none. Waits for the other
   client, at most a second. */
static char *receive_offer(struct files *files, struct wl_data_offer *offer, const char *mime);
static char **uri_list_paths(char *text, bool gnome, bool *cut, size_t *count);

static char *paste_text(struct files *files)
{
    if (files->clip_source && files->clip_text)
        return strdup(files->clip_text); /* Ours. */
    if (!files->selection_offer || !files->offer_type)
        return NULL;
    return receive_offer(files, files->selection_offer, files->offer_type);
}

/* Whether the clipboard has files to paste. */
static bool clipboard_has_files(const struct files *files)
{
    return (files->clip_source && files->clip_paths) || (files->selection_offer && files->offer_files_type);
}

/* Ctrl+V: the clipboard's files into destination: moved when they were
   cut, else copied. */
static void paste_files(struct files *files, const char *destination)
{
    char **paths = NULL;
    size_t count = 0;
    bool cut = false;
    if (files->trash_files && strcmp(destination, files->trash_files) == 0) {
        notify(files, "Delete moves things to the trash; pasting doesn't");
        return;
    }
    if (files->clip_source && files->clip_paths) {
        paths = calloc(files->clip_count, sizeof *paths);
        for (size_t i = 0; paths && i < files->clip_count; ++i) {
            if ((paths[count] = strdup(files->clip_paths[i])))
                ++count;
        }
        cut = files->clip_cut;
    } else if (files->selection_offer && files->offer_files_type) {
        bool gnome = strcmp(files->offer_files_type, "x-special/gnome-copied-files") == 0;
        char *text = receive_offer(files, files->selection_offer, files->offer_files_type);
        paths = uri_list_paths(text, gnome, &cut, &count);
        free(text);
    }
    if (!count) {
        free(paths);
        notify(files, files->offer_type || files->clip_text ? "The clipboard has no files, only text" : "Nothing to paste");
        return;
    }
    start_job(files, cut ? JOB_MOVE : JOB_COPY, paths, count, destination);
    /* Cut things are moved once: the clipboard lets go of them. */
    if (cut && files->clip_source && files->clip_paths && files->data_device) {
        wl_data_device_set_selection(files->data_device, NULL, files->input_serial);
        wl_data_source_destroy(files->clip_source);
        files->clip_source = NULL;
        free(files->clip_text);
        files->clip_text = NULL;
        clip_forget_files(files);
    }
}

/* The local paths in a URI list (GNOME's starts with "copy" or "cut",
   which sets *cut). Takes text apart. */
static char **uri_list_paths(char *text, bool gnome, bool *cut, size_t *count)
{
    char **paths = NULL;
    size_t capacity = 0;
    char *save = NULL;
    bool first = true;
    *count = 0;
    *cut = false;
    for (char *line = text ? strtok_r(text, "\r\n", &save) : NULL; line; line = strtok_r(NULL, "\r\n", &save)) {
        if (gnome && first) {
            first = false;
            *cut = strcmp(line, "cut") == 0;
            if (*cut || strcmp(line, "copy") == 0)
                continue;
        }
        first = false;
        if (line[0] == '#')
            continue;
        char *path = uri_path(line, strlen(line));
        if (!path)
            continue;
        if (*count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            char **grown = realloc(paths, capacity * sizeof *grown);
            if (!grown) {
                free(path);
                break;
            }
            paths = grown;
        }
        paths[(*count)++] = path;
    }
    return paths;
}

/* An offer's data as mime, read through a pipe; waits for the other
   client, at most a second. */
static char *receive_offer(struct files *files, struct wl_data_offer *offer, const char *mime)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return NULL;
    wl_data_offer_receive(offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(files->display);
    char *text = NULL;
    size_t length = 0, capacity = 0;
    for (;;) {
        struct pollfd wait = {fds[0], POLLIN, 0};
        if (poll(&wait, 1, 1000) <= 0)
            break;
        if (length + 4096 + 1 > capacity) {
            size_t grown = capacity ? capacity * 2 : 8192;
            char *bigger = grown <= (1u << 20) ? realloc(text, grown) : NULL;
            if (!bigger)
                break;
            text = bigger;
            capacity = grown;
        }
        ssize_t got = read(fds[0], text + length, 4096);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        length += (size_t)got;
    }
    close(fds[0]);
    if (text)
        text[length] = 0;
    return text;
}

/* Drag and drop

   Dragging a selected name carries the selection out as files (the same
   types as Copy), to another folder or place here, or to another
   application. Files dropped on the window go into the folder under the
   pointer, the place in the sidebar, or the folder shown: moved when the
   compositor settles on moving (on the same drive by default, or with
   Shift), copied otherwise (another drive, or Ctrl), through the same jobs
   as paste. Dropped on the Trash they're trashed. Nothing goes where
   files may not change things. */

static void drag_source_target(void *data, struct wl_data_source *source, const char *mime)
{
    (void)data;
    (void)source;
    (void)mime;
}

static void drag_source_send(void *data, struct wl_data_source *source, const char *mime, int32_t fd)
{
    (void)source;
    struct files *files = data;
    char *text = NULL;
    if (strcmp(mime, "x-special/gnome-copied-files") == 0 || strcmp(mime, "text/uri-list") == 0) {
        text = uri_text(files->drag_paths, files->drag_count, false, strcmp(mime, "text/uri-list") == 0);
    } else {
        size_t length = 1;
        for (size_t i = 0; i < files->drag_count; ++i)
            length += strlen(files->drag_paths[i]) + 1;
        if ((text = malloc(length))) {
            text[0] = 0;
            for (size_t i = 0; i < files->drag_count; ++i) {
                strcat(text, files->drag_paths[i]);
                if (i + 1 < files->drag_count)
                    strcat(text, "\n");
            }
        }
    }
    for (size_t left = text ? strlen(text) : 0, done = 0; left;) {
        ssize_t written = write(fd, text + done, left);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        done += (size_t)written;
        left -= (size_t)written;
    }
    close(fd);
    free(text);
}

static void drag_icon_free(struct files *files)
{
    if (files->drag_viewport)
        wp_viewport_destroy(files->drag_viewport);
    files->drag_viewport = NULL;
    if (files->drag_icon)
        wl_surface_destroy(files->drag_icon);
    files->drag_icon = NULL;
    desk_buffer_pool_finish(&files->drag_buffers);
}

/* The badge under the pointer while dragging: the icon and the name, or
   how many, a little below and right of the pointer. */
static void paint_drag_icon(struct files *files, const struct entry *first, size_t count)
{
    double scale = desk_window_scale(&files->window);
    struct layout ui = make_layout(files->font, files->italic, files->mono, scale);
    const struct colors *colors = &files->colors;
    int pad = ui.padding, line = scale >= 2 ? (int)scale : 1;
    int offset = (int)lround(14 * scale);
    char text[300];
    if (count == 1)
        snprintf(text, sizeof text, "%s", first->name);
    else
        snprintf(text, sizeof text, "%zu items", count);
    int text_width = desk_text_width(ui.font, text, strlen(text));
    int most = (int)lround(280 * scale);
    int badge = pad / 2 + ui.icon_width + text_width + pad;
    if (badge > most)
        badge = most;
    int badge_height = ui.row_height + pad / 2;
    int width = offset + badge, height = offset + badge_height;
    DeskBuffer *buffer = desk_buffer_get(&files->drag_buffers, files->globals.shm, width, height, WL_SHM_FORMAT_ARGB8888);
    if (!buffer)
        return;
    memset(buffer->pixels, 0, (size_t)width * height * 4);
    DeskCanvas canvas = desk_canvas(buffer->pixels, width, height);
    uint32_t background = (0xe8u << 24) | (mix(colors->background, colors->muted, 40) & 0xffffff);
    desk_canvas_fill(&canvas, offset, offset, badge, badge_height, background);
    outline(&canvas, offset, offset, badge, badge_height, line, colors->directory);
    int baseline = offset + pad / 4 + ui.baseline, x = offset + pad / 2;
    draw_icon(ui.font, &canvas, x, x + ui.icon_width, baseline, count == 1 ? entry_icon(first) : ICON_FOLDER,
              colors->directory);
    x += ui.icon_width;
    draw_clipped(ui.font, &canvas, x, offset + badge - pad / 2, baseline, text, colors->foreground);
    if (files->drag_viewport) {
        wp_viewport_set_destination(files->drag_viewport, (int)lround(width / scale) > 0 ? (int)lround(width / scale) : 1,
                                    (int)lround(height / scale) > 0 ? (int)lround(height / scale) : 1);
    } else {
        wl_surface_set_buffer_scale(files->drag_icon, scale >= 2 ? (int)scale : 1);
    }
    wl_surface_attach(files->drag_icon, buffer->buffer, 0, 0);
    wl_surface_damage_buffer(files->drag_icon, 0, 0, width, height);
    wl_surface_commit(files->drag_icon);
}

static void drag_end(struct files *files, struct wl_data_source *source)
{
    if (files->drag_source != source)
        return;
    wl_data_source_destroy(source);
    drag_icon_free(files);
    files->drag_source = NULL;
    free_paths(files->drag_paths, files->drag_count);
    files->drag_paths = NULL;
    files->drag_count = 0;
    desk_window_redraw(&files->window);
}

static void drag_source_cancelled(void *data, struct wl_data_source *source)
{
    drag_end(data, source);
}

static void drag_source_dropped(void *data, struct wl_data_source *source)
{
    (void)data;
    (void)source;
}

/* The other side has what it needs; a move is its to do, as with other
   file managers. */
static void drag_source_finished(void *data, struct wl_data_source *source)
{
    drag_end(data, source);
}

static void drag_source_action(void *data, struct wl_data_source *source, uint32_t action)
{
    (void)data;
    (void)source;
    (void)action;
}

static const struct wl_data_source_listener drag_source_listener = {
    .target = drag_source_target,
    .send = drag_source_send,
    .cancelled = drag_source_cancelled,
    .dnd_drop_performed = drag_source_dropped,
    .dnd_finished = drag_source_finished,
    .action = drag_source_action,
};

/* A press on a selected name moved: drag the selection. */
static void start_drag(struct files *files)
{
    files->band_pending = files->banding = false;
    if (!files->data_device || files->drag_source)
        return;
    size_t count;
    char **paths = selected_paths(files, &count);
    if (!count) {
        free(paths);
        return;
    }
    struct wl_data_source *source = wl_data_device_manager_create_data_source(files->data_manager);
    wl_data_source_add_listener(source, &drag_source_listener, files);
    static const char *const types[] = {"text/uri-list", "x-special/gnome-copied-files", "text/plain;charset=utf-8",
                                        "text/plain"};
    for (size_t i = 0; i < sizeof types / sizeof *types; ++i)
        wl_data_source_offer(source, types[i]);
    if (wl_data_source_get_version(source) >= WL_DATA_SOURCE_SET_ACTIONS_SINCE_VERSION) {
        uint32_t actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
        if (files->writable && !in_trash(files))
            actions |= WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
        wl_data_source_set_actions(source, actions);
    }
    files->drag_source = source;
    files->drag_paths = paths;
    files->drag_count = count;
    drag_icon_free(files);
    if (files->globals.compositor && files->globals.shm) {
        files->drag_icon = wl_compositor_create_surface(files->globals.compositor);
        if (files->globals.viewporter)
            files->drag_viewport = wp_viewporter_get_viewport(files->globals.viewporter, files->drag_icon);
    }
    wl_data_device_start_drag(files->data_device, source, files->window.surface, files->drag_icon,
                              files->input_serial);
    if (files->drag_icon) {
        const struct entry *first = cursor_entry(files);
        if (!first || !first->selected) {
            for (size_t i = 0; i < files->listing.shown_count; ++i) {
                if (shown_entry(files, i)->selected) {
                    first = shown_entry(files, i);
                    break;
                }
            }
        }
        if (first)
            paint_drag_icon(files, first, count);
    }
    desk_window_redraw(&files->window);
}

static void drop_clear(struct files *files)
{
    if (files->drop_offer)
        wl_data_offer_destroy(files->drop_offer);
    files->drop_offer = NULL;
    free(files->drop_type);
    files->drop_type = NULL;
    free(files->drop_into);
    files->drop_into = NULL;
    files->drop_target = (struct target){TARGET_NONE, 0};
    files->drop_action = 0;
}

/* Our own drag: whether all of it is in directory already. */
static bool dragged_from(const struct files *files, const char *directory)
{
    for (size_t i = 0; i < files->drag_count; ++i) {
        char *parent = parent_path(files->drag_paths[i]);
        bool same = parent && strcmp(parent, directory) == 0;
        free(parent);
        if (!same)
            return false;
    }
    return files->drag_count > 0;
}

/* Where the pointer is while something is dragged over: what it would go
   into, lit, and told to the compositor with the actions on offer. */
static void drop_update(struct files *files)
{
    struct target before = files->drop_target;
    bool had = files->drop_into != NULL;
    free(files->drop_into);
    files->drop_into = NULL;
    files->drop_target = (struct target){TARGET_NONE, 0};
    if (!files->drop_offer)
        return;
    bool own = files->drag_source != NULL;
    struct target target = target_at(files);
    const char *into = NULL;
    char *owned = NULL;
    if (files->drop_type && !files->dialog.open) {
        if (target.kind == TARGET_PLACE) {
            into = files->places[target.index].path;
            files->drop_target = target;
        } else if (target.kind == TARGET_ROW && target.index < files->listing.shown_count) {
            struct entry *entry = shown_entry(files, target.index);
            if (entry->directory && !(own && entry->selected)) {
                into = owned = join_path(files->path, entry->name);
                files->drop_target = target;
            } else if (!entry->directory) {
                into = files->path;
            }
        } else if (target.kind == TARGET_NONE && pointer_buffer_x(files) >= files->panes.list_left &&
                   pointer_buffer_y(files) >= files->panes.entries_top) {
            into = files->path;
        }
    }
    bool trash = into && files->trash_files && strcmp(into, files->trash_files) == 0;
    if (into && (trash ? !in_trash(files) || !own : may_modify(files, into)) && !(own && dragged_from(files, into)))
        files->drop_into = strdup(into);
    free(owned);
    if (!files->drop_into)
        files->drop_target = (struct target){TARGET_NONE, 0};

    uint32_t copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, move = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    uint32_t preferred = copy;
    if (files->drop_into) {
        /* Moved within a drive, copied to another, as other file managers do;
           Ctrl copies, Shift moves. Into the trash is always a move. */
        unsigned mods = desk_keyboard_mods(&files->keyboard);
        struct stat from, to;
        if (trash || (mods & DESK_MOD_SHIFT))
            preferred = move;
        else if (!(mods & DESK_MOD_CTRL) && own && lstat(files->drag_paths[0], &from) == 0 &&
                 stat(files->drop_into, &to) == 0 && from.st_dev == to.st_dev)
            preferred = move;
    }
    wl_data_offer_accept(files->drop_offer, files->drop_serial, files->drop_into ? files->drop_type : NULL);
    if (wl_data_offer_get_version(files->drop_offer) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION)
        wl_data_offer_set_actions(files->drop_offer, files->drop_into ? (trash ? move : copy | move) : 0,
                                  files->drop_into ? preferred : 0);
    if (had != (files->drop_into != NULL) || before.kind != files->drop_target.kind ||
        before.index != files->drop_target.index)
        desk_window_redraw(&files->window);
}

static void device_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface,
                         wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer)
{
    (void)device;
    struct files *files = data;
    drop_clear(files);
    if (!offer)
        return;
    files->drop_offer = offer;
    files->drop_serial = serial;
    if (offer == files->pending_offer) {
        files->drop_type = files->pending_files_type;
        files->pending_files_type = NULL;
        files->pending_offer = NULL;
    }
    if (surface != files->window.surface) {
        wl_data_offer_accept(offer, serial, NULL); /* A menu. */
        return;
    }
    files->pointer_surface = surface;
    files->pointer_x = wl_fixed_to_double(x);
    files->pointer_y = wl_fixed_to_double(y);
    drop_update(files);
}

static void device_leave(void *data, struct wl_data_device *device)
{
    (void)device;
    struct files *files = data;
    bool lit = files->drop_into != NULL;
    drop_clear(files);
    if (lit)
        desk_window_redraw(&files->window);
}

static void device_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)device;
    (void)time;
    struct files *files = data;
    files->pointer_x = wl_fixed_to_double(x);
    files->pointer_y = wl_fixed_to_double(y);
    drop_update(files);
}

/* Dropped: the files go where it was lit. */
static void device_drop(void *data, struct wl_data_device *device)
{
    (void)device;
    struct files *files = data;
    if (!files->drop_offer || !files->drop_into) {
        drop_clear(files);
        return;
    }
    char **paths = NULL;
    size_t count = 0;
    bool cut;
    if (files->drag_source) {
        /* Ours: no need to ask the compositor for what we have. */
        paths = calloc(files->drag_count, sizeof *paths);
        for (size_t i = 0; paths && i < files->drag_count; ++i) {
            if ((paths[count] = strdup(files->drag_paths[i])))
                ++count;
        }
    } else {
        char *text = receive_offer(files, files->drop_offer, files->drop_type);
        paths = uri_list_paths(text, strcmp(files->drop_type, "x-special/gnome-copied-files") == 0, &cut, &count);
        free(text);
    }
    bool move = files->drop_action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    char *into = strdup(files->drop_into);
    if (wl_data_offer_get_version(files->drop_offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION && files->drop_action)
        wl_data_offer_finish(files->drop_offer);
    drop_clear(files);
    if (!count || !into) {
        free(paths);
        notify(files, "Nothing in it is a file here");
    } else if (files->trash_files && strcmp(into, files->trash_files) == 0) {
        trash_paths(files, paths, count);
    } else {
        start_job(files, move ? JOB_MOVE : JOB_COPY, paths, count, into);
    }
    free(into);
    desk_window_redraw(&files->window);
}

static void device_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
    (void)device;
    struct files *files = data;
    if (files->selection_offer)
        wl_data_offer_destroy(files->selection_offer);
    files->selection_offer = offer;
    free(files->offer_type);
    files->offer_type = NULL;
    free(files->offer_files_type);
    files->offer_files_type = NULL;
    if (offer && offer == files->pending_offer) {
        files->offer_type = files->pending_type;
        files->pending_type = NULL;
        files->offer_files_type = files->pending_files_type;
        files->pending_files_type = NULL;
    }
    files->pending_offer = NULL;
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
    MENU_RENAME,
    MENU_TRASH,
    MENU_DELETE,
    MENU_RESTORE,
    MENU_EMPTY_TRASH,
    MENU_NEW_FOLDER,
    MENU_NEW_FILE,
    MENU_UNDO,
    MENU_CUT,
    MENU_COPY,
    MENU_PASTE,           /* Into the directory the menu is for. */
    MENU_PROPERTIES,
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
    int widest = 0, widest_key = 0;
    for (size_t i = 0; i < menu->count; ++i) {
        int width = desk_text_width(files->font, menu->items[i].label, strlen(menu->items[i].label));
        int key = desk_text_width(files->font, menu->items[i].shortcut, strlen(menu->items[i].shortcut));
        if (width > widest)
            widest = width;
        if (key > widest_key)
            widest_key = key;
    }
    /* A check column, the label, a gap, the shortcut and the submenu arrow. */
    int text = (int)lround((widest + (widest_key ? widest_key + 2 * font_pixels(files) : 0)) / scale);
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
        if (item->shortcut[0])
            draw_right(files->font, canvas, column, canvas->width - column, baseline, item->shortcut,
                       lit ? color : colors->muted);
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
    bool writable = files->writable;
    if (context == CONTEXT_BACKGROUND) {
        if (in_trash(files)) {
            menu_add(menu, MENU_EMPTY_TRASH, 0, "Empty trash")->enabled = files->listing.count > 0;
        } else {
            struct menu_item *item = menu_add(menu, MENU_NEW_FOLDER, 0, "New folder");
            item->enabled = writable;
            snprintf(item->shortcut, sizeof item->shortcut, "Ctrl+Shift+N");
            menu_add(menu, MENU_NEW_FILE, 0, "New file")->enabled = writable;
        }
        struct menu_item *paste = menu_add(menu, MENU_PASTE, 0, "Paste");
        paste->enabled = writable && clipboard_has_files(files) && !in_trash(files);
        snprintf(paste->shortcut, sizeof paste->shortcut, "Ctrl+V");
        if (files->undo.kind != UNDO_NONE)
            snprintf(menu_add(menu, MENU_UNDO, 0, "Undo")->shortcut, sizeof menu->items[0].shortcut, "Ctrl+Z");
        menu_separator(menu);
        menu_add(menu, MENU_HIDDEN, 0, "Show hidden files")->checked = files->show_hidden;
        menu_add(menu, MENU_SORT_LIST, 0, "Sort by")->submenu = true;
        menu_separator(menu);
        menu_add(menu, MENU_REFRESH, 0, "Refresh");
        menu_add(menu, MENU_COPY_PATH, 0, "Copy path");
        menu_add(menu, MENU_PROPERTIES, 0, "Properties");
    } else {
        menu_add(menu, MENU_OPEN, 0, "Open");
        if (directory) {
            menu_add(menu, MENU_NEW_TAB, 0, "Open in new tab");
        } else {
            menu_add(menu, MENU_OPEN_WITH_LIST, 0, "Open with")->submenu = true;
            if (executable)
                menu_add(menu, MENU_RUN, 0, "Run");
        }
        if (context == CONTEXT_ENTRY) {
            menu_separator(menu);
            size_t selected = selected_count(files);
            struct menu_item *item;
            if (in_trash(files)) {
                menu_add(menu, MENU_RESTORE, 0, "Restore");
                item = menu_add(menu, MENU_DELETE, 0, "Delete permanently");
                snprintf(item->shortcut, sizeof item->shortcut, "Delete");
            } else {
                item = menu_add(menu, MENU_CUT, 0, "Cut");
                item->enabled = writable;
                snprintf(item->shortcut, sizeof item->shortcut, "Ctrl+X");
                item = menu_add(menu, MENU_COPY, 0, "Copy");
                snprintf(item->shortcut, sizeof item->shortcut, "Ctrl+C");
                if (directory && selected <= 1)
                    menu_add(menu, MENU_PASTE, 0, "Paste into folder")->enabled =
                        clipboard_has_files(files) && may_modify(files, path);
                menu_separator(menu);
                item = menu_add(menu, MENU_RENAME, 0, "Rename");
                item->enabled = writable && selected <= 1;
                snprintf(item->shortcut, sizeof item->shortcut, "F2");
                item = menu_add(menu, MENU_TRASH, 0, "Move to trash");
                item->enabled = writable;
                snprintf(item->shortcut, sizeof item->shortcut, "Delete");
                item = menu_add(menu, MENU_DELETE, 0, "Delete permanently");
                item->enabled = writable;
                snprintf(item->shortcut, sizeof item->shortcut, "Shift+Delete");
            }
        } else if (files->trash_files && strcmp(path, files->trash_files) == 0) {
            menu_add(menu, MENU_EMPTY_TRASH, 0, "Empty trash");
        }
        menu_separator(menu);
        menu_add(menu, MENU_COPY_PATH, 0, "Copy path");
        snprintf(menu_add(menu, MENU_PROPERTIES, 0, "Properties")->shortcut, sizeof menu->items[0].shortcut,
                 context == CONTEXT_ENTRY ? "Alt+Enter" : "");
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
    case MENU_PROPERTIES:
        show_properties(files, files->menu_context == CONTEXT_ENTRY ? NULL : path);
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
    case MENU_RENAME:
        start_rename(files, files->cursor, false);
        break;
    case MENU_TRASH:
        trash_selection(files);
        break;
    case MENU_DELETE:
        ask_delete(files);
        break;
    case MENU_RESTORE:
        restore_selection(files);
        break;
    case MENU_EMPTY_TRASH:
        ask_empty_trash(files);
        break;
    case MENU_NEW_FOLDER:
    case MENU_NEW_FILE:
        create_item(files, action == MENU_NEW_FOLDER);
        break;
    case MENU_UNDO:
        undo(files);
        break;
    case MENU_CUT:
    case MENU_COPY:
        copy_files(files, action == MENU_CUT);
        break;
    case MENU_PASTE:
        paste_files(files, path);
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
    if (files->dialog.open)
        return;
    if (files->renaming) {
        finish_rename(files);
        if (files->renaming)
            return;
        target = target_at(files);
    }
    int x = (int)lround(files->pointer_x), y = (int)lround(files->pointer_y);
    if (target.kind == TARGET_ROW) {
        /* On the selection it stays; elsewhere that entry alone is selected. */
        if (shown_entry(files, target.index)->selected)
            files->cursor = target.index;
        else
            select_only(files, target.index);
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

/* Everything clickable lights up under the pointer, entries too. */
static bool shows_hover(enum target_kind kind)
{
    return kind != TARGET_NONE && kind != TARGET_SEARCH && kind != TARGET_PATH;
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

/* Where the list's content is, in its own pixels: y from the first line. */
static int content_y(const struct files *files)
{
    return pointer_buffer_y(files) - files->panes.entries_top + (int)files->top * files->panes.line_height;
}

/* Whether the pointer is on an entry's name (and icon) in the list view,
   where a drag will move it, rather than the rest of the row, where a drag
   draws a rubber band. */
static bool on_name(struct files *files, size_t index)
{
    if (files->view == VIEW_ICONS)
        return true;
    struct layout list = make_layout(files->list_font, files->list_italic, files->list_mono,
                                     desk_window_scale(&files->window));
    const struct entry *entry = shown_entry(files, index);
    int right = files->panes.list_left + list.padding / 2 + list.icon_width +
                desk_text_width(entry->link ? list.italic : list.font, entry->name, strlen(entry->name)) +
                list.padding / 2;
    if (right > files->panes.column_right[COLUMN_NAME])
        right = files->panes.column_right[COLUMN_NAME];
    return pointer_buffer_x(files) < right;
}

/* A press on an entry (index), or the list's empty space ((size_t)-1). A
   plain click selects the entry alone, Ctrl+click adds or removes it,
   Shift+click selects from the last one clicked; on empty space a plain
   click clears the selection. A drag from empty space, or from beside a
   name in the list view, draws a rubber band. */
static void press_entries(struct files *files, size_t index, unsigned mods)
{
    bool ctrl = mods & DESK_MOD_CTRL, shift = mods & DESK_MOD_SHIFT;
    for (size_t i = 0; i < files->listing.count; ++i)
        files->listing.entries[i].was_selected = files->listing.entries[i].selected;
    files->band_add = ctrl || shift;
    if (index == (size_t)-1) {
        if (!ctrl && !shift)
            clear_selection(files);
    } else if (shift) {
        select_range(files, index, ctrl);
    } else if (ctrl) {
        shown_entry(files, index)->selected = !shown_entry(files, index)->selected;
        files->cursor = files->anchor = index;
    } else if (!shown_entry(files, index)->selected || !on_name(files, index)) {
        select_only(files, index);
    } else {
        /* On a selected name: kept for a drag; alone selected when it's a click. */
        files->cursor = files->anchor = index;
    }
    files->free_scroll = false;
    files->band_pending = true;
    files->press_on_name = index != (size_t)-1 && on_name(files, index);
    files->band_x0 = files->band_x1 = pointer_buffer_x(files);
    files->band_y0 = files->band_y1 = content_y(files);
    desk_window_redraw(&files->window);
}

/* The rubber band moved: what it touches is selected, on top of what was
   with Ctrl or Shift. */
static void band_select(struct files *files)
{
    const struct panes *panes = &files->panes;
    int left = files->band_x0 < files->band_x1 ? files->band_x0 : files->band_x1;
    int right = files->band_x0 < files->band_x1 ? files->band_x1 : files->band_x0;
    int top = files->band_y0 < files->band_y1 ? files->band_y0 : files->band_y1;
    int bottom = files->band_y0 < files->band_y1 ? files->band_y1 : files->band_y0;
    size_t per_line = columns(files);
    long last = -1;
    for (size_t i = 0; i < files->listing.shown_count; ++i) {
        struct entry *entry = shown_entry(files, i);
        int line_top = (int)(i / per_line) * panes->line_height, line_bottom = line_top + panes->line_height;
        int tile_left = panes->list_left + (int)(i % per_line) * panes->tile_width;
        bool touched = top < line_bottom && bottom > line_top &&
                       (files->view != VIEW_ICONS || (left < tile_left + panes->tile_width && right > tile_left));
        entry->selected = touched || (files->band_add && entry->was_selected);
        if (touched)
            last = (long)i;
    }
    if (last >= 0)
        files->cursor = files->anchor = (size_t)last;
}

/* The pointer moved with the button down on the list. */
static void band_drag(struct files *files)
{
    int x = pointer_buffer_x(files), y = content_y(files);
    int reach = (int)lround(4 * desk_window_scale(&files->window));
    bool moved = abs(x - files->band_x0) >= reach || abs(y - files->band_y0) >= reach;
    if (files->press_on_name) {
        if (moved)
            start_drag(files); /* The selection, carried by the compositor from here. */
        return;
    }
    if (!files->banding) {
        if (!moved)
            return;
        files->banding = true;
    }
    /* Past the top or bottom: scroll a line toward it. */
    const struct panes *panes = &files->panes;
    int buffer_y = pointer_buffer_y(files);
    if (buffer_y < panes->entries_top && files->top > 0)
        --files->top;
    else if (buffer_y > panes->entries_top + panes->rows * panes->line_height &&
             (long)files->top + panes->rows < (long)line_count(files))
        ++files->top;
    files->free_scroll = true;
    files->band_x1 = x;
    files->band_y1 = content_y(files);
    band_select(files);
    desk_window_redraw(&files->window);
}

static void band_end(struct files *files)
{
    if (!files->banding && files->band_pending && files->press_on_name && !files->band_add) {
        /* A click on a selected name, not a drag: that entry alone. */
        int reach = (int)lround(4 * desk_window_scale(&files->window));
        if (abs(pointer_buffer_x(files) - files->band_x0) < reach && abs(content_y(files) - files->band_y0) < reach &&
            files->cursor < files->listing.shown_count)
            select_only(files, files->cursor);
    }
    files->band_pending = files->banding = false;
    desk_window_redraw(&files->window);
}

static void click(struct files *files, uint32_t time)
{
    struct target target = target_at(files);
    unsigned mods = desk_keyboard_mods(&files->keyboard);
    if (files->dialog.open) {
        if (target.kind == TARGET_DIALOG_BUTTON) {
            dialog_choose(files, (int)target.index);
        } else if (target.kind == TARGET_DIALOG_CHECK) {
            files->dialog.checked = !files->dialog.checked;
            desk_window_redraw(&files->window);
        }
        update_hover(files, true);
        return;
    }
    if (files->renaming && target.kind != TARGET_RENAME) {
        /* A click elsewhere takes the name as typed; one that can't be used
           keeps the field, and the click does nothing else. */
        finish_rename(files);
        if (files->renaming)
            return;
        target = target_at(files);
    }
    bool twice = target.kind != TARGET_NONE && target.kind == files->press.kind &&
                 target.index == files->press.index && time - files->press_time < DOUBLE_CLICK_MS;
    files->press = target;
    files->press_time = time;
    clear_notice(files);
    if (target.kind != TARGET_SEARCH && target.kind != TARGET_SEARCH_CLEAR)
        search_focus(files, false);
    /* Clicking anywhere but the path field or a suggestion puts the crumbs back. */
    if (files->path_editing && target.kind != TARGET_PATH && target.kind != TARGET_SUGGESTION)
        stop_path_edit(files);
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
        if (twice && !(mods & (DESK_MOD_CTRL | DESK_MOD_SHIFT))) {
            select_only(files, target.index);
            go_in(files);
            break;
        }
        press_entries(files, target.index, mods);
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
        files->search_focus = true;
        field_press(files, &files->search, twice);
        break;
    case TARGET_PATH:
        if (!files->path_editing)
            start_path_edit(files); /* All selected, ready to type over. */
        else
            field_press(files, &files->path_field, twice);
        break;
    case TARGET_SUGGESTION:
        if (target.index < files->suggestion_count) {
            use_suggestion(files, files->suggestions[target.index]);
            go_to_typed(files);
        }
        break;
    case TARGET_SEARCH_CLEAR:
        search_clear(files);
        search_focus(files, true);
        break;
    case TARGET_HEADING:
        sort_by(files, (enum column)target.index);
        break;
    case TARGET_RENAME:
        field_press(files, &files->rename_field, twice);
        break;
    case TARGET_DIALOG_BUTTON:
    case TARGET_DIALOG_CHECK:
        break;
    case TARGET_JOB_CANCEL:
        cancel_job(files);
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
        if (in_list(files) && pointer_buffer_y(files) >= files->panes.entries_top &&
            !empty_message(files, &(uint32_t){0}))
            press_entries(files, (size_t)-1, mods);
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
    if (files->dialog.open)
        return;
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
    else if (files->selecting)
        field_drag(files);
    else if (files->band_pending)
        band_drag(files);
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
    } else if (files->selecting) {
        files->selecting = NULL;
        update_hover(files, true);
    } else if (files->band_pending) {
        band_end(files);
        update_hover(files, true);
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
    free(files->offer_type);
    free(files->pending_type);
    free(files->offer_files_type);
    free(files->pending_files_type);
    clip_forget_files(files);
    drop_clear(files);
    if (files->drag_source)
        wl_data_source_destroy(files->drag_source);
    drag_icon_free(files);
    free_paths(files->drag_paths, files->drag_count);
    free_suggestions(files);
    desk_text_free(&files->search.text);
    desk_text_free(&files->path_field.text);
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
        struct pollfd fds[10] = {
            {.fd = wl_display_get_fd(display), .events = POLLIN},
            {.fd = files->repeat.fd, .events = POLLIN},
            {.fd = files->inotify, .events = POLLIN},
            {.fd = files->refresh, .events = POLLIN},
            {.fd = files->palette_watch.fd, .events = POLLIN},
            {.fd = files->mounts, .events = POLLPRI},
            {.fd = files->sizer ? files->sizer->event : -1, .events = POLLIN},
            {.fd = files->thumbnailer ? files->thumbnailer->event : -1, .events = POLLIN},
            {.fd = files->worker ? files->worker->event : -1, .events = POLLIN},
            {.fd = files->counter ? files->counter->event : -1, .events = POLLIN},
        };
        if (poll(fds, 10, -1) < 0) {
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
        if (files->thumbnailer && fds[7].revents & POLLIN)
            thumbs_arrived(files);
        if (files->worker && fds[8].revents & POLLIN)
            jobs_event(files);
        if (files->counter && fds[9].revents & POLLIN)
            counter_event(files);
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
    files.thumbs_dir = thumbnails_directory(files.home);
    files.trash_dir = trash_directory(files.home);
    if (files.trash_dir && asprintf(&files.trash_files, "%s/files", files.trash_dir) >= 0) {
        /* Made now, so the Trash place is there from the start. */
        char *made = strdup(files.trash_files);
        if (made)
            make_private_directories(made);
        free(made);
        if (asprintf(&made, "%s/info", files.trash_dir) >= 0) {
            make_private_directories(made);
            free(made);
        }
        files.trash_real = realpath(files.trash_dir, NULL);
    } else {
        files.trash_files = NULL;
    }
    desk_spawn_mark("files"); /* What it opens floats over it, in dwl. */
    files.thumbnailer = thumbnailer_create(files.thumbs_dir); /* Without it, images show their icon. */
    files.worker = worker_create(); /* Without it, copying and deleting say so. */
    files.job_top = -1;
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
    thumbs_clear(&files);
    thumbnailer_destroy(files.thumbnailer);
    worker_destroy(files.worker);
    counter_stop(&files);
    free(files.thumbs_dir);
    free(files.trash_dir);
    free(files.trash_files);
    free(files.trash_real);
    free(files.rename_name);
    desk_text_free(&files.rename_field.text);
    close_dialog(&files);
    undo_forget(&files);
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
