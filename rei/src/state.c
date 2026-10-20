/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "state.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cJSON* rei_load(const char* path) {
    FILE* f;
    Buf b = {0};
    char chunk[4096];
    size_t n;
    cJSON* state;
    if (!path || !*path) return cJSON_CreateObject();
    f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT) rei_log("%s: %s: it starts with no cards and no replies kept", path, strerror(errno));
        return cJSON_CreateObject();
    }
    while ((n = fread(chunk, 1, sizeof chunk, f))) buf_addn(&b, chunk, n);
    fclose(f);
    state = cJSON_Parse(b.text ? b.text : "");
    if (!cJSON_IsObject(state)) {
        rei_log("%s: its %zu bytes are no JSON object: it starts with no cards and no replies kept", path, b.len);
        cJSON_Delete(state);
        state = cJSON_CreateObject();
    }
    buf_free(&b);
    return state;
}

int rei_save(const char* path, const cJSON* state) {
    char *text, *temporary;
    FILE* f;
    int failed;
    if (!path || !*path) return 0;
    text = cJSON_PrintUnformatted(state);
    temporary = rei_format("%s.new", path);
    f = fopen(temporary, "wb");
    failed = !f || fputs(text, f) == EOF;
    if (f && fclose(f)) failed = 1;
    if (failed || rename(temporary, path)) {
        rei_log("%s: not written: %s", path, strerror(errno));
        remove(temporary);
        failed = 1;
    }
    free(temporary);
    free(text);
    return failed ? -1 : 0;
}
