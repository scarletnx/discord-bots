/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "rei.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void grow(Buf* b, size_t more) {
    size_t want = b->len + more + 1;
    char* bigger;
    if (want <= b->cap) return;
    if (want < 2 * b->cap) want = 2 * b->cap;
    bigger = realloc(b->text, want);
    if (!bigger) {
        fputs("reiwimsy: out of memory\n", stderr);
        abort();
    }
    b->text = bigger;
    b->cap = want;
}
void buf_addn(Buf* b, const char* s, size_t n) {
    grow(b, n);
    memcpy(b->text + b->len, s, n);
    b->len += n;
    b->text[b->len] = 0;
}
void buf_add(Buf* b, const char* s) { buf_addn(b, s, strlen(s)); }
void buf_printf(Buf* b, const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->text + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}
void buf_free(Buf* b) {
    free(b->text);
    b->text = NULL;
    b->len = b->cap = 0;
}
char* buf_take(Buf* b) {
    char* text = b->text ? b->text : rei_strdup("");
    b->text = NULL;
    b->len = b->cap = 0;
    return text;
}

char* rei_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* copy = malloc(n);
    if (!copy) {
        fputs("reiwimsy: out of memory\n", stderr);
        abort();
    }
    return memcpy(copy, s, n);
}
char* rei_format(const char* fmt, ...) {
    Buf b = {0};
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return rei_strdup("");
    grow(&b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b.text, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b.len = (size_t)n;
    return buf_take(&b);
}

const char* jstr(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
double jnum(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}
int jbool(const cJSON* o, const char* key) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key)); }
const cJSON* jpath(const cJSON* o, const char* path) {
    char key[64];
    while (o && *path) {
        size_t n = strcspn(path, ".");
        if (n >= sizeof key) return NULL;
        memcpy(key, path, n);
        key[n] = 0;
        o = cJSON_GetObjectItemCaseSensitive(o, key);
        path += n + (path[n] == '.');
    }
    return o;
}
void jset(cJSON* o, const char* key, const char* s) {
    if (s) cJSON_AddStringToObject(o, key, s);
}

void rei_log(const char* fmt, ...) {
    va_list ap;
    fputs("reiwimsy: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static double wall(void) { return (double)time(NULL); }
double (*rei_now)(void) = wall;
