#ifndef DEAL_TERMINAL_H
#define DEAL_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned char red;
    unsigned char green;
    unsigned char blue;
} TerminalColor;

typedef struct {
    uint32_t character;
    TerminalColor foreground;
    TerminalColor background;
    unsigned int width : 2; /* 0 for a wide character's trailing cell, 1 or 2 otherwise. */
    bool underline : 1;
    bool written : 1;
    bool narrowed : 1; /* A wide glyph retained while the grid is one column wide. */
    uint8_t foreground_source, background_source; /* Zero for fixed RGB. */
    uint16_t hyperlink; /* Zero or an ID owned by the terminal. */
    uint32_t combining[2];
} TerminalCell;

/* Logical row zero starts at first_row in the circular, row-major storage. */
typedef struct {
    size_t columns;
    size_t rows;
    TerminalCell *cells;
    bool *wrapped; /* The next logical row continues this row after a soft wrap. */
    size_t first_row;
} TerminalScreen;

/* Resize preserves the cells in the overlapping region. */
bool terminal_screen_resize(TerminalScreen *screen, size_t columns, size_t rows);
void terminal_screen_clear(TerminalScreen *screen, TerminalCell fill);
void terminal_screen_set(TerminalScreen *screen, size_t column, size_t row,
                         TerminalCell cell);
void terminal_screen_destroy(TerminalScreen *screen);

#endif
