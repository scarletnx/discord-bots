/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "event.h"
#include <stdlib.h>
#include <string.h>

/* a snowflake, a string of digits; 0 for anything else */
static u64 id_of(const cJSON* o, const char* key) {
    const char* s = jstr(o, key);
    char* end;
    u64 v;
    if (!s || *s < '0' || *s > '9') return 0;
    v = strtoull(s, &end, 10);
    return *end ? 0 : v;
}

int ev_message(const char* json, size_t len, EvMessage* m) {
    cJSON* d = cJSON_ParseWithLength(json, len);
    const cJSON* author = cJSON_GetObjectItemCaseSensitive(d, "author");
    const char* content = jstr(d, "content");
    int ok;
    memset(m, 0, sizeof *m);
    m->id = id_of(d, "id");
    m->channel = id_of(d, "channel_id");
    ok = cJSON_IsObject(author) && !jbool(author, "bot") && !jstr(d, "webhook_id") && m->id && m->channel;
    if (ok)
        m->content = rei_strdup(content ? content : "");
    else
        memset(m, 0, sizeof *m);
    cJSON_Delete(d);
    return ok;
}

void ev_message_free(EvMessage* m) {
    free(m->content);
    memset(m, 0, sizeof *m);
}

char* ev_ready_user(const char* json, size_t len) {
    cJSON* d = cJSON_ParseWithLength(json, len);
    const cJSON* name = jpath(d, "user.username");
    char* user = cJSON_IsString(name) ? rei_strdup(name->valuestring) : NULL;
    cJSON_Delete(d);
    return user;
}

void ev_queue_init(EvQueue* q) {
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->more, NULL);
}
int ev_push(EvQueue* q, EvMessage* m) {
    int room;
    pthread_mutex_lock(&q->lock);
    room = q->n < EV_WAITING;
    if (room) {
        q->m[(q->first + q->n++) % EV_WAITING] = *m;
        memset(m, 0, sizeof *m);
        pthread_cond_signal(&q->more);
    }
    pthread_mutex_unlock(&q->lock);
    return room;
}
void ev_pop(EvQueue* q, EvMessage* m) {
    pthread_mutex_lock(&q->lock);
    while (!q->n) pthread_cond_wait(&q->more, &q->lock);
    *m = q->m[q->first];
    q->first = (q->first + 1) % EV_WAITING;
    --q->n;
    pthread_mutex_unlock(&q->lock);
}
