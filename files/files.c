#define _GNU_SOURCE
#include "files.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "xdg-shell-client-protocol.h"

#define FONT_POINTS 11.0      /* UI text size, in points at 96 dpi. */
#define ROW_SPACING 1.5       /* Row height as a multiple of the line height. */
#define PADDING 10            /* Logical pixels around the list. */
#define GAP 16                /* Logical pixels between name and details. */
#define SCROLL_MARGIN 3       /* Rows kept visible above and below the cursor. */
#define REFRESH_DELAY_MS 100  /* Changes are gathered this long before rereading. */
#define MAX_REPEATS 8         /* Repeats handled at once after a stall. */

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
        return;
    }
    entry->mode = info.st_mode;
    entry->size = info.st_size;
    entry->modified = info.st_mtim;
    if (S_ISLNK(info.st_mode)) {
        entry->link = true;
        struct stat target;
        if (fstatat(directory, entry->name, &target, 0) != 0) {
            entry->broken = true;
            return;
        }
        info = target;
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

/* Put the cursor on the entry with this name, if shown. */
static bool select_name(struct files *files, const char *name)
{
    struct listing *listing = &files->listing;
    for (size_t i = 0; name && i < listing->shown_count; ++i) {
        if (strcmp(listing->entries[listing->shown[i]].name, name) == 0) {
            files->cursor = i;
            return true;
        }
    }
    return false;
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

/* Navigation */

static void update_title(struct files *files)
{
    char title[PATH_MAX + 2];
    display_path(files, files->path, title, sizeof title);
    desk_window_set_title(&files->window, title);
}

/* Replace the listing with path's, keeping the cursor on keep (a name) when
   it's still there, else near the old position. */
static void load(struct files *files, const char *keep, size_t near)
{
    listing_free(&files->listing);
    listing_read(&files->listing, files->path);
    listing_filter(&files->listing, files->show_hidden);
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
    /* The same entry, or the next one still shown. */
    files->cursor = 0;
    for (size_t i = 0; i < listing->shown_count; ++i) {
        files->cursor = i;
        if (listing->shown[i] >= old)
            break;
    }
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

/* Today as the time, this year as day and month, older as the date. */
static void format_date(const struct timespec *when, const struct tm *now, char *out, size_t length)
{
    struct tm local;
    if (when->tv_sec == 0 || !localtime_r(&when->tv_sec, &local)) {
        snprintf(out, length, "-");
        return;
    }
    const char *format = local.tm_year != now->tm_year ? "%Y-%m-%d"
                       : local.tm_yday != now->tm_yday ? "%d %b"
                                                       : "%H:%M";
    if (!strftime(out, length, format, &local))
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

/* Keep the cursor on screen with a margin, and the list filling the window. */
static void scroll_to_cursor(struct files *files, int rows)
{
    size_t count = files->listing.shown_count;
    size_t margin = (size_t)(rows / 4 < SCROLL_MARGIN ? rows / 4 : SCROLL_MARGIN);
    if (files->cursor < files->top + margin)
        files->top = files->cursor > margin ? files->cursor - margin : 0;
    if (files->cursor + margin + 1 > files->top + (size_t)rows)
        files->top = files->cursor + margin + 1 - (size_t)rows;
    if (count <= (size_t)rows)
        files->top = 0;
    else if (files->top > count - (size_t)rows)
        files->top = count - (size_t)rows;
}

static void paint(struct files *files, DeskCanvas *canvas)
{
    const struct colors *colors = &files->colors;
    double scale = desk_window_scale(&files->window);
    fonts_resize(files);
    DeskFontMetrics metrics = desk_font_metrics(files->font);
    int row_height = (int)lround(metrics.height * ROW_SPACING);
    int padding = (int)lround(PADDING * scale), gap = (int)lround(GAP * scale);
    int baseline_offset = (row_height - (metrics.ascent + metrics.descent)) / 2 + metrics.ascent;

    desk_canvas_fill(canvas, 0, 0, canvas->width, canvas->height, colors->background);
    int rows = (canvas->height - 2 * padding) / row_height;
    if (rows < 1)
        rows = 1;
    files->page_rows = rows;

    struct listing *listing = &files->listing;
    if (listing->error || listing->shown_count == 0) {
        char message[256];
        if (listing->error)
            snprintf(message, sizeof message, "%s", strerror(listing->error));
        else
            snprintf(message, sizeof message, listing->count ? "Only hidden files (. shows them)" : "Empty");
        desk_text_draw(files->italic, canvas, padding * 2, padding + baseline_offset, message, strlen(message),
                       listing->error ? colors->urgent : colors->muted);
        return;
    }

    scroll_to_cursor(files, rows);
    time_t now_seconds = time(NULL);
    struct tm now;
    localtime_r(&now_seconds, &now);

    /* Details column: size or count, then the date, right-aligned in fixed widths. */
    int size_width = desk_text_width(files->mono, "1023 K", 6);
    int date_width = desk_text_width(files->mono, "2026-11-28", 10);
    int details_width = size_width + gap + date_width;
    int icon_width = (int)lround(metrics.height * 1.4);
    int left = padding, right = canvas->width - padding;
    int name_x = left + padding / 2 + icon_width;
    bool show_details = right - name_x > details_width * 2;
    int name_right = show_details ? right - details_width - gap - padding / 2 : right - padding / 2;

    for (int row = 0; row < rows && files->top + (size_t)row < listing->shown_count; ++row) {
        size_t index = files->top + (size_t)row;
        struct entry *entry = &listing->entries[listing->shown[index]];
        bool selected = index == files->cursor;
        int y = padding + row * row_height, baseline = y + baseline_offset;
        if (selected)
            desk_canvas_fill(canvas, left, y, right - left, row_height, colors->selection_background);

        uint32_t color = entry->broken      ? colors->urgent
                       : entry->hidden      ? colors->muted
                       : entry->directory   ? colors->directory
                                            : colors->foreground;
        if (selected && !entry->broken)
            color = colors->selection_foreground;
        uint32_t detail_color = selected ? colors->selection_foreground : colors->muted;

        char icon[8];
        const char *icon_text = icon;
        uint32_t code = entry_icon(entry);
        icon[0] = (char)(0xe0 | code >> 12);
        icon[1] = (char)(0x80 | (code >> 6 & 0x3f));
        icon[2] = (char)(0x80 | (code & 0x3f));
        int glyph_width = desk_char_width(files->font, code);
        desk_text_draw(files->font, canvas, left + padding / 2 + (icon_width - glyph_width) / 2 - padding / 4,
                       baseline, icon_text, 3, color);

        desk_canvas_clip(canvas, name_x, y, name_right, y + row_height);
        draw_name(files, entry->link ? files->italic : files->font, canvas, name_x, baseline,
                  name_right - name_x, entry->name, entry->directory, color);
        desk_canvas_clip(canvas, 0, 0, canvas->width, canvas->height);

        if (!show_details)
            continue;
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
        format_date(&entry->modified, &now, date, sizeof date);
        int date_x = right - padding / 2 - desk_text_width(files->mono, date, strlen(date));
        int size_x = right - padding / 2 - date_width - gap - desk_text_width(files->mono, size, strlen(size));
        desk_text_draw(files->mono, canvas, size_x, baseline, size, strlen(size), detail_color);
        desk_text_draw(files->mono, canvas, date_x, baseline, date, strlen(date), detail_color);
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
    if (strcmp(interface, wl_seat_interface.name) == 0 && !files->seat) {
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
        struct pollfd fds[5] = {
            {.fd = wl_display_get_fd(display), .events = POLLIN},
            {.fd = files->repeat.fd, .events = POLLIN},
            {.fd = files->inotify, .events = POLLIN},
            {.fd = files->refresh, .events = POLLIN},
            {.fd = files->palette_watch.fd, .events = POLLIN},
        };
        if (poll(fds, 5, -1) < 0) {
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
          ". hidden files, ~ home, q quits.\n",
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
        .inotify = -1, .watch = -1, .refresh = -1, .running = true,
        .palette_watch = {.fd = -1}, .repeat = {.fd = -1},
    };
    const char *home = getenv("HOME");
    if (home && home[0] == '/') {
        files.home = strdup(home);
        size_t length = files.home ? strlen(files.home) : 0;
        while (length > 1 && files.home[length - 1] == '/')
            files.home[--length] = 0;
    }
    char *directory, *select = NULL;
    int status = start_at(optind < argc ? argv[optind] : files.home ? files.home : "/", &directory, &select);
    if (status) {
        free(files.home);
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

    go_to(&files, directory, select);
    free(select);
    if (!open_window(&files)) {
        status = EXIT_FAILED;
    } else if (!run(&files)) {
        fprintf(stderr, "files: lost the Wayland connection\n");
        status = EXIT_FAILED;
    }
    close_window(&files);

    desk_repeat_close(&files.repeat);
    desk_watch_close(&files.palette_watch);
    if (files.inotify >= 0)
        close(files.inotify);
    if (files.refresh >= 0)
        close(files.refresh);
    listing_free(&files.listing);
    for (size_t i = 0; i < files.memory_count; ++i) {
        free(files.memories[i].path);
        free(files.memories[i].name);
    }
    free(files.memories);
    free(files.palette_path);
    free(files.path);
    free(files.home);
    return status;
}
