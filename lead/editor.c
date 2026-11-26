#define _POSIX_C_SOURCE 200809L

#include "editor.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <wchar.h>

/* 1. Platform detection and terminal abstraction */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>

static HANDLE terminal_input;
static HANDLE terminal_output;
static DWORD original_input_mode;
static DWORD original_output_mode;
static UINT original_input_cp;
static UINT original_output_cp;
static WCHAR pending_high_surrogate;
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios original_termios;
static volatile sig_atomic_t interrupted;
static unsigned char utf8_pending[4];
static unsigned char utf8_length;
static unsigned char utf8_expected;
static int escape_state;
static int pending_byte = -1;

static void on_signal(int signum)
{
    (void)signum;
    interrupted = 1;
}
#endif

static int terminal_active;

static void terminal_size(int *columns, int *rows)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(terminal_output, &info)) {
        *columns = info.srWindow.Right - info.srWindow.Left + 1;
        *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    } else {
        *columns = 80;
        *rows = 24;
    }
#else
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
        size.ws_col > 0 && size.ws_row > 0) {
        *columns = size.ws_col;
        *rows = size.ws_row;
    } else {
        *columns = 80;
        *rows = 24;
    }
#endif
}

/* 2. Terminal initialization and cleanup */
static void terminal_cleanup(void)
{
    if (!terminal_active) return;
    fputs("\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
#ifdef _WIN32
    SetConsoleMode(terminal_input, original_input_mode);
    SetConsoleMode(terminal_output, original_output_mode);
    SetConsoleCP(original_input_cp);
    SetConsoleOutputCP(original_output_cp);
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
#endif
    terminal_active = 0;
}

static int terminal_init(void)
{
#ifdef _WIN32
    DWORD input_mode;
    DWORD output_mode;
    terminal_input = GetStdHandle(STD_INPUT_HANDLE);
    terminal_output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (terminal_input == INVALID_HANDLE_VALUE ||
        terminal_output == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(terminal_input, &original_input_mode) ||
        !GetConsoleMode(terminal_output, &original_output_mode)) return 0;
    original_input_cp = GetConsoleCP();
    original_output_cp = GetConsoleOutputCP();
    input_mode = original_input_mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                                ENABLE_PROCESSED_INPUT);
    input_mode |= ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
    input_mode &= ~(DWORD)ENABLE_QUICK_EDIT_MODE;
    output_mode = original_output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(terminal_output, output_mode)) return 0;
    if (!SetConsoleMode(terminal_input, input_mode)) {
        SetConsoleMode(terminal_output, original_output_mode);
        return 0;
    }
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#else
    struct termios raw;
    struct sigaction action;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        tcgetattr(STDIN_FILENO, &original_termios) != 0) return 0;
    raw = original_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag = (raw.c_cflag & (tcflag_t)~CSIZE) | CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return 0;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGINT, &action, NULL);
#endif
    terminal_active = 1;
    if (atexit(terminal_cleanup) != 0) {
        terminal_cleanup();
        return 0;
    }
    (void)setvbuf(stdout, NULL, _IOFBF, 16384);
    fputs("\x1b[?1049h\x1b[?25h\x1b[H\x1b[2J", stdout);
    fflush(stdout);
    return 1;
}

/* 3. Keyboard input handling */
static EditorInput no_input(void)
{
    EditorInput input = { EDITOR_KEY_NONE, { 0, 0, 0, 0 }, 0 };
    return input;
}

static EditorInput simple_input(EditorKey key)
{
    EditorInput input = no_input();
    input.key = key;
    return input;
}

#ifdef _WIN32
static EditorInput unicode_input(uint32_t codepoint)
{
    EditorInput input = no_input();
    input.key = EDITOR_KEY_TEXT;
    if (codepoint <= 0x7f) {
        input.text[0] = (char)codepoint;
        input.length = 1;
    } else if (codepoint <= 0x7ff) {
        input.text[0] = (char)(0xc0 | (codepoint >> 6));
        input.text[1] = (char)(0x80 | (codepoint & 0x3f));
        input.length = 2;
    } else if (codepoint <= 0xffff) {
        input.text[0] = (char)(0xe0 | (codepoint >> 12));
        input.text[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        input.text[2] = (char)(0x80 | (codepoint & 0x3f));
        input.length = 3;
    } else {
        input.text[0] = (char)(0xf0 | (codepoint >> 18));
        input.text[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        input.text[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        input.text[3] = (char)(0x80 | (codepoint & 0x3f));
        input.length = 4;
    }
    return input;
}

static EditorInput terminal_read_input(void)
{
    INPUT_RECORD record;
    DWORD count;
    WCHAR unit;
    uint32_t codepoint;
    if (WaitForSingleObject(terminal_input, 100) == WAIT_TIMEOUT)
        return no_input();
    if (!ReadConsoleInputW(terminal_input, &record, 1, &count) || count != 1)
        return simple_input(EDITOR_KEY_ERROR);
    if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
        return no_input();
    if (record.Event.KeyEvent.wVirtualKeyCode == VK_BACK)
        return simple_input(EDITOR_KEY_BACKSPACE);
    if (record.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE)
        return simple_input(EDITOR_KEY_ESCAPE);
    if (record.Event.KeyEvent.wVirtualKeyCode == VK_RETURN)
        return simple_input(EDITOR_KEY_ENTER);
    unit = record.Event.KeyEvent.uChar.UnicodeChar;
    if (unit == 3 || (record.Event.KeyEvent.wVirtualKeyCode == 'C' &&
        (record.Event.KeyEvent.dwControlKeyState &
         (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))))
        return simple_input(EDITOR_KEY_EXIT);
    if (unit == 18 || (record.Event.KeyEvent.wVirtualKeyCode == 'R' &&
        (record.Event.KeyEvent.dwControlKeyState &
         (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) &&
        !(record.Event.KeyEvent.dwControlKeyState &
          (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED))))
        return simple_input(EDITOR_KEY_REDO);
    if (unit >= 0xd800 && unit <= 0xdbff) {
        pending_high_surrogate = unit;
        return no_input();
    }
    if (unit >= 0xdc00 && unit <= 0xdfff && pending_high_surrogate) {
        codepoint = 0x10000u +
            (((uint32_t)pending_high_surrogate - 0xd800u) << 10) +
            ((uint32_t)unit - 0xdc00u);
        pending_high_surrogate = 0;
        return unicode_input(codepoint);
    }
    pending_high_surrogate = 0;
    if (unit >= 0x20 && (unit < 0xd800 || unit > 0xdfff) && unit != 0x7f)
        return unicode_input(unit);
    return no_input();
}
#else
static EditorInput terminal_read_input(void)
{
    unsigned char byte;
    ssize_t count;
    EditorInput input;
    if (interrupted) {
        interrupted = 0;
        return simple_input(EDITOR_KEY_EXIT);
    }
    if (pending_byte >= 0) {
        byte = (unsigned char)pending_byte;
        pending_byte = -1;
        count = 1;
    } else {
        count = read(STDIN_FILENO, &byte, 1);
    }
    if (count < 0) {
        if (errno == EINTR) return no_input();
        return simple_input(EDITOR_KEY_ERROR);
    }
    if (count == 0) {
        if (escape_state == 1) {
            escape_state = 0;
            return simple_input(EDITOR_KEY_ESCAPE);
        }
        return no_input();
    }
    if (escape_state) {
        if (escape_state == 1) {
            if (byte == '[') escape_state = 2;
            else if (byte == 'O') escape_state = 3;
            else if (byte == ']') escape_state = 4;
            else {
                escape_state = 0;
                pending_byte = byte;
                return simple_input(EDITOR_KEY_ESCAPE);
            }
        } else if (escape_state == 4) {
            if (byte == 7) escape_state = 0;
            else if (byte == 0x1b) escape_state = 5;
        } else if (escape_state == 5) {
            escape_state = byte == '\\' ? 0 : 4;
        } else if (byte >= 0x40 && byte <= 0x7e) {
            escape_state = 0;
        }
        return no_input();
    }
    if (byte == 0x1b) {
        escape_state = 1;
        utf8_length = 0;
        return no_input();
    }
    if (byte == 3) return simple_input(EDITOR_KEY_EXIT);
    if (byte == 18) return simple_input(EDITOR_KEY_REDO);
    if (byte == 8 || byte == 127) return simple_input(EDITOR_KEY_BACKSPACE);
    if (byte == '\r' || byte == '\n') return simple_input(EDITOR_KEY_ENTER);
    if (byte < 0x80) {
        utf8_length = 0;
        if (byte < 0x20 || byte == 0x7f) return no_input();
        input = no_input();
        input.key = EDITOR_KEY_TEXT;
        input.text[0] = (char)byte;
        input.length = 1;
        return input;
    }
    if ((byte & 0xc0) != 0x80) {
        utf8_length = 0;
        if (byte >= 0xc2 && byte <= 0xdf) utf8_expected = 2;
        else if (byte >= 0xe0 && byte <= 0xef) utf8_expected = 3;
        else if (byte >= 0xf0 && byte <= 0xf4) utf8_expected = 4;
        else return no_input();
        utf8_pending[utf8_length++] = byte;
        return no_input();
    }
    if (utf8_length == 0) return no_input();
    if (utf8_length == 1 &&
        ((utf8_pending[0] == 0xe0 && byte < 0xa0) ||
         (utf8_pending[0] == 0xed && byte >= 0xa0) ||
         (utf8_pending[0] == 0xf0 && byte < 0x90) ||
         (utf8_pending[0] == 0xf4 && byte >= 0x90))) {
        utf8_length = 0;
        return no_input();
    }
    utf8_pending[utf8_length++] = byte;
    if (utf8_length < utf8_expected) return no_input();
    input = no_input();
    input.key = EDITOR_KEY_TEXT;
    input.length = utf8_length;
    memcpy(input.text, utf8_pending, utf8_length);
    utf8_length = 0;
    return input;
}
#endif

/* 4. Dynamic text buffer */
int editor_buffer_replace(EditorBuffer *buffer, size_t position,
                          size_t remove_length, const char *text,
                          size_t insert_length)
{
    size_t needed;
    size_t capacity;
    char *grown;
    if (position > buffer->length ||
        remove_length > buffer->length - position ||
        insert_length > SIZE_MAX - (buffer->length - remove_length)) return 0;
    if (!remove_length && !insert_length) return 1;
    needed = buffer->length - remove_length + insert_length;
    if (needed > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : 64;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        grown = (char *)realloc(buffer->data, capacity);
        if (!grown) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memmove(buffer->data + position + insert_length,
            buffer->data + position + remove_length,
            buffer->length - position - remove_length);
    if (insert_length) memcpy(buffer->data + position, text, insert_length);
    buffer->length = needed;
    return 1;
}

int editor_buffer_append(EditorBuffer *buffer, const char *text, size_t length)
{
    return editor_buffer_replace(buffer, buffer->length, 0, text, length);
}

int editor_buffer_insert(EditorBuffer *buffer, size_t position,
                         const char *text, size_t length)
{
    return editor_buffer_replace(buffer, position, 0, text, length);
}

void editor_buffer_backspace(EditorBuffer *buffer)
{
    if (!buffer->length) return;
    --buffer->length;
    if (buffer->data[buffer->length] == '\n' && buffer->length &&
        buffer->data[buffer->length - 1] == '\r') {
        --buffer->length;
        return;
    }
    while (buffer->length &&
           ((unsigned char)buffer->data[buffer->length] & 0xc0) == 0x80)
        --buffer->length;
}

void editor_buffer_free(EditorBuffer *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

/* File management: the document owns its filename and byte-for-byte contents. */
static char *copy_string(const char *source)
{
    size_t length = strlen(source);
    char *copy;
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1);
    if (copy) memcpy(copy, source, length + 1);
    return copy;
}

static void document_free(EditorDocument *document)
{
    editor_buffer_free(&document->text);
    free(document->filename);
    document->filename = NULL;
    document->modified = 0;
    document->cursor = 0;
}

static void file_error(char *message, size_t size, const char *operation)
{
    snprintf(message, size, "%s: %s", operation, strerror(errno));
}

#ifdef _WIN32
static wchar_t *wide_path(const char *path)
{
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                                     NULL, 0);
    wchar_t *wide;
    if (!length) return NULL;
    wide = (wchar_t *)malloc((size_t)length * sizeof *wide);
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                             wide, length)) {
        free(wide);
        return NULL;
    }
    return wide;
}

static FILE *open_read_file(const char *path)
{
    wchar_t *wide = wide_path(path);
    FILE *file;
    if (!wide) { errno = EINVAL; return NULL; }
    file = _wfopen(wide, L"rb");
    free(wide);
    return file;
}
#else
static FILE *open_read_file(const char *path)
{
    return fopen(path, "rb");
}
#endif

static int document_load(EditorDocument *document, const char *path,
                         char *message, size_t message_size)
{
    EditorDocument next = { { NULL, 0, 0 }, NULL, 0, 0, 0 };
    unsigned char chunk[8192];
    FILE *file;
    size_t count;
    size_t index;
    next.filename = copy_string(path);
    if (!next.filename) {
        snprintf(message, message_size, "Out of memory");
        return 0;
    }
    file = open_read_file(path);
    if (!file) {
        if (errno == ENOENT) {
            document_free(document);
            *document = next;
            return 1;
        }
        file_error(message, message_size, "Open failed");
        document_free(&next);
        return 0;
    }
    while ((count = fread(chunk, 1, sizeof chunk, file)) != 0) {
        if (!editor_buffer_append(&next.text, (const char *)chunk, count)) {
            snprintf(message, message_size, "Out of memory");
            fclose(file);
            document_free(&next);
            return 0;
        }
    }
    if (ferror(file)) {
        file_error(message, message_size, "Read failed");
        fclose(file);
        document_free(&next);
        return 0;
    }
    if (fclose(file) != 0) {
        file_error(message, message_size, "Close failed");
        document_free(&next);
        return 0;
    }
    for (index = 0; index < next.text.length; ++index) {
        if (next.text.data[index] == '\n') {
            next.crlf = index > 0 && next.text.data[index - 1] == '\r';
            break;
        }
    }
    next.cursor = next.text.length;
    document_free(document);
    *document = next;
    return 1;
}

static int document_save(EditorDocument *document, const char *path,
                         char *message, size_t message_size)
{
    char *new_name = NULL;
    FILE *file = NULL;
    int success = 0;
#ifdef _WIN32
    wchar_t *wide = NULL;
    wchar_t *absolute = NULL;
    wchar_t temporary[MAX_PATH];
    wchar_t *separator;
    DWORD absolute_length;
    temporary[0] = L'\0';
#else
    char *temporary = NULL;
    int descriptor = -1;
    struct stat old_info;
#endif
    if (!document->filename || strcmp(document->filename, path) != 0) {
        new_name = copy_string(path);
        if (!new_name) {
            snprintf(message, message_size, "Out of memory");
            return 0;
        }
    }
#ifdef _WIN32
    wide = wide_path(path);
    if (!wide) {
        snprintf(message, message_size, "Invalid filename or out of memory");
        goto done;
    }
    absolute_length = GetFullPathNameW(wide, 0, NULL, NULL);
    if (!absolute_length) {
        snprintf(message, message_size, "Invalid save path");
        goto done;
    }
    absolute = (wchar_t *)malloc((size_t)absolute_length * sizeof *absolute);
    if (!absolute || !GetFullPathNameW(wide, absolute_length, absolute, NULL)) {
        snprintf(message, message_size, "Out of memory or invalid save path");
        goto done;
    }
    separator = wcsrchr(absolute, L'\\');
    if (!separator) {
        snprintf(message, message_size, "Invalid save path");
        goto done;
    }
    separator[1] = L'\0';
    if (!GetTempFileNameW(absolute, L"edt", 0, temporary)) {
        snprintf(message, message_size, "Could not create temporary file (%lu)",
                 (unsigned long)GetLastError());
        goto done;
    }
    file = _wfopen(temporary, L"wb");
    if (!file) { file_error(message, message_size, "Save failed"); goto done; }
#else
    if (strlen(path) > SIZE_MAX - 12) {
        snprintf(message, message_size, "Filename too long");
        goto done;
    }
    temporary = (char *)malloc(strlen(path) + 12);
    if (!temporary) { snprintf(message, message_size, "Out of memory"); goto done; }
    sprintf(temporary, "%s.tmp.XXXXXX", path);
    descriptor = mkstemp(temporary);
    if (descriptor < 0) { file_error(message, message_size, "Save failed"); goto done; }
    if (stat(path, &old_info) == 0 &&
        fchmod(descriptor, old_info.st_mode & 0777) != 0) {
        file_error(message, message_size, "Save failed");
        goto done;
    }
    file = fdopen(descriptor, "wb");
    if (!file) { file_error(message, message_size, "Save failed"); goto done; }
    descriptor = -1;
#endif
    if (document->text.length &&
        fwrite(document->text.data, 1, document->text.length, file) !=
        document->text.length) {
        file_error(message, message_size, "Write failed");
        goto done;
    }
    if (fflush(file) != 0) {
        file_error(message, message_size, "Flush failed");
        goto done;
    }
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
#else
    if (fsync(fileno(file)) != 0) {
#endif
        file_error(message, message_size, "Sync failed");
        goto done;
    }
    if (fclose(file) != 0) {
        file = NULL;
        file_error(message, message_size, "Close failed");
        goto done;
    }
    file = NULL;
#ifdef _WIN32
    if (!MoveFileExW(temporary, wide,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        snprintf(message, message_size, "Replace failed (%lu)",
                 (unsigned long)GetLastError());
        goto done;
    }
#else
    if (rename(temporary, path) != 0) {
        file_error(message, message_size, "Replace failed");
        goto done;
    }
#endif
    success = 1;
    if (new_name) {
        free(document->filename);
        document->filename = new_name;
        new_name = NULL;
    }
    document->modified = 0;
    snprintf(message, message_size, "Saved");
done:
    if (file) fclose(file);
#ifdef _WIN32
    if (!success && temporary[0]) DeleteFileW(temporary);
    free(wide);
    free(absolute);
#else
    if (descriptor >= 0) close(descriptor);
    if (!success && temporary) unlink(temporary);
    free(temporary);
#endif
    free(new_name);
    return success;
}

/* 5. Basic terminal rendering */
static uint32_t decode_utf8(const char *text, size_t length, size_t *index)
{
    unsigned char first = (unsigned char)text[*index];
    size_t extra = 0;
    size_t offset;
    uint32_t codepoint;
    if (first < 0x80) { ++*index; return first; }
    if (first >= 0xc2 && first <= 0xdf) { extra = 1; codepoint = first & 31u; }
    else if (first >= 0xe0 && first <= 0xef) { extra = 2; codepoint = first & 15u; }
    else if (first >= 0xf0 && first <= 0xf4) { extra = 3; codepoint = first & 7u; }
    else { ++*index; return 0xfffdu; }
    if (extra >= length - *index) { ++*index; return 0xfffdu; }
    for (offset = 1; offset <= extra; ++offset) {
        unsigned char next = (unsigned char)text[*index + offset];
        if ((next & 0xc0u) != 0x80u ||
            (offset == 1 && ((first == 0xe0 && next < 0xa0) ||
                             (first == 0xed && next >= 0xa0) ||
                             (first == 0xf0 && next < 0x90) ||
                             (first == 0xf4 && next >= 0x90)))) {
            ++*index;
            return 0xfffdu;
        }
        codepoint = (codepoint << 6) | (next & 0x3fu);
    }
    *index += extra + 1;
    return codepoint;
}

static int character_width(uint32_t c)
{
    if (c == 0x200d || c == 0xfe0e || c == 0xfe0f ||
        (c >= 0x300 && c <= 0x36f) || (c >= 0x1ab0 && c <= 0x1aff) ||
        (c >= 0x1dc0 && c <= 0x1dff) || (c >= 0x20d0 && c <= 0x20ff) ||
        (c >= 0xfe20 && c <= 0xfe2f)) return 0;
    if (c >= 0x1100 &&
        (c <= 0x115f || c == 0x2329 || c == 0x232a ||
         (c >= 0x2e80 && c <= 0xa4cf) ||
         (c >= 0xac00 && c <= 0xd7a3) ||
         (c >= 0xf900 && c <= 0xfaff) ||
         (c >= 0xfe10 && c <= 0xfe19) ||
         (c >= 0xfe30 && c <= 0xfe6f) ||
         (c >= 0xff00 && c <= 0xff60) ||
         (c >= 0xffe0 && c <= 0xffe6) ||
         (c >= 0x1f300 && c <= 0x1faff) ||
         (c >= 0x20000 && c <= 0x3fffd))) return 2;
    return 1;
}

static int display_width(uint32_t c, int column)
{
    return c == '\t' ? 4 - column % 4 : character_width(c);
}

static uint32_t next_display_character(const EditorBuffer *buffer, size_t *index)
{
    uint32_t c = decode_utf8(buffer->data, buffer->length, index);
    if (c == '\r') {
        if (*index < buffer->length && buffer->data[*index] == '\n') ++*index;
        return '\n';
    }
    if ((c < 0x20 && c != '\n' && c != '\t') ||
        c == 0x7f || c == 0xfffd)
        return '?';
    return c;
}

/* Cursor movement uses document byte offsets; columns are terminal cells. */
static size_t previous_character(const EditorBuffer *buffer, size_t position)
{
    if (!position) return 0;
    --position;
    if (buffer->data[position] == '\n' && position &&
        buffer->data[position - 1] == '\r') return position - 1;
    while (position && ((unsigned char)buffer->data[position] & 0xc0u) == 0x80u)
        --position;
    return position;
}

static size_t next_character(const EditorBuffer *buffer, size_t position)
{
    if (position >= buffer->length) return buffer->length;
    if (buffer->data[position] == '\r' && position + 1 < buffer->length &&
        buffer->data[position + 1] == '\n') return position + 2;
    (void)decode_utf8(buffer->data, buffer->length, &position);
    return position;
}

static size_t logical_line_start(const EditorBuffer *buffer, size_t position)
{
    while (position) {
        char previous = buffer->data[position - 1];
        if (previous == '\n' || previous == '\r') break;
        --position;
    }
    return position;
}

static size_t logical_line_end(const EditorBuffer *buffer, size_t position)
{
    while (position < buffer->length && buffer->data[position] != '\n' &&
           buffer->data[position] != '\r') ++position;
    return position;
}

static int visual_column(const EditorBuffer *buffer, size_t start, size_t position)
{
    int column = 0;
    while (start < position) {
        uint32_t c = decode_utf8(buffer->data, buffer->length, &start);
        int width = display_width(c, column);
        if (column > INT_MAX - width) return INT_MAX;
        column += width;
    }
    return column;
}

static size_t position_at_column(const EditorBuffer *buffer, size_t start,
                                 size_t end, int wanted)
{
    int column = 0;
    while (start < end) {
        size_t current = start;
        size_t next = start;
        int width = display_width(decode_utf8(buffer->data, buffer->length,
                                               &next), column);
        if (width > wanted - column) return current;
        column += width;
        start = next;
    }
    return end;
}

static int word_class(const EditorBuffer *buffer, size_t position)
{
    unsigned char c;
    if (position >= buffer->length) return 0;
    c = (unsigned char)buffer->data[position];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
        c == '\v' || c == '\f') return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c >= 0x80) return 1;
    return 2;
}

static void move_cursor(EditorDocument *document, char movement,
                        int *preferred_column)
{
    const EditorBuffer *buffer = &document->text;
    size_t position = document->cursor;
    size_t start;
    size_t end;
    int category;
    if (movement == 'h') position = previous_character(buffer, position);
    else if (movement == 'l') position = next_character(buffer, position);
    else if (movement == 'w') {
        category = word_class(buffer, position);
        if (category != 0) {
            while (position < buffer->length &&
                   word_class(buffer, position) == category)
                position = next_character(buffer, position);
        }
        while (position < buffer->length && word_class(buffer, position) == 0)
            position = next_character(buffer, position);
    } else if (movement == 'b') {
        if (position) {
            position = previous_character(buffer, position);
            while (position && word_class(buffer, position) == 0)
                position = previous_character(buffer, position);
            category = word_class(buffer, position);
            while (position) {
                size_t before = previous_character(buffer, position);
                if (word_class(buffer, before) != category) break;
                position = before;
            }
        }
    } else if (movement == 'j' || movement == 'k') {
        start = logical_line_start(buffer, position);
        end = logical_line_end(buffer, position);
        if (*preferred_column < 0)
            *preferred_column = visual_column(buffer, start, position);
        if (movement == 'j' && end < buffer->length) {
            start = next_character(buffer, end);
            end = logical_line_end(buffer, start);
            position = position_at_column(buffer, start, end, *preferred_column);
        } else if (movement == 'k' && start) {
            end = previous_character(buffer, start);
            start = logical_line_start(buffer, end);
            position = position_at_column(buffer, start, end, *preferred_column);
        }
    }
    if (movement != 'j' && movement != 'k') *preferred_column = -1;
    document->cursor = position;
}

static int status_text(const char *text, size_t length, int columns, int used)
{
    size_t index = 0;
    while (index < length && used < columns) {
        size_t start = index;
        uint32_t c = decode_utf8(text, length, &index);
        int width = character_width(c);
        if (width > columns - used) break;
        if (c < 0x20 || c == 0x7f || c == 0xfffd) fputc('?', stdout);
        else fwrite(text + start, 1, index - start, stdout);
        used += width;
    }
    return used;
}

static int visible_row(size_t row, size_t top, int body_rows)
{
    return body_rows > 0 && row >= top && row - top < (size_t)body_rows;
}

static void render_row_start(size_t row, size_t top, int body_rows,
                             size_t logical_line, size_t current_line,
                             int gutter, int first_fragment)
{
    int index;
    int digits = 1;
    size_t number = logical_line;
    if (!visible_row(row, top, body_rows)) return;
    while (number >= 10) { ++digits; number /= 10; }
    fprintf(stdout, "\x1b[%zu;1H\x1b[2K", row - top + 1);
    if (logical_line == current_line) fputs("\x1b[48;5;236m", stdout);
    fputs("\x1b[38;5;245m", stdout);
    if (first_fragment && gutter >= digits + 2) {
        fprintf(stdout, "%*zu  ", gutter - 2, logical_line);
    } else {
        for (index = 0; index < gutter; ++index) fputc(' ', stdout);
    }
    fputs("\x1b[39m", stdout);
}

static void render_row_end(size_t row, size_t top, int body_rows)
{
    if (visible_row(row, top, body_rows)) fputs("\x1b[K\x1b[0m", stdout);
}

static int render_editor(const EditorDocument *document, EditorMode mode,
                         const EditorBuffer *command, int prompting,
                         const char *message, int columns, int rows,
                         size_t *top_row)
{
    const EditorBuffer *buffer = &document->text;
    size_t index = 0;
    size_t row = 0;
    size_t logical_line = 1;
    size_t total_lines = 1;
    size_t cursor_row = 0;
    size_t cursor_line = 1;
    size_t first_visible;
    int column = 0;
    int cursor_column = 0;
    int body_rows = rows - 1;
    int gutter = 3;
    int body_columns;
    int status_column = 0;
    int status_width = columns > 1 ? columns - 1 : 1;
    char position_text[96];
    int position_width;
    if (columns < 1 || rows < 1) return 1;
    while (index < buffer->length) {
        if (next_display_character(buffer, &index) == '\n') ++total_lines;
    }
    for (size_t lines = total_lines; lines >= 10; lines /= 10) ++gutter;
    if (gutter >= columns) gutter = columns - 1;
    body_columns = columns - gutter;
    index = 0;
    while (index < buffer->length) {
        size_t start = index;
        uint32_t c = next_display_character(buffer, &index);
        int width = display_width(c, column);
        if (start == document->cursor) {
            cursor_row = row;
            cursor_column = column;
            cursor_line = logical_line;
            if (c == '\n' && cursor_column == body_columns)
                cursor_column = body_columns - 1;
            else if (c != '\n' &&
                     (column == body_columns || column + width > body_columns)) {
                ++cursor_row;
                cursor_column = 0;
            }
        }
        if (c == '\n') {
            ++row;
            ++logical_line;
            column = 0;
        } else {
            if (column == body_columns || column + width > body_columns) {
                ++row;
                column = 0;
                width = display_width(c, column);
            }
            if (width > body_columns) width = body_columns;
            column += width;
        }
    }
    if (document->cursor == buffer->length) {
        cursor_row = row;
        cursor_column = column;
        cursor_line = logical_line;
        if (cursor_column == body_columns) { ++cursor_row; cursor_column = 0; }
    }
    if (body_rows > 0) {
        if (cursor_row < *top_row) *top_row = cursor_row;
        if (cursor_row - *top_row >= (size_t)body_rows)
            *top_row = cursor_row - (size_t)body_rows + 1;
    }
    first_visible = *top_row;
    fputs("\x1b[?25l", stdout);
    index = 0;
    row = 0;
    logical_line = 1;
    column = 0;
    render_row_start(row, first_visible, body_rows, logical_line,
                     cursor_line, gutter, 1);
    while (index < buffer->length) {
        size_t start = index;
        uint32_t c = next_display_character(buffer, &index);
        int width = display_width(c, column);
        if (c == '\n') {
            render_row_end(row, first_visible, body_rows);
            ++row;
            ++logical_line;
            column = 0;
            render_row_start(row, first_visible, body_rows, logical_line,
                             cursor_line, gutter, 1);
            continue;
        }
        if (column == body_columns || column + width > body_columns) {
            render_row_end(row, first_visible, body_rows);
            ++row;
            column = 0;
            width = display_width(c, column);
            render_row_start(row, first_visible, body_rows, logical_line,
                             cursor_line, gutter, 0);
        }
        if (width > body_columns) width = body_columns;
        if (visible_row(row, first_visible, body_rows)) {
            if (c == '\t') {
                for (int cell = 0; cell < width; ++cell) fputc(' ', stdout);
            } else if (c == '?' &&
                       (index - start != 1 || buffer->data[start] != '?')) {
                fputc('?', stdout);
            } else {
                fwrite(buffer->data + start, 1, index - start, stdout);
            }
        }
        column += width;
    }
    render_row_end(row, first_visible, body_rows);
    if (column == body_columns) {
        ++row;
        render_row_start(row, first_visible, body_rows, logical_line,
                         cursor_line, gutter, 0);
        render_row_end(row, first_visible, body_rows);
    }
    if (body_rows > 0) {
        size_t screen = row >= first_visible ? row - first_visible + 2 : 1;
        while (screen <= (size_t)body_rows) {
            fprintf(stdout, "\x1b[%zu;1H\x1b[2K", screen);
            ++screen;
        }
    }
    fprintf(stdout, "\x1b[%d;1H\x1b[2K", rows);
    if (prompting) {
        fputc(':', stdout);
        status_column = status_text(command->data, command->length,
                                    status_width, 1);
    } else {
        const char *mode_text = mode == EDITOR_MODE_INSERT ? "INSERT  " : "NORMAL  ";
        const char *name = document->filename ? document->filename : "[No Name]";
        int logical_column = visual_column(buffer,
            logical_line_start(buffer, document->cursor), document->cursor);
        size_t reserve;
        int name_limit;
        if (logical_column < INT_MAX) ++logical_column;
        snprintf(position_text, sizeof position_text, "Ln %zu, Col %d  UTF-8",
                 cursor_line, logical_column);
        position_width = (int)strlen(position_text);
        reserve = (document->modified ? 4u : 0u) +
                  (message[0] ? strlen(message) + 5u : 0u) +
                  (size_t)position_width + 2u;
        name_limit = reserve < (size_t)status_width ?
                     status_width - (int)reserve : 0;
        fputs("\x1b[7m", stdout);
        status_column = status_text(mode_text, strlen(mode_text),
                                    status_width, status_column);
        if (name_limit < status_column) name_limit = status_column;
        status_column = status_text(name, strlen(name), name_limit, status_column);
        if (document->modified)
            status_column = status_text(" [+]", 4, status_width, status_column);
        if (message[0]) {
            status_column = status_text("  |  ", 5, status_width, status_column);
            status_column = status_text(message, strlen(message),
                                        status_width, status_column);
        }
        while (status_column + position_width + 1 < status_width) {
            fputc(' ', stdout);
            ++status_column;
        }
        (void)status_text(position_text, strlen(position_text),
                          status_width, status_column);
        fputs("\x1b[0m", stdout);
    }
    if (prompting)
        fprintf(stdout, "\x1b[%d;%dH", rows,
                status_column < columns ? status_column + 1 : columns);
    else if (body_rows > 0)
        fprintf(stdout, "\x1b[%zu;%dH", cursor_row - first_visible + 1,
                gutter + cursor_column + 1);
    else
        fprintf(stdout, "\x1b[%d;1H", rows);
    fputs("\x1b[?25h", stdout);
    return fflush(stdout) == 0;
}

/* 6. Application event loop */
typedef struct {
    size_t position;
    size_t removed_length;
    size_t inserted_length;
    char *removed;
    char *inserted;
    size_t cursor_before;
    size_t cursor_after;
} EditorEdit;

typedef struct {
    EditorEdit *edits;
    size_t length;
    size_t capacity;
    size_t index;
    size_t saved_index;
    int saved_valid;
    int group_active;
} EditorHistory;

typedef struct {
    EditorDocument document;
    EditorHistory history;
    EditorBuffer command;
    EditorMode mode;
    int prompting;
    int quit;
    char pending_command;
    int preferred_column;
    size_t top_row;
    char message[256];
} EditorState;

static void history_update_modified(EditorState *editor)
{
    editor->document.modified = !editor->history.saved_valid ||
        editor->history.index != editor->history.saved_index;
}

static void history_discard_from(EditorHistory *history, size_t first)
{
    size_t index;
    for (index = first; index < history->length; ++index) {
        free(history->edits[index].removed);
        free(history->edits[index].inserted);
    }
    history->length = first;
}

static void history_clear(EditorHistory *history)
{
    history_discard_from(history, 0);
    free(history->edits);
    memset(history, 0, sizeof *history);
    history->saved_valid = 1;
}

static void history_saved(EditorState *editor)
{
    editor->history.saved_index = editor->history.index;
    editor->history.saved_valid = 1;
    editor->history.group_active = 0;
    history_update_modified(editor);
}

static int document_edit(EditorState *editor, size_t position,
                         size_t remove_length, const char *insert,
                         size_t insert_length, size_t cursor_after,
                         int group_typing)
{
    EditorHistory *history = &editor->history;
    EditorBuffer *buffer = &editor->document.text;
    EditorEdit edit = { 0 };
    EditorEdit *last;
    size_t capacity;
    EditorEdit *grown;
    if (position > buffer->length || remove_length > buffer->length - position ||
        insert_length > SIZE_MAX - (buffer->length - remove_length) ||
        cursor_after > buffer->length - remove_length + insert_length ||
        (!remove_length && !insert_length)) return 0;
    if (group_typing && history->group_active && history->index == history->length &&
        history->index > 0) {
        last = &history->edits[history->index - 1];
        if (!last->removed_length && !remove_length &&
            last->position <= SIZE_MAX - last->inserted_length &&
            last->position + last->inserted_length == position &&
            insert_length <= SIZE_MAX - last->inserted_length) {
            size_t combined = last->inserted_length + insert_length;
            char *text = (char *)realloc(last->inserted, combined);
            if (!text) return 0;
            last->inserted = text;
            memcpy(text + last->inserted_length, insert, insert_length);
            if (!editor_buffer_replace(buffer, position, 0, insert,
                                       insert_length)) return 0;
            last->inserted_length = combined;
            last->cursor_after = cursor_after;
            editor->document.cursor = cursor_after;
            history_update_modified(editor);
            return 1;
        }
    }
    edit.position = position;
    edit.removed_length = remove_length;
    edit.inserted_length = insert_length;
    edit.cursor_before = editor->document.cursor;
    edit.cursor_after = cursor_after;
    if (remove_length) {
        edit.removed = (char *)malloc(remove_length);
        if (!edit.removed) return 0;
        memcpy(edit.removed, buffer->data + position, remove_length);
    }
    if (insert_length) {
        edit.inserted = (char *)malloc(insert_length);
        if (!edit.inserted) { free(edit.removed); return 0; }
        memcpy(edit.inserted, insert, insert_length);
    }
    if (history->index == history->capacity) {
        capacity = history->capacity ? history->capacity : 32;
        while (capacity <= history->index) {
            if (capacity > SIZE_MAX / 2) { capacity = history->index + 1; break; }
            capacity *= 2;
        }
        if (capacity > SIZE_MAX / sizeof *grown) {
            free(edit.removed);
            free(edit.inserted);
            return 0;
        }
        grown = (EditorEdit *)realloc(history->edits,
                                     capacity * sizeof *grown);
        if (!grown) {
            free(edit.removed);
            free(edit.inserted);
            return 0;
        }
        history->edits = grown;
        history->capacity = capacity;
    }
    if (!editor_buffer_replace(buffer, position, remove_length,
                               insert, insert_length)) {
        free(edit.removed);
        free(edit.inserted);
        return 0;
    }
    if (history->saved_valid && history->saved_index > history->index)
        history->saved_valid = 0;
    history_discard_from(history, history->index);
    history->edits[history->index++] = edit;
    history->length = history->index;
    history->group_active = group_typing;
    editor->document.cursor = cursor_after;
    history_update_modified(editor);
    return 1;
}

static int history_undo(EditorState *editor)
{
    EditorEdit *edit;
    EditorHistory *history = &editor->history;
    history->group_active = 0;
    if (!history->index) return 0;
    edit = &history->edits[history->index - 1];
    if (!editor_buffer_replace(&editor->document.text, edit->position,
                               edit->inserted_length, edit->removed,
                               edit->removed_length)) return -1;
    editor->document.cursor = edit->cursor_before;
    --history->index;
    history_update_modified(editor);
    return 1;
}

static int history_redo(EditorState *editor)
{
    EditorEdit *edit;
    EditorHistory *history = &editor->history;
    history->group_active = 0;
    if (history->index == history->length) return 0;
    edit = &history->edits[history->index];
    if (!editor_buffer_replace(&editor->document.text, edit->position,
                               edit->removed_length, edit->inserted,
                               edit->inserted_length)) return -1;
    editor->document.cursor = edit->cursor_after;
    ++history->index;
    history_update_modified(editor);
    return 1;
}

static void run_command(EditorState *editor)
{
    char *line;
    char *argument;
    size_t length = editor->command.length;
    if (length == SIZE_MAX) {
        snprintf(editor->message, sizeof editor->message, "Command too long");
        return;
    }
    line = (char *)malloc(length + 1);
    if (!line) {
        snprintf(editor->message, sizeof editor->message, "Out of memory");
        return;
    }
    memcpy(line, editor->command.data, length);
    line[length] = '\0';
    while (length && (line[length - 1] == ' ' || line[length - 1] == '\t'))
        line[--length] = '\0';
    if (strcmp(line, "q!") == 0) {
        editor->quit = 1;
    } else if (strcmp(line, "q") == 0) {
        if (editor->document.modified)
            snprintf(editor->message, sizeof editor->message,
                     "Unsaved changes; use :q! to discard");
        else editor->quit = 1;
    } else if (strcmp(line, "w") == 0 || strcmp(line, "wq") == 0) {
        if (!editor->document.filename)
            snprintf(editor->message, sizeof editor->message,
                     "No filename; use :w filename");
        else if (document_save(&editor->document, editor->document.filename,
                               editor->message, sizeof editor->message)) {
            history_saved(editor);
            if (strcmp(line, "wq") == 0) editor->quit = 1;
        }
    } else if (strncmp(line, "w ", 2) == 0) {
        argument = line + 2;
        while (*argument == ' ') ++argument;
        if (!*argument)
            snprintf(editor->message, sizeof editor->message, "Filename required");
        else if (document_save(&editor->document, argument,
                               editor->message, sizeof editor->message))
            history_saved(editor);
    } else if (strncmp(line, "e ", 2) == 0) {
        argument = line + 2;
        while (*argument == ' ') ++argument;
        if (!*argument)
            snprintf(editor->message, sizeof editor->message, "Filename required");
        else if (editor->document.modified)
            snprintf(editor->message, sizeof editor->message,
                     "Unsaved changes; save or use :q! first");
        else if (document_load(&editor->document, argument,
                               editor->message, sizeof editor->message)) {
            history_clear(&editor->history);
            editor->top_row = 0;
            editor->preferred_column = -1;
            snprintf(editor->message, sizeof editor->message, "Opened");
        }
    } else {
        snprintf(editor->message, sizeof editor->message, "Unknown command");
    }
    free(line);
}

static void edit_failed(EditorState *editor)
{
    snprintf(editor->message, sizeof editor->message, "Out of memory");
}

static void normal_key(EditorState *editor, char key)
{
    EditorDocument *document = &editor->document;
    EditorBuffer *buffer = &document->text;
    size_t start;
    size_t end;
    size_t position;
    size_t length;
    const char *newline = document->crlf ? "\r\n" : "\n";
    size_t newline_length = document->crlf ? 2u : 1u;
    editor->history.group_active = 0;
    if (editor->pending_command) {
        char pending = editor->pending_command;
        editor->pending_command = 0;
        if (pending == 'g' && key == 'g') {
            document->cursor = 0;
            editor->preferred_column = -1;
        } else if (pending == 'd' && key == 'd') {
            start = logical_line_start(buffer, document->cursor);
            end = logical_line_end(buffer, document->cursor);
            if (end < buffer->length) {
                position = start;
                length = next_character(buffer, end) - start;
            } else if (start) {
                position = previous_character(buffer, start);
                length = buffer->length - position;
            } else {
                position = 0;
                length = buffer->length;
            }
            if (length && !document_edit(editor, position, length, NULL, 0,
                                         position, 0)) edit_failed(editor);
            editor->preferred_column = -1;
        }
        return;
    }
    if (key == 'g' || key == 'd') {
        editor->pending_command = key;
    } else if (strchr("hjklwb", key)) {
        move_cursor(document, key, &editor->preferred_column);
    } else if (key == '0') {
        document->cursor = logical_line_start(buffer, document->cursor);
        editor->preferred_column = -1;
    } else if (key == '$') {
        document->cursor = logical_line_end(buffer, document->cursor);
        editor->preferred_column = -1;
    } else if (key == 'G') {
        document->cursor = buffer->length;
        editor->preferred_column = -1;
    } else if (key == 'i' || key == 'a') {
        if (key == 'a' && document->cursor < buffer->length &&
            buffer->data[document->cursor] != '\r' &&
            buffer->data[document->cursor] != '\n')
            document->cursor = next_character(buffer, document->cursor);
        editor->mode = EDITOR_MODE_INSERT;
        editor->preferred_column = -1;
        editor->message[0] = '\0';
    } else if (key == 'x') {
        position = document->cursor;
        if (position < buffer->length && buffer->data[position] != '\r' &&
            buffer->data[position] != '\n') {
            length = next_character(buffer, position) - position;
            if (!document_edit(editor, position, length, NULL, 0, position, 0))
                edit_failed(editor);
        }
        editor->preferred_column = -1;
    } else if (key == 'o' || key == 'O') {
        position = key == 'o' ? logical_line_end(buffer, document->cursor) :
                                 logical_line_start(buffer, document->cursor);
        if (document_edit(editor, position, 0, newline, newline_length,
                          key == 'o' ? position + newline_length : position, 0)) {
            editor->mode = EDITOR_MODE_INSERT;
            editor->preferred_column = -1;
        } else edit_failed(editor);
    } else if (key == 'u') {
        int outcome = history_undo(editor);
        if (outcome < 0) edit_failed(editor);
        else if (!outcome)
            snprintf(editor->message, sizeof editor->message, "Nothing to undo");
        else editor->message[0] = '\0';
        editor->preferred_column = -1;
    } else if (key == ':') {
        editor->command.length = 0;
        editor->prompting = 1;
        editor->message[0] = '\0';
    }
}

static void handle_input(EditorState *editor, EditorInput input, int *dirty)
{
    if (input.key == EDITOR_KEY_NONE) return;
    if (input.key == EDITOR_KEY_EXIT) {
        editor->prompting = 0;
        editor->pending_command = 0;
        editor->history.group_active = 0;
        if (editor->document.modified)
            snprintf(editor->message, sizeof editor->message,
                     "Unsaved changes; use :q! to discard");
        else editor->quit = 1;
        *dirty = 1;
        return;
    }
    if (editor->prompting) {
        if (input.key == EDITOR_KEY_ESCAPE) {
            editor->prompting = 0;
        } else if (input.key == EDITOR_KEY_ENTER) {
            editor->prompting = 0;
            editor->message[0] = '\0';
            run_command(editor);
        } else if (input.key == EDITOR_KEY_BACKSPACE) {
            editor_buffer_backspace(&editor->command);
        } else if (input.key == EDITOR_KEY_TEXT &&
                   !editor_buffer_append(&editor->command, input.text, input.length)) {
            editor->prompting = 0;
            snprintf(editor->message, sizeof editor->message, "Out of memory");
        }
        *dirty = 1;
        return;
    }
    if (editor->mode == EDITOR_MODE_NORMAL) {
        if (input.key == EDITOR_KEY_REDO) {
            int outcome = history_redo(editor);
            editor->pending_command = 0;
            editor->preferred_column = -1;
            if (outcome < 0) edit_failed(editor);
            else if (!outcome)
                snprintf(editor->message, sizeof editor->message, "Nothing to redo");
            else editor->message[0] = '\0';
            *dirty = 1;
        } else if (input.key == EDITOR_KEY_TEXT && input.length == 1) {
            normal_key(editor, input.text[0]);
            *dirty = 1;
        } else if (input.key == EDITOR_KEY_ESCAPE) {
            editor->pending_command = 0;
            *dirty = 1;
        }
        return;
    }
    if (input.key == EDITOR_KEY_ESCAPE) {
        editor->mode = EDITOR_MODE_NORMAL;
        editor->history.group_active = 0;
        *dirty = 1;
    } else if (input.key == EDITOR_KEY_TEXT || input.key == EDITOR_KEY_ENTER) {
        const char *text = input.text;
        size_t length = input.length;
        if (input.key == EDITOR_KEY_ENTER) {
            text = editor->document.crlf ? "\r\n" : "\n";
            length = editor->document.crlf ? 2 : 1;
        }
        if (input.key == EDITOR_KEY_ENTER) editor->history.group_active = 0;
        if (document_edit(editor, editor->document.cursor, 0, text, length,
                          editor->document.cursor + length,
                          input.key == EDITOR_KEY_TEXT)) {
            editor->preferred_column = -1;
        } else edit_failed(editor);
        *dirty = 1;
    } else if (input.key == EDITOR_KEY_BACKSPACE && editor->document.cursor) {
        size_t before = previous_character(&editor->document.text,
                                           editor->document.cursor);
        size_t length = editor->document.cursor - before;
        editor->history.group_active = 0;
        if (!document_edit(editor, before, length, NULL, 0, before, 0))
            edit_failed(editor);
        editor->preferred_column = -1;
        *dirty = 1;
    }
}

static int editor_run(EditorDocument *initial_document)
{
    EditorState editor = { 0 };
    int columns = 0;
    int rows = 0;
    int result = 0;
    int dirty = 1;
    editor.preferred_column = -1;
    editor.history.saved_valid = 1;
    editor.document = *initial_document;
    memset(initial_document, 0, sizeof *initial_document);
    while (!editor.quit) {
        int new_columns;
        int new_rows;
        EditorInput input;
        terminal_size(&new_columns, &new_rows);
        if (new_columns != columns || new_rows != rows) {
            columns = new_columns;
            rows = new_rows;
            dirty = 1;
        }
        if (dirty) {
            if (!render_editor(&editor.document, editor.mode, &editor.command,
                               editor.prompting, editor.message, columns, rows,
                               &editor.top_row)) {
                result = 1;
                break;
            }
            dirty = 0;
        }
        input = terminal_read_input();
        if (input.key == EDITOR_KEY_ERROR) {
            result = 1;
            break;
        }
        handle_input(&editor, input, &dirty);
    }
    editor_buffer_free(&editor.command);
    history_clear(&editor.history);
    document_free(&editor.document);
    return result;
}

/* 7. Main entry point */
int main(int argc, char **argv)
{
    EditorDocument document = { { NULL, 0, 0 }, NULL, 0, 0, 0 };
    char message[256];
    int result;
    if (argc > 2) {
        fputs("Usage: editor [filename]\n", stderr);
        return 1;
    }
    if (argc == 2 && !document_load(&document, argv[1], message, sizeof message)) {
        fprintf(stderr, "%s\n", message);
        return 1;
    }
    if (!terminal_init()) {
        fputs("Unable to initialize an interactive terminal.\n", stderr);
        document_free(&document);
        return 1;
    }
    result = editor_run(&document);
    terminal_cleanup();
    if (result != 0) fputs("Terminal input or output failed.\n", stderr);
    return result ? 1 : 0;
}
