#ifndef MARK_H
#define MARK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MARK_VERSION "0.4.0"

/* Exit statuses. */
enum {
    EXIT_OK = 0,
    EXIT_FAILED = 1, /* The file couldn't be read. */
    EXIT_USAGE = 2,
};

#define MAX_INPUT_BYTES ((size_t)64 << 20)
#define DEFAULT_WIDTH 80
#define MAX_WIDTH 1000
#define TAB_STOP 4
#define CODE_INDENT 4
/* Container blocks (quotes, lists and items) nested deeper than this are
   read as text, so a crafted file can't build an unbounded tree. */
#define MAX_NESTING 32
#define MAX_LINK_LABEL 999
#define MAX_LINK_PARENS 32
#define MAX_BACKTICKS 1000
#define MAX_TABLE_COLUMNS 256

/* A growable byte string. */
struct buf {
    char *data;
    size_t len, cap;
};

/* Memory for one document, released at once. */
struct arena_block;
struct arena {
    struct arena_block *head;
};

/* Block structure (CommonMark 0.31, section 4 and 5). */
enum block_type {
    B_DOCUMENT,
    B_QUOTE,
    B_LIST,
    B_ITEM,
    B_PARAGRAPH,
    B_HEADING,
    B_BREAK,
    B_CODE,
    B_HTML,
    B_TABLE, /* GitHub extension. */
    B_ROW,
};

struct list_data {
    bool ordered;
    char bullet;    /* - + or * */
    char delimiter; /* . or ) */
    int start;
    int marker_offset; /* Columns before the marker. */
    int padding;       /* Marker width plus the spaces after it. */
    bool tight;
};

struct inline_node;

struct block {
    enum block_type type;
    struct block *parent, *first, *last, *prev, *next;
    bool open;
    bool last_line_blank;
    int start_line;
    struct buf content;           /* Leaf text, one '\n' per line. */
    struct inline_node *inlines;  /* Paragraphs and headings, after parsing. */

    int level;           /* Heading. */
    bool setext;
    bool fenced;         /* Code. */
    char fence_char;
    int fence_length;
    int fence_offset;
    char *info;
    int html_type;       /* HTML block, 1 to 7. */
    struct list_data list; /* List and item. */
    int task;            /* Item: 0 not a task, 1 open, 2 done. */
    int columns;         /* Table. */
    char *align;         /* Table: 'l', 'c', 'r' or 0 per column. */
    bool header;         /* Row. */
    struct inline_node **cells; /* Row: one per table column, after parsing. */
};

/* Inline structure (section 6). */
enum inline_type {
    I_TEXT,
    I_SOFTBREAK,
    I_LINEBREAK,
    I_CODE,
    I_HTML,
    I_EMPH,
    I_STRONG,
    I_LINK,
    I_IMAGE,
    I_STRIKE, /* GitHub extension. */
};

struct inline_node {
    enum inline_type type;
    struct inline_node *parent, *first, *last, *prev, *next;
    const char *text; /* Text, code and HTML: bytes, not terminated. */
    size_t len;
    char *url;        /* Link and image, unescaped. */
    char *title;
};

struct ref {
    char *label; /* Normalized. NULL for an empty slot. */
    char *url;
    char *title;
};

struct refmap {
    struct ref *slots;
    size_t count, cap;
};

struct document {
    bool extensions; /* GitHub's tables, strikethrough, task lists, autolinks. */
    struct arena arena;
    struct block *root;
    struct refmap refs;
};

/* Rendering. A style is interned in the page and glyphs refer to it. */
enum {
    ATTR_BOLD = 1,
    ATTR_DIM = 2,
    ATTR_ITALIC = 4,
    ATTR_STRIKE = 8,
};

enum {
    UNDERLINE_NONE = 0,
    UNDERLINE_SINGLE = 1,
    UNDERLINE_DOUBLE = 2,
    UNDERLINE_DASHED = 5,
};

#define COLOR_NONE (-1)

struct style {
    uint8_t attr;
    uint8_t underline;
    int16_t fg, bg, underline_color; /* 0–255, or COLOR_NONE. */
};

/* One character cell or a wide pair, with its combining marks. A glyph
   with no bytes and link GLYPH_BREAK is a hard line break. */
#define GLYPH_BREAK (-2)
struct glyph {
    uint32_t offset; /* Into page.text. */
    uint8_t length;
    uint8_t width;
    uint16_t style;
    int32_t link;    /* Into page.links, or -1. */
};

struct line {
    size_t first, count; /* Into page.glyphs. */
    int width;
    int pad_to;          /* Fill the line to this width with pad_style. */
    uint16_t pad_style;
    int32_t block;       /* The block it came from, numbered in document order. */
    int32_t heading;     /* Into page.headings: the section it's in, or -1. */
};

struct heading {
    char *text;   /* As rendered, without the #### marks. */
    int level;
    size_t line;  /* Its first line. */
};

/* The document laid out for one width. */
struct page {
    struct buf text;
    struct glyph *glyphs;
    size_t glyph_count, glyph_cap;
    struct line *lines;
    size_t line_count, line_cap;
    struct style *styles;
    size_t style_count, style_cap;
    char **links;        /* Resolved for OSC 8; NULL when there's nothing to resolve against. */
    size_t link_count, link_cap;
    char **targets;      /* The pager's copy of each link as written. */
    size_t target_cap;
    struct heading *headings;
    size_t heading_count, heading_cap;
};

struct options {
    int width;           /* 0: pick from the terminal. */
    bool styled;         /* Escape sequences at all (off with --plain). */
    bool color;          /* Colors (off with NO_COLOR). */
    const char *path;    /* NULL for standard input. */
    char *file_url;      /* file://host/dir/name.md, or NULL for stdin. */
    char *dir_url;       /* file://host/dir/ */
    bool pager;          /* Laid out for the pager: wide tables aren't cut. */
    bool no_pager;       /* --no-pager */
#ifdef MARK_TEST
    bool html;
    bool commonmark; /* No extensions, for the CommonMark examples. */
#endif
};

#endif
