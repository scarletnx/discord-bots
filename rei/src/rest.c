/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _POSIX_C_SOURCE 200809L
#include "rest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void nap(double seconds) {
    struct timespec t;
    t.tv_sec = (time_t)seconds;
    t.tv_nsec = (long)((seconds - (double)t.tv_sec) * 1e9);
    nanosleep(&t, NULL);
}
void dc_init(Dc* d, RgFetch fetch, void* ctx) {
    memset(d, 0, sizeof *d);
    d->fetch = fetch;
    d->ctx = ctx;
    d->sleep = nap;
}
static void answer_free(RgAnswer* a) {
    free(a->body);
    free(a->etag);
    free(a->error);
    memset(a, 0, sizeof *a);
}
/* one request, asked again while Discord says to wait (429) or failed on its side (5xx, once): its status, *out the
   last answer (the caller frees it) */
static int ask(Dc* d, const char* method, const char* path, const char* body, RgAnswer* out) {
    char* url = rei_format(DC_API "%s", path);
    int tries, server_errors = 0;
    for (tries = 1;; ++tries) {
        d->fetch(d->ctx, method, url, body, NULL, out);
        if (out->status == 429 && tries < DC_TRIES) {
            cJSON* said = cJSON_Parse(out->body ? out->body : "");
            double wait = jnum(said, "retry_after");
            cJSON_Delete(said);
            answer_free(out);
            d->sleep(wait > 0 && wait < 60 ? wait : 1);
            continue;
        }
        if (out->status >= 500 && !server_errors++ && tries < DC_TRIES) {
            answer_free(out);
            d->sleep(2);
            continue;
        }
        break;
    }
    free(url);
    return out->status;
}
static void said_wrong(const char* what, const char* path, const RgAnswer* a) {
    rei_log("Discord: %s %s answered %d: %.200s", what, path, a->status,
            a->error  ? a->error
            : a->body ? a->body
                      : "nothing");
}
/* the body of a message carrying embed: no pings, a reply to reply_to when not 0; owned */
static char* message_of(const cJSON* embed, u64 reply_to) {
    cJSON *m = cJSON_CreateObject(), *embeds = cJSON_AddArrayToObject(m, "embeds"),
          *mentions = cJSON_AddObjectToObject(m, "allowed_mentions");
    char* text;
    cJSON_AddItemToArray(embeds, cJSON_Duplicate(embed, 1));
    cJSON_AddArrayToObject(mentions, "parse");
    if (reply_to) {
        cJSON* ref = cJSON_AddObjectToObject(m, "message_reference");
        char id[24];
        snprintf(id, sizeof id, "%llu", (unsigned long long)reply_to);
        cJSON_AddStringToObject(ref, "message_id", id);
        cJSON_AddBoolToObject(ref, "fail_if_not_exists", 0);
        cJSON_AddBoolToObject(mentions, "replied_user", 0);
    }
    text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    return text;
}
static u64 id_of(const cJSON* o, const char* key) {
    const char* s = jstr(o, key);
    return s ? strtoull(s, NULL, 10) : 0;
}

u64 dc_self(Dc* d) {
    RgAnswer a = {0};
    if (!d->self && ask(d, "GET", "users/@me", NULL, &a) == 200) {
        cJSON* me = cJSON_Parse(a.body);
        d->self = id_of(me, "id");
        cJSON_Delete(me);
    } else if (!d->self)
        said_wrong("GET", "users/@me", &a);
    answer_free(&a);
    return d->self;
}
u64 dc_post(Dc* d, u64 channel, const cJSON* embed, u64 reply_to) {
    char *path = rei_format("channels/%llu/messages", (unsigned long long)channel), *body = message_of(embed, reply_to);
    RgAnswer a = {0};
    u64 id = 0;
    if (ask(d, "POST", path, body, &a) == 200) {
        cJSON* sent = cJSON_Parse(a.body);
        id = id_of(sent, "id");
        cJSON_Delete(sent);
    }
    if (!id) said_wrong("POST", path, &a);
    answer_free(&a);
    free(body);
    free(path);
    return id;
}
/* 0 for a 2xx, 404 for gone, else the status, -1 for no answer (said in the log) */
static int done_or_gone(const char* what, const char* path, RgAnswer* a, int status) {
    int said;
    if (status >= 200 && status < 300)
        said = 0;
    else if (status == 404)
        said = 404;
    else {
        said = status ? status : -1;
        said_wrong(what, path, a);
    }
    answer_free(a);
    return said;
}
int dc_edit(Dc* d, u64 channel, u64 message, const cJSON* embed) {
    char *path = rei_format("channels/%llu/messages/%llu", (unsigned long long)channel, (unsigned long long)message),
         *body = message_of(embed, 0);
    RgAnswer a = {0};
    int said = done_or_gone("PATCH", path, &a, ask(d, "PATCH", path, body, &a));
    free(body);
    free(path);
    return said;
}
int dc_remove(Dc* d, u64 channel, u64 message) {
    char* path = rei_format("channels/%llu/messages/%llu", (unsigned long long)channel, (unsigned long long)message);
    RgAnswer a = {0};
    int said = done_or_gone("DELETE", path, &a, ask(d, "DELETE", path, NULL, &a));
    free(path);
    return said;
}
int dc_history(Dc* d, u64 channel, u64 before, u64* ids, int* webhook, int* mine, int max) {
    char* path =
        before ? rei_format("channels/%llu/messages?limit=%d&before=%llu", (unsigned long long)channel,
                            max > 100 ? 100 : max, (unsigned long long)before)
               : rei_format("channels/%llu/messages?limit=%d", (unsigned long long)channel, max > 100 ? 100 : max);
    RgAnswer a = {0};
    cJSON* list = NULL;
    const cJSON* m;
    int n = -1;
    u64 self = dc_self(d);
    if (ask(d, "GET", path, NULL, &a) == 200) list = cJSON_Parse(a.body);
    if (cJSON_IsArray(list)) {
        n = 0;
        cJSON_ArrayForEach(m, list) {
            if (n == max) break;
            ids[n] = id_of(m, "id");
            webhook[n] = jstr(m, "webhook_id") != NULL;
            mine[n] = self && id_of(cJSON_GetObjectItemCaseSensitive(m, "author"), "id") == self;
            ++n;
        }
    } else
        said_wrong("GET", path, &a);
    cJSON_Delete(list);
    answer_free(&a);
    free(path);
    return n;
}
