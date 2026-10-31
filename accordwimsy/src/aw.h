/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AccordWimsy's common pieces: strings that grow, cJSON's lookups by key, the log, one HTTP request */
#ifndef AW_H
#define AW_H
#include <stddef.h>
#include "cJSON.h"

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

char* aw_strdup(const char* s);
/* a formatted string, owned by the caller */
char* aw_format(const char* fmt, ...);
/* s in base64, owned */
char* aw_base64(const char* s);

/* key's value of an object, or NULL: a string, a number (0 when not one), a boolean (0 when not one) */
const char* jstr(const cJSON* o, const char* key);
double jnum(const cJSON* o, const char* key);
int jbool(const cJSON* o, const char* key);

/* a line of what a pass did, on stdout, and of what went wrong, on stderr (cron mails both): "accordwimsy: ..." */
void aw_log(const char* fmt, ...);
void aw_warn(const char* fmt, ...);

/* one HTTP request: method url, Authorization auth when not NULL, a JSON body when not NULL */
typedef struct {
    int status;  /* HTTP's, 0 when no answer came */
    char* body;  /* owned, or NULL */
    char* error; /* owned: why no answer came, or NULL */
} AwAnswer;
typedef void (*AwFetch)(void* ctx, const char* method, const char* url, const char* auth, const char* body,
                        AwAnswer* out);
void aw_answer_free(AwAnswer* a);
#endif
