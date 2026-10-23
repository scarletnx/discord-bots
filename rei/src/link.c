/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "link.h"
#include "cards.h"
#include "look.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GREY 0x6E7781

void lk_init(Link* l, Rg* gh) {
    memset(l, 0, sizeof *l);
    l->gh = gh;
}
void lk_free(Link* l) {
    int i;
    for (i = 0; i < l->nitems; ++i) cJSON_Delete(l->items[i].item);
    for (i = 0; i < l->nreleases; ++i) {
        free(l->releases[i].url);
        cJSON_Delete(l->releases[i].release);
    }
    for (i = 0; i < l->nreplies; ++i) cJSON_Delete(l->replies[i].embed);
    free(l->items);
    free(l->releases);
    free(l->replies);
    memset(l, 0, sizeof *l);
}
/* one more slot of an array that grows */
static void* slot(void* array, int* n, int* cap, size_t size) {
    char* a = array;
    if (*n == *cap) {
        int more = *cap ? 2 * *cap : 16;
        a = realloc(a, (size_t)more * size);
        if (!a) abort();
        *cap = more;
    }
    memset(a + (size_t)(*n)++ * size, 0, size);
    return a;
}

static int settled(const cJSON* item) {
    const char* s = rei_state(item);
    return !strcmp(s, "merged") || !strcmp(s, "closed") || !strcmp(s, "done") || !strcmp(s, "not planned");
}
const cJSON* lk_item(Link* l, int number) {
    double now = rei_now();
    LkItem* it = NULL;
    char* path;
    cJSON* found;
    int i;
    for (i = 0; i < l->nitems; ++i)
        if (l->items[i].number == number) it = &l->items[i];
    if (it && now - it->when < (it->item && settled(it->item) ? LK_SETTLED : LK_CACHE)) return it->item;
    path = rei_format("repos/%s/issues/%d", l->gh->repo, number);
    found = rg_get(l->gh, path, NULL);
    free(path);
    if (found && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(found, "pull_request"))) {
        /* whether it merged, and whether it is a draft, are the pull request's */
        cJSON* pull;
        path = rei_format("repos/%s/pulls/%d", l->gh->repo, number);
        pull = rg_get(l->gh, path, NULL);
        free(path);
        cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(found, "pull_request"), "merged_at");
        if (jstr(pull, "merged_at"))
            cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(found, "pull_request"), "merged_at",
                                    jstr(pull, "merged_at"));
        cJSON_DeleteItemFromObjectCaseSensitive(found, "draft");
        cJSON_AddBoolToObject(found, "draft", jbool(pull, "draft"));
        cJSON_Delete(pull);
    }
    if (!it) {
        l->items = slot(l->items, &l->nitems, &l->capitems, sizeof *l->items);
        it = &l->items[l->nitems - 1];
        it->number = number;
    }
    cJSON_Delete(it->item);
    it->item = found;
    it->when = now;
    return found;
}
const cJSON* lk_release(Link* l, const char* repo, const char* which) {
    char* path = rei_format("repos/%s/releases/%s", repo, which);
    double now = rei_now();
    LkRelease* r = NULL;
    int i;
    for (i = 0; i < l->nreleases; ++i)
        if (!strcmp(l->releases[i].url, path)) r = &l->releases[i];
    if (r && now - r->when < LK_CACHE) {
        free(path);
        return r->release;
    }
    if (!r) {
        l->releases = slot(l->releases, &l->nreleases, &l->capreleases, sizeof *l->releases);
        r = &l->releases[l->nreleases - 1];
        r->url = rei_strdup(path);
    }
    cJSON_Delete(r->release);
    r->release = rg_get(l->gh, path, NULL);
    r->when = now;
    free(path);
    return r->release;
}

/* the first n letters of s; owned */
static char* letters(const char* s, size_t n) { return rei_format("%.*s", (int)rei_bytes_of(s, n), s); }
/* one item as a reply says it */
static void line(Buf* b, const cJSON* item) {
    const char *title = jstr(item, "title"), *p;
    char* chips = rei_chips(item);
    buf_printf(b, "**%s [#%d](%s)**: ",
               cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(item, "pull_request")) ? "PR" : "Issue",
               (int)jnum(item, "number"), jstr(item, "html_url") ? jstr(item, "html_url") : "");
    for (p = title ? title : ""; *p; ++p) {
        if (*p == '[' || *p == ']') buf_add(b, "\\");
        buf_addn(b, p, 1);
    }
    buf_printf(b, "  (%s)", rei_state(item));
    if (*chips) buf_printf(b, "\n%s", chips);
    free(chips);
}
static cJSON* embed(const char* description, int color) {
    cJSON* e = cJSON_CreateObject();
    char* cut = letters(description, 4096);
    cJSON_AddStringToObject(e, "description", cut);
    cJSON_AddNumberToObject(e, "color", color);
    free(cut);
    return e;
}
cJSON* lk_reply(const cJSON* const* items, int n) {
    Buf b = {0};
    cJSON* e;
    int i;
    for (i = 0; i < n; ++i) {
        if (i) buf_add(&b, "\n");
        line(&b, items[i]);
    }
    e = embed(b.text ? b.text : "", n == 1 ? rei_state_color(rei_state(items[0])) : GREY);
    buf_free(&b);
    return e;
}
cJSON* lk_build(const cJSON* release, const char* said, const char* repo) {
    const char* name = jstr(release, "name");
    char *title, *mascot = cd_mascot(repo);
    Buf b = {0}, lines = {0};
    cJSON* e;
    if (said && *said) buf_add(&b, said);
    if (cd_downloads(&lines, release))
        buf_printf(&b, "%s%s", b.len ? "\n" : "", lines.text);
    else
        buf_printf(&b, "%sNo downloads on it yet.", b.len ? "\n" : "");
    e = embed(b.text, jbool(release, "prerelease") ? GREY : REI_OPEN);
    buf_free(&lines);
    buf_free(&b);
    title = letters(name && *name ? name : jstr(release, "tag_name") ? jstr(release, "tag_name") : "", 256);
    cJSON_AddStringToObject(e, "title", title);
    jset(e, "url", jstr(release, "html_url"));
    cJSON_AddStringToObject(cJSON_AddObjectToObject(e, "thumbnail"), "url", mascot);
    free(title);
    free(mascot);
    return e;
}
cJSON* lk_note(const char* text) { return embed(text, GREY); }

static cJSON* command(Link* l, int kind, int number) {
    const cJSON *found, *newest;
    if (kind == REI_NIGHTLY) {
        found = lk_release(l, l->gh->nightly, "latest");
        return found ? lk_build(found, NULL, l->gh->repo) : lk_note("No nightly is published yet.");
    }
    if (kind == REI_RELEASE_PR) {
        char *tag = rei_format("tags/pr-%d", number), *missing;
        cJSON* e;
        found = lk_release(l, l->gh->nightly, tag);
        free(tag);
        if (found) return lk_build(found, NULL, l->gh->repo);
        missing =
            rei_format("No builds for #%d: a pull request has them while it is open and its last push built.", number);
        e = lk_note(missing);
        free(missing);
        return e;
    }
    found = lk_release(l, l->gh->repo, "latest");
    if (found) return lk_build(found, NULL, l->gh->repo);
    newest = lk_release(l, l->gh->nightly, "latest");
    if (newest) return lk_build(newest, "No version is released yet. The newest nightly:", l->gh->repo);
    return lk_note("No version and no nightly are published yet.");
}
cJSON* lk_answer(Link* l, const char* text, int numbers[REI_MOST], int* n) {
    const cJSON* items[REI_MOST];
    int asked[REI_MOST], count, kind, number, i;
    *n = 0;
    kind = rei_command(text, &number);
    if (kind != REI_NO_COMMAND) return command(l, kind, number);
    count = rei_references(text, asked);
    for (i = 0; i < count; ++i) {
        const cJSON* item = lk_item(l, asked[i]);
        if (item) {
            items[*n] = item;
            numbers[(*n)++] = asked[i];
        }
    }
    return *n ? lk_reply(items, *n) : NULL;
}

void lk_keep(Link* l, u64 channel, u64 message, const int* numbers, int n, const cJSON* embed) {
    LkReply* r;
    if (l->nreplies == LK_TRACK) { /* the oldest makes room */
        cJSON_Delete(l->replies[0].embed);
        memmove(l->replies, l->replies + 1, (size_t)(--l->nreplies) * sizeof *l->replies);
    }
    l->replies = slot(l->replies, &l->nreplies, &l->capreplies, sizeof *l->replies);
    r = &l->replies[l->nreplies - 1];
    r->when = rei_now();
    r->channel = channel;
    r->message = message;
    r->n = n < REI_MOST ? n : REI_MOST;
    memcpy(r->numbers, numbers, (size_t)r->n * sizeof *numbers);
    r->embed = cJSON_Duplicate(embed, 1);
}
static void remove_reply(Link* l, int i) {
    cJSON_Delete(l->replies[i].embed);
    memmove(l->replies + i, l->replies + i + 1, (size_t)(l->nreplies - i - 1) * sizeof *l->replies);
    --l->nreplies;
}
void lk_drop(Link* l, u64 channel, u64 message) {
    int i;
    for (i = l->nreplies - 1; i >= 0; --i)
        if (l->replies[i].channel == channel && l->replies[i].message == message) remove_reply(l, i);
}
void lk_refresh(Link* l, LkEdit edit, void* ctx) {
    double now = rei_now();
    int i;
    for (i = l->nreplies - 1; i >= 0; --i)
        if (now - l->replies[i].when >= LK_AGE) remove_reply(l, i);
    for (i = 0; i < l->nreplies; ++i) {
        LkReply* r = &l->replies[i];
        const cJSON* items[REI_MOST];
        int n = 0, k, said;
        cJSON* now_said;
        for (k = 0; k < r->n; ++k) {
            const cJSON* item = lk_item(l, r->numbers[k]);
            if (item) items[n++] = item;
        }
        if (!n) continue;
        now_said = lk_reply(items, n);
        if (cJSON_Compare(now_said, r->embed, 1)) {
            cJSON_Delete(now_said);
            continue;
        }
        said = edit(ctx, r->channel, r->message, now_said);
        if (said == 404) {
            cJSON_Delete(now_said);
            remove_reply(l, i--);
            continue;
        }
        if (!said) {
            cJSON_Delete(r->embed);
            r->embed = now_said;
        } else
            cJSON_Delete(now_said); /* tried again at the next refresh */
    }
}

cJSON* lk_save(const Link* l) {
    cJSON *saved = cJSON_CreateObject(), *items = cJSON_AddObjectToObject(saved, "items"),
          *replies = cJSON_AddArrayToObject(saved, "replies");
    int i, k;
    for (i = 0; i < l->nitems; ++i) {
        char key[16];
        cJSON* kept;
        if (!l->items[i].item) continue;
        snprintf(key, sizeof key, "%d", l->items[i].number);
        kept = cJSON_AddArrayToObject(items, key);
        cJSON_AddItemToArray(kept, cJSON_CreateNumber(l->items[i].when));
        cJSON_AddItemToArray(kept, cJSON_Duplicate(l->items[i].item, 1));
    }
    for (i = 0; i < l->nreplies; ++i) {
        const LkReply* r = &l->replies[i];
        cJSON *entry = cJSON_CreateArray(), *numbers = cJSON_CreateArray();
        char id[24];
        cJSON_AddItemToArray(entry, cJSON_CreateNumber(r->when));
        snprintf(id, sizeof id, "%llu", (unsigned long long)r->channel);
        cJSON_AddItemToArray(entry, cJSON_CreateString(id));
        snprintf(id, sizeof id, "%llu", (unsigned long long)r->message);
        cJSON_AddItemToArray(entry, cJSON_CreateString(id));
        for (k = 0; k < r->n; ++k) cJSON_AddItemToArray(numbers, cJSON_CreateNumber(r->numbers[k]));
        cJSON_AddItemToArray(entry, numbers);
        cJSON_AddItemToArray(entry, cJSON_Duplicate(r->embed, 1));
        cJSON_AddItemToArray(replies, entry);
    }
    return saved;
}
void lk_load(Link* l, const cJSON* saved) {
    const cJSON *kept, *entry;
    cJSON_ArrayForEach(kept, cJSON_GetObjectItemCaseSensitive(saved, "items")) {
        const cJSON *when = cJSON_GetArrayItem(kept, 0), *item = cJSON_GetArrayItem(kept, 1);
        LkItem* it;
        if (!kept->string || !cJSON_IsNumber(when) || !cJSON_IsObject(item)) continue;
        l->items = slot(l->items, &l->nitems, &l->capitems, sizeof *l->items);
        it = &l->items[l->nitems - 1];
        it->number = atoi(kept->string);
        it->when = when->valuedouble;
        it->item = cJSON_Duplicate(item, 1);
    }
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(saved, "replies")) {
        const cJSON *when = cJSON_GetArrayItem(entry, 0), *channel = cJSON_GetArrayItem(entry, 1),
                    *message = cJSON_GetArrayItem(entry, 2), *numbers = cJSON_GetArrayItem(entry, 3),
                    *embed = cJSON_GetArrayItem(entry, 4), *number;
        int list[REI_MOST], n = 0;
        if (!cJSON_IsNumber(when) || !cJSON_IsString(channel) || !cJSON_IsString(message) || !cJSON_IsObject(embed))
            continue;
        cJSON_ArrayForEach(number, numbers) if (cJSON_IsNumber(number) && n < REI_MOST) list[n++] = number->valueint;
        lk_keep(l, strtoull(channel->valuestring, NULL, 10), strtoull(message->valuestring, NULL, 10), list, n, embed);
        l->replies[l->nreplies - 1].when = when->valuedouble;
    }
}
