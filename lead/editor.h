#ifndef EDITOR_H
#define EDITOR_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} EditorBuffer;

typedef enum {
    EDITOR_KEY_NONE,
    EDITOR_KEY_TEXT,
    EDITOR_KEY_ENTER,
    EDITOR_KEY_BACKSPACE,
    EDITOR_KEY_ESCAPE,
    EDITOR_KEY_REDO,
    EDITOR_KEY_EXIT,
    EDITOR_KEY_ERROR
} EditorKey;

typedef struct {
    EditorKey key;
    char text[4];
    unsigned char length;
} EditorInput;

typedef enum {
    EDITOR_MODE_NORMAL,
    EDITOR_MODE_INSERT
} EditorMode;

typedef struct {
    EditorBuffer text;
    char *filename;
    int modified;
    int crlf;
    size_t cursor;
} EditorDocument;

int editor_buffer_append(EditorBuffer *buffer, const char *text, size_t length);
int editor_buffer_insert(EditorBuffer *buffer, size_t position,
                         const char *text, size_t length);
int editor_buffer_replace(EditorBuffer *buffer, size_t position,
                          size_t remove_length, const char *text,
                          size_t insert_length);
void editor_buffer_backspace(EditorBuffer *buffer);
void editor_buffer_free(EditorBuffer *buffer);

#endif
