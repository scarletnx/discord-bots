/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The replies to messages: the issues and pull requests a message names (#N), each linked with its title, its state and
   its labels in GitHub's colours; !nightly, !release and !release #N, the builds. GitHub's answers are remembered, an
   open one LK_CACHE seconds and a closed or merged one LK_SETTLED, and the replies of the last LK_AGE seconds (their
   channel, message and numbers, LK_TRACK of them at most) are edited when what they link changes */
#ifndef REI_LINK_H
#define REI_LINK_H
#include "github.h"
#include "refs.h"

#define LK_CACHE   600
#define LK_SETTLED 86400
#define LK_AGE     (7 * 86400)
#define LK_TRACK   200
#define LK_REFRESH 600 /* seconds between two looks at what the kept replies link */

typedef struct {
    int number;
    double when;
    cJSON* item; /* NULL: GitHub has none */
} LkItem;
typedef struct {
    char* url;
    double when;
    cJSON* release;
} LkRelease;
typedef struct {
    double when;
    u64 channel, message;
    int numbers[REI_MOST], n;
    cJSON* embed; /* as last sent */
} LkReply;
typedef struct {
    Rg* gh;
    LkItem* items;
    int nitems, capitems;
    LkRelease* releases;
    int nreleases, capreleases;
    LkReply* replies;
    int nreplies, capreplies;
} Link;

void lk_init(Link* l, Rg* gh);
void lk_free(Link* l);
/* issue or pull request number as GitHub has it (a pull request's merged_at and draft in it), or NULL; kept by l */
const cJSON* lk_item(Link* l, int number);
/* release which ("latest", "tags/<tag>") of repo, or NULL; kept by l */
const cJSON* lk_release(Link* l, const char* repo, const char* which);

/* the embeds a reply carries, as Discord takes them; owned */
cJSON* lk_reply(const cJSON* const* items, int n);
/* a release's downloads as the cards show them, said over them, the mascot of repo beside them */
cJSON* lk_build(const cJSON* release, const char* said, const char* repo);
cJSON* lk_note(const char* text);
/* the reply to a message's text: a command's embed, else the one of the items it names (numbers[] and *n theirs,
   for lk_keep); NULL when it asks for nothing GitHub has. Owned */
cJSON* lk_answer(Link* l, const char* text, int numbers[REI_MOST], int* n);

void lk_keep(Link* l, u64 channel, u64 message, const int* numbers, int n, const cJSON* embed);
void lk_drop(Link* l, u64 channel, u64 message);
/* an edit of a sent reply: 0 done, 404 when it or its channel is gone (then dropped), anything else a failure */
typedef int (*LkEdit)(void* ctx, u64 channel, u64 message, const cJSON* embed);
/* each kept reply whose items read otherwise now, edited; those older than LK_AGE dropped */
void lk_refresh(Link* l, LkEdit edit, void* ctx);

/* what lasts across a restart: {"items": {"N": [when, item]}, "replies": [[when, "channel", "message", [N...], embed]]}
   (ids as strings: a JSON number would round them) */
cJSON* lk_save(const Link* l);
void lk_load(Link* l, const cJSON* saved);
#endif
