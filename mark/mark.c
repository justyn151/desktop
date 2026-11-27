#define _GNU_SOURCE
#include "mark.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

/* Colors until 0.5 brings the palette. deal derives the sixteen ANSI colors
   from the wallpaper, so these follow it already: 8 is the muted role and
   0 is the background shifted a little, which shades code. */
enum {
    COLOR_ACCENT = 4,
    COLOR_HIGHLIGHT = 3,
    COLOR_MUTED = 8,
    COLOR_SHADE = 0,
};

/* Memory */

static void out_of_memory(void)
{
    fputs("mark: out of memory\n", stderr);
    exit(EXIT_FAILED);
}

static void *xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

static void *xrealloc(void *p, size_t size)
{
    p = realloc(p, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

static char *xstrdup(const char *s)
{
    char *copy = strdup(s);
    if (!copy) out_of_memory();
    return copy;
}

/* Make room for need items in a vector. */
static void grow(void *items, size_t *cap, size_t need, size_t size)
{
    if (need <= *cap) return;
    size_t next = *cap ? *cap : 16;
    while (next < need) next *= 2;
    *(void **)items = xrealloc(*(void **)items, next * size);
    *cap = next;
}

static void buf_reserve(struct buf *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) grow(&b->data, &b->cap, b->len + extra + 1, 1);
}

static void buf_put(struct buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_putc(struct buf *b, char c)
{
    buf_put(b, &c, 1);
}

static void buf_puts(struct buf *b, const char *s)
{
    buf_put(b, s, strlen(s));
}

static void buf_free(struct buf *b)
{
    free(b->data);
    *b = (struct buf){0};
}

struct arena_block {
    struct arena_block *next;
    size_t used, size;
    max_align_t data[];
};

#define ARENA_BLOCK 65536

static void *arena_alloc(struct arena *a, size_t size)
{
    size = (size + 15) & ~(size_t)15;
    struct arena_block *b = a->head;
    if (!b || b->size - b->used < size) {
        size_t capacity = size > ARENA_BLOCK ? size : ARENA_BLOCK;
        b = xmalloc(sizeof *b + capacity);
        b->used = 0;
        b->size = capacity;
        b->next = a->head;
        a->head = b;
    }
    void *p = (char *)b->data + b->used;
    b->used += size;
    memset(p, 0, size);
    return p;
}

static char *arena_strndup(struct arena *a, const char *s, size_t n)
{
    char *copy = arena_alloc(a, n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

static void arena_free(struct arena *a)
{
    while (a->head) {
        struct arena_block *next = a->head->next;
        free(a->head);
        a->head = next;
    }
}

/* UTF-8 and character classes */

/* Length of the valid sequence at s, or 0. */
static size_t utf8_valid(const unsigned char *s, size_t n)
{
    unsigned c = s[0];
    if (c < 0x80) return 1;
    size_t len;
    uint32_t cp, min;
    if (c >= 0xc2 && c <= 0xdf) len = 2, cp = c & 0x1f, min = 0x80;
    else if (c >= 0xe0 && c <= 0xef) len = 3, cp = c & 0x0f, min = 0x800;
    else if (c >= 0xf0 && c <= 0xf4) len = 4, cp = c & 0x07, min = 0x10000;
    else return 0;
    if (n < len) return 0;
    for (size_t i = 1; i < len; ++i) {
        if ((s[i] & 0xc0) != 0x80) return 0;
        cp = cp << 6 | (s[i] & 0x3f);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    return len;
}

/* Decode valid UTF-8 (the input is normalized first). */
static size_t utf8_decode(const char *str, size_t n, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)str;
    size_t len = utf8_valid(s, n);
    if (len == 0) {
        *cp = 0xfffd;
        return 1;
    }
    if (len == 1) {
        *cp = s[0];
        return 1;
    }
    uint32_t value = s[0] & (len == 2 ? 0x1f : len == 3 ? 0x0f : 0x07);
    for (size_t i = 1; i < len; ++i) value = value << 6 | (s[i] & 0x3f);
    *cp = value;
    return len;
}

static size_t utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3f));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* The code point that ends just before pos, or '\n' at the start. */
static uint32_t utf8_before(const char *s, size_t pos)
{
    if (pos == 0) return '\n';
    size_t start = pos - 1;
    while (start > 0 && pos - start < 4 && ((unsigned char)s[start] & 0xc0) == 0x80) --start;
    uint32_t cp;
    utf8_decode(s + start, pos - start, &cp);
    return cp;
}

static bool is_space_or_tab(int c)
{
    return c == ' ' || c == '\t';
}

static bool is_line_end(int c)
{
    return c == '\n' || c == '\0';
}

static bool is_ascii_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_ascii_digit(int c)
{
    return c >= '0' && c <= '9';
}

static bool is_ascii_alnum(int c)
{
    return is_ascii_alpha(c) || is_ascii_digit(c);
}

static bool is_ascii_punct(int c)
{
    return (c >= 0x21 && c <= 0x2f) || (c >= 0x3a && c <= 0x40) || (c >= 0x5b && c <= 0x60) ||
           (c >= 0x7b && c <= 0x7e);
}

/* Whitespace as the spec counts it: spaces, tabs, line endings, form feed. */
static bool is_whitespace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool is_unicode_space(uint32_t c)
{
    if (c < 0x80) return is_whitespace((int)c);
    return c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x202f || c == 0x205f ||
           c == 0x3000;
}

/* Unicode punctuation and symbols (categories P and S). */
static bool is_unicode_punct(uint32_t c)
{
    if (c < 0x80) return is_ascii_punct((int)c);
    return iswpunct((wint_t)c) && !is_unicode_space(c);
}

/* Input */

/* Replace invalid UTF-8 and NUL with U+FFFD, turn CRLF and CR into LF,
   drop a byte order mark and make sure the text ends with a newline. */
static char *normalize_input(const char *in, size_t n, size_t *out_len)
{
    struct buf out = {0};
    buf_reserve(&out, n + 1);
    const unsigned char *s = (const unsigned char *)in;
    size_t i = 0;
    if (n >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) i = 3;
    while (i < n) {
        unsigned char c = s[i];
        if (c == '\r') {
            buf_putc(&out, '\n');
            i += i + 1 < n && s[i + 1] == '\n' ? 2 : 1;
        } else if (c == '\0') {
            buf_put(&out, "\xef\xbf\xbd", 3);
            ++i;
        } else if (c < 0x80) {
            size_t start = i;
            while (i < n && s[i] < 0x80 && s[i] != '\r' && s[i] != '\0') ++i;
            buf_put(&out, in + start, i - start);
        } else {
            size_t len = utf8_valid(s + i, n - i);
            if (len) {
                buf_put(&out, in + i, len);
                i += len;
            } else {
                buf_put(&out, "\xef\xbf\xbd", 3);
                ++i;
            }
        }
    }
    if (out.len == 0 || out.data[out.len - 1] != '\n') buf_putc(&out, '\n');
    *out_len = out.len;
    return out.data;
}

/* Entities. A handful of named ones plus every numeric reference; the full
   HTML table has over two thousand names. */
static const struct {
    const char *name;
    const char *text;
} entities[] = {
    {"AElig", "Æ"}, {"Aacute", "Á"}, {"Agrave", "À"}, {"Auml", "Ä"}, {"Ccedil", "Ç"},
    {"ClockwiseContourIntegral", "∲"}, {"Dcaron", "Ď"}, {"DifferentialD", "ⅆ"}, {"Eacute", "É"},
    {"HilbertSpace", "ℋ"}, {"Ouml", "Ö"}, {"Uuml", "Ü"}, {"aacute", "á"}, {"acirc", "â"},
    {"agrave", "à"}, {"amp", "&"}, {"apos", "'"}, {"auml", "ä"}, {"bull", "•"}, {"ccedil", "ç"},
    {"cent", "¢"}, {"check", "✓"}, {"copy", "©"}, {"darr", "↓"}, {"deg", "°"}, {"divide", "÷"},
    {"eacute", "é"}, {"ecirc", "ê"}, {"egrave", "è"}, {"emsp", "\xe2\x80\x83"}, {"ensp", "\xe2\x80\x82"},
    {"euml", "ë"}, {"euro", "€"}, {"frac12", "½"}, {"frac14", "¼"}, {"frac34", "¾"}, {"ge", "≥"},
    {"gt", ">"}, {"harr", "↔"}, {"hearts", "♥"}, {"hellip", "…"}, {"iacute", "í"}, {"infin", "∞"},
    {"iuml", "ï"}, {"laquo", "«"}, {"larr", "←"}, {"ldquo", "“"}, {"le", "≤"}, {"lsquo", "‘"},
    {"lt", "<"}, {"mdash", "—"}, {"micro", "µ"}, {"middot", "·"}, {"minus", "−"},
    {"nbsp", "\xc2\xa0"}, {"ndash", "–"}, {"ne", "≠"}, {"ngE", "≧̸"}, {"not", "¬"}, {"ntilde", "ñ"},
    {"oacute", "ó"}, {"ocirc", "ô"}, {"ouml", "ö"}, {"para", "¶"}, {"plusmn", "±"}, {"pound", "£"},
    {"quot", "\""}, {"raquo", "»"}, {"rarr", "→"}, {"rdquo", "”"}, {"reg", "®"}, {"rsquo", "’"},
    {"sect", "§"}, {"shy", "\xc2\xad"}, {"sup2", "²"}, {"sup3", "³"}, {"szlig", "ß"},
    {"thinsp", "\xe2\x80\x89"}, {"times", "×"}, {"trade", "™"}, {"uacute", "ú"}, {"uarr", "↑"},
    {"uuml", "ü"}, {"yen", "¥"},
};

/* Decode the entity at s ('&'), appending its text. Returns the length
   consumed, or 0 when it isn't one. */
static size_t decode_entity(const char *s, size_t n, struct buf *out)
{
    if (n < 3 || s[0] != '&') return 0;
    if (s[1] == '#') {
        size_t i = 2;
        bool hex = i < n && (s[i] == 'x' || s[i] == 'X');
        if (hex) ++i;
        size_t start = i;
        uint32_t cp = 0;
        while (i < n && i - start < (hex ? 6u : 7u) &&
               (hex ? isxdigit((unsigned char)s[i]) : is_ascii_digit(s[i]))) {
            int digit = is_ascii_digit(s[i]) ? s[i] - '0' : (s[i] | 0x20) - 'a' + 10;
            cp = cp * (hex ? 16 : 10) + (uint32_t)digit;
            ++i;
        }
        if (i == start || i >= n || s[i] != ';') return 0;
        if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
        char bytes[4];
        buf_put(out, bytes, utf8_encode(cp, bytes));
        return i + 1;
    }
    size_t i = 1;
    while (i < n && i <= 32 && is_ascii_alnum(s[i])) ++i;
    if (i == 1 || i >= n || s[i] != ';') return 0;
    for (size_t e = 0; e < sizeof entities / sizeof entities[0]; ++e) {
        if (strlen(entities[e].name) == i - 1 && memcmp(entities[e].name, s + 1, i - 1) == 0) {
            buf_puts(out, entities[e].text);
            return i + 1;
        }
    }
    return 0;
}

/* Resolve backslash escapes and entities. */
static char *unescape(struct arena *a, const char *s, size_t n)
{
    struct buf out = {0};
    buf_reserve(&out, n);
    for (size_t i = 0; i < n;) {
        if (s[i] == '\\' && i + 1 < n && is_ascii_punct(s[i + 1])) {
            buf_putc(&out, s[i + 1]);
            i += 2;
        } else if (s[i] == '&') {
            size_t used = decode_entity(s + i, n - i, &out);
            if (used) {
                i += used;
            } else {
                buf_putc(&out, '&');
                ++i;
            }
        } else {
            size_t start = i;
            while (i < n && s[i] != '\\' && s[i] != '&') ++i;
            if (i == start) ++i;
            buf_put(&out, s + start, i - start);
        }
    }
    char *result = arena_strndup(a, out.data ? out.data : "", out.len);
    buf_free(&out);
    return result;
}

/* Link scanning shared by inline links and reference definitions */

static size_t skip_spacechars(const char *s, size_t n, size_t i)
{
    while (i < n && is_whitespace(s[i])) ++i;
    return i;
}

/* A link destination at s[i]. On success returns the end and sets the
   destination's bytes (without angle brackets). */
static bool scan_link_url(const char *s, size_t n, size_t i, size_t *end, size_t *start_out, size_t *len_out)
{
    if (i < n && s[i] == '<') {
        size_t j = i + 1;
        while (j < n) {
            if (s[j] == '>') {
                *start_out = i + 1;
                *len_out = j - i - 1;
                *end = j + 1;
                return true;
            }
            if (s[j] == '\\' && j + 1 < n && is_ascii_punct(s[j + 1])) j += 2;
            else if (s[j] == '\n' || s[j] == '<') return false;
            else ++j;
        }
        return false;
    }
    size_t j = i;
    int parens = 0;
    while (j < n) {
        unsigned char c = (unsigned char)s[j];
        if (c == '\\' && j + 1 < n && is_ascii_punct(s[j + 1])) {
            j += 2;
        } else if (c == '(') {
            if (++parens > MAX_LINK_PARENS) return false;
            ++j;
        } else if (c == ')') {
            if (parens == 0) break;
            --parens;
            ++j;
        } else if (c <= 0x20 || c == 0x7f) {
            break;
        } else {
            ++j;
        }
    }
    if (j == i || parens != 0) return false;
    *start_out = i;
    *len_out = j - i;
    *end = j;
    return true;
}

/* A link title at s[i]: "…", '…' or (…). Returns its length or 0. */
static size_t scan_link_title(const char *s, size_t n, size_t i)
{
    if (i >= n) return 0;
    char open = s[i];
    char close = open == '(' ? ')' : open;
    if (open != '"' && open != '\'' && open != '(') return 0;
    for (size_t j = i + 1; j < n; ++j) {
        if (s[j] == '\\' && j + 1 < n && is_ascii_punct(s[j + 1])) {
            ++j;
        } else if (s[j] == close) {
            return j + 1 - i;
        } else if (open == '(' && s[j] == '(') {
            return 0;
        }
    }
    return 0;
}

/* A link label at s[i] ('['). Sets the trimmed inner text. */
static bool scan_link_label(const char *s, size_t n, size_t i, size_t *end, size_t *start_out, size_t *len_out)
{
    if (i >= n || s[i] != '[') return false;
    size_t j = i + 1, length = 0;
    while (j < n && s[j] != '[' && s[j] != ']') {
        if (s[j] == '\\' && j + 1 < n && is_ascii_punct(s[j + 1])) {
            j += 2;
            length += 2;
        } else {
            ++j;
            ++length;
        }
        if (length > MAX_LINK_LABEL) return false;
    }
    if (j >= n || s[j] != ']') return false;
    size_t a = i + 1, b = j;
    while (a < b && is_whitespace(s[a])) ++a;
    while (b > a && is_whitespace(s[b - 1])) --b;
    *start_out = a;
    *len_out = b - a;
    *end = j + 1;
    return true;
}

/* Case-fold and collapse whitespace, for matching labels. */
static void normalize_label(const char *s, size_t n, struct buf *out)
{
    out->len = 0;
    bool space = false;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        if (is_unicode_space(cp)) {
            space = out->len > 0;
            continue;
        }
        if (space) buf_putc(out, ' ');
        space = false;
        if (cp == 0xdf || cp == 0x1e9e) {
            buf_put(out, "ss", 2);
            continue;
        }
        if (cp >= 0x80) cp = (uint32_t)towlower(towupper((wint_t)cp));
        else if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
        char bytes[4];
        buf_put(out, bytes, utf8_encode(cp, bytes));
    }
    buf_reserve(out, 0);
    out->data[out->len] = '\0';
}

static uint64_t hash_string(const char *s)
{
    uint64_t h = 1469598103934665603u;
    for (; *s; ++s) h = (h ^ (unsigned char)*s) * 1099511628211u;
    return h;
}

static struct ref *refmap_slot(struct refmap *map, const char *label)
{
    size_t mask = map->cap - 1;
    for (size_t i = hash_string(label) & mask;; i = (i + 1) & mask) {
        if (!map->slots[i].label || strcmp(map->slots[i].label, label) == 0) return &map->slots[i];
    }
}

static const struct ref *refmap_find(struct refmap *map, const char *s, size_t n)
{
    if (map->count == 0 || n > MAX_LINK_LABEL) return NULL;
    struct buf label = {0};
    normalize_label(s, n, &label);
    const struct ref *found = NULL;
    if (label.len > 0) {
        struct ref *slot = refmap_slot(map, label.data);
        if (slot->label) found = slot;
    }
    buf_free(&label);
    return found;
}

/* The first definition of a label wins. */
static void refmap_add(struct document *doc, const char *s, size_t n, char *url, char *title)
{
    struct refmap *map = &doc->refs;
    if ((map->count + 1) * 2 > map->cap) {
        struct refmap bigger = {.cap = map->cap ? map->cap * 2 : 64, .count = map->count};
        bigger.slots = xmalloc(bigger.cap * sizeof *bigger.slots);
        memset(bigger.slots, 0, bigger.cap * sizeof *bigger.slots);
        for (size_t i = 0; i < map->cap; ++i) {
            if (map->slots[i].label) *refmap_slot(&bigger, map->slots[i].label) = map->slots[i];
        }
        free(map->slots);
        *map = bigger;
    }
    struct buf label = {0};
    normalize_label(s, n, &label);
    if (label.len > 0) {
        struct ref *slot = refmap_slot(map, label.data);
        if (!slot->label) {
            slot->label = arena_strndup(&doc->arena, label.data, label.len);
            slot->url = url;
            slot->title = title;
            ++map->count;
        }
    }
    buf_free(&label);
}

/* Parse one link reference definition at the start of s. Returns the bytes
   it used (through its line ending), or 0. */
static size_t parse_reference(struct document *doc, const char *s, size_t n)
{
    size_t i, label_start, label_len;
    if (!scan_link_label(s, n, 0, &i, &label_start, &label_len) || label_len == 0) return 0;
    if (i >= n || s[i] != ':') return 0;
    ++i;
    /* Spaces, at most one line ending, spaces. */
    while (i < n && is_space_or_tab(s[i])) ++i;
    if (i < n && s[i] == '\n') ++i;
    while (i < n && is_space_or_tab(s[i])) ++i;
    size_t url_end, url_start, url_len;
    if (!scan_link_url(s, n, i, &url_end, &url_start, &url_len)) return 0;
    if (s[i] != '<' && url_len == 0) return 0;
    i = url_end;
    size_t before_title = i;
    while (i < n && is_space_or_tab(s[i])) ++i;
    if (i < n && s[i] == '\n') ++i;
    while (i < n && is_space_or_tab(s[i])) ++i;
    size_t title_len = i == before_title ? 0 : scan_link_title(s, n, i);
    size_t title_start = i;
    if (title_len) i += title_len;
    else i = before_title;
    while (i < n && is_space_or_tab(s[i])) ++i;
    if (i < n && s[i] != '\n') {
        if (!title_len) return 0;
        /* The title isn't followed by the line's end: try without it. */
        title_len = 0;
        i = before_title;
        while (i < n && is_space_or_tab(s[i])) ++i;
        if (i < n && s[i] != '\n') return 0;
    }
    if (i < n) ++i;
    char *url = unescape(&doc->arena, s + url_start, url_len);
    char *title = title_len ? unescape(&doc->arena, s + title_start + 1, title_len - 2) : NULL;
    refmap_add(doc, s + label_start, label_len, url, title);
    return i;
}

/* HTML scanning, for HTML blocks and inline raw HTML */

static size_t scan_tag_name(const char *s, size_t n, size_t i)
{
    if (i >= n || !is_ascii_alpha(s[i])) return 0;
    while (i < n && (is_ascii_alnum(s[i]) || s[i] == '-')) ++i;
    return i;
}

struct html_memo {
    bool no_comment_end, no_pi_end, no_declaration_end, no_cdata_end;
    bool no_double_quote, no_single_quote;
};

/* Find the needle after s[i]; remembers when it can't be found. */
static size_t find_after(const char *s, size_t n, size_t i, const char *needle, bool *missing)
{
    if (*missing) return 0;
    size_t m = strlen(needle);
    const char *hit = i <= n ? memmem(s + i, n - i, needle, m) : NULL;
    if (!hit) {
        *missing = true;
        return 0;
    }
    return (size_t)(hit - s) + m;
}

static size_t scan_open_tag(const char *s, size_t n, struct html_memo *memo)
{
    size_t i = scan_tag_name(s, n, 1);
    if (!i) return 0;
    for (;;) {
        size_t before = i;
        while (i < n && is_whitespace(s[i])) ++i;
        if (i == before || i >= n || !(is_ascii_alpha(s[i]) || s[i] == '_' || s[i] == ':')) {
            i = before;
            break;
        }
        while (i < n && (is_ascii_alnum(s[i]) || s[i] == '_' || s[i] == '.' || s[i] == ':' || s[i] == '-')) ++i;
        size_t after_name = i;
        while (i < n && is_whitespace(s[i])) ++i;
        if (i < n && s[i] == '=') {
            ++i;
            while (i < n && is_whitespace(s[i])) ++i;
            if (i >= n) return 0;
            if (s[i] == '"' || s[i] == '\'') {
                bool *missing = s[i] == '"' ? &memo->no_double_quote : &memo->no_single_quote;
                size_t end = find_after(s, n, i + 1, s[i] == '"' ? "\"" : "'", missing);
                if (!end) return 0;
                i = end;
            } else {
                size_t start = i;
                while (i < n && !is_whitespace(s[i]) && !strchr("\"'=<>`", s[i])) ++i;
                if (i == start) return 0;
            }
        } else {
            i = after_name;
        }
    }
    while (i < n && is_whitespace(s[i])) ++i;
    if (i < n && s[i] == '/') ++i;
    return i < n && s[i] == '>' ? i + 1 : 0;
}

static size_t scan_closing_tag(const char *s, size_t n)
{
    if (n < 3 || s[1] != '/') return 0;
    size_t i = scan_tag_name(s, n, 2);
    if (!i) return 0;
    while (i < n && is_whitespace(s[i])) ++i;
    return i < n && s[i] == '>' ? i + 1 : 0;
}

/* Inline raw HTML at s ('<'): a tag, comment, processing instruction,
   declaration or CDATA section. Returns its length or 0. */
static size_t scan_inline_html(const char *s, size_t n, struct html_memo *memo)
{
    if (n < 2) return 0;
    if (s[1] == '/') return scan_closing_tag(s, n);
    if (s[1] == '?') return find_after(s, n, 2, "?>", &memo->no_pi_end);
    if (s[1] == '!') {
        if (n >= 4 && s[2] == '-' && s[3] == '-') {
            if (n >= 5 && s[4] == '>') return 5;
            if (n >= 6 && s[4] == '-' && s[5] == '>') return 6;
            return find_after(s, n, 4, "-->", &memo->no_comment_end);
        }
        if (n >= 9 && memcmp(s + 2, "[CDATA[", 7) == 0) return find_after(s, n, 9, "]]>", &memo->no_cdata_end);
        if (n >= 3 && is_ascii_alpha(s[2])) return find_after(s, n, 3, ">", &memo->no_declaration_end);
        return 0;
    }
    return scan_open_tag(s, n, memo);
}

static const char *const block_tags[] = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body", "caption", "center", "col",
    "colgroup", "dd", "details", "dialog", "dir", "div", "dl", "dt", "fieldset", "figcaption", "figure",
    "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hr",
    "html", "iframe", "legend", "li", "link", "main", "menu", "menuitem", "nav", "noframes", "ol",
    "optgroup", "option", "p", "param", "search", "section", "summary", "table", "tbody", "td", "tfoot",
    "th", "thead", "title", "tr", "track", "ul",
};

static bool tag_is(const char *s, size_t len, const char *name)
{
    return strlen(name) == len && strncasecmp(s, name, len) == 0;
}

/* The HTML block type (1 to 7) starting at s, or 0. Type 7 can't
   interrupt a paragraph. s runs through the line's '\n'. */
static int scan_html_block_start(const char *s, size_t n, bool allow_type_7)
{
    if (n < 2 || s[0] != '<') return 0;
    static const char *const raw[] = {"script", "pre", "style", "textarea"};
    for (size_t t = 0; t < 4; ++t) {
        size_t len = strlen(raw[t]);
        if (n > len + 1 && strncasecmp(s + 1, raw[t], len) == 0 &&
            (is_space_or_tab(s[len + 1]) || s[len + 1] == '>' || s[len + 1] == '\n')) {
            return 1;
        }
    }
    if (n >= 4 && memcmp(s, "<!--", 4) == 0) return 2;
    if (s[1] == '?') return 3;
    if (n >= 9 && memcmp(s, "<![CDATA[", 9) == 0) return 5;
    if (n >= 3 && s[1] == '!' && is_ascii_alpha(s[2])) return 4;
    size_t start = s[1] == '/' ? 2 : 1;
    size_t end = start;
    while (end < n && is_ascii_alnum(s[end])) ++end;
    if (end > start) {
        for (size_t t = 0; t < sizeof block_tags / sizeof block_tags[0]; ++t) {
            if (tag_is(s + start, end - start, block_tags[t]) &&
                (is_space_or_tab(s[end]) || s[end] == '\n' || s[end] == '>' ||
                 (s[end] == '/' && end + 1 < n && s[end + 1] == '>'))) {
                return 6;
            }
        }
    }
    if (!allow_type_7) return 0;
    struct html_memo memo = {0};
    size_t len = s[1] == '/' ? scan_closing_tag(s, n) : scan_open_tag(s, n, &memo);
    if (!len) return 0;
    size_t name_end = scan_tag_name(s, n, start);
    for (size_t t = 0; t < 4; ++t) {
        if (tag_is(s + start, name_end - start, raw[t])) return 0;
    }
    for (size_t i = len; i < n; ++i) {
        if (s[i] == '\n') return 7;
        if (!is_space_or_tab(s[i])) return 0;
    }
    return 0;
}

static bool html_block_ends(int type, const char *s, size_t n)
{
    switch (type) {
    case 1:
        for (size_t i = 0; i + 2 < n; ++i) {
            if (s[i] != '<' || s[i + 1] != '/') continue;
            static const char *const raw[] = {"script>", "pre>", "style>", "textarea>"};
            for (size_t t = 0; t < 4; ++t) {
                size_t len = strlen(raw[t]);
                if (i + 2 + len <= n && strncasecmp(s + i + 2, raw[t], len) == 0) return true;
            }
        }
        return false;
    case 2: return memmem(s, n, "-->", 3) != NULL;
    case 3: return memmem(s, n, "?>", 2) != NULL;
    case 4: return memchr(s, '>', n) != NULL;
    case 5: return memmem(s, n, "]]>", 3) != NULL;
    default: return false;
    }
}

/* Block parsing, following the strategy in the spec's appendix */

struct parser {
    struct document *doc;
    struct block *current; /* The deepest open block. */
    const char *line;      /* Through its '\n'. */
    size_t len;
    size_t offset, first_nonspace;
    int column, first_nonspace_column, indent;
    bool blank, partially_consumed_tab;
    int line_number;
};

static char peek(const struct parser *p, size_t i)
{
    return i < p->len ? p->line[i] : '\0';
}

static void find_first_nonspace(struct parser *p)
{
    size_t i = p->offset;
    int column = p->column;
    int to_tab = TAB_STOP - column % TAB_STOP;
    for (;;) {
        char c = peek(p, i);
        if (c == ' ') {
            ++i;
            ++column;
            if (--to_tab == 0) to_tab = TAB_STOP;
        } else if (c == '\t') {
            ++i;
            column += to_tab;
            to_tab = TAB_STOP;
        } else {
            break;
        }
    }
    p->first_nonspace = i;
    p->first_nonspace_column = column;
    p->indent = column - p->column;
    p->blank = is_line_end(peek(p, i));
}

/* Advance by count bytes, or by count columns, splitting a tab if needed. */
static void advance_offset(struct parser *p, int count, bool columns)
{
    while (count > 0) {
        char c = peek(p, p->offset);
        if (c == '\0') break;
        if (c == '\t') {
            int to_tab = TAB_STOP - p->column % TAB_STOP;
            if (columns) {
                p->partially_consumed_tab = to_tab > count;
                int step = to_tab < count ? to_tab : count;
                p->column += step;
                p->offset += p->partially_consumed_tab ? 0 : 1;
                count -= step;
            } else {
                p->partially_consumed_tab = false;
                p->column += to_tab;
                p->offset += 1;
                count -= 1;
            }
        } else {
            p->partially_consumed_tab = false;
            p->offset += 1;
            p->column += 1;
            count -= 1;
        }
    }
}

static void advance_to(struct parser *p, size_t position)
{
    advance_offset(p, (int)(position - p->offset), false);
}

static void add_line(struct parser *p, struct block *b)
{
    if (p->partially_consumed_tab) {
        ++p->offset;
        int to_tab = TAB_STOP - p->column % TAB_STOP;
        for (int i = 0; i < to_tab; ++i) buf_putc(&b->content, ' ');
    }
    if (p->offset < p->len) buf_put(&b->content, p->line + p->offset, p->len - p->offset);
}

static bool can_contain(enum block_type parent, enum block_type child)
{
    switch (parent) {
    case B_DOCUMENT:
    case B_QUOTE:
    case B_ITEM: return child != B_ITEM && child != B_ROW;
    case B_LIST: return child == B_ITEM;
    case B_TABLE: return child == B_ROW;
    default: return false;
    }
}

static bool accepts_lines(enum block_type type)
{
    return type == B_PARAGRAPH || type == B_HEADING || type == B_CODE || type == B_TABLE;
}

static void append_block(struct block *parent, struct block *child)
{
    child->parent = parent;
    child->prev = parent->last;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

static void remove_block(struct block *b)
{
    struct block *parent = b->parent;
    if (b->prev) b->prev->next = b->next;
    else parent->first = b->next;
    if (b->next) b->next->prev = b->prev;
    else parent->last = b->prev;
    buf_free(&b->content);
}

static bool is_blank_text(const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (!is_whitespace(s[i])) return false;
    }
    return true;
}

/* Strip link reference definitions from the start of a paragraph. Returns
   whether anything is left. */
static bool resolve_references(struct document *doc, struct block *b)
{
    size_t pos = 0;
    while (pos < b->content.len && b->content.data[pos] == '[') {
        size_t used = parse_reference(doc, b->content.data + pos, b->content.len - pos);
        if (!used) break;
        pos += used;
    }
    if (pos > 0) {
        memmove(b->content.data, b->content.data + pos, b->content.len - pos);
        b->content.len -= pos;
        b->content.data[b->content.len] = '\0';
    }
    return !is_blank_text(b->content.data, b->content.len);
}

static bool ends_with_blank_line(struct block *b)
{
    while (b) {
        if (b->last_line_blank) return true;
        if (b->type != B_LIST && b->type != B_ITEM) return false;
        b = b->last;
    }
    return false;
}

static struct block *finalize(struct parser *p, struct block *b)
{
    struct block *parent = b->parent;
    b->open = false;
    switch (b->type) {
    case B_PARAGRAPH:
        if (!resolve_references(p->doc, b)) remove_block(b);
        break;
    case B_CODE:
        if (b->fenced) {
            /* The first line is the info string. */
            char *data = b->content.data ? b->content.data : "";
            char *newline = memchr(data, '\n', b->content.len);
            size_t first = newline ? (size_t)(newline - data) : b->content.len;
            size_t a = 0, e = first;
            while (a < e && is_whitespace(data[a])) ++a;
            while (e > a && is_whitespace(data[e - 1])) --e;
            b->info = unescape(&p->doc->arena, data + a, e - a);
            size_t rest = newline ? first + 1 : first;
            memmove(data, data + rest, b->content.len - rest);
            b->content.len -= rest;
            if (b->content.data) b->content.data[b->content.len] = '\0';
        } else {
            /* Drop trailing blank lines. */
            size_t end = b->content.len;
            while (end > 0) {
                size_t start = end - 1;
                while (start > 0 && b->content.data[start - 1] != '\n') --start;
                if (!is_blank_text(b->content.data + start, end - start)) break;
                end = start;
            }
            b->content.len = end;
            if (b->content.data) b->content.data[end] = '\0';
        }
        break;
    case B_LIST:
        b->list.tight = true;
        for (struct block *item = b->first; item && b->list.tight; item = item->next) {
            if (item->last_line_blank && item->next) {
                b->list.tight = false;
                break;
            }
            for (struct block *sub = item->first; sub; sub = sub->next) {
                if ((item->next || sub->next) && ends_with_blank_line(sub)) {
                    b->list.tight = false;
                    break;
                }
            }
        }
        break;
    default: break;
    }
    return parent;
}

static struct block *add_child(struct parser *p, struct block *parent, enum block_type type)
{
    while (!can_contain(parent->type, type)) parent = finalize(p, parent);
    struct block *child = arena_alloc(&p->doc->arena, sizeof *child);
    child->type = type;
    child->open = true;
    child->start_line = p->line_number;
    append_block(parent, child);
    return child;
}

static int block_depth(const struct block *b)
{
    int depth = 0;
    for (; b->parent; b = b->parent) ++depth;
    return depth;
}

static size_t scan_atx_heading_start(const struct parser *p, size_t pos, int *level)
{
    size_t i = pos;
    while (peek(p, i) == '#' && i - pos < 7) ++i;
    size_t hashes = i - pos;
    if (hashes < 1 || hashes > 6) return 0;
    char c = peek(p, i);
    if (c != '\n' && !is_space_or_tab(c)) return 0;
    while (is_space_or_tab(peek(p, i))) ++i;
    *level = (int)hashes;
    return i - pos;
}

static size_t scan_open_fence(const struct parser *p, size_t pos)
{
    char c = peek(p, pos);
    if (c != '`' && c != '~') return 0;
    size_t i = pos;
    while (peek(p, i) == c) ++i;
    size_t length = i - pos;
    if (length < 3) return 0;
    if (c == '`') {
        for (; i < p->len; ++i) {
            if (p->line[i] == '`') return 0;
        }
    }
    return length;
}

static size_t scan_close_fence(const struct parser *p, size_t pos, char c)
{
    size_t i = pos;
    while (peek(p, i) == c) ++i;
    size_t length = i - pos;
    if (length < 3) return 0;
    while (is_space_or_tab(peek(p, i))) ++i;
    return is_line_end(peek(p, i)) ? length : 0;
}

static int scan_setext_underline(const struct parser *p, size_t pos)
{
    char c = peek(p, pos);
    if (c != '=' && c != '-') return 0;
    size_t i = pos;
    while (peek(p, i) == c) ++i;
    while (is_space_or_tab(peek(p, i))) ++i;
    if (!is_line_end(peek(p, i))) return 0;
    return c == '=' ? 1 : 2;
}

static bool scan_thematic_break(const struct parser *p, size_t pos)
{
    char c = peek(p, pos);
    if (c != '*' && c != '-' && c != '_') return false;
    int count = 0;
    for (size_t i = pos;; ++i) {
        char d = peek(p, i);
        if (d == c) ++count;
        else if (is_line_end(d)) break;
        else if (!is_space_or_tab(d)) return false;
    }
    return count >= 3;
}

static size_t parse_list_marker(const struct parser *p, size_t pos, bool interrupts_paragraph, struct list_data *data)
{
    size_t i = pos;
    char c = peek(p, i);
    *data = (struct list_data){0};
    if (c == '*' || c == '-' || c == '+') {
        ++i;
        if (!is_whitespace(peek(p, i)) && peek(p, i) != '\0') return 0;
        data->bullet = c;
    } else if (is_ascii_digit(c)) {
        int start = 0, digits = 0;
        do {
            start = start * 10 + (peek(p, i) - '0');
            ++i;
            ++digits;
        } while (digits < 9 && is_ascii_digit(peek(p, i)));
        if (interrupts_paragraph && start != 1) return 0;
        c = peek(p, i);
        if (c != '.' && c != ')') return 0;
        ++i;
        if (!is_whitespace(peek(p, i)) && peek(p, i) != '\0') return 0;
        data->ordered = true;
        data->start = start;
        data->delimiter = c;
    } else {
        return 0;
    }
    if (interrupts_paragraph) {
        size_t j = i;
        while (is_space_or_tab(peek(p, j))) ++j;
        if (is_line_end(peek(p, j))) return 0;
    }
    return i - pos;
}

static bool lists_match(const struct list_data *a, const struct list_data *b)
{
    return a->ordered == b->ordered && a->delimiter == b->delimiter && a->bullet == b->bullet;
}

/* Table rows (GitHub extension) */

/* Split a row into cells at unescaped pipes; a leading and a trailing pipe
   don't start or end a cell. Returns the number of cells, filling at most
   max of them. */
static int split_row(const char *s, size_t n, size_t *starts, size_t *lens, int max)
{
    size_t i = 0;
    while (i < n && is_whitespace(s[i])) ++i;
    while (n > i && is_whitespace(s[n - 1])) --n;
    if (i < n && s[i] == '|') ++i;
    int count = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && s[i] != '|') i += s[i] == '\\' && i + 1 < n ? 2 : 1;
        size_t a = start, b = i < n ? i : n;
        while (a < b && is_space_or_tab(s[a])) ++a;
        while (b > a && is_space_or_tab(s[b - 1])) --b;
        if (count < max) {
            starts[count] = a;
            lens[count] = b - a;
        }
        ++count;
        if (i < n) ++i; /* The pipe. */
    }
    return count;
}

/* A delimiter row such as "| :-- | :-: | --: |". Returns its number of
   columns and their alignment, or 0. At least one pipe is needed, so a
   lone "---" stays a setext underline or a thematic break. */
static int scan_table_delimiter(const char *s, size_t n, char *align)
{
    if (!memchr(s, '|', n)) return 0;
    size_t i = 0;
    while (i < n && is_space_or_tab(s[i])) ++i;
    if (i < n && s[i] == '|') ++i;
    int columns = 0;
    for (;;) {
        while (i < n && is_space_or_tab(s[i])) ++i;
        if (i >= n || s[i] == '\n') break;
        bool left = false, right = false;
        if (s[i] == ':') {
            left = true;
            ++i;
        }
        size_t dashes = i;
        while (i < n && s[i] == '-') ++i;
        if (i == dashes) return 0;
        if (i < n && s[i] == ':') {
            right = true;
            ++i;
        }
        while (i < n && is_space_or_tab(s[i])) ++i;
        if (columns >= MAX_TABLE_COLUMNS) return 0;
        align[columns++] = left && right ? 'c' : right ? 'r' : left ? 'l' : 0;
        if (i < n && s[i] == '|') {
            ++i;
            continue;
        }
        if (i >= n || s[i] == '\n') break;
        return 0;
    }
    return columns;
}

/* The paragraph's last line becomes a table's header when it has as many
   cells as the delimiter row has columns. */
static bool try_table(struct parser *p, struct block **container, size_t pos)
{
    char align[MAX_TABLE_COLUMNS];
    int columns = scan_table_delimiter(p->line + pos, p->len - pos, align);
    if (!columns) return false;
    struct block *para = *container;
    struct buf *content = &para->content;
    if (content->len == 0) return false;
    size_t end = content->len - 1; /* Its final '\n'. */
    size_t start = end;
    while (start > 0 && content->data[start - 1] != '\n') --start;
    size_t starts[1], lens[1];
    if (split_row(content->data + start, end - start, starts, lens, 0) != columns) return false;

    char *header = arena_strndup(&p->doc->arena, content->data + start, end - start);
    content->len = start;
    content->data[start] = '\0';
    struct block *table = add_child(p, para, B_TABLE);
    table->columns = columns;
    table->align = arena_strndup(&p->doc->arena, align, (size_t)columns);
    struct block *row = add_child(p, table, B_ROW);
    row->open = false;
    row->header = true;
    buf_puts(&row->content, header);
    advance_to(p, p->len - 1);
    *container = table;
    return true;
}

/* Walk the open blocks, consuming each one's continuation markers. Returns
   the last block that matched, or NULL when a closing fence used the line. */
static struct block *check_open_blocks(struct parser *p, bool *all_matched)
{
    struct block *container = p->doc->root;
    *all_matched = false;
    while (container->last && container->last->open) {
        container = container->last;
        find_first_nonspace(p);
        switch (container->type) {
        case B_QUOTE:
            if (p->indent > 3 || peek(p, p->first_nonspace) != '>') goto done;
            advance_offset(p, p->indent + 1, true);
            if (is_space_or_tab(peek(p, p->offset))) advance_offset(p, 1, true);
            break;
        case B_ITEM:
            if (p->indent >= container->list.marker_offset + container->list.padding) {
                advance_offset(p, container->list.marker_offset + container->list.padding, true);
            } else if (p->blank && container->first) {
                advance_to(p, p->first_nonspace);
            } else {
                goto done;
            }
            break;
        case B_CODE:
            if (!container->fenced) {
                if (p->indent >= CODE_INDENT) advance_offset(p, CODE_INDENT, true);
                else if (p->blank) advance_to(p, p->first_nonspace);
                else goto done;
            } else {
                size_t length = 0;
                if (p->indent <= 3 && peek(p, p->first_nonspace) == container->fence_char) {
                    length = scan_close_fence(p, p->first_nonspace, container->fence_char);
                }
                if (length >= (size_t)container->fence_length) {
                    p->current = finalize(p, container);
                    return NULL;
                }
                for (int i = container->fence_offset; i > 0 && is_space_or_tab(peek(p, p->offset)); --i) {
                    advance_offset(p, 1, true);
                }
            }
            break;
        case B_HEADING: goto done;
        case B_HTML:
            if (container->html_type >= 6 && p->blank) goto done;
            break;
        case B_PARAGRAPH:
        case B_TABLE:
            if (p->blank) goto done;
            break;
        default: break;
        }
    }
    *all_matched = true;
    return container;
done:
    return container->parent;
}

static void open_new_blocks(struct parser *p, struct block **container, bool all_matched)
{
    bool maybe_lazy = p->current->type == B_PARAGRAPH;
    while ((*container)->type != B_CODE && (*container)->type != B_HTML) {
        find_first_nonspace(p);
        bool indented = p->indent >= CODE_INDENT;
        bool room = block_depth(*container) < MAX_NESTING - 1;
        enum block_type type = (*container)->type;
        size_t pos = p->first_nonspace;
        size_t matched;
        int level;
        struct list_data data;

        if (!indented && room && peek(p, pos) == '>') {
            advance_to(p, pos + 1);
            if (is_space_or_tab(peek(p, p->offset))) advance_offset(p, 1, true);
            *container = add_child(p, *container, B_QUOTE);
        } else if (!indented && (matched = scan_atx_heading_start(p, pos, &level))) {
            advance_to(p, pos + matched);
            *container = add_child(p, *container, B_HEADING);
            (*container)->level = level;
        } else if (!indented && (matched = scan_open_fence(p, pos))) {
            *container = add_child(p, *container, B_CODE);
            (*container)->fenced = true;
            (*container)->fence_char = peek(p, pos);
            (*container)->fence_length = (int)matched;
            (*container)->fence_offset = (int)(pos - p->offset);
            advance_to(p, pos + matched);
        } else if (!indented && (level = scan_html_block_start(p->line + pos, p->len - pos, type != B_PARAGRAPH))) {
            *container = add_child(p, *container, B_HTML);
            (*container)->html_type = level;
        } else if (!indented && type == B_PARAGRAPH && p->doc->extensions && try_table(p, container, pos)) {
            /* The delimiter row is used. */
        } else if (!indented && type == B_PARAGRAPH && (level = scan_setext_underline(p, pos))) {
            if (resolve_references(p->doc, *container)) {
                (*container)->type = B_HEADING;
                (*container)->level = level;
                (*container)->setext = true;
                advance_to(p, p->len - 1);
            }
        } else if (!indented && !(type == B_PARAGRAPH && !all_matched) && scan_thematic_break(p, pos)) {
            *container = add_child(p, *container, B_BREAK);
            advance_to(p, p->len - 1);
        } else if (p->indent < 4 && room && (matched = parse_list_marker(p, pos, type == B_PARAGRAPH, &data))) {
            advance_to(p, pos + matched);
            bool saved_tab = p->partially_consumed_tab;
            size_t saved_offset = p->offset;
            int saved_column = p->column;
            while (p->column - saved_column <= 5 && is_space_or_tab(peek(p, p->offset))) advance_offset(p, 1, true);
            int spaces = p->column - saved_column;
            if (spaces >= 5 || spaces < 1 || is_line_end(peek(p, p->offset))) {
                data.padding = (int)matched + 1;
                p->offset = saved_offset;
                p->column = saved_column;
                p->partially_consumed_tab = saved_tab;
                if (spaces > 0) advance_offset(p, 1, true);
            } else {
                data.padding = (int)matched + spaces;
            }
            data.marker_offset = p->indent;
            if (type != B_LIST || !lists_match(&(*container)->list, &data)) {
                *container = add_child(p, *container, B_LIST);
                (*container)->list = data;
            }
            *container = add_child(p, *container, B_ITEM);
            (*container)->list = data;
        } else if (indented && !maybe_lazy && !p->blank) {
            advance_offset(p, CODE_INDENT, true);
            *container = add_child(p, *container, B_CODE);
        } else {
            break;
        }
        if (accepts_lines((*container)->type)) break;
        maybe_lazy = false;
    }
}

/* Remove an ATX heading's closing sequence from the end of the line. */
static size_t heading_end(const struct parser *p)
{
    size_t end = p->len;
    if (end > 0 && p->line[end - 1] == '\n') --end;
    while (end > p->offset && is_space_or_tab(p->line[end - 1])) --end;
    size_t hashes = end;
    while (hashes > 0 && p->line[hashes - 1] == '#') --hashes;
    if (hashes != end && (hashes == 0 || is_space_or_tab(p->line[hashes - 1]))) {
        end = hashes;
        while (end > p->offset && is_space_or_tab(p->line[end - 1])) --end;
    }
    return end < p->offset ? p->offset : end;
}

static void add_text(struct parser *p, struct block *container, struct block *last_matched)
{
    find_first_nonspace(p);
    if (p->blank && container->last) container->last->last_line_blank = true;
    enum block_type type = container->type;
    container->last_line_blank =
        p->blank && type != B_QUOTE && type != B_HEADING && type != B_BREAK && type != B_TABLE &&
        !(type == B_CODE && container->fenced) &&
        !(type == B_ITEM && !container->first && container->start_line == p->line_number);
    for (struct block *b = container; b->parent; b = b->parent) b->parent->last_line_blank = false;

    if (p->current != last_matched && container == last_matched && !p->blank && p->current->type == B_PARAGRAPH) {
        /* A lazy continuation line. */
        add_line(p, p->current);
        return;
    }
    while (p->current != last_matched) p->current = finalize(p, p->current);
    if (type == B_CODE) {
        add_line(p, container);
    } else if (type == B_HTML) {
        add_line(p, container);
        if (container->html_type <= 5 &&
            html_block_ends(container->html_type, p->line + p->first_nonspace, p->len - p->first_nonspace)) {
            container = finalize(p, container);
        }
    } else if (p->blank) {
        /* Nothing to add. */
    } else if (type == B_TABLE) {
        struct block *row = add_child(p, container, B_ROW);
        row->open = false;
        advance_to(p, p->first_nonspace);
        buf_put(&row->content, p->line + p->offset, p->len - p->offset);
    } else if (accepts_lines(type)) {
        if (type == B_HEADING && !container->setext) {
            advance_to(p, p->first_nonspace);
            size_t end = heading_end(p);
            buf_put(&container->content, p->line + p->offset, end - p->offset);
            buf_putc(&container->content, '\n');
        } else {
            advance_to(p, p->first_nonspace);
            add_line(p, container);
        }
    } else {
        container = add_child(p, container, B_PARAGRAPH);
        advance_to(p, p->first_nonspace);
        add_line(p, container);
    }
    p->current = container;
}

static void process_line(struct parser *p, const char *line, size_t len)
{
    p->line = line;
    p->len = len;
    p->offset = p->first_nonspace = 0;
    p->column = p->first_nonspace_column = p->indent = 0;
    p->blank = p->partially_consumed_tab = false;
    ++p->line_number;
    bool all_matched;
    struct block *last_matched = check_open_blocks(p, &all_matched);
    if (!last_matched) return;
    struct block *container = last_matched;
    open_new_blocks(p, &container, all_matched);
    /* A thematic break or setext underline leaves the offset on the line's
       end, so the rest reads as blank. */
    add_text(p, container, last_matched);
}

/* Inline parsing */

struct delimiter {
    struct delimiter *prev, *next;
    struct inline_node *node;
    size_t position;
    int count, original;
    char c;
    bool can_open, can_close;
};

struct bracket {
    struct bracket *prev;
    struct inline_node *node;
    size_t position;
    bool image, bracket_after;
};

struct subject {
    struct document *doc;
    const char *s;
    size_t n, pos;
    struct inline_node *parent;
    struct delimiter *last_delim;
    struct bracket *last_bracket;
    bool no_link_openers;
    bool scanned_backticks;
    size_t backticks[MAX_BACKTICKS + 1];
    struct html_memo html;
};

static struct inline_node *make_inline(struct subject *sub, enum inline_type type)
{
    struct inline_node *node = arena_alloc(&sub->doc->arena, sizeof *node);
    node->type = type;
    return node;
}

static struct inline_node *make_text(struct subject *sub, const char *s, size_t n)
{
    struct inline_node *node = make_inline(sub, I_TEXT);
    node->text = arena_strndup(&sub->doc->arena, s, n);
    node->len = n;
    return node;
}

static void append_inline(struct inline_node *parent, struct inline_node *child)
{
    child->parent = parent;
    child->prev = parent->last;
    child->next = NULL;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

static void unlink_inline(struct inline_node *node)
{
    struct inline_node *parent = node->parent;
    if (node->prev) node->prev->next = node->next;
    else if (parent) parent->first = node->next;
    if (node->next) node->next->prev = node->prev;
    else if (parent) parent->last = node->prev;
    node->prev = node->next = node->parent = NULL;
}

static void insert_after(struct inline_node *anchor, struct inline_node *node)
{
    node->parent = anchor->parent;
    node->prev = anchor;
    node->next = anchor->next;
    if (anchor->next) anchor->next->prev = node;
    else anchor->parent->last = node;
    anchor->next = node;
}

static void insert_before(struct inline_node *anchor, struct inline_node *node)
{
    node->parent = anchor->parent;
    node->next = anchor;
    node->prev = anchor->prev;
    if (anchor->prev) anchor->prev->next = node;
    else anchor->parent->first = node;
    anchor->prev = node;
}

static bool is_special(const struct subject *sub, char c)
{
    if (c == '~') return sub->doc->extensions;
    switch (c) {
    case '\n': case '`': case '\\': case '&': case '<': case '*': case '_': case '[': case ']': case '!':
        return true;
    default:
        return false;
    }
}

static struct inline_node *handle_newline(struct subject *sub)
{
    size_t newline = sub->pos++;
    while (sub->pos < sub->n && is_space_or_tab(sub->s[sub->pos])) ++sub->pos;
    bool hard = newline >= 2 && sub->s[newline - 1] == ' ' && sub->s[newline - 2] == ' ';
    return make_inline(sub, hard ? I_LINEBREAK : I_SOFTBREAK);
}

static struct inline_node *handle_backslash(struct subject *sub)
{
    ++sub->pos;
    char next = sub->pos < sub->n ? sub->s[sub->pos] : '\0';
    if (is_ascii_punct(next)) {
        ++sub->pos;
        return make_text(sub, sub->s + sub->pos - 1, 1);
    }
    if (next == '\n' && sub->pos + 1 < sub->n) {
        ++sub->pos;
        while (sub->pos < sub->n && is_space_or_tab(sub->s[sub->pos])) ++sub->pos;
        return make_inline(sub, I_LINEBREAK);
    }
    return make_text(sub, "\\", 1);
}

/* The position after a closing run of exactly count backticks, or 0. */
static size_t scan_closing_backticks(struct subject *sub, size_t count)
{
    if (count > MAX_BACKTICKS) return 0;
    if (sub->scanned_backticks && sub->backticks[count] <= sub->pos) return 0;
    size_t i = sub->pos;
    for (;;) {
        while (i < sub->n && sub->s[i] != '`') ++i;
        if (i >= sub->n) break;
        size_t start = i;
        while (i < sub->n && sub->s[i] == '`') ++i;
        size_t run = i - start;
        if (run <= MAX_BACKTICKS) sub->backticks[run] = start;
        if (run == count) return i;
    }
    sub->scanned_backticks = true;
    return 0;
}

static struct inline_node *handle_backticks(struct subject *sub)
{
    size_t start = sub->pos;
    while (sub->pos < sub->n && sub->s[sub->pos] == '`') ++sub->pos;
    size_t count = sub->pos - start;
    size_t end = scan_closing_backticks(sub, count);
    if (!end) return make_text(sub, sub->s + start, count);
    size_t a = sub->pos, b = end - count;
    sub->pos = end;
    struct inline_node *node = make_inline(sub, I_CODE);
    char *text = arena_strndup(&sub->doc->arena, sub->s + a, b - a);
    size_t len = b - a;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '\n') text[i] = ' ';
    }
    if (len >= 2 && text[0] == ' ' && text[len - 1] == ' ' && !is_blank_text(text, len)) {
        ++text;
        len -= 2;
    }
    node->text = text;
    node->len = len;
    return node;
}

static struct inline_node *handle_entity(struct subject *sub)
{
    struct buf out = {0};
    size_t used = decode_entity(sub->s + sub->pos, sub->n - sub->pos, &out);
    struct inline_node *node;
    if (used) {
        node = make_text(sub, out.data, out.len);
        sub->pos += used;
    } else {
        node = make_text(sub, "&", 1);
        ++sub->pos;
    }
    buf_free(&out);
    return node;
}

static size_t scan_autolink_uri(const char *s, size_t n)
{
    size_t i = 1;
    if (i >= n || !is_ascii_alpha(s[i])) return 0;
    while (i < n && (is_ascii_alnum(s[i]) || s[i] == '+' || s[i] == '.' || s[i] == '-')) ++i;
    if (i - 1 < 2 || i - 1 > 32 || i >= n || s[i] != ':') return 0;
    for (++i; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '>') return i + 1;
        if (c <= 0x20 || c == '<' || c == 0x7f) return 0;
    }
    return 0;
}

static size_t scan_autolink_email(const char *s, size_t n)
{
    size_t i = 1;
    while (i < n && (is_ascii_alnum(s[i]) || strchr(".!#$%&'*+/=?^_`{|}~-", s[i]))) ++i;
    if (i == 1 || i >= n || s[i] != '@') return 0;
    ++i;
    for (;;) {
        size_t start = i;
        if (i >= n || !is_ascii_alnum(s[i])) return 0;
        while (i < n && (is_ascii_alnum(s[i]) || s[i] == '-') && i - start < 63) ++i;
        if (s[i - 1] == '-') return 0;
        if (i < n && s[i] == '.') {
            ++i;
            continue;
        }
        return i < n && s[i] == '>' ? i + 1 : 0;
    }
}

static struct inline_node *handle_pointy(struct subject *sub)
{
    const char *s = sub->s + sub->pos;
    size_t n = sub->n - sub->pos;
    size_t len;
    if ((len = scan_autolink_uri(s, n)) || (len = scan_autolink_email(s, n))) {
        bool email = !scan_autolink_uri(s, n);
        struct inline_node *link = make_inline(sub, I_LINK);
        struct buf url = {0};
        if (email) buf_puts(&url, "mailto:");
        buf_put(&url, s + 1, len - 2);
        link->url = arena_strndup(&sub->doc->arena, url.data, url.len);
        buf_free(&url);
        append_inline(link, make_text(sub, s + 1, len - 2));
        sub->pos += len;
        return link;
    }
    if ((len = scan_inline_html(s, n, &sub->html))) {
        struct inline_node *node = make_inline(sub, I_HTML);
        node->text = arena_strndup(&sub->doc->arena, s, len);
        node->len = len;
        sub->pos += len;
        return node;
    }
    ++sub->pos;
    return make_text(sub, "<", 1);
}

static struct inline_node *handle_delimiters(struct subject *sub, char c)
{
    size_t start = sub->pos;
    while (sub->pos < sub->n && sub->s[sub->pos] == c) ++sub->pos;
    int count = (int)(sub->pos - start);
    uint32_t before = utf8_before(sub->s, start);
    uint32_t after = '\n';
    if (sub->pos < sub->n) utf8_decode(sub->s + sub->pos, sub->n - sub->pos, &after);
    bool before_space = is_unicode_space(before), after_space = is_unicode_space(after);
    bool before_punct = is_unicode_punct(before), after_punct = is_unicode_punct(after);
    bool left = !after_space && (!after_punct || before_space || before_punct);
    bool right = !before_space && (!before_punct || after_space || after_punct);
    bool can_open, can_close;
    if (c == '_') {
        can_open = left && (!right || before_punct);
        can_close = right && (!left || after_punct);
    } else {
        can_open = left;
        can_close = right;
    }
    struct inline_node *node = make_text(sub, sub->s + start, (size_t)count);
    /* Strikethrough is one or two tildes; a longer run is text. */
    if (c == '~' && count > 2) can_open = can_close = false;
    if (can_open || can_close) {
        struct delimiter *d = arena_alloc(&sub->doc->arena, sizeof *d);
        d->node = node;
        d->c = c;
        d->count = d->original = count;
        d->can_open = can_open;
        d->can_close = can_close;
        d->position = sub->pos;
        d->prev = sub->last_delim;
        if (d->prev) d->prev->next = d;
        sub->last_delim = d;
    }
    return node;
}

static void remove_delimiter(struct subject *sub, struct delimiter *d)
{
    if (d->next) d->next->prev = d->prev;
    else sub->last_delim = d->prev;
    if (d->prev) d->prev->next = d->next;
}

/* Wrap the nodes between opener and closer in emphasis. Returns the next
   closer to look at. */
static struct delimiter *insert_emphasis(struct subject *sub, struct delimiter *opener, struct delimiter *closer)
{
    int use = closer->c == '~' ? closer->count : closer->count >= 2 && opener->count >= 2 ? 2 : 1;
    opener->count -= use;
    closer->count -= use;
    opener->node->len -= (size_t)use;
    closer->node->len -= (size_t)use;
    for (struct delimiter *d = closer->prev; d && d != opener;) {
        struct delimiter *prev = d->prev;
        remove_delimiter(sub, d);
        d = prev;
    }
    struct inline_node *emph = make_inline(sub, closer->c == '~' ? I_STRIKE : use == 1 ? I_EMPH : I_STRONG);
    for (struct inline_node *node = opener->node->next; node && node != closer->node;) {
        struct inline_node *next = node->next;
        unlink_inline(node);
        append_inline(emph, node);
        node = next;
    }
    insert_after(opener->node, emph);
    if (opener->count == 0) {
        unlink_inline(opener->node);
        remove_delimiter(sub, opener);
    }
    if (closer->count == 0) {
        unlink_inline(closer->node);
        struct delimiter *next = closer->next;
        remove_delimiter(sub, closer);
        return next;
    }
    return closer;
}

static void process_emphasis(struct subject *sub, size_t stack_bottom)
{
    size_t openers_bottom[14];
    for (int i = 0; i < 14; ++i) openers_bottom[i] = stack_bottom;
    struct delimiter *closer = NULL;
    for (struct delimiter *d = sub->last_delim; d && d->position >= stack_bottom; d = d->prev) closer = d;
    while (closer) {
        if (!closer->can_close) {
            closer = closer->next;
            continue;
        }
        int index = closer->c == '~' ? 12 + (closer->original == 2)
                                     : (closer->c == '*' ? 0 : 6) + (closer->can_open ? 3 : 0) + closer->original % 3;
        struct delimiter *opener = closer->prev;
        bool found = false;
        while (opener && opener->position >= stack_bottom && opener->position >= openers_bottom[index]) {
            if (opener->can_open && opener->c == '~' && closer->c == '~') {
                /* Tildes pair only with a run of the same length. */
                if (opener->original == closer->original) {
                    found = true;
                    break;
                }
            } else if (opener->can_open && opener->c == closer->c) {
                bool odd_match = (closer->can_open || opener->can_close) && closer->original % 3 != 0 &&
                                 (opener->original + closer->original) % 3 == 0;
                if (!odd_match) {
                    found = true;
                    break;
                }
            }
            opener = opener->prev;
        }
        struct delimiter *old_closer = closer;
        if (found) {
            closer = insert_emphasis(sub, opener, closer);
        } else {
            closer = closer->next;
            openers_bottom[index] = old_closer->position;
            if (!old_closer->can_open) remove_delimiter(sub, old_closer);
        }
    }
    while (sub->last_delim && sub->last_delim->position >= stack_bottom) remove_delimiter(sub, sub->last_delim);
}

static void push_bracket(struct subject *sub, bool image, struct inline_node *node)
{
    if (sub->last_bracket) sub->last_bracket->bracket_after = true;
    struct bracket *b = arena_alloc(&sub->doc->arena, sizeof *b);
    b->image = image;
    b->node = node;
    b->position = sub->pos;
    b->prev = sub->last_bracket;
    sub->last_bracket = b;
    if (!image) sub->no_link_openers = false;
}

static struct inline_node *handle_close_bracket(struct subject *sub)
{
    ++sub->pos;
    size_t initial = sub->pos;
    struct bracket *opener = sub->last_bracket;
    if (!opener) return make_text(sub, "]", 1);
    if (!opener->image && sub->no_link_openers) {
        sub->last_bracket = opener->prev;
        return make_text(sub, "]", 1);
    }
    const char *s = sub->s;
    size_t n = sub->n;
    char *url = NULL, *title = NULL;
    bool matched = false;

    if (sub->pos < n && s[sub->pos] == '(') {
        size_t i = skip_spacechars(s, n, sub->pos + 1);
        size_t url_end, url_start, url_len;
        if (scan_link_url(s, n, i, &url_end, &url_start, &url_len) ||
            (i < n && s[i] == ')' && (url_end = i, url_start = i, url_len = 0, true))) {
            size_t title_start = skip_spacechars(s, n, url_end);
            size_t title_len = title_start == url_end ? 0 : scan_link_title(s, n, title_start);
            size_t end = skip_spacechars(s, n, title_start + title_len);
            if (title_len == 0) end = title_start;
            if (end < n && s[end] == ')') {
                url = unescape(&sub->doc->arena, s + url_start, url_len);
                title = title_len ? unescape(&sub->doc->arena, s + title_start + 1, title_len - 2) : NULL;
                sub->pos = end + 1;
                matched = true;
            }
        }
    }
    if (!matched) {
        size_t label_end, label_start, label_len;
        bool found = scan_link_label(s, n, sub->pos, &label_end, &label_start, &label_len);
        if (found) sub->pos = label_end;
        if ((!found || label_len == 0) && !opener->bracket_after) {
            label_start = opener->position;
            label_len = initial - 1 - opener->position;
            found = true;
        }
        const struct ref *ref = found ? refmap_find(&sub->doc->refs, s + label_start, label_len) : NULL;
        if (ref) {
            url = ref->url;
            title = ref->title;
            matched = true;
        } else {
            sub->pos = initial;
        }
    }
    if (!matched) {
        sub->last_bracket = opener->prev;
        sub->pos = initial;
        return make_text(sub, "]", 1);
    }
    struct inline_node *link = make_inline(sub, opener->image ? I_IMAGE : I_LINK);
    link->url = url;
    link->title = title;
    insert_before(opener->node, link);
    for (struct inline_node *node = opener->node->next; node;) {
        struct inline_node *next = node->next;
        unlink_inline(node);
        append_inline(link, node);
        node = next;
    }
    unlink_inline(opener->node);
    process_emphasis(sub, opener->position);
    sub->last_bracket = opener->prev;
    if (!opener->image) sub->no_link_openers = true;
    return NULL;
}

static void parse_inline(struct subject *sub)
{
    char c = sub->s[sub->pos];
    struct inline_node *node = NULL;
    switch (c) {
    case '\n': node = handle_newline(sub); break;
    case '`': node = handle_backticks(sub); break;
    case '\\': node = handle_backslash(sub); break;
    case '&': node = handle_entity(sub); break;
    case '<': node = handle_pointy(sub); break;
    case '*':
    case '_':
    case '~': node = handle_delimiters(sub, c); break;
    case '[':
        ++sub->pos;
        node = make_text(sub, "[", 1);
        push_bracket(sub, false, node);
        break;
    case '!':
        if (sub->pos + 1 < sub->n && sub->s[sub->pos + 1] == '[') {
            sub->pos += 2;
            node = make_text(sub, "![", 2);
            push_bracket(sub, true, node);
        } else {
            ++sub->pos;
            node = make_text(sub, "!", 1);
        }
        break;
    case ']': node = handle_close_bracket(sub); break;
    default: {
        size_t start = sub->pos;
        while (sub->pos < sub->n && !is_special(sub, sub->s[sub->pos])) ++sub->pos;
        size_t end = sub->pos;
        if (sub->pos < sub->n && sub->s[sub->pos] == '\n') {
            while (end > start && sub->s[end - 1] == ' ') --end;
        }
        node = make_text(sub, sub->s + start, end - start);
        break;
    }
    }
    if (node) append_inline(sub->parent, node);
}

/* Extended autolinks (GitHub extension): www., http://, https:// and
   ftp:// links and email addresses in plain text. */

/* A domain at s[i]: segments of letters, digits, '-' and '_' separated by
   periods, with at least one period and no '_' in the last two segments.
   Returns its end, or 0. */
static size_t scan_domain(const char *s, size_t n, size_t i)
{
    size_t periods = 0;
    bool underscore_last = false, underscore_before = false;
    size_t start = i;
    while (i < n) {
        char c = s[i];
        if (c == '.') {
            if (i == start || s[i - 1] == '.') break;
            ++periods;
            underscore_before = underscore_last;
            underscore_last = false;
        } else if (c == '_') {
            underscore_last = true;
        } else if (!is_ascii_alnum(c) && c != '-' && (unsigned char)c < 0x80) {
            break;
        }
        ++i;
    }
    if (i == start || periods == 0 || underscore_last || underscore_before) return 0;
    return i;
}

/* Drop trailing punctuation, unbalanced closing parentheses and a
   trailing entity reference from a link that ends at end. */
static size_t trim_autolink(const char *s, size_t start, size_t end)
{
    while (end > start) {
        char c = s[end - 1];
        if (strchr("?!.,:*_~'\"", c)) {
            --end;
        } else if (c == ')') {
            size_t open = 0, close = 0;
            for (size_t i = start; i < end; ++i) {
                open += s[i] == '(';
                close += s[i] == ')';
            }
            if (close <= open) break;
            --end;
        } else if (c == ';') {
            size_t i = end - 1;
            while (i > start && is_ascii_alnum(s[i - 1])) --i;
            if (i > start && i < end - 1 && s[i - 1] == '&') end = i - 1;
            else break;
        } else {
            break;
        }
    }
    return end;
}

static size_t scan_link_path(const char *s, size_t n, size_t i)
{
    while (i < n && !is_whitespace(s[i]) && s[i] != '<') ++i;
    return i;
}

/* A www or scheme autolink at s[i]. Sets the URL prefix to add. */
static size_t scan_web_autolink(const char *s, size_t n, size_t i, const char **prefix)
{
    static const char *const schemes[] = {"http://", "https://", "ftp://"};
    size_t domain = 0;
    *prefix = "";
    if (n - i >= 4 && strncasecmp(s + i, "www.", 4) == 0) {
        domain = i;
        *prefix = "http://";
    } else {
        for (size_t k = 0; k < 3; ++k) {
            size_t len = strlen(schemes[k]);
            if (n - i > len && strncasecmp(s + i, schemes[k], len) == 0) domain = i + len;
        }
        if (!domain) return 0;
    }
    size_t end = scan_domain(s, n, domain);
    if (!end) return 0;
    end = trim_autolink(s, i, scan_link_path(s, n, end));
    return end > domain ? end : 0;
}

/* An email address around the '@' at s[at], not starting before from. */
static bool scan_email_autolink(const char *s, size_t n, size_t from, size_t at, size_t *start, size_t *end)
{
    size_t a = at;
    while (a > from && (is_ascii_alnum(s[a - 1]) || strchr(".+-_", s[a - 1]))) --a;
    if (a == at) return false;
    size_t i = at + 1, periods = 0;
    while (i < n && (is_ascii_alnum(s[i]) || s[i] == '-' || s[i] == '_' ||
                     (s[i] == '.' && i + 1 < n && (is_ascii_alnum(s[i + 1]) || s[i + 1] == '-' || s[i + 1] == '_')))) {
        periods += s[i] == '.';
        ++i;
    }
    if (i == at + 1 || periods == 0 || s[i - 1] == '-' || s[i - 1] == '_') return false;
    *start = a;
    *end = i;
    return true;
}

static bool autolink_boundary(const char *s, size_t i)
{
    return i == 0 || is_whitespace(s[i - 1]) || strchr("*_~(", s[i - 1]);
}

/* Split one text node around the autolinks in it. */
static void autolink_text(struct subject *sub, struct inline_node *text)
{
    const char *s = text->text;
    size_t n = text->len, done = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t start = i, end = 0;
        const char *prefix = "";
        if ((s[i] == 'w' || s[i] == 'W' || s[i] == 'h' || s[i] == 'H' || s[i] == 'f' || s[i] == 'F') &&
            autolink_boundary(s, i)) {
            end = scan_web_autolink(s, n, i, &prefix);
        } else if (s[i] == '@' && scan_email_autolink(s, n, done, i, &start, &end)) {
            prefix = "mailto:";
        }
        if (!end) continue;
        if (start > done) insert_before(text, make_text(sub, s + done, start - done));
        struct inline_node *link = make_inline(sub, I_LINK);
        struct buf url = {0};
        buf_puts(&url, prefix);
        buf_put(&url, s + start, end - start);
        link->url = arena_strndup(&sub->doc->arena, url.data, url.len);
        buf_free(&url);
        insert_before(text, link);
        append_inline(link, make_text(sub, s + start, end - start));
        done = end;
        i = end - 1;
    }
    if (done == 0) return;
    if (done < n) {
        text->text += done;
        text->len -= done;
    } else {
        unlink_inline(text);
    }
}

/* Merge neighbouring text nodes, then find autolinks in each, outside
   links and images. Walks without recursion. */
static void autolink_children(struct subject *sub, struct inline_node *parent)
{
    for (struct inline_node *node = parent->first; node;) {
        if (node->type == I_TEXT && node->next && node->next->type == I_TEXT) {
            /* Join the whole run at once, so it costs its length. */
            size_t total = 0;
            struct inline_node *end = node;
            for (; end && end->type == I_TEXT; end = end->next) total += end->len;
            char *joined = arena_alloc(&sub->doc->arena, total + 1);
            size_t used = 0;
            for (struct inline_node *t = node; t != end;) {
                struct inline_node *following = t->next;
                memcpy(joined + used, t->text, t->len);
                used += t->len;
                if (t != node) unlink_inline(t);
                t = following;
            }
            node->text = joined;
            node->len = total;
        }
        struct inline_node *next = node->next;
        if (node->type == I_TEXT) autolink_text(sub, node);
        node = next;
    }
}

static void find_autolinks(struct subject *sub, struct inline_node *root)
{
    autolink_children(sub, root);
    struct inline_node *node = root->first;
    while (node) {
        bool container = node->type == I_EMPH || node->type == I_STRONG || node->type == I_STRIKE;
        if (container && node->first) {
            autolink_children(sub, node);
            node = node->first;
            continue;
        }
        while (node && !node->next) {
            node = node->parent;
            if (node == root) return;
        }
        if (node) node = node->next;
    }
}

static struct inline_node *parse_inlines(struct document *doc, const char *s, size_t n)
{
    size_t start = 0, end = n;
    while (start < end && is_whitespace(s[start])) ++start;
    while (end > start && is_whitespace(s[end - 1])) --end;
    struct subject *sub = xmalloc(sizeof *sub);
    memset(sub, 0, sizeof *sub);
    sub->doc = doc;
    sub->s = s + start;
    sub->n = end - start;
    sub->parent = arena_alloc(&doc->arena, sizeof *sub->parent);
    while (sub->pos < sub->n) parse_inline(sub);
    process_emphasis(sub, 0);
    if (doc->extensions) find_autolinks(sub, sub->parent);
    struct inline_node *root = sub->parent;
    free(sub);
    return root;
}

/* Each of a row's cells, with "\|" turned into "|" before parsing. */
static void parse_row(struct document *doc, struct block *row)
{
    int columns = row->parent->columns;
    size_t starts[MAX_TABLE_COLUMNS], lens[MAX_TABLE_COLUMNS];
    const char *s = row->content.data ? row->content.data : "";
    int count = split_row(s, row->content.len, starts, lens, columns);
    row->cells = arena_alloc(&doc->arena, (size_t)columns * sizeof *row->cells);
    struct buf cell = {0};
    for (int c = 0; c < columns; ++c) {
        cell.len = 0;
        buf_reserve(&cell, 0);
        if (c < count) {
            for (size_t i = starts[c]; i < starts[c] + lens[c]; ++i) {
                if (s[i] == '\\' && i + 1 < starts[c] + lens[c] && s[i + 1] == '|') continue;
                buf_putc(&cell, s[i]);
            }
        }
        row->cells[c] = parse_inlines(doc, cell.data, cell.len);
    }
    buf_free(&cell);
}

/* "[ ] " or "[x] " at the start of an item's first paragraph. */
static void find_task(struct block *item)
{
    struct block *para = item->first;
    if (!para || para->type != B_PARAGRAPH || para->content.len < 3) return;
    const char *s = para->content.data;
    if (s[0] != '[' || s[2] != ']' || !(s[1] == ' ' || s[1] == 'x' || s[1] == 'X')) return;
    if (para->content.len > 3 && !is_whitespace(s[3])) return;
    item->task = s[1] == ' ' ? 1 : 2;
    memmove(para->content.data, s + 3, para->content.len - 3 + 1);
    para->content.len -= 3;
}

static void parse_all_inlines(struct document *doc, struct block *b)
{
    for (; b; b = b->next) {
        if (b->type == B_ITEM && doc->extensions) find_task(b);
        if (b->type == B_PARAGRAPH || b->type == B_HEADING) {
            b->inlines = parse_inlines(doc, b->content.data ? b->content.data : "", b->content.len);
        } else if (b->type == B_ROW) {
            parse_row(doc, b);
        }
        parse_all_inlines(doc, b->first);
    }
}

static struct document *parse_document(const char *text, size_t len, bool extensions)
{
    struct document *doc = xmalloc(sizeof *doc);
    memset(doc, 0, sizeof *doc);
    doc->extensions = extensions;
    doc->root = arena_alloc(&doc->arena, sizeof *doc->root);
    doc->root->type = B_DOCUMENT;
    doc->root->open = true;
    struct parser p = {.doc = doc, .current = doc->root};
    for (size_t start = 0; start < len;) {
        const char *newline = memchr(text + start, '\n', len - start);
        size_t end = newline ? (size_t)(newline - text) + 1 : len;
        process_line(&p, text + start, end - start);
        start = end;
    }
    while (p.current != doc->root) p.current = finalize(&p, p.current);
    finalize(&p, doc->root);
    parse_all_inlines(doc, doc->root);
    return doc;
}

static void free_blocks(struct block *b)
{
    for (; b; b = b->next) {
        buf_free(&b->content);
        free_blocks(b->first);
    }
}

static void free_document(struct document *doc)
{
    free_blocks(doc->root);
    free(doc->refs.slots);
    arena_free(&doc->arena);
    free(doc);
}

#ifdef MARK_TEST
/* HTML output, only in the test build: it lets `make check` compare the
   parser with the CommonMark spec's examples. */

static void html_escape(struct buf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        switch (s[i]) {
        case '&': buf_puts(out, "&amp;"); break;
        case '<': buf_puts(out, "&lt;"); break;
        case '>': buf_puts(out, "&gt;"); break;
        case '"': buf_puts(out, "&quot;"); break;
        default: buf_putc(out, s[i]); break;
        }
    }
}

static void html_href(struct buf *out, const char *s)
{
    static const char safe[] = "-_.+!*(),%#@?=;:/$~";
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '&') buf_puts(out, "&amp;");
        else if (c == '\'') buf_puts(out, "&#x27;");
        else if (is_ascii_alnum(c) || (c && strchr(safe, c))) buf_putc(out, (char)c);
        else {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", c);
            buf_puts(out, hex);
        }
    }
}

static void html_cr(struct buf *out)
{
    if (out->len && out->data[out->len - 1] != '\n') buf_putc(out, '\n');
}

static void html_plain(struct buf *out, const struct inline_node *node)
{
    for (; node; node = node->next) {
        switch (node->type) {
        case I_TEXT:
        case I_CODE:
        case I_HTML: html_escape(out, node->text, node->len); break;
        case I_SOFTBREAK: buf_putc(out, '\n'); break;
        case I_LINEBREAK: buf_putc(out, '\n'); break;
        default: html_plain(out, node->first); break;
        }
    }
}

static void html_inlines(struct buf *out, const struct inline_node *node)
{
    for (; node; node = node->next) {
        switch (node->type) {
        case I_TEXT: html_escape(out, node->text, node->len); break;
        case I_SOFTBREAK: buf_putc(out, '\n'); break;
        case I_LINEBREAK: buf_puts(out, "<br />\n"); break;
        case I_CODE:
            buf_puts(out, "<code>");
            html_escape(out, node->text, node->len);
            buf_puts(out, "</code>");
            break;
        case I_HTML: buf_put(out, node->text, node->len); break;
        case I_EMPH:
            buf_puts(out, "<em>");
            html_inlines(out, node->first);
            buf_puts(out, "</em>");
            break;
        case I_STRONG:
            buf_puts(out, "<strong>");
            html_inlines(out, node->first);
            buf_puts(out, "</strong>");
            break;
        case I_STRIKE:
            buf_puts(out, "<del>");
            html_inlines(out, node->first);
            buf_puts(out, "</del>");
            break;
        case I_LINK:
            buf_puts(out, "<a href=\"");
            html_href(out, node->url);
            buf_putc(out, '"');
            if (node->title) {
                buf_puts(out, " title=\"");
                html_escape(out, node->title, strlen(node->title));
                buf_putc(out, '"');
            }
            buf_putc(out, '>');
            html_inlines(out, node->first);
            buf_puts(out, "</a>");
            break;
        case I_IMAGE:
            buf_puts(out, "<img src=\"");
            html_href(out, node->url);
            buf_puts(out, "\" alt=\"");
            html_plain(out, node->first);
            buf_putc(out, '"');
            if (node->title) {
                buf_puts(out, " title=\"");
                html_escape(out, node->title, strlen(node->title));
                buf_putc(out, '"');
            }
            buf_puts(out, " />");
            break;
        }
    }
}

static void html_blocks(struct buf *out, const struct block *b)
{
    for (; b; b = b->next) {
        switch (b->type) {
        case B_DOCUMENT: html_blocks(out, b->first); break;
        case B_QUOTE:
            html_cr(out);
            buf_puts(out, "<blockquote>\n");
            html_blocks(out, b->first);
            html_cr(out);
            buf_puts(out, "</blockquote>\n");
            break;
        case B_LIST:
            html_cr(out);
            if (!b->list.ordered) {
                buf_puts(out, "<ul>\n");
            } else if (b->list.start == 1) {
                buf_puts(out, "<ol>\n");
            } else {
                char start[32];
                snprintf(start, sizeof start, "<ol start=\"%d\">\n", b->list.start);
                buf_puts(out, start);
            }
            html_blocks(out, b->first);
            buf_puts(out, b->list.ordered ? "</ol>\n" : "</ul>\n");
            break;
        case B_ITEM:
            html_cr(out);
            buf_puts(out, "<li>");
            if (b->task) buf_puts(out, b->task == 2 ? "<input type=\"checkbox\" checked=\"\" disabled=\"\" /> "
                                                    : "<input type=\"checkbox\" disabled=\"\" /> ");
            html_blocks(out, b->first);
            buf_puts(out, "</li>\n");
            break;
        case B_PARAGRAPH: {
            bool tight = b->parent->type == B_ITEM && b->parent->parent->list.tight;
            if (!tight) {
                html_cr(out);
                buf_puts(out, "<p>");
            }
            html_inlines(out, b->inlines->first);
            if (!tight) buf_puts(out, "</p>\n");
            break;
        }
        case B_HEADING: {
            char tag[8];
            html_cr(out);
            snprintf(tag, sizeof tag, "<h%d>", b->level);
            buf_puts(out, tag);
            html_inlines(out, b->inlines->first);
            snprintf(tag, sizeof tag, "</h%d>", b->level);
            buf_puts(out, tag);
            buf_putc(out, '\n');
            break;
        }
        case B_BREAK:
            html_cr(out);
            buf_puts(out, "<hr />\n");
            break;
        case B_CODE:
            html_cr(out);
            buf_puts(out, "<pre><code");
            if (b->info && b->info[0]) {
                size_t word = strcspn(b->info, " \t\n");
                buf_puts(out, " class=\"language-");
                html_escape(out, b->info, word);
                buf_putc(out, '"');
            }
            buf_putc(out, '>');
            html_escape(out, b->content.data ? b->content.data : "", b->content.len);
            buf_puts(out, "</code></pre>\n");
            break;
        case B_HTML:
            html_cr(out);
            buf_put(out, b->content.data ? b->content.data : "", b->content.len);
            html_cr(out);
            break;
        case B_TABLE:
            html_cr(out);
            buf_puts(out, "<table>\n<thead>\n");
            html_blocks(out, b->first);
            if (b->first && b->first->next) buf_puts(out, "</tbody>\n");
            buf_puts(out, "</table>\n");
            break;
        case B_ROW: {
            const char *tag = b->header ? "th" : "td";
            if (b->prev && b->prev->header) buf_puts(out, "<tbody>\n");
            buf_puts(out, "<tr>\n");
            for (int c = 0; c < b->parent->columns; ++c) {
                char open[32];
                char align = b->parent->align[c];
                snprintf(open, sizeof open, "<%s%s>", tag,
                         align == 'l' ? " align=\"left\"" : align == 'c' ? " align=\"center\"" : align == 'r' ? " align=\"right\"" : "");
                buf_puts(out, open);
                html_inlines(out, b->cells[c]->first);
                buf_puts(out, b->header ? "</th>\n" : "</td>\n");
            }
            buf_puts(out, "</tr>\n");
            if (b->header) buf_puts(out, "</thead>\n");
            break;
        }
        }
    }
}
#endif

/* Layout */

struct prefix {
    const char *first; /* On the first line of the block. */
    const char *rest;
    uint16_t style;
    bool used;
};

struct glyphs {
    struct glyph *items;
    size_t count, cap;
};

struct layout {
    struct page *page;
    const struct options *opt;
    int width;
    struct prefix prefixes[MAX_NESTING * 2];
    int depth;
    int prefix_width;
    struct style base; /* Italic inside block quotes. */
    int list_depth;
    struct glyphs tmp;
    int32_t block, blocks; /* The current block's number, and the count so far. */
    int32_t heading;
};

static const struct style plain_style = {0, 0, COLOR_NONE, COLOR_NONE, COLOR_NONE};

static uint16_t intern_style(struct page *pg, struct style s)
{
    for (size_t i = 0; i < pg->style_count; ++i) {
        if (memcmp(&pg->styles[i], &s, sizeof s) == 0) return (uint16_t)i;
    }
    if (pg->style_count >= UINT16_MAX) return 0;
    grow(&pg->styles, &pg->style_cap, pg->style_count + 1, sizeof *pg->styles);
    pg->styles[pg->style_count] = s;
    return (uint16_t)pg->style_count++;
}

static int32_t add_link(struct page *pg, char *url)
{
    if (!url) return -1;
    grow(&pg->links, &pg->link_cap, pg->link_count + 1, sizeof *pg->links);
    pg->links[pg->link_count] = url;
    return (int32_t)pg->link_count++;
}

static void push_glyph(struct glyphs *g, struct glyph glyph)
{
    grow(&g->items, &g->cap, g->count + 1, sizeof *g->items);
    g->items[g->count++] = glyph;
}

static int char_width(uint32_t cp)
{
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

/* Append text as glyphs. Control characters become U+FFFD, so nothing in
   the file can reach the terminal as an escape sequence. With tab_column,
   tabs expand to stops counted from *tab_column; otherwise they're spaces. */
static void push_text(struct page *pg, struct glyphs *out, const char *s, size_t n, uint16_t style, int32_t link,
                      int *tab_column)
{
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        if (cp == '\t' || cp == '\n') {
            int spaces = 1;
            if (cp == '\t' && tab_column) spaces = TAB_STOP - *tab_column % TAB_STOP;
            for (int k = 0; k < spaces; ++k) {
                push_glyph(out, (struct glyph){(uint32_t)pg->text.len, 1, 1, style, link});
                buf_putc(&pg->text, ' ');
            }
            if (tab_column) *tab_column += spaces;
            continue;
        }
        if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) cp = 0xfffd;
        char bytes[4];
        size_t len = utf8_encode(cp, bytes);
        int width = char_width(cp);
        if (width == 0 && out->count > 0) {
            struct glyph *prev = &out->items[out->count - 1];
            if (prev->link != GLYPH_BREAK && prev->offset + prev->length == pg->text.len && prev->length + len <= 255) {
                buf_put(&pg->text, bytes, len);
                prev->length = (uint8_t)(prev->length + len);
            }
            continue;
        }
        push_glyph(out, (struct glyph){(uint32_t)pg->text.len, (uint8_t)len, (uint8_t)width, style, link});
        buf_put(&pg->text, bytes, len);
        if (tab_column) *tab_column += width;
    }
}

static void push_string(struct layout *L, struct glyphs *out, const char *s, struct style style)
{
    push_text(L->page, out, s, strlen(s), intern_style(L->page, style), -1, NULL);
}

static int text_width(const char *s)
{
    int width = 0;
    for (size_t i = 0, n = strlen(s); i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        width += char_width(cp);
    }
    return width;
}

static int available(const struct layout *L)
{
    int avail = L->width - L->prefix_width;
    return avail < 1 ? 1 : avail;
}

static void push_prefix(struct layout *L, const char *first, const char *rest, struct style style)
{
    if (L->depth >= (int)(sizeof L->prefixes / sizeof L->prefixes[0])) return;
    L->prefixes[L->depth++] = (struct prefix){first, rest, intern_style(L->page, style), false};
    L->prefix_width += text_width(first);
}

static void pop_prefix(struct layout *L)
{
    --L->depth;
    L->prefix_width -= text_width(L->prefixes[L->depth].first);
}

/* Add a line: the prefixes, then the glyphs. */
static void emit_line(struct layout *L, const struct glyph *glyphs, size_t count, int pad_to, uint16_t pad_style)
{
    struct page *pg = L->page;
    struct glyphs line = {0};
    for (int i = 0; i < L->depth; ++i) {
        struct prefix *p = &L->prefixes[i];
        push_text(pg, &line, p->used ? p->rest : p->first, strlen(p->used ? p->rest : p->first), p->style, -1, NULL);
        p->used = true;
    }
    for (size_t i = 0; i < count; ++i) push_glyph(&line, glyphs[i]);
    /* Blank lines don't keep trailing prefix spaces, unless they're padded
       (a code block's shaded rows). */
    if (count == 0 && pad_to == 0) {
        while (line.count > 0 && pg->text.data[line.items[line.count - 1].offset] == ' ' &&
               line.items[line.count - 1].length == 1) {
            --line.count;
        }
    }
    int width = 0;
    for (size_t i = 0; i < line.count; ++i) width += line.items[i].width;
    grow(&pg->glyphs, &pg->glyph_cap, pg->glyph_count + line.count, sizeof *pg->glyphs);
    if (line.count) memcpy(pg->glyphs + pg->glyph_count, line.items, line.count * sizeof *line.items);
    grow(&pg->lines, &pg->line_cap, pg->line_count + 1, sizeof *pg->lines);
    pg->lines[pg->line_count++] = (struct line){
        .first = pg->glyph_count,
        .count = line.count,
        .width = width,
        .pad_to = pad_to ? L->prefix_width + pad_to : 0,
        .pad_style = pad_style,
        .block = L->block,
        .heading = L->heading,
    };
    pg->glyph_count += line.count;
    free(line.items);
}

static void blank_line(struct layout *L)
{
    /* A blank line inside a block keeps its prefixes but uses none up. */
    bool used[sizeof L->prefixes / sizeof L->prefixes[0]];
    for (int i = 0; i < L->depth; ++i) {
        used[i] = L->prefixes[i].used;
        L->prefixes[i].used = true;
    }
    emit_line(L, NULL, 0, 0, 0);
    for (int i = 0; i < L->depth; ++i) L->prefixes[i].used = used[i];
}

static bool is_space_glyph(const struct page *pg, const struct glyph *g)
{
    return g->length == 1 && pg->text.data[g->offset] == ' ';
}

struct range {
    size_t start, end;
};

struct ranges {
    struct range *items;
    size_t count, cap;
};

/* Greedy wrapping at spaces; words longer than a line break by cells.
   Always gives at least one line. */
static void wrap_ranges(const struct page *pg, const struct glyph *g, size_t n, int avail, struct ranges *out)
{
    out->count = 0;
    size_t start = 0;
    for (;;) {
        while (start < n && is_space_glyph(pg, &g[start])) ++start;
        size_t i = start, last_space = SIZE_MAX, end, next;
        int width = 0;
        for (;;) {
            if (i == n) {
                end = n;
                next = n + 1;
                break;
            }
            if (g[i].link == GLYPH_BREAK) {
                end = i;
                next = i + 1;
                break;
            }
            if (is_space_glyph(pg, &g[i])) last_space = i;
            if (width + g[i].width > avail && i > start) {
                if (last_space != SIZE_MAX && last_space > start) {
                    end = last_space;
                    next = last_space + 1;
                } else {
                    end = i;
                    next = i;
                }
                break;
            }
            width += g[i].width;
            ++i;
        }
        while (end > start && is_space_glyph(pg, &g[end - 1])) --end;
        grow(&out->items, &out->cap, out->count + 1, sizeof *out->items);
        out->items[out->count++] = (struct range){start, end};
        if (next > n) break;
        start = next;
    }
}

static int range_width(const struct glyph *g, struct range r)
{
    int width = 0;
    for (size_t i = r.start; i < r.end; ++i) width += g[i].width;
    return width;
}

static void wrap(struct layout *L, const struct glyph *g, size_t n)
{
    struct ranges lines = {0};
    wrap_ranges(L->page, g, n, available(L), &lines);
    for (size_t i = 0; i < lines.count; ++i) {
        emit_line(L, g + lines.items[i].start, lines.items[i].end - lines.items[i].start, 0, 0);
    }
    free(lines.items);
}

static bool has_scheme(const char *url)
{
    if (!is_ascii_alpha(url[0])) return false;
    size_t i = 1;
    while (is_ascii_alnum(url[i]) || url[i] == '+' || url[i] == '.' || url[i] == '-') ++i;
    return url[i] == ':' && i >= 2;
}

/* Percent-encode what an OSC 8 URI can't carry. */
static void url_encode(struct buf *out, const char *s)
{
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c > 0x20 && c < 0x7f) {
            buf_putc(out, (char)c);
        } else {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", c);
            buf_puts(out, hex);
        }
    }
}

/* The URI an OSC 8 link points to. Relative links resolve against the
   file's directory; from standard input they get no link. */
static char *resolve_url(const struct options *opt, const char *url)
{
    struct buf out = {0};
    if (has_scheme(url)) {
        url_encode(&out, url);
    } else if (!opt->file_url) {
        return NULL;
    } else if (url[0] == '#') {
        buf_puts(&out, opt->file_url);
        url_encode(&out, url);
    } else if (url[0] == '/') {
        const char *host_end = strchr(opt->dir_url + strlen("file://"), '/');
        buf_put(&out, opt->dir_url, (size_t)(host_end - opt->dir_url));
        url_encode(&out, url);
    } else if (url[0] == '\0') {
        buf_puts(&out, opt->file_url);
    } else {
        buf_puts(&out, opt->dir_url);
        url_encode(&out, url);
    }
    return out.data;
}

static struct style with_color(struct style s, int fg)
{
    s.fg = (int16_t)fg;
    return s;
}

/* Turn inlines into glyphs, without recursion: emphasis can nest as deep
   as the text is long. */
static void flatten(struct layout *L, const struct inline_node *first, struct style base, struct glyphs *out)
{
    struct frame {
        const struct inline_node *node;
        struct style style;
        int32_t link;
    };
    struct frame *stack = NULL;
    size_t depth = 0, cap = 0;
    struct style style = base;
    int32_t link = -1;
    const struct inline_node *node = first;
    struct page *pg = L->page;
    for (;;) {
        if (!node) {
            if (depth == 0) break;
            struct frame f = stack[--depth];
            if (f.node->type == I_IMAGE) push_text(pg, out, "]", 1, intern_style(pg, style), link, NULL);
            style = f.style;
            link = f.link;
            node = f.node->next;
            continue;
        }
        switch (node->type) {
        case I_TEXT:
            push_text(pg, out, node->text, node->len, intern_style(pg, style), link, NULL);
            break;
        case I_SOFTBREAK: push_text(pg, out, " ", 1, intern_style(pg, style), link, NULL); break;
        case I_LINEBREAK: push_glyph(out, (struct glyph){(uint32_t)pg->text.len, 0, 0, 0, GLYPH_BREAK}); break;
        case I_CODE: {
            struct style code = style;
            code.fg = COLOR_HIGHLIGHT;
            code.bg = COLOR_SHADE;
            push_text(pg, out, node->text, node->len, intern_style(pg, code), link, NULL);
            break;
        }
        case I_HTML:
            push_text(pg, out, node->text, node->len, intern_style(pg, with_color(style, COLOR_MUTED)), link, NULL);
            break;
        case I_EMPH:
        case I_STRONG:
        case I_STRIKE:
        case I_LINK:
        case I_IMAGE:
            grow(&stack, &cap, depth + 1, sizeof *stack);
            stack[depth++] = (struct frame){node, style, link};
            if (node->type == I_EMPH) {
                style.attr |= ATTR_ITALIC;
            } else if (node->type == I_STRIKE) {
                style.attr |= ATTR_STRIKE;
            } else if (node->type == I_STRONG) {
                style.attr |= ATTR_BOLD;
            } else if (node->type == I_LINK) {
                style.underline = UNDERLINE_SINGLE;
                style.underline_color = COLOR_ACCENT;
                link = add_link(pg, resolve_url(L->opt, node->url));
            } else {
                style.attr |= ATTR_ITALIC;
                style.fg = COLOR_MUTED;
                link = add_link(pg, resolve_url(L->opt, node->url));
                push_text(pg, out, "[image: ", 8, intern_style(pg, style), link, NULL);
            }
            node = node->first;
            continue;
        }
        node = node->next;
    }
    free(stack);
}

static void layout_blocks(struct layout *L, const struct block *first, bool tight);

static void layout_paragraph(struct layout *L, const struct block *b)
{
    L->tmp.count = 0;
    flatten(L, b->inlines->first, L->base, &L->tmp);
    wrap(L, L->tmp.items, L->tmp.count);
}

static void layout_heading(struct layout *L, const struct block *b)
{
    struct style style = L->base;
    style.attr |= ATTR_BOLD;
    L->tmp.count = 0;
    switch (b->level) {
    case 1:
        style.fg = COLOR_ACCENT;
        style.underline = UNDERLINE_DOUBLE;
        break;
    case 2: style.fg = COLOR_ACCENT; break;
    case 3: break;
    default: {
        char marks[8];
        memset(marks, '#', (size_t)b->level);
        marks[b->level] = ' ';
        marks[b->level + 1] = '\0';
        struct style dim = with_color(L->base, COLOR_MUTED);
        dim.attr |= ATTR_DIM;
        push_string(L, &L->tmp, marks, dim);
        style.fg = COLOR_MUTED;
        break;
    }
    }
    size_t marks = L->tmp.count;
    flatten(L, b->inlines->first, style, &L->tmp);
    struct buf text = {0};
    for (size_t i = marks; i < L->tmp.count; ++i) {
        const struct glyph *g = &L->tmp.items[i];
        if (g->link == GLYPH_BREAK) buf_putc(&text, ' ');
        else buf_put(&text, L->page->text.data + g->offset, g->length);
    }
    struct page *pg = L->page;
    grow(&pg->headings, &pg->heading_cap, pg->heading_count + 1, sizeof *pg->headings);
    pg->headings[pg->heading_count] = text.data ? text.data : xstrdup("");
    L->heading = (int32_t)pg->heading_count++;
    wrap(L, L->tmp.items, L->tmp.count);
}

/* A preformatted line, cut with … when it's wider than max. */
static void cut_line(struct layout *L, struct glyphs *g, size_t from, int max, struct style ellipsis)
{
    int width = 0;
    for (size_t i = from; i < g->count; ++i) width += g->items[i].width;
    if (width <= max) return;
    width = 0;
    size_t keep = from;
    while (keep < g->count && width + g->items[keep].width <= max - 1) width += g->items[keep++].width;
    g->count = keep;
    push_string(L, g, "…", ellipsis);
}

/* Code sits on a shaded band with two columns of padding on each side and
   a shaded row above and below. Lines too long for the band wrap: at the
   last space when there is one, otherwise at the cell, and the rest
   continues under the line's own indentation. */
static void layout_code(struct layout *L, const struct block *b)
{
    struct page *pg = L->page;
    int avail = available(L);
    struct style code = L->base;
    code.attr &= (uint8_t)~ATTR_ITALIC;
    code.bg = COLOR_SHADE;
    uint16_t code_style = intern_style(pg, code);
    bool shade = L->opt->styled && L->opt->color;
    int pad = shade ? avail : 0;
    int inner = avail - (shade ? 4 : 2);
    if (inner < 1) inner = 1;

    if (shade) emit_line(L, NULL, 0, pad, code_style);
    const char *s = b->content.data ? b->content.data : "";
    size_t n = b->content.len;
    struct glyphs out = {0};
    for (size_t start = 0; start < n;) {
        const char *newline = memchr(s + start, '\n', n - start);
        size_t end = newline ? (size_t)(newline - s) : n;
        L->tmp.count = 0;
        int column = 0;
        push_text(pg, &L->tmp, s + start, end - start, code_style, -1, &column);
        const struct glyph *g = L->tmp.items;
        size_t count = L->tmp.count;
        size_t lead = 0;
        int indent = 0;
        while (lead < count && is_space_glyph(pg, &g[lead])) indent += g[lead++].width;
        if (indent > inner / 2) indent = 0;
        size_t pos = 0;
        do {
            bool first = pos == 0;
            int room = inner - (first ? 0 : indent);
            size_t stop = pos;
            int width = 0;
            while (stop < count && (width + g[stop].width <= room || stop == pos)) width += g[stop++].width;
            /* Combining marks stay with their base. */
            while (stop < count && g[stop].width == 0) ++stop;
            if (stop < count) {
                size_t space = stop;
                while (space > pos + 1 && space > lead && !is_space_glyph(pg, &g[space - 1])) --space;
                if (space > pos + 1 && space > lead && is_space_glyph(pg, &g[space - 1])) stop = space;
            }
            out.count = 0;
            push_text(pg, &out, "  ", 2, code_style, -1, NULL);
            for (int i = 0; !first && i < indent; ++i) push_text(pg, &out, " ", 1, code_style, -1, NULL);
            size_t shown = stop;
            while (stop < count && shown > pos && is_space_glyph(pg, &g[shown - 1])) --shown;
            for (size_t i = pos; i < shown; ++i) push_glyph(&out, g[i]);
            emit_line(L, out.items, out.count, pad, code_style);
            pos = stop;
            while (pos < count && is_space_glyph(pg, &g[pos])) ++pos;
        } while (pos < count);
        start = end + 1;
    }
    free(out.items);
    if (n == 0 && !shade) emit_line(L, NULL, 0, 0, 0);
    if (shade) emit_line(L, NULL, 0, pad, code_style);
}

static void layout_html(struct layout *L, const struct block *b)
{
    struct page *pg = L->page;
    int avail = available(L);
    struct style muted = with_color(L->base, COLOR_MUTED);
    uint16_t style = intern_style(pg, muted);
    const char *s = b->content.data ? b->content.data : "";
    size_t n = b->content.len;
    while (n > 0 && s[n - 1] == '\n') --n;
    for (size_t start = 0; start <= n;) {
        const char *newline = memchr(s + start, '\n', n - start);
        size_t end = newline ? (size_t)(newline - s) : n;
        L->tmp.count = 0;
        int column = 0;
        push_text(pg, &L->tmp, s + start, end - start, style, -1, &column);
        cut_line(L, &L->tmp, 0, avail, muted);
        emit_line(L, L->tmp.items, L->tmp.count, 0, 0);
        start = end + 1;
    }
}

static void layout_rule(struct layout *L)
{
    int avail = available(L);
    L->tmp.count = 0;
    struct style muted = with_color(L->base, COLOR_MUTED);
    muted.attr &= (uint8_t)~ATTR_ITALIC;
    for (int i = 0; i < avail; ++i) push_string(L, &L->tmp, "─", muted);
    emit_line(L, L->tmp.items, L->tmp.count, 0, 0);
}

static void layout_quote(struct layout *L, const struct block *b)
{
    struct style bar = with_color(plain_style, COLOR_MUTED);
    push_prefix(L, "│ ", "│ ", bar);
    struct style saved = L->base;
    L->base.attr |= ATTR_ITALIC;
    if (b->first) layout_blocks(L, b->first, false);
    else emit_line(L, NULL, 0, 0, 0);
    L->base = saved;
    pop_prefix(L);
}

static void layout_list(struct layout *L, const struct block *b)
{
    static const char *const bullets[] = {"•", "◦", "▪"};
    int count = 0;
    for (const struct block *item = b->first; item; item = item->next) ++count;
    long last = (long)b->list.start + count - 1;
    int digits = 1;
    for (long v = last > b->list.start ? last : b->list.start; v >= 10; v /= 10) ++digits;
    struct style marker_style = with_color(plain_style, COLOR_ACCENT);
    long number = b->list.start;
    for (const struct block *item = b->first; item; item = item->next, ++number) {
        char first[64], rest[64];
        const char *box = item->task == 2 ? "☑ " : item->task == 1 ? "☐ " : "";
        if (b->list.ordered) {
            char value[24];
            int len = snprintf(value, sizeof value, "%ld", number);
            memset(first, ' ', (size_t)(digits - len));
            snprintf(first + digits - len, sizeof first - (size_t)(digits - len), "%s%c %s", value,
                     b->list.delimiter, box);
        } else {
            /* A task's box takes the bullet's place. */
            snprintf(first, sizeof first, "%s", item->task ? box : bullets[L->list_depth % 3]);
            if (!item->task) strcat(first, " ");
        }
        int indent = text_width(first);
        memset(rest, ' ', (size_t)indent);
        rest[indent] = '\0';
        if (item != b->first && !b->list.tight) blank_line(L);
        push_prefix(L, first, rest, item->task == 2 ? with_color(plain_style, COLOR_MUTED) : marker_style);
        ++L->list_depth;
        struct style saved = L->base;
        if (item->task == 2) {
            /* Done: muted and struck through. */
            L->base.attr |= ATTR_STRIKE;
            L->base.fg = COLOR_MUTED;
        }
        if (item->first) layout_blocks(L, item->first, b->list.tight);
        else emit_line(L, NULL, 0, 0, 0);
        L->base = saved;
        --L->list_depth;
        pop_prefix(L);
    }
}

/* A border line such as ┌───┬───┐. */
static void table_border(struct layout *L, const int *widths, int columns, const char *left, const char *middle,
                         const char *right, struct style style)
{
    L->tmp.count = 0;
    push_string(L, &L->tmp, left, style);
    for (int c = 0; c < columns; ++c) {
        for (int i = 0; i < widths[c] + 2; ++i) push_string(L, &L->tmp, "─", style);
        push_string(L, &L->tmp, c + 1 < columns ? middle : right, style);
    }
    if (!L->opt->pager) cut_line(L, &L->tmp, 0, available(L), style);
    emit_line(L, L->tmp.items, L->tmp.count, 0, 0);
}

static void layout_table(struct layout *L, const struct block *b)
{
    struct page *pg = L->page;
    int columns = b->columns;
    int rows = 0;
    for (const struct block *row = b->first; row; row = row->next) ++rows;

    /* Every cell as glyphs, header cells bold. */
    struct glyphs *cells = xmalloc((size_t)rows * (size_t)columns * sizeof *cells);
    memset(cells, 0, (size_t)rows * (size_t)columns * sizeof *cells);
    int *natural = xmalloc((size_t)columns * sizeof *natural);
    int *word = xmalloc((size_t)columns * sizeof *word);
    int *widths = xmalloc((size_t)columns * sizeof *widths);
    memset(natural, 0, (size_t)columns * sizeof *natural);
    memset(word, 0, (size_t)columns * sizeof *word);
    struct style base = L->base;
    struct ranges lines = {0};
    int r = 0;
    for (const struct block *row = b->first; row; row = row->next, ++r) {
        struct style style = base;
        if (row->header) style.attr |= ATTR_BOLD;
        for (int c = 0; c < columns; ++c) {
            struct glyphs *cell = &cells[r * columns + c];
            flatten(L, row->cells[c]->first, style, cell);
            wrap_ranges(pg, cell->items, cell->count, INT_MAX, &lines);
            for (size_t k = 0; k < lines.count; ++k) {
                int w = range_width(cell->items, lines.items[k]);
                if (w > natural[c]) natural[c] = w;
            }
            int run = 0;
            for (size_t i = 0; i < cell->count; ++i) {
                run = is_space_glyph(pg, &cell->items[i]) || cell->items[i].link == GLYPH_BREAK ? 0
                                                                                                 : run + cell->items[i].width;
                if (run > word[c]) word[c] = run;
            }
        }
    }

    /* Fit. Each column gets its longest word, and the room left is shared
       in proportion to what each still wants. When even the words don't
       fit, cap the widest columns together instead, keeping each at least
       6 cells (or its natural width) wide. */
    int budget = available(L) - (3 * columns + 1);
    long natural_total = 0, word_total = 0;
    for (int c = 0; c < columns; ++c) {
        natural_total += natural[c];
        word_total += word[c];
    }
    int widest = 0;
    for (int c = 0; c < columns; ++c) widest = natural[c] > widest ? natural[c] : widest;
    int low = 0, high = widest;
    while (low < high) {
        int cap = low + (high - low + 1) / 2;
        long total = 0;
        for (int c = 0; c < columns; ++c) {
            int minimum = natural[c] < 6 ? natural[c] : 6;
            int w = natural[c] < cap ? natural[c] : cap;
            total += w > minimum ? w : minimum;
        }
        if (total <= budget) low = cap;
        else high = cap - 1;
    }
    for (int c = 0; c < columns; ++c) {
        int minimum = natural[c] < 6 ? natural[c] : 6;
        int w = natural[c] < low ? natural[c] : low;
        widths[c] = w > minimum ? w : minimum;
        if (widths[c] < 1) widths[c] = 1;
    }
    if (natural_total <= budget) {
        for (int c = 0; c < columns; ++c) widths[c] = natural[c] > 0 ? natural[c] : 1;
    } else if (word_total <= budget) {
        long spare = budget - word_total, wanted = natural_total - word_total, given = 0;
        for (int c = 0; c < columns; ++c) {
            long extra = wanted ? (long)(natural[c] - word[c]) * spare / wanted : 0;
            widths[c] = word[c] + (int)extra;
            given += extra;
        }
        /* Rounding leftovers go to the columns that still want room. */
        for (int c = 0; c < columns && given < spare; ++c) {
            if (widths[c] < natural[c]) {
                ++widths[c];
                ++given;
            }
        }
        for (int c = 0; c < columns; ++c) {
            if (widths[c] < 1) widths[c] = 1;
        }
    }

    struct style border = with_color(plain_style, COLOR_MUTED);
    uint16_t space = intern_style(pg, base);
    struct ranges *cell_lines = xmalloc((size_t)columns * sizeof *cell_lines);
    memset(cell_lines, 0, (size_t)columns * sizeof *cell_lines);
    /* When a body row wraps, rule between all the body rows so they stay
       apart. */
    bool ruled = false;
    r = 0;
    for (const struct block *row = b->first; row && !ruled; row = row->next, ++r) {
        for (int c = 0; c < columns && !row->header; ++c) {
            struct glyphs *cell = &cells[r * columns + c];
            wrap_ranges(pg, cell->items, cell->count, widths[c], &lines);
            if (lines.count > 1) ruled = true;
        }
    }
    table_border(L, widths, columns, "┌", "┬", "┐", border);
    r = 0;
    for (const struct block *row = b->first; row; row = row->next, ++r) {
        size_t height = 1;
        for (int c = 0; c < columns; ++c) {
            struct glyphs *cell = &cells[r * columns + c];
            wrap_ranges(pg, cell->items, cell->count, widths[c], &cell_lines[c]);
            if (cell_lines[c].count > height) height = cell_lines[c].count;
        }
        for (size_t k = 0; k < height; ++k) {
            L->tmp.count = 0;
            push_string(L, &L->tmp, "│", border);
            for (int c = 0; c < columns; ++c) {
                struct glyphs *cell = &cells[r * columns + c];
                struct range range = k < cell_lines[c].count ? cell_lines[c].items[k] : (struct range){0, 0};
                int gap = widths[c] - range_width(cell->items, range);
                if (gap < 0) gap = 0;
                char align = b->align[c];
                int before = align == 'r' ? gap : align == 'c' ? gap / 2 : 0;
                for (int i = 0; i < before + 1; ++i) push_text(pg, &L->tmp, " ", 1, space, -1, NULL);
                for (size_t i = range.start; i < range.end; ++i) push_glyph(&L->tmp, cell->items[i]);
                for (int i = 0; i < gap - before + 1; ++i) push_text(pg, &L->tmp, " ", 1, space, -1, NULL);
                push_string(L, &L->tmp, "│", border);
            }
            if (!L->opt->pager) cut_line(L, &L->tmp, 0, available(L), border);
            emit_line(L, L->tmp.items, L->tmp.count, 0, 0);
        }
        if (row->next && (row->header || ruled)) table_border(L, widths, columns, "├", "┼", "┤", border);
    }
    table_border(L, widths, columns, "└", "┴", "┘", border);

    for (int i = 0; i < rows * columns; ++i) free(cells[i].items);
    for (int c = 0; c < columns; ++c) free(cell_lines[c].items);
    free(cell_lines);
    free(cells);
    free(natural);
    free(word);
    free(widths);
    free(lines.items);
}

static void layout_block(struct layout *L, const struct block *b)
{
    L->block = L->blocks++;
    switch (b->type) {
    case B_PARAGRAPH: layout_paragraph(L, b); break;
    case B_HEADING: layout_heading(L, b); break;
    case B_BREAK: layout_rule(L); break;
    case B_CODE: layout_code(L, b); break;
    case B_HTML: layout_html(L, b); break;
    case B_QUOTE: layout_quote(L, b); break;
    case B_LIST: layout_list(L, b); break;
    case B_TABLE: layout_table(L, b); break;
    case B_ROW: break;
    case B_DOCUMENT:
    case B_ITEM: layout_blocks(L, b->first, false); break;
    }
}

static void layout_blocks(struct layout *L, const struct block *first, bool tight)
{
    for (const struct block *b = first; b; b = b->next) {
        if (b != first && !tight) blank_line(L);
        layout_block(L, b);
    }
}

static void layout_document(struct page *pg, const struct document *doc, const struct options *opt)
{
    struct layout L = {.page = pg, .opt = opt, .width = opt->width, .base = plain_style, .heading = -1};
    intern_style(pg, plain_style);
    layout_blocks(&L, doc->root->first, false);
    free(L.tmp.items);
}

static void free_page(struct page *pg)
{
    buf_free(&pg->text);
    free(pg->glyphs);
    free(pg->lines);
    free(pg->styles);
    for (size_t i = 0; i < pg->link_count; ++i) free(pg->links[i]);
    free(pg->links);
    for (size_t i = 0; i < pg->heading_count; ++i) free(pg->headings[i]);
    free(pg->headings);
    *pg = (struct page){0};
}

/* Output */

static void put_color(struct buf *out, int base, int color)
{
    char code[16];
    if (color < 8) snprintf(code, sizeof code, ";%d", base + color);
    else if (color < 16) snprintf(code, sizeof code, ";%d", base + 60 + color - 8);
    else snprintf(code, sizeof code, ";%d:5:%d", base + 8, color);
    buf_puts(out, code);
}

static void put_style(struct buf *out, const struct style *s, bool color)
{
    buf_puts(out, "\x1b[0");
    if (s->attr & ATTR_BOLD) buf_puts(out, ";1");
    if (s->attr & ATTR_DIM) buf_puts(out, ";2");
    if (s->attr & ATTR_ITALIC) buf_puts(out, ";3");
    if (s->underline == UNDERLINE_SINGLE) buf_puts(out, ";4");
    else if (s->underline) {
        char code[8];
        snprintf(code, sizeof code, ";4:%d", s->underline);
        buf_puts(out, code);
    }
    if (s->attr & ATTR_STRIKE) buf_puts(out, ";9");
    if (color) {
        if (s->fg != COLOR_NONE) put_color(out, 30, s->fg);
        if (s->bg != COLOR_NONE) put_color(out, 40, s->bg);
        if (s->underline && s->underline_color != COLOR_NONE) {
            char code[16];
            snprintf(code, sizeof code, ";58:5:%d", s->underline_color);
            buf_puts(out, code);
        }
    }
    buf_putc(out, 'm');
}

static void put_link(struct buf *out, const char *url)
{
    buf_puts(out, "\x1b]8;;");
    if (url) buf_puts(out, url);
    buf_puts(out, "\x1b\\");
}

/* The line's cells from column from, at most cols of them (INT_MAX for
   all). A wide character cut by the left edge shows as spaces in its style. */
static void render_cells(struct buf *out, const struct page *pg, const struct line *line, const struct options *opt,
                         int from, int cols)
{
    int style = 0;
    int32_t link = -1;
    int end = cols == INT_MAX ? INT_MAX : from + cols;
    int column = 0;
    for (size_t i = 0; i < line->count && column < end; ++i) {
        const struct glyph *g = &pg->glyphs[line->first + i];
        int next = column + g->width;
        if (next > end) break;
        bool partial = column < from;
        if (partial && next <= from) {
            column = next;
            continue;
        }
        if (opt->styled) {
            if (g->style != style) {
                put_style(out, &pg->styles[g->style], opt->color);
                style = g->style;
            }
            int32_t target = g->link >= 0 && !partial ? g->link : -1;
            if (target != link) {
                put_link(out, target >= 0 ? pg->links[target] : NULL);
                link = target;
            }
        }
        if (partial) {
            for (int k = from; k < next; ++k) buf_putc(out, ' ');
        } else {
            buf_put(out, pg->text.data + g->offset, g->length);
        }
        column = next;
    }
    int pad_end = line->pad_to < end ? line->pad_to : end;
    int pad_start = line->width > from ? line->width : from;
    if (opt->styled && opt->color && pad_end > pad_start) {
        if (link >= 0) {
            put_link(out, NULL);
            link = -1;
        }
        if (line->pad_style != style) {
            put_style(out, &pg->styles[line->pad_style], opt->color);
            style = line->pad_style;
        }
        for (int i = pad_start; i < pad_end; ++i) buf_putc(out, ' ');
    }
    if (link >= 0) put_link(out, NULL);
    if (style != 0) buf_puts(out, "\x1b[0m");
}

static void render_line(struct buf *out, const struct page *pg, const struct line *line, const struct options *opt)
{
    render_cells(out, pg, line, opt, 0, INT_MAX);
    buf_putc(out, '\n');
}

static bool write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t written = write(fd, s, n);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        s += written;
        n -= (size_t)written;
    }
    return true;
}

static bool print_page(const struct page *pg, const struct options *opt)
{
    struct buf out = {0};
    bool ok = true;
    for (size_t i = 0; i < pg->line_count && ok; ++i) {
        render_line(&out, pg, &pg->lines[i], opt);
        if (out.len >= 65536) {
            ok = write_all(STDOUT_FILENO, out.data, out.len);
            out.len = 0;
        }
    }
    if (ok && out.len) ok = write_all(STDOUT_FILENO, out.data, out.len);
    buf_free(&out);
    return ok;
}

/* Pager */

static volatile sig_atomic_t pager_resized, pager_stop, pager_continued;

static void pager_signal(int sig)
{
    if (sig == SIGWINCH) pager_resized = 1;
    else if (sig == SIGCONT) pager_continued = 1;
    else pager_stop = sig;
}

struct pager {
    int tty;                 /* Keys come from here; frames go to stdout. */
    struct termios saved;
    bool active;
    int rows, cols;
    size_t top;              /* The first line on screen. */
    int left;                /* Columns scrolled off the left of wide lines. */
    int widest;              /* The widest line, padding included. */
    bool fixed_width;        /* --width was given. */
    const char *name;
    const struct document *doc;
    struct options *opt;
    struct page page;
    unsigned char input[256];
    size_t input_len;
    bool quit;
};

static const int pager_signals[] = {SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCONT};

static bool pager_write(const char *s)
{
    return write_all(STDOUT_FILENO, s, strlen(s));
}

/* Raw input with ISIG off, so Ctrl+C and Ctrl+Z arrive as keys; the
   alternate screen; no cursor; wheel and click reports in SGR form. */
static void pager_enter(struct pager *P)
{
    struct termios raw = P->saved;
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(P->tty, TCSAFLUSH, &raw);
    pager_write("\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1006h");
    P->active = true;
}

static void pager_leave(struct pager *P)
{
    if (!P->active) return;
    pager_write("\x1b[?1006l\x1b[?1000l\x1b[0m\x1b[?1049l\x1b[?25h");
    tcsetattr(P->tty, TCSAFLUSH, &P->saved);
    P->active = false;
}

static int pager_text_rows(const struct pager *P)
{
    return P->rows > 1 ? P->rows - 1 : P->rows;
}

static size_t pager_max_top(const struct pager *P)
{
    size_t rows = (size_t)pager_text_rows(P);
    return P->page.line_count > rows ? P->page.line_count - rows : 0;
}

static void pager_clamp(struct pager *P)
{
    if (P->top > pager_max_top(P)) P->top = pager_max_top(P);
    int max_left = P->widest > P->cols ? P->widest - P->cols : 0;
    if (P->left > max_left) P->left = max_left;
    if (P->left < 0) P->left = 0;
}

static void pager_layout(struct pager *P)
{
    free_page(&P->page);
    layout_document(&P->page, P->doc, P->opt);
    P->widest = 0;
    for (size_t i = 0; i < P->page.line_count; ++i) {
        const struct line *line = &P->page.lines[i];
        int width = line->width > line->pad_to ? line->width : line->pad_to;
        if (width > P->widest) P->widest = width;
    }
}

static void pager_size(struct pager *P)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        P->cols = ws.ws_col;
        P->rows = ws.ws_row;
    } else {
        P->cols = DEFAULT_WIDTH;
        P->rows = 24;
    }
}

/* A new terminal size. The text reflows when its width follows the
   terminal, and the block at the top of the screen stays there, as far
   through itself as it was. */
static void pager_resize(struct pager *P)
{
    pager_size(P);
    int width = P->cols < DEFAULT_WIDTH ? P->cols : DEFAULT_WIDTH;
    if (!P->fixed_width && width != P->opt->width) {
        int32_t block = -1;
        size_t into = 0, length = 1;
        if (P->top < P->page.line_count) {
            const struct line *lines = P->page.lines;
            block = lines[P->top].block;
            size_t first = P->top, last = P->top;
            while (first > 0 && lines[first - 1].block == block) --first;
            while (last + 1 < P->page.line_count && lines[last + 1].block == block) ++last;
            into = P->top - first;
            length = last - first + 1;
        }
        P->opt->width = width;
        pager_layout(P);
        if (block >= 0) {
            const struct line *lines = P->page.lines;
            size_t first = 0;
            while (first < P->page.line_count && lines[first].block < block) ++first;
            size_t last = first;
            while (last + 1 < P->page.line_count && lines[last + 1].block == block) ++last;
            size_t now = first < P->page.line_count ? last - first + 1 : 1;
            P->top = first + into * now / length;
        }
    }
    pager_clamp(P);
}

/* Text for the status line, made safe and cut with … to max cells. */
static int put_clipped(struct buf *out, const char *s, int max)
{
    int width = text_width(s) <= max ? INT_MAX : max - 1;
    int used = 0;
    for (size_t i = 0, n = strlen(s); i < n;) {
        uint32_t cp;
        i += utf8_decode(s + i, n - i, &cp);
        if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) cp = 0xfffd;
        int w = char_width(cp);
        if (used + w > width) break;
        char bytes[4];
        buf_put(out, bytes, utf8_encode(cp, bytes));
        used += w;
    }
    if (width != INT_MAX && max > 0) {
        buf_puts(out, "…");
        ++used;
    }
    return used;
}

static void pager_status(const struct pager *P, struct buf *out)
{
    char position[32];
    size_t rows = (size_t)pager_text_rows(P);
    if (P->page.line_count <= rows) {
        snprintf(position, sizeof position, "All ");
    } else if (P->top == 0) {
        snprintf(position, sizeof position, "Top ");
    } else if (P->top >= pager_max_top(P)) {
        snprintf(position, sizeof position, "End ");
    } else {
        snprintf(position, sizeof position, "%zu%% ", (P->top + rows) * 100 / P->page.line_count);
    }
    struct buf left = {0};
    buf_putc(&left, ' ');
    buf_puts(&left, P->name);
    int32_t heading = P->top < P->page.line_count ? P->page.lines[P->top].heading : -1;
    if (heading >= 0) {
        buf_puts(&left, "  ›  ");
        buf_puts(&left, P->page.headings[heading]);
    }
    buf_putc(&left, '\0');
    int right = (int)strlen(position);
    buf_puts(out, "\x1b[0;7m");
    int room = P->cols - right - 1;
    int used = room > 0 ? put_clipped(out, left.data, room) : 0;
    for (int i = used; i < P->cols - right; ++i) buf_putc(out, ' ');
    if (right <= P->cols) buf_puts(out, position);
    buf_puts(out, "\x1b[0m");
    buf_free(&left);
}

/* One frame, drawn whole inside synchronized output so it never tears. */
static bool pager_draw(const struct pager *P)
{
    struct buf out = {0};
    buf_puts(&out, "\x1b[?2026h\x1b[H");
    int rows = pager_text_rows(P);
    for (int r = 0; r < rows; ++r) {
        size_t i = P->top + (size_t)r;
        if (i < P->page.line_count) {
            const struct line *line = &P->page.lines[i];
            int width = line->width > line->pad_to ? line->width : line->pad_to;
            int from = width > P->cols ? (P->left < width - P->cols ? P->left : width - P->cols) : 0;
            render_cells(&out, &P->page, line, P->opt, from, P->cols);
        }
        buf_puts(&out, "\x1b[K");
        if (r + 1 < P->rows) buf_puts(&out, "\r\n");
    }
    if (rows < P->rows) pager_status(P, &out);
    buf_puts(&out, "\x1b[?2026l");
    bool ok = write_all(STDOUT_FILENO, out.data, out.len);
    buf_free(&out);
    return ok;
}

static void pager_scroll(struct pager *P, long delta)
{
    if (delta < 0 && (size_t)-delta > P->top) P->top = 0;
    else P->top += (size_t)delta;
    pager_clamp(P);
}

static void pager_suspend(struct pager *P)
{
    pager_leave(P);
    raise(SIGTSTP);
    pager_enter(P);
    pager_resize(P);
}

/* Keys: arrows and the like come as CSI or SS3 sequences, the wheel as SGR
   mouse reports. */
static void pager_key(struct pager *P, int key)
{
    long page = pager_text_rows(P);
    long half = page / 2 > 0 ? page / 2 : 1;
    int step = P->cols / 4 > 8 ? P->cols / 4 : 8;
    switch (key) {
    case 'j': case 'e': case '\r': case '\n': case 0x05: case 0x0e: pager_scroll(P, 1); break;
    case 'k': case 'y': case 0x10: case 0x19: pager_scroll(P, -1); break;
    case ' ': case 'f': case 0x06: pager_scroll(P, page); break;
    case 'b': case 0x02: pager_scroll(P, -page); break;
    case 'd': case 0x04: pager_scroll(P, half); break;
    case 'u': case 0x15: pager_scroll(P, -half); break;
    case 'g': case '<': P->top = 0; break;
    case 'G': case '>': P->top = pager_max_top(P); break;
    case 'l': P->left += step; pager_clamp(P); break;
    case 'h': P->left -= step; pager_clamp(P); break;
    case 'q': case 'Q': case 0x03: case 0x1b: P->quit = true; break;
    case 0x1a: pager_suspend(P); break;
    default: break; /* r and Ctrl+L just redraw, as every key does. */
    }
}

enum { KEY_UP = 0x100, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN };

static void pager_special(struct pager *P, int key)
{
    static const int keys[][2] = {
        {KEY_UP, 'k'},   {KEY_DOWN, 'j'},     {KEY_RIGHT, 'l'},     {KEY_LEFT, 'h'},
        {KEY_HOME, 'g'}, {KEY_END, 'G'},      {KEY_PAGE_UP, 'b'},   {KEY_PAGE_DOWN, ' '},
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        if (keys[i][0] == key) pager_key(P, keys[i][1]);
    }
}

static void pager_csi(struct pager *P, bool mouse, const int *params, int count, char final)
{
    int first = count > 0 ? params[0] : 0;
    if (mouse) {
        if (final != 'M') return;
        int button = first & ~(4 | 8 | 16); /* Without Shift, Alt and Ctrl. */
        if (button == 64) pager_scroll(P, -3);
        else if (button == 65) pager_scroll(P, 3);
        else if (button == 66) pager_special(P, KEY_LEFT);
        else if (button == 67) pager_special(P, KEY_RIGHT);
        return;
    }
    switch (final) {
    case 'A': pager_special(P, KEY_UP); break;
    case 'B': pager_special(P, KEY_DOWN); break;
    case 'C': pager_special(P, KEY_RIGHT); break;
    case 'D': pager_special(P, KEY_LEFT); break;
    case 'H': pager_special(P, KEY_HOME); break;
    case 'F': pager_special(P, KEY_END); break;
    case '~':
        if (first == 1 || first == 7) pager_special(P, KEY_HOME);
        else if (first == 4 || first == 8) pager_special(P, KEY_END);
        else if (first == 5) pager_special(P, KEY_PAGE_UP);
        else if (first == 6) pager_special(P, KEY_PAGE_DOWN);
        break;
    default: break;
    }
}

/* Handles the keys in P->input and keeps an unfinished escape sequence
   for later, unless flush says no more is coming: then a lone Escape is
   the Escape key and a cut-off sequence is dropped. */
static void pager_input(struct pager *P, bool flush)
{
    const unsigned char *s = P->input;
    size_t n = P->input_len, i = 0;
    while (i < n && !P->quit) {
        if (s[i] != 0x1b) {
            pager_key(P, s[i++]);
            continue;
        }
        if (i + 1 == n) {
            if (!flush) break;
            pager_key(P, 0x1b);
            ++i;
            continue;
        }
        unsigned char next = s[i + 1];
        if (next == '[') {
            size_t j = i + 2;
            bool mouse = j < n && s[j] == '<';
            if (mouse) ++j;
            int params[4] = {0}, count = 0;
            bool digits = false;
            while (j < n && s[j] >= 0x20 && s[j] <= 0x3f) {
                if (s[j] >= '0' && s[j] <= '9') {
                    if (count < 4 && params[count] < 100000) params[count] = params[count] * 10 + (s[j] - '0');
                    digits = true;
                } else if (s[j] == ';') {
                    if (count < 4) ++count;
                    digits = false;
                }
                ++j;
            }
            if (j == n) {
                if (!flush) break;
                i = n;
                continue;
            }
            if (digits || count > 0) ++count;
            if (count > 4) count = 4;
            if (s[j] >= 0x40 && s[j] <= 0x7e) pager_csi(P, mouse, params, count, (char)s[j]);
            i = j + 1;
        } else if (next == 'O') {
            if (i + 2 == n) {
                if (!flush) break;
                i = n;
                continue;
            }
            pager_csi(P, false, NULL, 0, (char)s[i + 2]);
            i += 3;
        } else if (next == 0x1b) {
            pager_key(P, 0x1b);
            ++i;
        } else {
            i += 2; /* Alt and a key: nothing is bound to it. */
        }
    }
    if (P->quit) i = n;
    memmove(P->input, P->input + i, n - i);
    P->input_len = n - i;
}

/* Shows the page until a quit key or a signal. Returns false when there's
   no terminal to page on, and the caller prints instead. */
static bool run_pager(const struct document *doc, struct options *opt, bool fixed_width)
{
    struct pager P = {.doc = doc, .opt = opt, .fixed_width = fixed_width};
    P.tty = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (P.tty < 0) return false;
    if (tcgetattr(P.tty, &P.saved) != 0) {
        close(P.tty);
        return false;
    }
    if (opt->path) {
        const char *slash = strrchr(opt->path, '/');
        P.name = slash && slash[1] ? slash + 1 : opt->path;
    } else {
        P.name = "standard input";
    }

    sigset_t blocked, waiting;
    sigemptyset(&blocked);
    struct sigaction action = {.sa_handler = pager_signal};
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof pager_signals / sizeof pager_signals[0]; ++i) {
        sigaction(pager_signals[i], &action, NULL);
        sigaddset(&blocked, pager_signals[i]);
    }
    sigprocmask(SIG_BLOCK, &blocked, &waiting);
    for (size_t i = 0; i < sizeof pager_signals / sizeof pager_signals[0]; ++i) sigdelset(&waiting, pager_signals[i]);

    opt->pager = true;
    pager_size(&P);
    if (!fixed_width) opt->width = P.cols < DEFAULT_WIDTH ? P.cols : DEFAULT_WIDTH;
    pager_layout(&P);
    pager_enter(&P);
    bool redraw = true;
    while (!P.quit && !pager_stop) {
        if (pager_continued) {
            pager_continued = 0;
            pager_enter(&P);
            redraw = true;
        }
        if (pager_resized) {
            pager_resized = 0;
            pager_resize(&P);
            redraw = true;
        }
        if (redraw && !pager_draw(&P)) break;
        redraw = false;
        struct pollfd fd = {.fd = P.tty, .events = POLLIN};
        /* A lone Escape is the key itself once 25 ms pass with nothing after it. */
        struct timespec wait = {0, 25 * 1000 * 1000};
        int ready = ppoll(&fd, 1, P.input_len ? &wait : NULL, &waiting);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) {
            pager_input(&P, true);
            redraw = true;
            continue;
        }
        if (fd.revents & POLLIN) {
            ssize_t got = read(P.tty, P.input + P.input_len, sizeof P.input - P.input_len);
            if (got <= 0) {
                if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                break;
            }
            P.input_len += (size_t)got;
            pager_input(&P, P.input_len == sizeof P.input);
            redraw = true;
        } else if (fd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            break;
        }
    }
    pager_leave(&P);
    close(P.tty);
    free_page(&P.page);
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGWINCH, &ignore, NULL);
    sigaction(SIGCONT, &ignore, NULL);
    /* Killed: go on to die by the same signal, so the shell sees it. */
    if (pager_stop) {
        signal(pager_stop, SIG_DFL);
        raise(pager_stop);
    }
    sigprocmask(SIG_UNBLOCK, &blocked, NULL);
    return true;
}

/* Command line */

static void usage(FILE *out)
{
    fputs("Usage: mark [OPTIONS] [FILE]\n"
          "Show a markdown file in the terminal, in a pager when the output is one.\n"
          "\n"
          "  FILE          Markdown file. \"-\" or no argument reads standard input\n"
          "                when it isn't a terminal.\n"
          "  --width N     Text width (default: the terminal's, up to 80)\n"
          "  --plain       No colors or attributes; prints instead of paging\n"
          "  --no-pager    Print instead of paging, even to a terminal\n"
          "  -h, --help    Show this help\n"
          "  -v, --version Show the version\n",
          out);
}

static bool read_input(const char *path, char **data, size_t *len)
{
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : STDIN_FILENO;
    const char *name = path ? path : "standard input";
    if (fd < 0) {
        fprintf(stderr, "mark: %s: %s\n", name, strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        fprintf(stderr, "mark: %s: %s\n", name, strerror(EISDIR));
        if (path) close(fd);
        return false;
    }
    struct buf b = {0};
    for (;;) {
        buf_reserve(&b, 65536);
        ssize_t got = read(fd, b.data + b.len, b.cap - b.len - 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "mark: %s: %s\n", name, strerror(errno));
            buf_free(&b);
            if (path) close(fd);
            return false;
        }
        if (got == 0) break;
        b.len += (size_t)got;
        if (b.len > MAX_INPUT_BYTES) {
            fprintf(stderr, "mark: %s: larger than %zu MiB\n", name, MAX_INPUT_BYTES >> 20);
            buf_free(&b);
            if (path) close(fd);
            return false;
        }
    }
    if (path) close(fd);
    *data = b.data ? b.data : xstrdup("");
    *len = b.len;
    return true;
}

/* file:// URLs for the file and its directory, for relative links. */
static void set_file_urls(struct options *opt)
{
    char *real = realpath(opt->path, NULL);
    if (!real) return;
    char host[256] = "";
    if (gethostname(host, sizeof host - 1) != 0) host[0] = '\0';
    struct buf url = {0};
    buf_puts(&url, "file://");
    buf_puts(&url, host);
    url_encode(&url, real);
    opt->file_url = url.data;
    char *slash = strrchr(real, '/');
    slash[1] = '\0';
    struct buf dir = {0};
    buf_puts(&dir, "file://");
    buf_puts(&dir, host);
    url_encode(&dir, real);
    opt->dir_url = dir.data;
    free(real);
}

static void set_locale(void)
{
    setlocale(LC_CTYPE, "");
    if (MB_CUR_MAX == 1) setlocale(LC_CTYPE, "C.UTF-8");
}

int main(int argc, char **argv)
{
    set_locale();
    struct options opt = {.styled = true, .color = true};
    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0]) opt.color = false;

    enum { OPT_WIDTH = 256, OPT_PLAIN, OPT_NO_PAGER, OPT_HTML, OPT_COMMONMARK };
    static const struct option long_options[] = {
        {"width", required_argument, NULL, OPT_WIDTH},
        {"plain", no_argument, NULL, OPT_PLAIN},
        {"no-pager", no_argument, NULL, OPT_NO_PAGER},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
#ifdef MARK_TEST
        {"html", no_argument, NULL, OPT_HTML},
        {"commonmark", no_argument, NULL, OPT_COMMONMARK},
#endif
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "hv", long_options, NULL)) != -1) {
        switch (c) {
        case OPT_WIDTH: {
            char *end;
            errno = 0;
            long width = strtol(optarg, &end, 10);
            if (errno || *end || end == optarg || width < 1 || width > MAX_WIDTH) {
                fprintf(stderr, "mark: --width takes a number from 1 to %d\n", MAX_WIDTH);
                return EXIT_USAGE;
            }
            opt.width = (int)width;
            break;
        }
        case OPT_PLAIN: opt.styled = false; break;
        case OPT_NO_PAGER: opt.no_pager = true; break;
#ifdef MARK_TEST
        case OPT_HTML: opt.html = true; break;
        case OPT_COMMONMARK: opt.commonmark = true; break;
#endif
        case 'h': usage(stdout); return EXIT_OK;
        case 'v': puts("mark " MARK_VERSION); return EXIT_OK;
        default: usage(stderr); return EXIT_USAGE;
        }
    }
    if (argc - optind > 1) {
        usage(stderr);
        return EXIT_USAGE;
    }
    if (optind < argc && strcmp(argv[optind], "-") != 0) {
        opt.path = argv[optind];
    } else if (isatty(STDIN_FILENO)) {
        if (optind < argc) {
            fputs("mark: standard input is a terminal\n", stderr);
        } else {
            usage(stderr);
        }
        return EXIT_USAGE;
    }

    bool fixed_width = opt.width != 0;
    if (opt.width == 0) {
        struct winsize ws;
        opt.width = DEFAULT_WIDTH;
        if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 &&
            ws.ws_col < DEFAULT_WIDTH) {
            opt.width = ws.ws_col;
        }
    }

    char *raw;
    size_t raw_len;
    if (!read_input(opt.path, &raw, &raw_len)) return EXIT_FAILED;
    size_t len;
    char *text = normalize_input(raw, raw_len, &len);
    free(raw);
    if (opt.path) set_file_urls(&opt);

    bool extensions = true;
#ifdef MARK_TEST
    extensions = !opt.commonmark;
#endif
    struct document *doc = parse_document(text, len, extensions);
    bool ok;
#ifdef MARK_TEST
    if (opt.html) {
        struct buf html = {0};
        html_blocks(&html, doc->root);
        ok = write_all(STDOUT_FILENO, html.data ? html.data : "", html.len);
        buf_free(&html);
    } else
#endif
    {
        ok = true;
        bool paged = !opt.no_pager && opt.styled && isatty(STDOUT_FILENO) && run_pager(doc, &opt, fixed_width);
        if (!paged) {
            struct page page = {0};
            layout_document(&page, doc, &opt);
            ok = print_page(&page, &opt);
            free_page(&page);
        }
    }
    free_document(doc);
    free(text);
    free(opt.file_url);
    free(opt.dir_url);
    if (!ok && errno != EPIPE) {
        fprintf(stderr, "mark: %s\n", strerror(errno));
        return EXIT_FAILED;
    }
    return EXIT_OK;
}
