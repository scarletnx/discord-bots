/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Discord's REST API for the messages the bot sends, edits and deletes, through a fetch it is given (http.c's, with
   "Bot <token>", or the tests'). A 429 waits what Discord says and asks again, DC_TRIES in all; its messages ping no
   one. The gateway (the messages it reads) is Concord's, in main.c */
#ifndef REI_REST_H
#define REI_REST_H
#include "github.h"

#define DC_API   "https://discord.com/api/v10/"
#define DC_TRIES 5

typedef struct {
    RgFetch fetch;
    void* ctx;
    void (*sleep)(double seconds); /* the wait a 429 asks for */
    u64 self;                      /* the bot's own user id, once dc_self found it */
} Dc;

void dc_init(Dc* d, RgFetch fetch, void* ctx);
/* the bot's user id, asked once; 0 when Discord did not say */
u64 dc_self(Dc* d);
/* the application's emojis (its bot's id is the application's), Discord's list of them; owned, or NULL, said */
cJSON* dc_emojis(Dc* d);
/* embed posted in channel, a reply to message reply_to when not 0: the new message's id, or 0 (said in the log) */
u64 dc_post(Dc* d, u64 channel, const cJSON* embed, u64 reply_to);
/* 0 done, 404 when the message or its channel is gone, else Discord's status (-1 for no answer), said in the log */
int dc_edit(Dc* d, u64 channel, u64 message, const cJSON* embed);
int dc_remove(Dc* d, u64 channel, u64 message);
/* up to max (100 at most) messages of channel older than before (0: from the newest), newest first: their ids, whether a
   webhook sent each, whether this bot did. Their count, or -1 */
int dc_history(Dc* d, u64 channel, u64 before, u64* ids, int* webhook, int* mine, int max);
#endif
