/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the bot reads of the gateway's events, from their JSON as Discord sent it (the payload's "d"), and the messages
   waiting for an answer. Concord decodes none of them: its decoder of MESSAGE_CREATE crashed the bot on messages of 2
   to 3 KB, and leaves strings escaped */
#ifndef REI_EVENT_H
#define REI_EVENT_H
#include <pthread.h>

#include "rei.h"

#define EV_WAITING 64 /* the messages that wait for an answer, at most */

typedef struct {
    u64 id, channel;
    char* content; /* owned; "" for a message of files alone */
} EvMessage;

/* MESSAGE_CREATE's message, *m filled and 1 when a person wrote it; 0, nothing owned, for a bot's or a webhook's (a
   card), and for one that is no JSON object with an author, an id and a channel id */
int ev_message(const char* json, size_t len, EvMessage* m);
void ev_message_free(EvMessage* m);
/* READY's user name, owned, or NULL */
char* ev_ready_user(const char* json, size_t len);

/* the messages taken off the gateway for an answer, oldest first, for one thread to push and another to pop */
typedef struct {
    EvMessage m[EV_WAITING];
    int first, n;
    pthread_mutex_t lock;
    pthread_cond_t more;
} EvQueue;
void ev_queue_init(EvQueue* q);
/* 1 and *m given to q (emptied); 0 when EV_WAITING wait already, *m as it was */
int ev_push(EvQueue* q, EvMessage* m);
/* the oldest message, waited for while there is none; owned */
void ev_pop(EvQueue* q, EvMessage* m);
#endif
