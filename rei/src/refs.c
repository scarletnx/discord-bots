/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "refs.h"
#include "rei.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* a letter, a digit or _; any byte of a UTF-8 letter counts, as Python's \w takes the letter */
static int word(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }
static int space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static void blank(char* s, size_t from, size_t to) { memset(s + from, ' ', to - from); }

/* text without its code and links, which name nothing; owned */
static char* prose(const char* text) {
    char* s = rei_strdup(text);
    size_t i = 0, n = strlen(s);
    while (i < n) { /* code first, as a link inside it is code */
        if (!strncmp(s + i, "```", 3)) {
            const char* end = strstr(s + i + 3, "```");
            if (end) {
                size_t to = (size_t)(end - s) + 3;
                blank(s, i, to);
                i = to;
                continue;
            }
        }
        if (s[i] == '`') {
            size_t k = i + 1;
            while (k < n && s[k] != '`' && s[k] != '\n') ++k;
            if (k < n && s[k] == '`') {
                blank(s, i, k + 1);
                i = k + 1;
                continue;
            }
        }
        ++i;
    }
    for (i = 0; i < n;) {
        size_t j = i + (s[i] == '<'), scheme = !strncmp(s + j, "https://", 8)  ? 8
                                               : !strncmp(s + j, "http://", 7) ? 7
                                                                               : 0;
        if (scheme) {
            size_t k = j + scheme;
            while (k < n && !space((unsigned char)s[k])) ++k;
            if (k > j + scheme) {
                blank(s, i, k);
                i = k;
                continue;
            }
        }
        ++i;
    }
    return s;
}

int rei_references(const char* text, int numbers[REI_MOST]) {
    char* s = prose(text ? text : "");
    int found = 0;
    size_t i;
    for (i = 0; s[i] && found < REI_MOST; ++i) {
        size_t k = i + 1;
        int number = 0, seen, j;
        unsigned char before = i ? (unsigned char)s[i - 1] : ' ';
        if (s[i] != '#' || word(before) || before == '/' || before == '#' || before == '&') continue;
        while (isdigit((unsigned char)s[k]) && k - i <= 7) number = number * 10 + (s[k++] - '0');
        if (k == i + 1 || k - i > 7 || word((unsigned char)s[k]) || !number) continue;
        for (seen = 0, j = 0; j < found; ++j) seen |= numbers[j] == number;
        if (!seen) numbers[found++] = number;
    }
    free(s);
    return found;
}

static int word_at(const char** p, const char* w) {
    size_t n = strlen(w), i;
    for (i = 0; i < n; ++i)
        if (tolower((unsigned char)(*p)[i]) != w[i]) return 0;
    *p += n;
    return 1;
}
int rei_command(const char* text, int* number) {
    const char* p = text ? text : "";
    int kind;
    *number = 0;
    while (space((unsigned char)*p)) ++p;
    if (*p++ != '!') return REI_NO_COMMAND;
    if (word_at(&p, "nightly"))
        kind = REI_NIGHTLY;
    else if (word_at(&p, "release")) {
        const char* q = p;
        kind = REI_RELEASE;
        while (space((unsigned char)*q)) ++q;
        if (q > p && *q == '#' && isdigit((unsigned char)q[1])) {
            int digits = 0, n = 0;
            ++q;
            while (isdigit((unsigned char)*q) && digits < 7) n = n * 10 + (*q++ - '0'), ++digits;
            if (digits > 6) return REI_NO_COMMAND;
            *number = n;
            kind = REI_RELEASE_PR;
            p = q;
        }
    } else
        return REI_NO_COMMAND;
    while (space((unsigned char)*p)) ++p;
    if (*p) {
        *number = 0;
        return REI_NO_COMMAND;
    }
    return kind;
}
