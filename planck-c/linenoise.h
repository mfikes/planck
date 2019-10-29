/* linenoise.h -- VERSION 1.0
 *
 * Guerrilla line editing library against the idea that a line editing lib
 * needs to be 20,000 lines of C code.
 *
 * See linenoise.c for more information.
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
 */

#ifndef __LINENOISE_H
#define __LINENOISE_H

#ifdef __cplusplus
extern "C" {
#endif

void linenoiseSetupSigWinchHandler();

typedef struct linenoiseCompletions {
    size_t len;
    char **cvec;
} linenoiseCompletions;

typedef void(linenoiseCompletionCallback)(const char *, linenoiseCompletions *);

void linenoiseSetCompletionCallback(linenoiseCompletionCallback *);

void linenoiseAddCompletion(linenoiseCompletions *, const char *);

typedef void(linenoiseHighlightCallback)(const char *, int pos);

void linenoiseSetHighlightCallback(linenoiseHighlightCallback *);

typedef void(linenoiseHighlightCancelCallback)();

void linenoiseSetHighlightCancelCallback(linenoiseHighlightCancelCallback *);

void linenoisePrintNow(const char *text);

char *linenoise(const char *prompt, const char *secondary_prompt, const char *promptAnsiCode, int spaces);

int linenoiseHistoryAdd(const char *line);

int linenoiseHistorySetMaxLen(int len);

int linenoiseHistorySave(const char *filename);

int linenoiseHistoryLoad(const char *filename);

void linenoiseClearScreen(void);

void linenoiseSetMultiLine(int ml);

void linenoisePrintKeyCodes(void);

void linenoiseSetKeymapEntry(int action, char key);

int is_pasting();

char linenoiseGetKeyPress();

#define KM_GO_TO_START_OF_LINE 0
#define KM_MOVE_LEFT 1
#define KM_CANCEL 2
#define KM_DELETE_RIGHT 3
#define KM_GO_TO_END_OF_LINE 4
#define KM_MOVE_RIGHT 5
#define KM_DELETE 6
#define KM_TAB 7
#define KM_DELETE_TO_END_OF_LINE 8
#define KM_CLEAR_SCREEN 9
#define KM_ENTER 10
#define KM_HISTORY_NEXT 11
#define KM_HISTORY_PREVIOUS 12
#define KM_SWAP_CHARS 13
#define KM_CLEAR_LINE 14
#define KM_DELETE_PREVIOUS_WORD 15
#define KM_ESC 16
#define KM_BACKSPACE 17
#define KM_REVERSE_I_SEARCH 18
#define KM_CANCEL_SEARCH 19
#define KM_FINISH_SEARCH 20

#ifdef __cplusplus
}
#endif

#endif /* __LINENOISE_H */
