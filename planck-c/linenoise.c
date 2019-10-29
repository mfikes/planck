/* linenoise.c -- VERSION 1.0
 *
 * Guerrilla line editing library against the idea that a line editing lib
 * needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 *
 *   http://github.com/antirez/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ------------------------------------------------------------------------
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars
 *
 * CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars
 *
 * The following is used to get the terminal width if getting
 * the width with the TIOCGWINSZ ioctl fails
 *
 * DSR (Device Status Report)
 *    Sequence: ESC [ 6 n
 *    Effect: reports the current cusor position as ESC [ n ; m R
 *            where n is the row and m is the column
 *
 * When multi line mode is enabled, we also use an additional escape
 * sequence. However multi line editing is disabled by default.
 *
 * CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * When linenoiseClearScreen() is called, two additional escape sequences
 * are used in order to clear the screen and position the cursor at home
 * position.
 *
 * CUP (Cursor position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED (Erase display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 *
 */

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include "linenoise.h"
#include "clock.h"

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096
static char *unsupported_term[] = {"dumb", "cons25", "emacs", NULL};
static linenoiseCompletionCallback *completionCallback = NULL;
static linenoiseHighlightCallback *highlightCallback = NULL;
static linenoiseHighlightCancelCallback *highlightCancelCallback = NULL;

static struct termios orig_termios; /* In order to restore at exit.*/
static int rawmode = 0; /* For atexit() function to check if restore is needed*/
static int mlmode = 0;  /* Multi line mode. Default is single line. */
static int atexit_registered = 0; /* Register atexit just 1 time. */
static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
static char **history = NULL;

uint64_t lastCharRead;
static int pasting = 0;
static const char *currentPromptAnsiCode;

static struct linenoiseState *activeState;

/* The linenoiseState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct linenoiseState {
    int ifd;            /* Terminal stdin file descriptor. */
    int ofd;            /* Terminal stdout file descriptor. */
    char *buf;          /* Edited line buffer. */
    size_t buflen;      /* Edited line buffer size. */
    const char *prompt; /* Prompt to display. */
    size_t plen;        /* Prompt length. */
    size_t pos;         /* Current cursor position. */
    size_t oldpos;      /* Previous refresh cursor position. */
    size_t len;         /* Current edited line length. */
    size_t cols;        /* Number of columns in terminal. */
    size_t maxrows;     /* Maximum num of rows used so far (multiline mode) */
    int history_index;  /* The history index we are currently editing. */
};

#define KEYMAP_SIZE (sizeof keymap / sizeof keymap[0])

static char keymap[] = {
        1, // KM_GO_TO_START_OF_LINE
        2, // KM_MOVE_LEFT
        3, // KM_CANCEL
        4, // KM_DELETE_RIGHT
        5, // KM_GO_TO_END_OF_LINE
        6, // KM_MOVE_RIGHT
        8, // KM_DELETE
        9, // KM_TAB
        11, // KM_DELETE_TO_END_OF_LINE
        12, // KM_CLEAR_SCREEN
        13, // KM_ENTER
        14, // KM_HISTORY_NEXT
        16, // KM_HISTORY_PREVIOUS
        20, // KM_SWAP_CHARS
        21, // KM_CLEAR_LINE
        23, // KM_DELETE_PREVIOUS_WORD
        27, // KM_ESC
        127, // KM_BACKSPACE
        18, // KM_REVERSE_I_SEARCH
        7,  // KM_CANCEL_SEARCH
        10  // KM_FINISH_SEARCH

};

void linenoiseSetKeymapEntry(int action, char key) {
    // First clear any associated action
    size_t i;
    for (i = 0; i < KEYMAP_SIZE; i++) {
        if (keymap[i] == key) {
            keymap[i] = 0;
            break;
        }
    }
    // Associate the action
    keymap[action] = key;
}

static void linenoiseAtExit(void);

int linenoiseHistoryAdd(const char *line);

static void refreshLine(struct linenoiseState *l);

/* Debugging macro. */
#if 0
FILE *lndebug_fp = NULL;
#define lndebug(...) \
    do { \
        if (lndebug_fp == NULL) { \
            lndebug_fp = fopen("/tmp/lndebug.txt","a"); \
            fprintf(lndebug_fp, \
            "[%d %d %d] p: %d, rows: %d, rpos: %d, max: %d, oldmax: %d\n", \
            (int)l->len,(int)l->pos,(int)l->oldpos,plen,rows,rpos, \
            (int)l->maxrows,old_rows); \
        } \
        fprintf(lndebug_fp, ", " __VA_ARGS__); \
        fflush(lndebug_fp); \
    } while (0)
#else
#define lndebug(fmt, ...)
#endif

/* ======================= Low level terminal handling ====================== */

/* Set if to use or not the multi line mode. */
void linenoiseSetMultiLine(int ml) {
    mlmode = ml;
}

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static int isUnsupportedTerm(void) {
    char *term = getenv("TERM");
    int j;

    if (term == NULL) return 0;
    for (j = 0; unsupported_term[j]; j++)
        if (!strcasecmp(term, unsupported_term[j])) return 1;
    return 0;
}

/* Raw mode: 1960 magic shit. */
static int enableRawMode(int fd) {
    struct termios raw;

    if (!isatty(STDIN_FILENO)) goto fatal;
    if (!atexit_registered) {
        atexit(linenoiseAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd, &orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd, TCSADRAIN, &raw) < 0) goto fatal;
    rawmode = 1;
    return 0;

    fatal:
    errno = ENOTTY;
    return -1;
}

static void disableRawMode(int fd) {
    /* Don't even check the return value as it's too late. */
    if (rawmode && tcsetattr(fd, TCSADRAIN, &orig_termios) != -1)
        rawmode = 0;
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor. */
static int getCursorPosition(int ifd, int ofd) {
    char buf[32];
    int cols, rows;
    unsigned int i = 0;

    /* Report cursor location */
    if (write(ofd, "\x1b[6n", 4) != 4) return -1;

    /* Read the response: ESC [ rows ; cols R */
    while (i < sizeof(buf) - 1) {
        if (read(ifd, buf + i, 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    /* Parse it. */
    if (buf[0] != keymap[KM_ESC] || buf[1] != '[') return -1;
    if (sscanf(buf + 2, "%d;%d", &rows, &cols) != 2) return -1;
    return cols;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int getColumns(int ifd, int ofd) {
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /* ioctl() failed. Try to query the terminal itself. */
        int start, cols;

        /* Get the initial position so we can restore it later. */
        start = getCursorPosition(ifd, ofd);
        if (start == -1) goto failed;

        /* Go to right margin and get position. */
        if (write(ofd, "\x1b[999C", 6) != 6) goto failed;
        cols = getCursorPosition(ifd, ofd);
        if (cols == -1) goto failed;

        /* Restore position. */
        if (cols > start) {
            char seq[32];
            snprintf(seq, 32, "\x1b[%dD", cols - start);
            if (write(ofd, seq, strlen(seq)) == -1) {
                /* Can't recover... */
            }
        }
        return cols;
    } else {
        return ws.ws_col;
    }

    failed:
    return 80;
}

/* Clear the screen. Used to handle ctrl+l */
void linenoiseClearScreen(void) {
    if (write(STDOUT_FILENO, "\x1b[H\x1b[2J", 7) <= 0) {
        /* nothing to do, just to avoid warning. */
    }
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void linenoiseBeep(void) {
    fprintf(stderr, "\x7");
    fflush(stderr);
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by linenoiseAddCompletion(). */
static void freeCompletions(linenoiseCompletions *lc) {
    size_t i;
    for (i = 0; i < lc->len; i++)
        free(lc->cvec[i]);
    if (lc->cvec != NULL)
        free(lc->cvec);
}

int linenoiseEditInsert(struct linenoiseState *l, char c);

/* This is an helper function for linenoiseEdit() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 *
 * The state of the editing is encapsulated into the pointed linenoiseState
 * structure as described in the structure definition. */
static int completeLine(struct linenoiseState *ls) {
    linenoiseCompletions lc = {0, NULL};
    char c = 0;

    completionCallback(ls->buf, &lc);
    if (lc.len == 1) {
        linenoiseBeep();
    } else if (lc.len == 2) {
        size_t i;
        size_t prefix_len = strlen(lc.cvec[0]);
        size_t count = strlen(lc.cvec[1]) - prefix_len;
        for (i = 0; i < count; i++) {
            linenoiseEditInsert(ls, lc.cvec[1][prefix_len + i]);
        }
        refreshLine(ls);
    } else {
        size_t i;
        size_t max_completion_length = 0;
        for (i = 1; i < lc.len; i++) {
            if (strlen(lc.cvec[i]) > max_completion_length) {
                max_completion_length = strlen(lc.cvec[i]);
            }
        }
        size_t column_width = max_completion_length + 1;

        size_t columns = (getColumns(STDIN_FILENO, STDOUT_FILENO) - 4) / column_width;

        char format[100];
        sprintf(format, "%%-%zus", column_width);

        for (i = 1; i < lc.len; i++) {
            if ((i - 1) % columns == 0) {
                printf("\r\n");
            }
            printf(format, lc.cvec[i]);
            printf(" ");
        }
        printf("\n");
        printf("\n");
        refreshLine(ls);
    }
    freeCompletions(&lc);
    return c; /* Return last read character */
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn) {
    completionCallback = fn;
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str) {
    size_t len = strlen(str);
    char *copy, **cvec;

    copy = malloc(len + 1);
    if (copy == NULL) return;
    memcpy(copy, str, len + 1);
    cvec = realloc(lc->cvec, sizeof(char *) * (lc->len + 1));
    if (cvec == NULL) {
        free(copy);
        return;
    }
    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* Register a callback function to be called for brace highlighting. */
void linenoiseSetHighlightCallback(linenoiseHighlightCallback *fn) {
    highlightCallback = fn;
}

/* Register a callback function to be called for canceling brace highlighting actions. */
void linenoiseSetHighlightCancelCallback(linenoiseHighlightCancelCallback *fn) {
    highlightCancelCallback = fn;
}

/* =========================== Line editing ================================= */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
};

static void abInit(struct abuf *ab) {
    ab->b = NULL;
    ab->len = 0;
}

static void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(new + ab->len, s, len);
    ab->b = new;
    ab->len += len;
}

static void abFree(struct abuf *ab) {
    free(ab->b);
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshSingleLine(struct linenoiseState *l) {
    char seq[64];
    size_t plen = strlen(l->prompt);
    int fd = l->ofd;
    char *buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;
    struct abuf ab;

    while ((plen + pos) >= l->cols) {
        buf++;
        len--;
        pos--;
    }
    while (plen + len > l->cols) {
        len--;
    }

    abInit(&ab);
    /* Cursor to left edge */
    snprintf(seq, 64, "\r");
    abAppend(&ab, seq, strlen(seq));
    /* Write the prompt and the current buffer content */
    abAppend(&ab, currentPromptAnsiCode, strlen(currentPromptAnsiCode));
    abAppend(&ab, l->prompt, strlen(l->prompt));
    abAppend(&ab, "\x1b[m", 3);
    abAppend(&ab, buf, len);
    /* Erase to right */
    snprintf(seq, 64, "\x1b[0K");
    abAppend(&ab, seq, strlen(seq));
    /* Move cursor to original position. */
    snprintf(seq, 64, "\r\x1b[%dC", (int) (pos + plen));
    abAppend(&ab, seq, strlen(seq));
    if (write(fd, ab.b, ab.len) == -1) {} /* Can't recover from write error. */
    abFree(&ab);
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshMultiLine(struct linenoiseState *l) {
    char seq[64];
    int plen = strlen(l->prompt);
    int rows = (plen + l->len + l->cols - 1) / l->cols; /* rows used by current buf. */
    int rpos = (plen + l->oldpos + l->cols) / l->cols; /* cursor relative row. */
    int rpos2; /* rpos after refresh. */
    int col; /* colum position, zero-based. */
    int old_rows = l->maxrows;
    int fd = l->ofd, j;
    struct abuf ab;

    /* Update maxrows if needed. */
    if (rows > (int) l->maxrows) l->maxrows = rows;

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    abInit(&ab);
    if (old_rows - rpos > 0) {
        lndebug("go down %d", old_rows - rpos);
        snprintf(seq, 64, "\x1b[%dB", old_rows - rpos);
        abAppend(&ab, seq, strlen(seq));
    }

    /* Now for every row clear it, go up. */
    for (j = 0; j < old_rows - 1; j++) {
        lndebug("clear+up");
        snprintf(seq, 64, "\r\x1b[0K\x1b[1A");
        abAppend(&ab, seq, strlen(seq));
    }

    /* Clean the top line. */
    lndebug("clear");
    snprintf(seq, 64, "\r\x1b[0K");
    abAppend(&ab, seq, strlen(seq));

    /* Write the prompt and the current buffer content */
    abAppend(&ab, currentPromptAnsiCode, strlen(currentPromptAnsiCode));
    abAppend(&ab, l->prompt, strlen(l->prompt));
    abAppend(&ab, "\x1b[m", 3);
    abAppend(&ab, l->buf, l->len);

    /* If we are at the very end of the screen with our prompt, we need to
     * emit a newline and move the prompt to the first column. */
    if (l->pos &&
        l->pos == l->len &&
        (l->pos + plen) % l->cols == 0) {
        lndebug("<newline>");
        abAppend(&ab, "\n", 1);
        snprintf(seq, 64, "\r");
        abAppend(&ab, seq, strlen(seq));
        rows++;
        if (rows > (int) l->maxrows) l->maxrows = rows;
    }

    /* Move cursor to right position. */
    rpos2 = (plen + l->pos + l->cols) / l->cols; /* current cursor relative row. */
    lndebug("rpos2 %d", rpos2);

    /* Go up till we reach the expected positon. */
    if (rows - rpos2 > 0) {
        lndebug("go-up %d", rows - rpos2);
        snprintf(seq, 64, "\x1b[%dA", rows - rpos2);
        abAppend(&ab, seq, strlen(seq));
    }

    /* Set column. */
    col = (plen + (int) l->pos) % (int) l->cols;
    lndebug("set col %d", 1 + col);
    if (col)
        snprintf(seq, 64, "\r\x1b[%dC", col);
    else
        snprintf(seq, 64, "\r");
    abAppend(&ab, seq, strlen(seq));

    lndebug("\n");
    l->oldpos = l->pos;

    if (write(fd, ab.b, ab.len) == -1) {} /* Can't recover from write error. */
    abFree(&ab);
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void refreshLine(struct linenoiseState *l) {
    if (mlmode)
        refreshMultiLine(l);
    else
        refreshSingleLine(l);
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
int linenoiseEditInsert(struct linenoiseState *l, char c) {
    if (l->len < l->buflen) {
        if (l->len == l->pos) {
            l->buf[l->pos] = c;
            l->pos++;
            l->len++;
            l->buf[l->len] = '\0';
            if ((!mlmode && l->plen + l->len < l->cols) /* || mlmode */) {
                /* Avoid a full update of the line in the
                 * trivial case. */
                if (write(l->ofd, &c, 1) == -1) return -1;
            } else {
                refreshLine(l);
            }
        } else {
            memmove(l->buf + l->pos + 1, l->buf + l->pos, l->len - l->pos);
            l->buf[l->pos] = c;
            l->len++;
            l->pos++;
            l->buf[l->len] = '\0';
            refreshLine(l);
        }

        if (!pasting && highlightCallback != NULL) {
            highlightCallback(l->buf, l->pos - 1);
        }

    }
    return 0;
}

/* Move cursor on the left. */
void linenoiseEditMoveLeft(struct linenoiseState *l) {
    if (l->pos > 0) {
        l->pos--;
        refreshLine(l);
    }
}

/* Move cursor on the right. */
void linenoiseEditMoveRight(struct linenoiseState *l) {
    if (l->pos != l->len) {
        l->pos++;
        refreshLine(l);
    }
}

/* Move cursor to the end of the current word. */
void linenoiseEditMoveWordEnd(struct linenoiseState *l) {
    if (l->len == 0 || l->pos >= l->len) return;
    if (l->buf[l->pos] == ' ')
        while (l->pos < l->len && l->buf[l->pos] == ' ') ++l->pos;
    while (l->pos < l->len && l->buf[l->pos] != ' ') ++l->pos;
    refreshLine(l);
}

/* Move cursor to the start of the current word. */
void linenoiseEditMoveWordStart(struct linenoiseState *l) {
    if (l->len == 0) return;
    if (l->buf[l->pos-1] == ' ') --l->pos;
    if (l->buf[l->pos] == ' ')
        while (l->pos > 0 && l->buf[l->pos] == ' ') --l->pos;
    while (l->pos > 0 && l->buf[l->pos-1] != ' ') --l->pos;
    refreshLine(l);
}

/* Move cursor to the start of the line. */
void linenoiseEditMoveHome(struct linenoiseState *l) {
    if (l->pos != 0) {
        l->pos = 0;
        refreshLine(l);
    }
}

/* Move cursor to the end of the line. */
void linenoiseEditMoveEnd(struct linenoiseState *l) {
    if (l->pos != l->len) {
        l->pos = l->len;
        refreshLine(l);
    }
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1

void linenoiseEditHistoryNext(struct linenoiseState *l, int dir) {
    if (history_len > 1) {
        /* Update the current history entry before to
         * overwrite it with the next one. */
        free(history[history_len - 1 - l->history_index]);
        history[history_len - 1 - l->history_index] = strdup(l->buf);
        /* Show the new entry */
        l->history_index += (dir == LINENOISE_HISTORY_PREV) ? 1 : -1;
        if (l->history_index < 0) {
            l->history_index = 0;
            return;
        } else if (l->history_index >= history_len) {
            l->history_index = history_len - 1;
            return;
        }
        strncpy(l->buf, history[history_len - 1 - l->history_index], l->buflen);
        l->buf[l->buflen - 1] = '\0';
        l->len = l->pos = strlen(l->buf);
        refreshLine(l);
    }
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
void linenoiseEditDelete(struct linenoiseState *l) {
    if (l->len > 0 && l->pos < l->len) {
        memmove(l->buf + l->pos, l->buf + l->pos + 1, l->len - l->pos - 1);
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Backspace implementation. */
void linenoiseEditBackspace(struct linenoiseState *l) {
    if (l->pos > 0 && l->len > 0) {
        memmove(l->buf + l->pos - 1, l->buf + l->pos, l->len - l->pos);
        l->pos--;
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Delete the previosu word, maintaining the cursor at the start of the
 * current word. */
void linenoiseEditDeletePrevWord(struct linenoiseState *l) {
    size_t old_pos = l->pos;
    size_t diff;

    while (l->pos > 0 && l->buf[l->pos - 1] == ' ')
        l->pos--;
    while (l->pos > 0 && l->buf[l->pos - 1] != ' ')
        l->pos--;
    diff = old_pos - l->pos;
    memmove(l->buf + l->pos, l->buf + old_pos, l->len - old_pos + 1);
    l->len -= diff;
    refreshLine(l);
}

/* Delete the next word, maintaining the cursor at the same position */
void linenoiseEditDeleteNextWord(struct linenoiseState *l) {
    size_t next_word_end = l->pos;
    while (next_word_end < l->len && l->buf[next_word_end] == ' ') ++next_word_end;
    while (next_word_end < l->len && l->buf[next_word_end] != ' ') ++next_word_end;
    memmove(l->buf+l->pos, l->buf+next_word_end, l->len-next_word_end);
    l->len -= next_word_end - l->pos;
    refreshLine(l);
}

static void set_current(struct linenoiseState *current, const char *str) {
    strncpy(current->buf, str, current->buflen);
    current->buf[current->buflen - 1] = 0;
    current->len = strlen(current->buf);
}

/* This function is the core of the line editing capability of linenoise.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 *
 * The resulting string is put into 'buf' when the user type enter, or
 * when ctrl+d is typed.
 *
 * The function returns the length of the current buffer. */
static int linenoiseEdit(int stdin_fd, int stdout_fd, char *buf, size_t buflen, const char *prompt, int spaces, char peek_char) {
    struct linenoiseState l;

    /* Populate the linenoise state that we pass to functions implementing
     * specific editing functionalities. */
    l.ifd = stdin_fd;
    l.ofd = stdout_fd;
    l.buf = buf;
    l.buflen = buflen;
    l.prompt = prompt;
    l.plen = strlen(prompt);
    l.oldpos = l.pos = 0;
    l.len = 0;
    l.cols = getColumns(stdin_fd, stdout_fd);
    l.maxrows = 0;
    l.history_index = 0;

    /* Buffer starts empty. */
    l.buf[0] = '\0';
    l.buflen--; /* Make sure there is always space for the nulterm */

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    linenoiseHistoryAdd("");

    if (write(l.ofd, prompt, l.plen) == -1) return -1;

    int i;
    for (i = 0; i < spaces; i++) {
        linenoiseEditInsert(&l, ' ');
    }
    refreshLine(&l);

    // Set things up so we can handle async prints coming int
    activeState = &l;

    while (1) {
        char c;
        int nread;
        char seq[3];

        if (peek_char) {
            c = peek_char;
            peek_char = 0;
            nread = 1;
        } else {
            nread = read(l.ifd, &c, 1);
        }

        if (c != keymap[KM_ENTER] && c != '>') {  // Also check for '>' so we can catch pasting involving prompt
            lastCharRead = system_time();
        } else {
            uint64_t now = system_time();
            pasting = now - lastCharRead < 10000;
        }

        if (nread <= 0) {
            activeState = NULL;
            return l.len;
        }

        if (highlightCancelCallback != NULL) {
            highlightCancelCallback();
        }

        /**
         * When pasting a multi-line form that has been copied,
         * detect "#_=> " preceeded by spaces, or primary prompt, 
         * and delete portion of line entered up to that point.
         */
        if (pasting &&
            c == ' ' &&
            l.pos > 1 &&
            l.buf[l.pos - 1] == '>' &&
            l.buf[l.pos - 2] == '=') {

            int delete = 0;

            if (l.pos > 3 &&
                l.buf[l.pos - 3] == '_' &&
                l.buf[l.pos - 4] == '#') {
                delete = 1;
                int i;
                for (i = 0; i < l.pos - 4; i++) {
                    if (l.buf[i] != ' ') {
                        delete = 0;
                        break;
                    }
                }
            } else if (l.pos + 1 == l.plen) { // Check for current primary prompt
                delete = 1;
                int i;
                for (i = 0; i < l.pos; i++) {
                    if (l.buf[i] != l.prompt[i]) {
                        delete = 0;
                        break;
                    }
                }
            }

            if (delete) {
                buf[0] = '\0';
                l.pos = l.len = 0;
                refreshLine(&l);
                continue;
            }
        }

        /* Only autocomplete when the callback is set. It returns < 0 when
         * there was an error reading from fd. Otherwise it will return the
         * character that should be handled next. */
        if (c == keymap[KM_TAB] && completionCallback != NULL) {
            c = completeLine(&l);
            /* Return on errors */
            if (c < 0) {
                activeState = NULL;
                return l.len;
            }
            /* Read next character when 0 */
            if (c == 0) continue;
        }

        process_char:
        if (c == 0) {
            continue;
        } else if (c == keymap[KM_ENTER]) {
            history_len--;
            free(history[history_len]);
            if (mlmode) linenoiseEditMoveEnd(&l);
            activeState = NULL;
            return (int) l.len;
        } else if (c == keymap[KM_CANCEL]) {
            errno = EAGAIN;
            activeState = NULL;
            return -1;
        } else if (c == keymap[KM_BACKSPACE] || c == keymap[KM_DELETE]) {
            linenoiseEditBackspace(&l);
        } else if (c == keymap[KM_DELETE_RIGHT]) {     /* remove char at right of cursor, or if the
                            line is empty, act as end-of-file. */
            if (l.len > 0) {
                linenoiseEditDelete(&l);
            } else {
                history_len--;
                free(history[history_len]);
                activeState = NULL;
                return -1;
            }
        } else if (c == keymap[KM_SWAP_CHARS]) { /* swaps current character with previous. */
            if (l.pos > 0 && l.pos < l.len) {
                int aux = buf[l.pos - 1];
                buf[l.pos - 1] = buf[l.pos];
                buf[l.pos] = aux;
                if (l.pos != l.len - 1) l.pos++;
                refreshLine(&l);
            }
        } else if (c == keymap[KM_MOVE_LEFT]) {
            linenoiseEditMoveLeft(&l);
        } else if (c == keymap[KM_MOVE_RIGHT]) {
            linenoiseEditMoveRight(&l);
        } else if (c == keymap[KM_HISTORY_PREVIOUS]) {
            linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_PREV);
        } else if (c == keymap[KM_HISTORY_NEXT]) {
            linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_NEXT);
        } else if (c == keymap[KM_REVERSE_I_SEARCH]) {

            /* Display the reverse-i-search prompt and process chars */

            char rbuf[50];
            char rprompt[80];
            int rlen = 0;
            int searchpos = history_len - 1;

            rbuf[0] = 0;
            while (1) {
                const char *p = NULL;
                int skipsame = 0;
                int searchdir = -1;

                snprintf(rprompt, sizeof(rprompt), "(reverse-i-search)`%s': ", rbuf);
                l.prompt = rprompt;
                refreshLine(&l);

                if (read(l.ifd, &c, 1)) {

                    if (c == keymap[KM_BACKSPACE] || c == keymap[KM_DELETE]) {
                        if (rlen) {
                            rbuf[--rlen] = 0;
                        }
                        continue;
                    }

                    if (c == keymap[KM_REVERSE_I_SEARCH] || c == keymap[KM_HISTORY_PREVIOUS]) {
                        /* Search for the previous (earlier) match */
                        if (searchpos > 0) {
                            searchpos--;
                        }
                        skipsame = 1;
                    } else if (c == keymap[KM_HISTORY_NEXT]) {
                        /* Search for the next (later) match */
                        if (searchpos < history_len) {
                            searchpos++;
                        }
                        searchdir = 1;
                        skipsame = 1;
                    } else if (c >= ' ') {
                        if (rlen >= (int) sizeof(rbuf) - 1) {
                            continue;
                        }

                        rbuf[rlen++] = c;
                        rbuf[rlen] = 0;

                        /* Adding a new char resets the search location */
                        searchpos = history_len - 1;
                    } else {
                        /* Exit from incremental search mode */
                        break;
                    }

                    /* Now search through the history for a match */
                    for (; searchpos >= 0 && searchpos < history_len; searchpos += searchdir) {
                        p = strstr(history[searchpos], rbuf);
                        if (p) {
                            /* Found a match */
                            if (skipsame && strcmp(history[searchpos], l.buf) == 0) {
                                /* But it is identical, so skip it */
                                continue;
                            }
                            /* Copy the matching line and set the cursor position */
                            set_current(&l, history[searchpos]);
                            l.pos = p - history[searchpos];
                            l.history_index = history_len - searchpos - 1;
                            break;
                        }
                    }

                    if (!p && c >= ' ') {
                        /* No match, so don't add it */
                        rlen--;
                        rbuf[rlen] = 0;
                    }
                }
            }

            if (c == keymap[KM_CANCEL_SEARCH]) {
                /* terminates the search with no effect */
                set_current(&l, "");
                l.pos = 0;
                c = 0;
            } else if (c == keymap[KM_FINISH_SEARCH]) {
                /* terminates the search leaving the buffer in place */
                c = 0;
            }

            /* Go process the char normally */
            l.prompt = prompt;
            refreshLine(&l);
            goto process_char;

        } else if (c == keymap[KM_ESC]) {    /* escape sequence */
            /* Read the next two bytes representing the escape sequence.
             * Use two calls to handle slow terminals returning the two
             * chars at different times. */
            if (read(l.ifd, seq, 1) != -1) {
                if (seq[0] != '[' && seq[0] != 'O') {
                    c = seq[0];
                    switch (seq[0]) {
                    case 'f':
                        linenoiseEditMoveWordEnd(&l);
                        break;
                    case 'b':
                        linenoiseEditMoveWordStart(&l);
                        break;
                    case 'd':
                        linenoiseEditDeleteNextWord(&l);
                        break;
                    default:
                        goto process_char;
                    }
                } else if (read(l.ifd, seq + 1, 1) != -1) {

                    /* ESC [ sequences. */
                    if (seq[0] == '[') {
                        if (seq[1] >= '0' && seq[1] <= '9') {
                            /* Extended escape, read additional byte. */
                            if (read(l.ifd, seq + 2, 1) == -1) break;
                            if (seq[2] == '~') {
                                switch (seq[1]) {
                                    case '3': /* Delete key. */
                                        linenoiseEditDelete(&l);
                                        break;
                                }
                            }
                        } else {
                            switch (seq[1]) {
                                case 'A': /* Up */
                                    linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_PREV);
                                    break;
                                case 'B': /* Down */
                                    linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_NEXT);
                                    break;
                                case 'C': /* Right */
                                    linenoiseEditMoveRight(&l);
                                    break;
                                case 'D': /* Left */
                                    linenoiseEditMoveLeft(&l);
                                    break;
                                case 'H': /* Home */
                                    linenoiseEditMoveHome(&l);
                                    break;
                                case 'F': /* End*/
                                    linenoiseEditMoveEnd(&l);
                                    break;
                            }
                        }
                    }

                        /* ESC O sequences. */
                    else if (seq[0] == 'O') {
                        switch (seq[1]) {
                            case 'H': /* Home */
                                linenoiseEditMoveHome(&l);
                                break;
                            case 'F': /* End*/
                                linenoiseEditMoveEnd(&l);
                                break;
                        }
                    }
                }
            }
        } else if (c == keymap[KM_CLEAR_LINE]) { /*  delete the whole line. */
            buf[0] = '\0';
            l.pos = l.len = 0;
            refreshLine(&l);
        } else if (c == keymap[KM_DELETE_TO_END_OF_LINE]) { /* delete from current to end of line. */
            buf[l.pos] = '\0';
            l.len = l.pos;
            refreshLine(&l);
        } else if (c == keymap[KM_GO_TO_START_OF_LINE]) { /*  go to the start of the line */
            linenoiseEditMoveHome(&l);
        } else if (c == keymap[KM_GO_TO_END_OF_LINE]) { /* go to the end of the line */
            linenoiseEditMoveEnd(&l);
        } else if (c == keymap[KM_CLEAR_SCREEN]) { /* clear screen */
            linenoiseClearScreen();
            refreshLine(&l);
        } else if (c == keymap[KM_DELETE_PREVIOUS_WORD]) { /* delete previous word */
            linenoiseEditDeletePrevWord(&l);
        } else {
            if (linenoiseEditInsert(&l, c)) {
                activeState = NULL;
                return -1;
            }
        }
    }
    activeState = NULL;
    return l.len;
}

/* This special mode is used by linenoise in order to print scan codes
 * on screen for debugging / development purposes. It is implemented
 * by the linenoise_example program using the --keycodes option. */
void linenoisePrintKeyCodes(void) {
    char quit[4];

    printf("Linenoise key codes debugging mode.\n"
                   "Press keys to see scan codes. Type 'quit' at any time to exit.\n");
    if (enableRawMode(STDIN_FILENO) == -1) return;
    memset(quit, ' ', 4);
    while (1) {
        char c;
        int nread;

        nread = read(STDIN_FILENO, &c, 1);
        if (nread <= 0) continue;
        memmove(quit, quit + 1, sizeof(quit) - 1); /* shift string to left. */
        quit[sizeof(quit) - 1] = c; /* Insert current char on the right. */
        if (memcmp(quit, "quit", sizeof(quit)) == 0) break;

        printf("'%c' %02x (%d) (type quit to exit)\n",
               isprint(c) ? c : '?', (int) c, (int) c);
        printf("\r"); /* Go left edge manually, we are in raw mode. */
        fflush(stdout);
    }
    disableRawMode(STDIN_FILENO);
}

static char get_next_char() {
    char c = 0;
    int orig = fcntl(STDIN_FILENO, F_GETFL);
    if (fcntl(STDIN_FILENO, F_SETFL, orig | O_NONBLOCK) == -1) {
        fprintf(stderr, "Failed to set `O_NONBLOCK` on `STDIN_FILENO`\n");
    }
    ssize_t nread = read(STDIN_FILENO, &c, 1);
    if (nread == -1) {
        c = 0;
    }
    if (fcntl(STDIN_FILENO, F_SETFL, orig) == -1) {
        fprintf(stderr, "Failed to turn off `O_NONBLOCK` on `STDIN_FILENO`\n");
    }
    errno = 0;
    return c;
}

/* This function calls the line editing function linenoiseEdit() using
 * the STDIN file descriptor set in raw mode. */
static char* linenoiseRaw(const char *prompt, const char *secondary_prompt, int spaces) {

    if (!isatty(STDIN_FILENO)) {
        /* Not a tty: read from file / pipe. */
        char buf[LINENOISE_MAX_LINE];
        if (fgets(buf, LINENOISE_MAX_LINE, stdin) == NULL) return NULL;
        size_t count = strlen(buf);
        if (count && buf[count - 1] == '\n') {
            count--;
            buf[count] = '\0';
        }
        return strdup(buf);
    } else {
        /* Interactive editing. */

        if (enableRawMode(STDIN_FILENO) == -1) {
            fprintf(stderr, "failed to enable raw mode");
            return NULL; // TODO handle better?
        }

        size_t accum_buf_size = 2 * LINENOISE_MAX_LINE;
        size_t accum_count = 0;
        char* accum_buf = malloc(accum_buf_size);


        char peek_char = 0;
        int done = 0;
        const char *current_prompt = prompt;
        while (!done) {
            char buf[LINENOISE_MAX_LINE];
            int count = linenoiseEdit(STDIN_FILENO, STDOUT_FILENO, buf, LINENOISE_MAX_LINE, current_prompt, spaces, peek_char);

            done = 1;
            if (count == -1) {
                free(accum_buf);
                accum_buf = NULL;
            } else {
                if (accum_count + count + 2 > accum_buf_size) { // 1 for newline and 1 for null-terminator
                    accum_buf_size *= 2;
                    accum_buf = realloc(accum_buf, accum_buf_size);
                }
                if (accum_count) {
                    accum_buf[accum_count++] = '\n';
                }
                memcpy(accum_buf + accum_count, buf, count);
                accum_count += count;
                accum_buf[accum_count] = '\0';

                peek_char = get_next_char();

                if (peek_char) {
                    current_prompt = secondary_prompt;
                    printf("\n");
                    done = 0;
                    pasting = 1;
                }
            }
        }
        disableRawMode(STDIN_FILENO);
        printf("\n");
        //pasting = 0;
        return accum_buf;
    }
}

void linenoisePrintNow(const char *text) {

    if (strcmp(text, "\n") != 0) {
        fprintf(stdout, "\r\x1b[0K%s\n", text);

        if (activeState) {
            refreshLine(activeState);
        }
    }
}

int is_pasting() {
    return pasting;
}

/* The high level function that is the main API of the linenoise library.
 * This function checks if the terminal has basic capabilities, just checking
 * for a blacklist of stupid terminals, and later either calls the line
 * editing function or uses dummy fgets() so that you will be able to type
 * something even in the most desperate of the conditions. */
char *linenoise(const char *prompt, const char* secondary_prompt, const char *promptAnsiCode, int spaces) {
    char buf[LINENOISE_MAX_LINE];

    if (isUnsupportedTerm()) {
        size_t len;

        printf("%s", prompt);
        fflush(stdout);
        if (fgets(buf, LINENOISE_MAX_LINE, stdin) == NULL) return NULL;
        len = strlen(buf);
        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            len--;
            buf[len] = '\0';
        }
        return strdup(buf);
    } else {
        currentPromptAnsiCode = promptAnsiCode;
        return linenoiseRaw(prompt, secondary_prompt, spaces);
    }
}

/* ================================ History ================================= */

/* Free the history, but does not reset it. Only used when we have to
 * exit() to avoid memory leaks are reported by valgrind & co. */
static void freeHistory(void) {
    if (history) {
        int j;

        for (j = 0; j < history_len; j++)
            free(history[j]);
        free(history);
    }
}

/* At exit we'll try to fix the terminal to the initial conditions. */
static void linenoiseAtExit(void) {
    disableRawMode(STDIN_FILENO);
    freeHistory();
}

/* This is the API call to add a new entry in the linenoise history.
 * It uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(const char *line) {
    char *linecopy;

    if (history_max_len == 0) return 0;

    /* Initialization on first call. */
    if (history == NULL) {
        history = malloc(sizeof(char *) * history_max_len);
        if (history == NULL) return 0;
        memset(history, 0, (sizeof(char *) * history_max_len));
    }

    /* Don't add duplicated lines. */
    if (history_len && !strcmp(history[history_len - 1], line)) return 0;

    /* Add an heap allocated copy of the line in the history.
     * If we reached the max length, remove the older line. */
    linecopy = strdup(line);
    if (!linecopy) return 0;
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char *) * (history_max_len - 1));
        history_len--;
    }
    history[history_len] = linecopy;
    history_len++;
    return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int linenoiseHistorySetMaxLen(int len) {
    char **new;

    if (len < 1) return 0;
    if (history) {
        int tocopy = history_len;

        new = malloc(sizeof(char *) * len);
        if (new == NULL) return 0;

        /* If we can't copy everything, free the elements we'll not use. */
        if (len < tocopy) {
            int j;

            for (j = 0; j < tocopy - len; j++) free(history[j]);
            tocopy = len;
        }
        memset(new, 0, sizeof(char *) * len);
        memcpy(new, history + (history_len - tocopy), sizeof(char *) * tocopy);
        free(history);
        history = new;
    }
    history_max_len = len;
    if (history_len > history_max_len)
        history_len = history_max_len;
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(const char *filename) {
    FILE *fp = fopen(filename, "w");
    int j;

    if (fp == NULL) return -1;
    for (j = 0; j < history_len; j++)
        fprintf(fp, "%s\n", history[j]);
    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(const char *filename) {
    // Take opportunity to initialize lastKeyPress
    lastCharRead = system_time();

    FILE *fp = fopen(filename, "r");
    char buf[LINENOISE_MAX_LINE];

    if (fp == NULL) return -1;

    while (fgets(buf, LINENOISE_MAX_LINE, fp) != NULL) {
        char *p;

        p = strchr(buf, '\r');
        if (!p) p = strchr(buf, '\n');
        if (p) *p = '\0';
        linenoiseHistoryAdd(buf);
    }
    fclose(fp);
    return 0;
}

void sigwinchHandler( int sig_number ) {
    if (activeState) {
        activeState->cols = getColumns(activeState->ifd, activeState->ofd);
    }
}

void linenoiseSetupSigWinchHandler() {
    signal(SIGWINCH, sigwinchHandler);
}

char linenoiseGetKeyPress() {
    if (enableRawMode(STDIN_FILENO) == -1) {
        fprintf(stderr, "failed to enable raw mode");
        return 0;
    }
    char c;
    ssize_t nread = read(STDIN_FILENO, &c, 1);
    if (nread == -1) {
        c = 0;
    }
    disableRawMode(STDIN_FILENO);
    return c;
}
