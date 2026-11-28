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
#include <sys/statvfs.h>
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
#define TEXT_LIMIT 32768      /* Bytes of a file read for a text preview. */
#define SNIFF_LIMIT 4096      /* Bytes checked to tell text from binary. */
#define IMAGE_LIMIT (64u << 20) /* Largest decoded image for a preview, in bytes. */
#define TAB_WIDTH 4
#define PARENT_CELLS 60       /* Narrower than this, the parent column goes. */
#define PREVIEW_CELLS 40      /* Narrower than this, the preview goes too. */
#define MIN_COLUMN_SHARE 0.1  /* No column narrower than this share of the shown width. */
#define RESIZE_STEP 0.05      /* < and > move a divider by this share of the width. */
#define DIVIDER_GRAB 5        /* Logical pixels either side of a divider that grab it. */
#define DOUBLE_CLICK_MS 400

static const double default_weights[3] = {1, 2, 3};

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
    qsort(listing->entries, listing->count, sizeof *listing->entries, entry_order);
    listing->shown = malloc((listing->count ? listing->count : 1) * sizeof *listing->shown);
    if (!listing->shown) {
        listing_free(listing);
        listing->error = ENOMEM;
        return ENOMEM;
    }
    return 0;
}

static void listing_filter(struct listing *listing, bool show_hidden)
{
    listing->shown_count = 0;
    for (size_t i = 0; i < listing->count; ++i) {
        if (show_hidden || !listing->entries[i].hidden)
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
    /* The parent too, for its column. Its own removal shows up as the current one's. */
    if (files->parent_watch >= 0 && files->parent_watch != files->watch)
        inotify_rm_watch(files->inotify, files->parent_watch);
    files->parent_watch = -1;
    if (strcmp(files->path, "/") != 0) {
        char *parent = parent_path(files->path);
        if (parent)
            files->parent_watch = inotify_add_watch(files->inotify, parent,
                                                    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                                        IN_ATTRIB | IN_ONLYDIR);
        free(parent);
    }
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

static bool parse_weights(const char *text, double weights[3])
{
    double parsed[3];
    char *end;
    for (int i = 0; i < 3; ++i) {
        parsed[i] = strtod(text, &end);
        if (end == text || !(parsed[i] > 0) || parsed[i] > 100 || (i < 2 && *end != ':'))
            return false;
        text = end + (i < 2);
    }
    if (*end)
        return false;
    memcpy(weights, parsed, sizeof parsed);
    return true;
}

static void state_entry(void *data, const char *section, const char *key, const char *value, int line)
{
    (void)line;
    struct files *files = data;
    if (key && strcmp(section, "layout") == 0 && strcmp(key, "columns") == 0)
        parse_weights(value, files->weights);
}

static void load_state(struct files *files)
{
    memcpy(files->weights, default_weights, sizeof files->weights);
    if (files->state_path)
        desk_ini_load(files->state_path, state_entry, files);
}

/* Write the state atomically. Failing only loses the widths, so it's quiet. */
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
    fprintf(atomic.file, "# Written by files. Column widths as parent : current : preview.\n[layout]\n"
                         "columns = %.3f:%.3f:%.3f\n",
            files->weights[0], files->weights[1], files->weights[2]);
    desk_atomic_commit(&atomic, true);
    desk_atomic_abort(&atomic); /* Frees what's left, after success or failure. */
}

/* Navigation */

static void update_title(struct files *files)
{
    char title[PATH_MAX + 2];
    display_path(files, files->path, title, sizeof title);
    desk_window_set_title(&files->window, title);
}

/* Replace the listing with path's, keeping the cursor on keep (a name) when
   it's still there, else near the old position. */
static void load_parent(struct files *files)
{
    listing_free(&files->parent);
    files->parent_cursor = SIZE_MAX;
    if (strcmp(files->path, "/") == 0)
        return;
    char *parent = parent_path(files->path);
    if (!parent)
        return;
    listing_read(&files->parent, parent);
    listing_filter(&files->parent, files->show_hidden);
    find_shown(&files->parent, base_name(files->path), &files->parent_cursor);
    free(parent);
}

static void load(struct files *files, const char *keep, size_t near)
{
    listing_free(&files->listing);
    listing_read(&files->listing, files->path);
    listing_filter(&files->listing, files->show_hidden);
    load_parent(files);
    files->writable = may_modify(files, files->path);
    struct statvfs volume;
    files->free_space = statvfs(files->path, &volume) == 0
                            ? (unsigned long long)volume.f_bavail * volume.f_frsize
                            : ULLONG_MAX;
    if (!select_name(files, keep))
        files->cursor = near < files->listing.shown_count ? near
                      : files->listing.shown_count ? files->listing.shown_count - 1
                                                   : 0;
}

/* Go to directory path (taken over), selecting select or what was selected
   there last time. */
static void go_to(struct files *files, char *path, const char *select)
{
    remember(files);
    free(files->path);
    files->path = path;
    files->top = 0;
    struct memory *memory = find_memory(files, path);
    load(files, select ? select : memory ? memory->name : NULL, 0);
    watch_directory(files);
    if (files->window.toplevel)
        update_title(files);
    desk_window_redraw(&files->window);
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

/* After the current directory vanished: the nearest ancestor that exists. */
static void go_to_existing(struct files *files)
{
    char *path = strdup(files->path);
    struct stat info;
    while (path && strcmp(path, "/") != 0 && (stat(path, &info) != 0 || !S_ISDIR(info.st_mode))) {
        char *parent = parent_path(path);
        free(path);
        path = parent;
    }
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
            if (event->wd == files->parent_watch && event->wd != files->watch) {
                changed = true;
                continue;
            }
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
    if ((size_t)target != files->cursor) {
        files->cursor = (size_t)target;
        desk_window_redraw(&files->window);
    }
}

/* Half a page, moving the view with the cursor like a pager. */
static void scroll_half(struct files *files, int direction)
{
    long half = files->page_rows > 1 ? files->page_rows / 2 : 1;
    long top = (long)files->top + direction * half;
    long last_top = (long)files->listing.shown_count - files->page_rows;
    if (top > last_top)
        top = last_top;
    if (top < 0)
        top = 0;
    files->top = (size_t)top;
    move_cursor(files, direction * half);
    desk_window_redraw(&files->window);
}

static void toggle_hidden(struct files *files)
{
    struct listing *listing = &files->listing;
    size_t old = files->cursor < listing->shown_count ? listing->shown[files->cursor] : 0;
    files->show_hidden = !files->show_hidden;
    listing_filter(listing, files->show_hidden);
    listing_filter(&files->parent, files->show_hidden);
    files->parent_cursor = SIZE_MAX;
    find_shown(&files->parent, base_name(files->path), &files->parent_cursor);
    /* The same entry, or the next one still shown. */
    files->cursor = 0;
    for (size_t i = 0; i < listing->shown_count; ++i) {
        files->cursor = i;
        if (listing->shown[i] >= old)
            break;
    }
    desk_window_redraw(&files->window);
}

/* Resizing */

/* Move divider (0: parent|current, 1: current|preview) to buffer x, keeping
   the two columns' combined width. */
static void move_divider(struct files *files, int divider, int x)
{
    const struct columns *columns = &files->columns;
    int a = divider, b = divider + 1;
    if (divider < 0 || divider > 1 || !columns->shown[a] || !columns->shown[b] || columns->inner <= 0)
        return;
    double sum = 0;
    for (int i = 0; i < 3; ++i)
        sum += columns->shown[i] ? files->weights[i] : 0;
    double pair = files->weights[a] + files->weights[b], gap = columns->left[b] - columns->right[a];
    double width = x - gap / 2 - columns->left[a];
    double weight = width / columns->inner * sum, least = MIN_COLUMN_SHARE * sum;
    if (weight < least)
        weight = least;
    if (weight > pair - least)
        weight = pair - least;
    if (pair - least < least)
        return;
    files->weights[a] = weight;
    files->weights[b] = pair - weight;
    desk_window_redraw(&files->window);
}

/* < and >: the current column narrower or wider, against the preview, or
   against the parent when there's no preview. */
static void resize_current(struct files *files, int direction)
{
    const struct columns *columns = &files->columns;
    int step = (int)lround(columns->inner * RESIZE_STEP);
    if (columns->divider[1] >= 0)
        move_divider(files, 1, columns->divider[1] + direction * step);
    else if (columns->divider[0] >= 0)
        move_divider(files, 0, columns->divider[0] - direction * step);
    save_state(files);
}

static void reset_columns(struct files *files)
{
    memcpy(files->weights, default_weights, sizeof files->weights);
    save_state(files);
    desk_window_redraw(&files->window);
}

/* Keys */

/* Act on a key. Return whether it repeats while held. */
static bool key_action(struct files *files, xkb_keysym_t sym, unsigned mods)
{
    if (mods & DESK_MOD_CTRL) {
        switch (sym) {
        case XKB_KEY_d: scroll_half(files, 1); return true;
        case XKB_KEY_u: scroll_half(files, -1); return true;
        case XKB_KEY_q: files->running = false; return false;
        default: return false;
        }
    }
    if (mods & (DESK_MOD_ALT | DESK_MOD_LOGO))
        return false;
    switch (sym) {
    case XKB_KEY_j:
    case XKB_KEY_Down:
        move_cursor(files, 1);
        return true;
    case XKB_KEY_k:
    case XKB_KEY_Up:
        move_cursor(files, -1);
        return true;
    case XKB_KEY_l:
    case XKB_KEY_Right:
        go_in(files);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        go_in(files);
        return false;
    case XKB_KEY_h:
    case XKB_KEY_Left:
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
        move_cursor(files, files->page_rows > 1 ? files->page_rows - 1 : 1);
        return true;
    case XKB_KEY_Page_Up:
        move_cursor(files, -(files->page_rows > 1 ? files->page_rows - 1 : 1));
        return true;
    case XKB_KEY_J:
        files->preview_scroll++;
        desk_window_redraw(&files->window);
        return true;
    case XKB_KEY_K:
        if (files->preview_scroll > 0) {
            files->preview_scroll--;
            desk_window_redraw(&files->window);
        }
        return true;
    case XKB_KEY_less:
        resize_current(files, -1);
        return true;
    case XKB_KEY_greater:
        resize_current(files, 1);
        return true;
    case XKB_KEY_equal:
        reset_columns(files);
        return false;
    case XKB_KEY_z:
        files->show_preview = !files->show_preview;
        desk_window_redraw(&files->window);
        return false;
    case XKB_KEY_period:
        toggle_hidden(files);
        return false;
    case XKB_KEY_asciitilde:
        go_home(files);
        return false;
    case XKB_KEY_q:
        files->running = false;
        return false;
    default:
        return false;
    }
}

/* Drawing */

static double font_pixels(const struct files *files)
{
    return FONT_POINTS * 96.0 / 72.0 * desk_window_scale(&files->window);
}

static void fonts_resize(struct files *files)
{
    double size = font_pixels(files);
    if (desk_font_size(files->font) != size) {
        desk_font_resize(files->font, size);
        desk_font_resize(files->italic, size);
        desk_font_resize(files->mono, size * 0.9);
    }
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

/* Draw a name in width pixels: whole, cut in the middle for files, or cut at
   the end for directories. */
static void draw_name(struct files *files, DeskFont *font, DeskCanvas *canvas, int x, int baseline,
                      int width, const char *name, bool directory, uint32_t color)
{
    size_t length = strlen(name);
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
    (void)files;
}

/* Previews */

static void job_free(struct job *job)
{
    if (job) {
        free(job->path);
        free(job);
    }
}

static void preview_free(struct preview *preview)
{
    if (!preview)
        return;
    free(preview->path);
    listing_free(&preview->listing);
    free(preview->pixels);
    free(preview->text);
    free(preview);
}

/* Fit width × height in a box without enlarging. */
static void fit_box(int width, int height, int box_width, int box_height, int *out_width, int *out_height)
{
    double scale = fmin(1.0, fmin((double)box_width / width, (double)box_height / height));
    *out_width = (int)lround(width * scale);
    *out_height = (int)lround(height * scale);
    if (*out_width < 1)
        *out_width = 1;
    if (*out_height < 1)
        *out_height = 1;
}

/* Shrink an image to fit the box by averaging the area each pixel covers,
   then turn it upright by its EXIF orientation. */
static bool fit_image(struct preview *preview, const DeskImage *image)
{
    int orientation = image->orientation >= 1 && image->orientation <= 8 ? image->orientation : 1;
    bool swap = orientation >= 5;
    int width = (int)image->width, height = (int)image->height;
    int upright_width = swap ? height : width, upright_height = swap ? width : height;
    int out_width, out_height;
    fit_box(upright_width, upright_height, preview->box_width, preview->box_height, &out_width, &out_height);
    int small_width = swap ? out_height : out_width, small_height = swap ? out_width : out_height;

    uint32_t *small = malloc((size_t)small_width * (size_t)small_height * 4);
    uint32_t *out = malloc((size_t)out_width * (size_t)out_height * 4);
    if (!small || !out) {
        free(small);
        free(out);
        return false;
    }
    for (int y = 0; y < small_height; ++y) {
        int y0 = (int)((long long)y * height / small_height), y1 = (int)((long long)(y + 1) * height / small_height);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < small_width; ++x) {
            int x0 = (int)((long long)x * width / small_width), x1 = (int)((long long)(x + 1) * width / small_width);
            if (x1 <= x0)
                x1 = x0 + 1;
            uint64_t sum[4] = {0};
            for (int v = y0; v < y1; ++v) {
                const unsigned char *pixel = image->pixels + ((size_t)v * (size_t)width + (size_t)x0) * 4;
                for (int u = x0; u < x1; ++u, pixel += 4) {
                    sum[0] += pixel[0];
                    sum[1] += pixel[1];
                    sum[2] += pixel[2];
                    sum[3] += pixel[3];
                }
            }
            uint64_t count = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0), half = count / 2;
            small[(size_t)y * (size_t)small_width + (size_t)x] =
                (uint32_t)((sum[3] + half) / count) << 24 | (uint32_t)((sum[0] + half) / count) << 16 |
                (uint32_t)((sum[1] + half) / count) << 8 | (uint32_t)((sum[2] + half) / count);
        }
    }
    /* Each upright pixel (x, y) comes from (u, v) of the stored image. */
    int w = small_width, h = small_height;
    for (int y = 0; y < out_height; ++y) {
        for (int x = 0; x < out_width; ++x) {
            int u, v;
            switch (orientation) {
            case 2: u = w - 1 - x, v = y; break;
            case 3: u = w - 1 - x, v = h - 1 - y; break;
            case 4: u = x, v = h - 1 - y; break;
            case 5: u = y, v = x; break;
            case 6: u = y, v = h - 1 - x; break;
            case 7: u = w - 1 - y, v = h - 1 - x; break;
            case 8: u = w - 1 - y, v = x; break;
            default: u = x, v = y; break;
            }
            out[(size_t)y * (size_t)out_width + (size_t)x] = small[(size_t)v * (size_t)w + (size_t)u];
        }
    }
    free(small);
    preview->pixels = out;
    preview->width = out_width;
    preview->height = out_height;
    preview->image_width = upright_width;
    preview->image_height = upright_height;
    return true;
}

static bool looks_like_image(const unsigned char *head, size_t length)
{
    return (length >= 8 && memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0) ||
           (length >= 3 && memcmp(head, "\xff\xd8\xff", 3) == 0) ||
           (length >= 6 && (memcmp(head, "GIF87a", 6) == 0 || memcmp(head, "GIF89a", 6) == 0)) ||
           (length >= 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WEBP", 4) == 0);
}

static void preview_image(struct preview *preview, int fd)
{
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        preview->kind = PREVIEW_ERROR;
        snprintf(preview->message, sizeof preview->message, "%s", strerror(errno));
        return;
    }
    DeskImage image;
    char error[160];
    bool loaded = desk_image_load(file, &image, IMAGE_LIMIT, DESK_IMAGE_SHRINK, error, sizeof error);
    fclose(file);
    if (!loaded) {
        preview->kind = PREVIEW_ERROR;
        snprintf(preview->message, sizeof preview->message, "%s", error);
        return;
    }
    if (fit_image(preview, &image)) {
        preview->kind = PREVIEW_IMAGE;
    } else {
        preview->kind = PREVIEW_ERROR;
        snprintf(preview->message, sizeof preview->message, "Out of memory");
    }
    free(image.pixels);
}

/* Text when the start has no NUL and hardly any invalid UTF-8. */
static void preview_text(struct preview *preview, int fd)
{
    char *text = malloc(TEXT_LIMIT);
    size_t length = 0;
    ssize_t got = 0;
    while (text && length < TEXT_LIMIT && (got = read(fd, text + length, TEXT_LIMIT - length)) > 0)
        length += (size_t)got;
    close(fd);
    if (!text || got < 0) {
        preview->kind = PREVIEW_ERROR;
        snprintf(preview->message, sizeof preview->message, "%s", text ? strerror(errno) : "Out of memory");
        free(text);
        return;
    }
    size_t checked = length < SNIFF_LIMIT ? length : SNIFF_LIMIT, invalid = 0;
    bool binary = memchr(text, 0, checked) != NULL;
    const char *at = text, *end = text + checked;
    /* A character cut at the end of the checked part isn't counted against it. */
    while (!binary && at < end && (checked == length || end - at >= 4)) {
        const char *start = at;
        if (desk_utf8_next(&at, end) == 0xfffd && !(at - start == 3 && memcmp(start, "\xef\xbf\xbd", 3) == 0))
            ++invalid;
    }
    if (binary || invalid * 100 > checked) {
        free(text);
        preview->kind = PREVIEW_BINARY;
        return;
    }
    preview->kind = PREVIEW_TEXT;
    preview->text = text;
    preview->text_length = length;
}

/* Runs on a worker thread: only the job and the filesystem are touched. */
static struct preview *build_preview(const struct job *job)
{
    struct preview *preview = calloc(1, sizeof *preview);
    if (!preview || !(preview->path = strdup(job->path))) {
        free(preview);
        return NULL;
    }
    preview->modified = job->modified;
    preview->size = job->size;
    preview->box_width = job->box_width > 0 ? job->box_width : 1;
    preview->box_height = job->box_height > 0 ? job->box_height : 1;
    if (job->directory) {
        if (listing_read(&preview->listing, job->path) != 0) {
            preview->kind = PREVIEW_ERROR;
            snprintf(preview->message, sizeof preview->message, "%s", strerror(preview->listing.error));
        } else {
            preview->kind = PREVIEW_DIRECTORY;
        }
        return preview;
    }
    int fd = open(job->path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        preview->kind = PREVIEW_ERROR;
        snprintf(preview->message, sizeof preview->message, "%s", strerror(errno));
        return preview;
    }
    unsigned char head[16];
    ssize_t got = pread(fd, head, sizeof head, 0);
    if (got > 0 && looks_like_image(head, (size_t)got))
        preview_image(preview, fd);
    else
        preview_text(preview, fd);
    return preview;
}

static void *worker_main(void *data)
{
    struct worker *worker = data;
    struct files *files = worker->files;
    pthread_mutex_lock(&files->lock);
    for (;;) {
        while (!files->stopping && !worker->pending)
            pthread_cond_wait(&worker->wake, &files->lock);
        if (files->stopping)
            break;
        struct job *job = worker->pending;
        worker->pending = NULL;
        pthread_mutex_unlock(&files->lock);
        struct preview *preview = build_preview(job);
        job_free(job);
        pthread_mutex_lock(&files->lock);
        if (preview) {
            preview->next = files->done;
            files->done = preview;
            uint64_t one = 1;
            (void)!write(files->done_fd, &one, sizeof one);
        }
    }
    pthread_mutex_unlock(&files->lock);
    return NULL;
}

static void workers_start(struct files *files)
{
    pthread_mutex_init(&files->lock, NULL);
    files->done_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    for (int i = 0; i < WORKER_COUNT; ++i) {
        struct worker *worker = &files->workers[i];
        worker->files = files;
        pthread_cond_init(&worker->wake, NULL);
        worker->started = files->done_fd >= 0 && pthread_create(&worker->thread, NULL, worker_main, worker) == 0;
    }
}

static void workers_stop(struct files *files)
{
    pthread_mutex_lock(&files->lock);
    files->stopping = true;
    for (int i = 0; i < WORKER_COUNT; ++i)
        pthread_cond_signal(&files->workers[i].wake);
    pthread_mutex_unlock(&files->lock);
    for (int i = 0; i < WORKER_COUNT; ++i) {
        struct worker *worker = &files->workers[i];
        if (worker->started)
            pthread_join(worker->thread, NULL);
        job_free(worker->pending);
        free(worker->sent);
        pthread_cond_destroy(&worker->wake);
    }
    while (files->done) {
        struct preview *next = files->done->next;
        preview_free(files->done);
        files->done = next;
    }
    for (int i = 0; i < PREVIEW_CACHE; ++i)
        preview_free(files->cache[i]);
    if (files->done_fd >= 0)
        close(files->done_fd);
    pthread_mutex_destroy(&files->lock);
}

static char *preview_key(const char *path, const struct timespec *modified, off_t size, int box_width,
                         int box_height)
{
    char *key;
    return asprintf(&key, "%s\n%lld.%09ld\n%lld\n%dx%d", path, (long long)modified->tv_sec, modified->tv_nsec,
                    (long long)size, box_width, box_height) < 0
               ? NULL
               : key;
}

static uint32_t entry_icon(const struct entry *entry);

/* Ask a worker for a preview, unless it was the last thing asked of it. */
static void request_preview(struct files *files, const char *path, const struct entry *entry, int box_width,
                            int box_height)
{
    struct worker *worker = &files->workers[entry->directory || entry_icon(entry) != ICON_IMAGE ? WORKER_GENERAL
                                                                                                 : WORKER_IMAGE];
    if (!worker->started)
        return;
    char *key = preview_key(path, &entry->modified, entry->size, box_width, box_height);
    struct job *job = calloc(1, sizeof *job);
    if (!key || !job || (worker->sent && strcmp(worker->sent, key) == 0) || !(job->path = strdup(path))) {
        free(key);
        job_free(job);
        return;
    }
    job->modified = entry->modified;
    job->size = entry->size;
    job->directory = entry->directory;
    job->box_width = box_width;
    job->box_height = box_height;
    pthread_mutex_lock(&files->lock);
    job_free(worker->pending);
    worker->pending = job;
    pthread_cond_signal(&worker->wake);
    pthread_mutex_unlock(&files->lock);
    free(worker->sent);
    worker->sent = key;
}

/* The cached preview of this version of path, asking for one (or for a
   better fitting image) when needed. NULL while it's being made. */
static struct preview *preview_get(struct files *files, const char *path, const struct entry *entry,
                                   int box_width, int box_height)
{
    if (entry->directory)
        box_width = box_height = 0;
    for (int i = 0; i < PREVIEW_CACHE; ++i) {
        struct preview *preview = files->cache[i];
        if (!preview || strcmp(preview->path, path) != 0 || preview->size != entry->size ||
            preview->modified.tv_sec != entry->modified.tv_sec ||
            preview->modified.tv_nsec != entry->modified.tv_nsec)
            continue;
        preview->used = ++files->preview_clock;
        if (preview->kind == PREVIEW_IMAGE) {
            int width, height;
            fit_box(preview->image_width, preview->image_height, box_width, box_height, &width, &height);
            if (width != preview->width || height != preview->height)
                request_preview(files, path, entry, box_width, box_height);
        }
        return preview;
    }
    request_preview(files, path, entry, box_width, box_height);
    return NULL;
}

/* Take finished previews into the cache, replacing older versions of the
   same path and then the least recently used. */
static void collect_previews(struct files *files)
{
    uint64_t count;
    (void)!read(files->done_fd, &count, sizeof count);
    pthread_mutex_lock(&files->lock);
    struct preview *list = files->done;
    files->done = NULL;
    pthread_mutex_unlock(&files->lock);
    while (list) {
        struct preview *preview = list;
        list = list->next;
        preview->next = NULL;
        char *key = preview_key(preview->path, &preview->modified, preview->size,
                                preview->kind == PREVIEW_DIRECTORY ? 0 : preview->box_width,
                                preview->kind == PREVIEW_DIRECTORY ? 0 : preview->box_height);
        for (int i = 0; key && i < WORKER_COUNT; ++i) {
            struct worker *worker = &files->workers[i];
            if (worker->sent && strcmp(worker->sent, key) == 0) {
                free(worker->sent);
                worker->sent = NULL;
            }
        }
        free(key);
        int slot = -1;
        for (int i = 0; i < PREVIEW_CACHE && slot < 0; ++i) {
            if (files->cache[i] && strcmp(files->cache[i]->path, preview->path) == 0)
                slot = i;
        }
        for (int i = 0; i < PREVIEW_CACHE && slot < 0; ++i) {
            if (!files->cache[i])
                slot = i;
        }
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < PREVIEW_CACHE; ++i) {
                if (files->cache[i]->used < files->cache[slot]->used)
                    slot = i;
            }
        }
        preview_free(files->cache[slot]);
        preview->used = ++files->preview_clock;
        files->cache[slot] = preview;
    }
    desk_window_redraw(&files->window);
}

/* Layout */

/* Sizes in buffer pixels for one frame. */
struct layout {
    int row_height, baseline;   /* baseline: from the top of a row. */
    int padding, gap, icon_width;
    int size_width, date_width;
};

static uint32_t mix(uint32_t a, uint32_t b, unsigned amount)
{
    uint32_t out = 0xff000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        unsigned x = (a >> shift) & 0xff, y = (b >> shift) & 0xff;
        out |= ((x * (255 - amount) + y * amount + 127) / 255) << shift;
    }
    return out;
}

/* Keep the cursor on screen with a margin, and the list filling the column. */
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

/* The first row that puts the cursor in the middle, for side columns. */
static size_t centered_top(size_t cursor, size_t count, int rows)
{
    if (cursor == SIZE_MAX || count <= (size_t)rows)
        return 0;
    size_t top = cursor > (size_t)rows / 2 ? cursor - (size_t)rows / 2 : 0;
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

/* A message in place of a list, such as "Empty". */
static void draw_message(struct files *files, DeskCanvas *canvas, const struct layout *layout, int x, int y,
                         const char *message, uint32_t color)
{
    desk_text_draw(files->italic, canvas, x + layout->padding, y + layout->baseline, message, strlen(message),
                   color);
}

/* One row of a list between x0 and x1. focused: the current column, whose
   selection is drawn in full. details: size and date on the right. */
static void draw_row(struct files *files, DeskCanvas *canvas, const struct layout *layout, struct entry *entry,
                     int x0, int x1, int y, bool selected, bool focused, bool details)
{
    const struct colors *colors = &files->colors;
    if (selected)
        desk_canvas_fill(canvas, x0, y, x1 - x0, layout->row_height,
                         focused ? colors->selection_background
                                 : mix(colors->background, colors->selection_background, 150));
    uint32_t color = entry->broken    ? colors->urgent
                   : entry->hidden    ? colors->muted
                   : entry->directory ? colors->directory
                                      : colors->foreground;
    if (selected && focused && !entry->broken)
        color = colors->selection_foreground;
    int baseline = y + layout->baseline, inner = layout->padding / 2;

    char icon[4];
    size_t icon_length;
    uint32_t code = entry_icon(entry);
    encode_utf8(code, icon, &icon_length);
    int glyph_width = desk_char_width(files->font, code);
    desk_text_draw(files->font, canvas, x0 + inner + (layout->icon_width - glyph_width) / 2 - inner / 2, baseline,
                   icon, icon_length, color);

    int name_x = x0 + inner + layout->icon_width;
    int details_width = layout->size_width + layout->gap + layout->date_width;
    int name_right = details ? x1 - details_width - layout->gap - inner : x1 - inner;
    DeskCanvas clipped = *canvas;
    desk_canvas_clip(&clipped, name_x > canvas->clip_left ? name_x : canvas->clip_left, canvas->clip_top,
                     name_right < canvas->clip_right ? name_right : canvas->clip_right, canvas->clip_bottom);
    draw_name(files, entry->link ? files->italic : files->font, &clipped, name_x, baseline, name_right - name_x,
              entry->name, entry->directory, color);
    if (!details)
        return;

    uint32_t detail_color = selected && focused ? colors->selection_foreground : colors->muted;
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
    int date_x = right - desk_text_width(files->mono, date, strlen(date));
    int size_x = right - layout->date_width - layout->gap - desk_text_width(files->mono, size, strlen(size));
    desk_text_draw(files->mono, canvas, size_x, baseline, size, strlen(size), detail_color);
    desk_text_draw(files->mono, canvas, date_x, baseline, date, strlen(date), detail_color);
}

/* A list of entries from row top, in rows rows from y. */
static void paint_list(struct files *files, DeskCanvas *canvas, const struct layout *layout,
                       struct listing *listing, size_t cursor, size_t top, int x0, int x1, int y, int rows,
                       bool focused, bool details, bool messages)
{
    const struct colors *colors = &files->colors;
    if (listing->error || listing->shown_count == 0) {
        if (messages)
            draw_message(files, canvas, layout, x0, y,
                         listing->error ? strerror(listing->error)
                         : listing->count ? "Only hidden files (. shows them)"
                                          : "Empty",
                         listing->error ? colors->urgent : colors->muted);
        return;
    }
    for (int row = 0; row < rows && top + (size_t)row < listing->shown_count; ++row) {
        size_t index = top + (size_t)row;
        draw_row(files, canvas, layout, &listing->entries[listing->shown[index]], x0, x1,
                 y + row * layout->row_height, index == cursor, focused, details);
    }
}

static const char *type_name(mode_t mode, enum preview_kind kind)
{
    switch (mode & S_IFMT) {
    case S_IFDIR: return "Directory";
    case S_IFLNK: return "Symbolic link";
    case S_IFIFO: return "Named pipe";
    case S_IFSOCK: return "Socket";
    case S_IFCHR: return "Character device";
    case S_IFBLK: return "Block device";
    case S_IFREG: return kind == PREVIEW_BINARY ? "Binary file" : "File";
    default: return "Unknown";
    }
}

/* "drwxr-xr-x" with setuid, setgid and sticky bits, as ls shows them. */
static void format_mode(mode_t mode, char out[11])
{
    static const char types[] = "?pc?d?b?-?l?s???";
    out[0] = types[(mode & S_IFMT) >> 12 & 15];
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        out[i + 1] = mode & (0400 >> i) ? rwx[i] : '-';
    if (mode & S_ISUID) out[3] = mode & S_IXUSR ? 's' : 'S';
    if (mode & S_ISGID) out[6] = mode & S_IXGRP ? 's' : 'S';
    if (mode & S_ISVTX) out[9] = mode & S_IXOTH ? 't' : 'T';
    out[10] = 0;
}

static void format_owner(uid_t owner, gid_t group, char *out, size_t length)
{
    struct passwd *user = getpwuid(owner);
    char user_name[64];
    snprintf(user_name, sizeof user_name, "%s", user ? user->pw_name : "");
    if (!user)
        snprintf(user_name, sizeof user_name, "%u", (unsigned)owner);
    struct group *entry = getgrgid(group);
    if (entry)
        snprintf(out, length, "%s %s", user_name, entry->gr_name);
    else
        snprintf(out, length, "%s %u", user_name, (unsigned)group);
}

/* The entry's details as label and value lines. */
static void paint_details(struct files *files, DeskCanvas *canvas, const struct layout *layout,
                          const struct entry *entry, enum preview_kind kind, int x, int y)
{
    char size[64], mode[11], owner[160], date[32];
    if (S_ISREG(entry->target_mode)) {
        char short_size[32];
        format_size(entry->size, short_size, sizeof short_size);
        if (entry->size >= 1024)
            snprintf(size, sizeof size, "%s (%lld bytes)", short_size, (long long)entry->size);
        else
            snprintf(size, sizeof size, "%s", short_size);
    } else {
        snprintf(size, sizeof size, "-");
    }
    format_mode(entry->target_mode ? entry->target_mode : entry->mode, mode);
    format_owner(entry->owner, entry->group, owner, sizeof owner);
    format_date(&entry->modified, date, sizeof date);
    const char *labels[] = {"Type", "Size", "Permissions", "Owner", "Modified"};
    const char *values[] = {type_name(entry->target_mode ? entry->target_mode : entry->mode, kind), size, mode,
                            owner, date};
    int label_width = desk_text_width(files->font, "Permissions", 11) + layout->gap;
    for (int i = 0; i < 5; ++i) {
        int baseline = y + layout->padding / 2 + i * layout->row_height + layout->baseline;
        desk_text_draw(files->font, canvas, x + layout->padding, baseline, labels[i], strlen(labels[i]),
                       files->colors.muted);
        desk_text_draw(files->font, canvas, x + layout->padding + label_width, baseline, values[i],
                       strlen(values[i]), files->colors.foreground);
    }
}

/* The start of a text file in monospace, tabs expanded, from line scroll. */
static void paint_text(struct files *files, DeskCanvas *canvas, const struct preview *preview, int x0, int y0,
                       int y1)
{
    DeskFontMetrics metrics = desk_font_metrics(files->mono);
    int line_height = (int)lround(metrics.height * 1.2), cell = desk_char_width(files->mono, '0');
    const char *text = preview->text, *end = text + preview->text_length;
    long lines = 0;
    for (const char *at = text; at < end; ++lines) {
        const char *newline = memchr(at, '\n', (size_t)(end - at));
        at = newline ? newline + 1 : end;
    }
    long visible = (y1 - y0) / line_height;
    long last = lines > visible ? lines - visible : 0;
    if (files->preview_scroll > last)
        files->preview_scroll = last;
    const char *at = text;
    for (long skip = files->preview_scroll; skip > 0 && at < end; --skip) {
        const char *newline = memchr(at, '\n', (size_t)(end - at));
        at = newline ? newline + 1 : end;
    }
    for (int y = y0; at < end && y + line_height <= y1; y += line_height) {
        const char *newline = memchr(at, '\n', (size_t)(end - at));
        const char *line_end = newline ? newline : end;
        if (line_end > at && line_end[-1] == '\r')
            --line_end;
        int column = 0, baseline = y + (line_height - metrics.height) / 2 + metrics.ascent;
        /* Tab-separated segments, each at its column. */
        const char *segment = at;
        while (segment < line_end && x0 + column * cell < canvas->clip_right) {
            const char *tab = memchr(segment, '\t', (size_t)(line_end - segment));
            const char *segment_end = tab ? tab : line_end;
            desk_text_draw(files->mono, canvas, x0 + column * cell, baseline, segment,
                           (size_t)(segment_end - segment), files->colors.foreground);
            for (const char *c = segment; c < segment_end;) {
                desk_utf8_next(&c, segment_end);
                ++column;
            }
            if (!tab)
                break;
            column = (column / TAB_WIDTH + 1) * TAB_WIDTH;
            segment = tab + 1;
        }
        at = newline ? newline + 1 : end;
    }
}

/* The selected entry's preview in the right column. */
static void paint_preview(struct files *files, DeskCanvas *canvas, const struct layout *layout,
                          struct entry *entry, int x0, int x1, int y0, int y1)
{
    if (!entry)
        return;
    char *path = join_path(files->path, entry->name);
    if (!path)
        return;
    if (!files->preview_path || strcmp(files->preview_path, path) != 0) {
        free(files->preview_path);
        files->preview_path = strdup(path);
        files->preview_scroll = 0;
    }
    const struct colors *colors = &files->colors;
    DeskCanvas clipped = *canvas;
    desk_canvas_clip(&clipped, x0, y0, x1, y1);
    int y = y0;
    if (entry->link) {
        char target[PATH_MAX + 8] = "→ ";
        ssize_t length = readlink(path, target + strlen(target), sizeof target - strlen(target) - 1);
        if (length >= 0) {
            target[strlen("→ ") + (size_t)length] = 0;
            draw_message(files, &clipped, layout, x0, y, target, colors->muted);
            y += layout->row_height;
        }
    }
    int rows = (y1 - y) / layout->row_height;
    if (entry->broken) {
        draw_message(files, &clipped, layout, x0, y, "Broken link", colors->urgent);
    } else if (entry->directory) {
        struct preview *preview = preview_get(files, path, entry, 0, 0);
        if (preview && preview->kind == PREVIEW_ERROR) {
            draw_message(files, &clipped, layout, x0, y, preview->message, colors->urgent);
        } else if (preview && rows > 0) {
            struct listing *listing = &preview->listing;
            listing_filter(listing, files->show_hidden);
            struct memory *memory = find_memory(files, path);
            size_t cursor = 0;
            if (memory)
                find_shown(listing, memory->name, &cursor);
            long base = (long)centered_top(cursor, listing->shown_count, rows);
            long last = listing->shown_count > (size_t)rows ? (long)listing->shown_count - rows : 0;
            if (base + files->preview_scroll > last)
                files->preview_scroll = last > base ? last - base : 0;
            paint_list(files, &clipped, layout, listing, cursor, (size_t)(base + files->preview_scroll), x0, x1,
                       y, rows, false, false, true);
        }
    } else if (S_ISREG(entry->target_mode)) {
        struct preview *preview = preview_get(files, path, entry, x1 - x0, y1 - y);
        if (!preview) {
            /* Still being made. */
        } else if (preview->kind == PREVIEW_IMAGE) {
            int width = preview->width < x1 - x0 ? preview->width : x1 - x0;
            desk_canvas_blit(&clipped, x0 + (x1 - x0 - width) / 2, y, preview->pixels, preview->width,
                             preview->height);
        } else if (preview->kind == PREVIEW_TEXT && preview->text_length) {
            paint_text(files, &clipped, preview, x0 + layout->padding / 2, y, y1);
        } else if (preview->kind == PREVIEW_TEXT) {
            draw_message(files, &clipped, layout, x0, y, "Empty file", colors->muted);
        } else {
            if (preview->kind == PREVIEW_ERROR) {
                draw_message(files, &clipped, layout, x0, y, preview->message, colors->urgent);
                y += layout->row_height;
            }
            paint_details(files, &clipped, layout, entry, preview->kind, x0, y);
        }
    } else {
        paint_details(files, &clipped, layout, entry, PREVIEW_BINARY, x0, y);
    }
    free(path);
}

/* Bottom line: the entry's mode, owner, size, time and link target on the
   left; position and free space on the right. */
static void paint_status(struct files *files, DeskCanvas *canvas, const struct layout *layout, int x0, int x1,
                         int y)
{
    const struct colors *colors = &files->colors;
    int baseline = y + layout->baseline;
    char right[96], free_text[32] = "";
    if (files->free_space != ULLONG_MAX) {
        format_size((off_t)(files->free_space > (unsigned long long)INT64_MAX ? INT64_MAX : files->free_space),
                    free_text, sizeof free_text);
        snprintf(right, sizeof right, "%zu/%zu   %s free",
                 files->listing.shown_count ? files->cursor + 1 : 0, files->listing.shown_count, free_text);
    } else {
        snprintf(right, sizeof right, "%zu/%zu", files->listing.shown_count ? files->cursor + 1 : 0,
                 files->listing.shown_count);
    }
    int right_x = x1 - layout->padding / 2 - desk_text_width(files->mono, right, strlen(right));
    desk_text_draw(files->mono, canvas, right_x, baseline, right, strlen(right), colors->foreground);
    /* Root may change anything here; otherwise say when this place is protected. */
    const char *mark = files->root ? "\xef\x80\xa3 root" : !files->writable ? "\xef\x80\xa3 read-only" : NULL;
    if (mark) {
        right_x -= layout->gap + desk_text_width(files->mono, mark, strlen(mark));
        desk_text_draw(files->mono, canvas, right_x, baseline, mark, strlen(mark),
                       files->root ? colors->urgent : colors->muted);
    }

    struct entry *entry = cursor_entry(files);
    if (!entry)
        return;
    char mode[11], owner[160], size[48], date[32], left[PATH_MAX + 320];
    format_mode(entry->mode, mode);
    format_owner(entry->owner, entry->group, owner, sizeof owner);
    if (entry->directory) {
        count_items(files, entry);
        long items = files->show_hidden ? entry->items : entry->items - entry->hidden_items;
        if (entry->items == COUNT_UNREADABLE)
            snprintf(size, sizeof size, "unreadable");
        else
            snprintf(size, sizeof size, "%ld %s", items, items == 1 ? "entry" : "entries");
    } else {
        format_size(entry->size, size, sizeof size);
    }
    format_date(&entry->modified, date, sizeof date);
    int length = snprintf(left, sizeof left, "%s  %s  %s  %s", mode, owner, size, date);
    if (entry->link && length > 0 && (size_t)length < sizeof left - 8) {
        char *path = join_path(files->path, entry->name);
        strcat(left, "  → ");
        size_t used = strlen(left);
        ssize_t target = path ? readlink(path, left + used, sizeof left - used - 1) : -1;
        left[target >= 0 ? used + (size_t)target : used] = 0;
        free(path);
    }
    DeskCanvas clipped = *canvas;
    desk_canvas_clip(&clipped, x0, y, right_x - layout->gap, y + layout->row_height);
    desk_text_draw(files->mono, &clipped, x0 + layout->padding / 2, baseline, left, strlen(left), colors->muted);
}

static void paint(struct files *files, DeskCanvas *canvas)
{
    const struct colors *colors = &files->colors;
    double scale = desk_window_scale(&files->window);
    fonts_resize(files);
    DeskFontMetrics metrics = desk_font_metrics(files->font);
    struct layout layout = {
        .row_height = (int)lround(metrics.height * ROW_SPACING),
        .padding = (int)lround(PADDING * scale),
        .gap = (int)lround(GAP * scale),
        .icon_width = (int)lround(metrics.height * 1.4),
        .size_width = desk_text_width(files->mono, "1023 K", 6),
        .date_width = desk_text_width(files->mono, "2026-11-28 14:03", 16),
    };
    layout.baseline = (layout.row_height - (metrics.ascent + metrics.descent)) / 2 + metrics.ascent;
    desk_canvas_fill(canvas, 0, 0, canvas->width, canvas->height, colors->background);

    /* The status line at the bottom, the columns above it. */
    int status_y = canvas->height - layout.row_height - layout.padding / 2;
    int top = layout.padding, bottom = status_y - layout.padding / 2;
    int rows = (bottom - top) / layout.row_height;
    if (rows < 1)
        rows = 1;
    files->page_rows = rows;

    int cell = desk_char_width(files->mono, '0');
    int cells = canvas->width / (cell > 0 ? cell : 1);
    bool parent_shown = cells >= PARENT_CELLS, preview_shown = files->show_preview && cells >= PREVIEW_CELLS;
    double weights[3] = {parent_shown ? files->weights[0] : 0, files->weights[1],
                         preview_shown ? files->weights[2] : 0};
    double total = weights[0] + weights[1] + weights[2];
    int gaps = parent_shown + preview_shown;
    int inner = canvas->width - 2 * layout.padding - gaps * layout.padding;
    int edges[3][2], x = layout.padding;
    for (int i = 0; i < 3; ++i) {
        int width = (int)lround(inner * weights[i] / total);
        edges[i][0] = x;
        edges[i][1] = x + width;
        if (weights[i])
            x += width + layout.padding;
    }
    if (preview_shown)
        edges[2][1] = canvas->width - layout.padding;
    else
        edges[1][1] = canvas->width - layout.padding;

    struct columns *columns = &files->columns;
    *columns = (struct columns){
        .shown = {parent_shown, true, preview_shown},
        .divider = {parent_shown ? edges[0][1] + layout.padding / 2 : -1,
                    preview_shown ? edges[1][1] + layout.padding / 2 : -1},
        .inner = inner,
    };
    for (int i = 0; i < 3; ++i) {
        columns->left[i] = edges[i][0];
        columns->right[i] = edges[i][1];
    }

    uint32_t rule = mix(colors->background, colors->muted, 70);
    int rule_width = scale >= 2 ? (int)scale : 1;
    for (int i = 0; i < 2; ++i) {
        if (columns->divider[i] < 0)
            continue;
        /* The divider under the pointer or being dragged stands out. */
        bool active = files->drag == i || (files->drag < 0 && files->hover == i);
        int width = active ? rule_width * 3 : rule_width;
        desk_canvas_fill(canvas, columns->divider[i] - (width - rule_width) / 2, top, width, bottom - top,
                         active ? colors->directory : rule);
    }
    desk_canvas_fill(canvas, layout.padding, status_y - layout.padding / 4, canvas->width - 2 * layout.padding,
                     rule_width, rule);

    if (parent_shown) {
        DeskCanvas clipped = *canvas;
        desk_canvas_clip(&clipped, edges[0][0], top, edges[0][1], bottom);
        paint_list(files, &clipped, &layout, &files->parent, files->parent_cursor,
                   centered_top(files->parent_cursor, files->parent.shown_count, rows), edges[0][0], edges[0][1],
                   top, rows, false, false, false);
    }

    DeskCanvas current = *canvas;
    desk_canvas_clip(&current, edges[1][0], top, edges[1][1], bottom);
    files->top = keep_visible(files->top, files->cursor, files->listing.shown_count, rows);
    int name_room = edges[1][1] - edges[1][0] - layout.icon_width - layout.padding;
    bool details = name_room > 2 * (layout.size_width + layout.gap + layout.date_width);
    paint_list(files, &current, &layout, &files->listing, files->cursor, files->top, edges[1][0], edges[1][1], top,
               rows, true, details, true);

    if (preview_shown)
        paint_preview(files, canvas, &layout, cursor_entry(files), edges[2][0], edges[2][1], top, bottom);
    paint_status(files, canvas, &layout, layout.padding, canvas->width - layout.padding, status_y);
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
    bool repeats = key_action(files, desk_keyboard_sym(&files->keyboard, key), desk_keyboard_mods(&files->keyboard));
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

/* Pointer: only the dividers for now; clicking entries comes in 0.5. */

static void set_cursor(struct files *files)
{
    if (files->cursor_shape)
        wp_cursor_shape_device_v1_set_shape(files->cursor_shape, files->pointer_serial,
                                            files->drag >= 0 || files->hover >= 0
                                                ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE
                                                : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

/* The pointer's x in buffer pixels. */
static int pointer_buffer_x(const struct files *files)
{
    return (int)lround(files->pointer_x * desk_window_scale(&files->window));
}

static int divider_at(const struct files *files)
{
    int x = pointer_buffer_x(files), reach = (int)lround(DIVIDER_GRAB * desk_window_scale(&files->window));
    for (int i = 0; i < 2; ++i) {
        int divider = files->columns.divider[i];
        if (divider >= 0 && abs(x - divider) <= reach)
            return i;
    }
    return -1;
}

static void update_hover(struct files *files)
{
    int hover = divider_at(files);
    if (hover != files->hover) {
        files->hover = hover;
        set_cursor(files);
        desk_window_redraw(&files->window);
    }
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
    files->hover = -2; /* Forces the cursor to be set. */
    update_hover(files);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    struct files *files = data;
    if (files->drag >= 0)
        save_state(files);
    files->drag = -1;
    files->hover = -1;
    desk_window_redraw(&files->window);
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)time;
    struct files *files = data;
    files->pointer_x = wl_fixed_to_double(x);
    files->pointer_y = wl_fixed_to_double(y);
    if (files->drag >= 0)
        move_divider(files, files->drag, pointer_buffer_x(files));
    else
        update_hover(files);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)serial;
    struct files *files = data;
    if (button != BTN_LEFT)
        return;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        int divider = divider_at(files);
        if (divider < 0)
            return;
        if (divider == files->press_divider && time - files->press_time < DOUBLE_CLICK_MS) {
            files->press_divider = -1;
            reset_columns(files);
            return;
        }
        files->press_divider = divider;
        files->press_time = time;
        files->drag = divider;
    } else if (files->drag >= 0) {
        files->drag = -1;
        save_state(files);
        update_hover(files);
        desk_window_redraw(&files->window);
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data;
    (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)data;
    (void)pointer;
    (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
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
        files->drag = files->hover = -1;
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
        !(files->mono = desk_font_open("monospace", size * 0.9, error, sizeof error))) {
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
            {.fd = files->done_fd, .events = POLLIN},
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
                key_action(files, desk_keyboard_sym(&files->keyboard, files->repeat.key),
                           desk_keyboard_mods(&files->keyboard));
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
        if (files->done_fd >= 0 && fds[5].revents & POLLIN)
            collect_previews(files);
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
          "Keys: j/k move, l enters, h goes up, g/G first/last, Ctrl+D/U half a page,\n"
          "J/K scroll the preview, z hides it, < and > resize the columns (= resets),\n"
          ". hidden files, ~ home, q quits. Drag the dividers to resize with the mouse.\n",
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
        .inotify = -1, .watch = -1, .parent_watch = -1, .refresh = -1, .done_fd = -1, .running = true,
        .show_preview = true, .parent_cursor = SIZE_MAX, .free_space = ULLONG_MAX,
        .hover = -1, .drag = -1, .press_divider = -1, .root = geteuid() == 0,
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

    workers_start(&files);
    go_to(&files, directory, select);
    free(select);
    if (!open_window(&files)) {
        status = EXIT_FAILED;
    } else if (!run(&files)) {
        fprintf(stderr, "files: lost the Wayland connection\n");
        status = EXIT_FAILED;
    }
    close_window(&files);
    workers_stop(&files);

    desk_repeat_close(&files.repeat);
    desk_watch_close(&files.palette_watch);
    if (files.inotify >= 0)
        close(files.inotify);
    if (files.refresh >= 0)
        close(files.refresh);
    listing_free(&files.listing);
    listing_free(&files.parent);
    free(files.preview_path);
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
