/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ReiWimsy's common pieces: strings that grow, cJSON's lookups by key, the log, the clock */
#ifndef REI_H
#define REI_H
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

typedef uint64_t u64;

/* a string that grows; its text is NUL-terminated, "" while empty */
typedef struct {
    char* text;
    size_t len, cap;
} Buf;
void buf_add(Buf* b, const char* s);
void buf_addn(Buf* b, const char* s, size_t n);
void buf_printf(Buf* b, const char* fmt, ...);
void buf_free(Buf* b);
/* the text, given away (the buffer empty after); never NULL */
char* buf_take(Buf* b);

char* rei_strdup(const char* s);
/* a formatted string, owned by the caller */
char* rei_format(const char* fmt, ...);

/* key's value of an object, or NULL: a string, a number (0 when not one), a boolean (0 when not one) */
const char* jstr(const cJSON* o, const char* key);
double jnum(const cJSON* o, const char* key);
int jbool(const cJSON* o, const char* key);
/* a value down a path of keys, "pull_request.merged_at", or NULL */
const cJSON* jpath(const cJSON* o, const char* path);
/* a string member added to an object, or skipped when s is NULL */
void jset(cJSON* o, const char* key, const char* s);

/* a line in the log (stderr, which daemon(8) sends to syslog): "reiwimsy: ..." */
void rei_log(const char* fmt, ...);

/* seconds since the epoch, settable for the tests */
extern double (*rei_now)(void);
#endif
