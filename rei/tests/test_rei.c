/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ReiWimsy without Discord or the network: what a message names, how GitHub's answers read, what is remembered, the
   REST calls' waits and answers, and the cards: drawn, posted, edited, deleted a day after they close */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cards.h"
#include "event.h"
#include "link.h"
#include "look.h"
#include "refs.h"
#include "rest.h"
#include "state.h"

static int failures, checks;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                 \
        }                                                                                                              \
    } while (0)
#define SAME(a, b)                                                                                                     \
    do {                                                                                                               \
        const char *x_ = (a), *y_ = (b);                                                                               \
        ++checks;                                                                                                      \
        if (!x_ || !y_ || strcmp(x_, y_)) {                                                                            \
            ++failures;                                                                                                \
            fprintf(stderr, "%s:%d: %s\n  is: %s\n  not: %s\n", __FILE__, __LINE__, #a, x_ ? x_ : "(null)",            \
                    y_ ? y_ : "(null)");                                                                               \
        }                                                                                                              \
    } while (0)
#define HAS(text, part) CHECK((text) && strstr((text), (part)))

/* ---- the clock ---- */
static double now_is;
static double clock_now(void) { return now_is; }

/* ---- a fake network: each answer by method and URL, every request kept ---- */
typedef struct {
    char *method, *url, *body, *etag;
    int status;
} Fixture;
static Fixture fixtures[256];
static int nfixtures;
static char* asked[4096];
static int nasked;
static char* posted_body; /* the last POST's or PATCH's body */
static void answer(const char* method, const char* url, int status, const char* body) {
    int i;
    for (i = 0; i < nfixtures; ++i)
        if (!strcmp(fixtures[i].method, method) && !strcmp(fixtures[i].url, url)) break;
    if (i == nfixtures) {
        ++nfixtures;
        fixtures[i].method = rei_strdup(method);
        fixtures[i].url = rei_strdup(url);
    } else {
        free(fixtures[i].body);
        free(fixtures[i].etag);
    }
    fixtures[i].status = status;
    fixtures[i].body = rei_strdup(body);
    fixtures[i].etag = NULL;
}
static void gh(const char* path, const char* body) {
    char* url = rei_format("https://api.github.com/%s", path);
    answer("GET", url, 200, body);
    free(url);
}
static void forget(void) {
    int i;
    for (i = 0; i < nfixtures; ++i) {
        free(fixtures[i].method);
        free(fixtures[i].url);
        free(fixtures[i].body);
        free(fixtures[i].etag);
    }
    for (i = 0; i < nasked; ++i) free(asked[i]);
    nfixtures = nasked = 0;
}
static int times_asked(const char* method, const char* url) {
    int i, n = 0;
    char* want = rei_format("%s %s", method, url);
    for (i = 0; i < nasked; ++i) n += !strcmp(asked[i], want);
    free(want);
    return n;
}
static void fake(void* ctx, const char* method, const char* url, const char* body, const char* etag, RgAnswer* out) {
    int i;
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (nasked < 4096) asked[nasked++] = rei_format("%s %s", method, url);
    if (body) {
        free(posted_body);
        posted_body = rei_strdup(body);
    }
    for (i = 0; i < nfixtures; ++i)
        if (!strcmp(fixtures[i].method, method) && !strcmp(fixtures[i].url, url)) {
            if (fixtures[i].etag && etag && !strcmp(etag, fixtures[i].etag)) {
                out->status = 304;
                return;
            }
            out->status = fixtures[i].status;
            out->body = rei_strdup(fixtures[i].body);
            if (fixtures[i].etag) out->etag = rei_strdup(fixtures[i].etag);
            return;
        }
    out->status = 404;
    out->body = rei_strdup("{\"message\":\"Not Found\"}");
}

/* ---- refs ---- */
static void refs(void) {
    int n[REI_MOST], c, number;
    c = rei_references("see #160, issue #12, PR #305, pull request #7 and pull #8", n);
    CHECK(c == 5 && n[0] == 160 && n[1] == 12 && n[2] == 305 && n[3] == 7 && n[4] == 8);
    c = rei_references("#1 #1 #2 #3 #4 #5 #6", n);
    CHECK(c == 5 && n[0] == 1 && n[4] == 5);
    c = rei_references("run `make #12`, ```\n#13\n``` see https://x/issues/14#15 a#16 &#17 #0 color #fff", n);
    CHECK(c == 0);
    c = rei_references("Issue #9 and pr #10", n);
    CHECK(c == 2 && n[0] == 9 && n[1] == 10);
    CHECK(rei_references("issue 12, PR 305, pull request 7, pull 8 and pr 2 broke", n) == 0);
    CHECK(rei_references("#1234567 #123456 <https://x/#5> #7.", n) == 2 && n[0] == 123456 && n[1] == 7);
    CHECK(rei_references("\xc3\xa9#4 #4", n) == 1 && n[0] == 4); /* after a letter of UTF-8: in a word */
    CHECK(rei_command("!nightly", &number) == REI_NIGHTLY);
    CHECK(rei_command("  !Release ", &number) == REI_RELEASE);
    CHECK(rei_command("!release #355", &number) == REI_RELEASE_PR && number == 355);
    CHECK(rei_command("try !nightly later", &number) == REI_NO_COMMAND);
    CHECK(rei_command("!nightlies", &number) == REI_NO_COMMAND);
    CHECK(rei_command("!release#355", &number) == REI_NO_COMMAND);
    CHECK(rei_command("!release #1234567", &number) == REI_NO_COMMAND);
}

/* ---- look ---- */
#define ISSUE                                                                                                          \
    "{\"number\":160,\"title\":\"todo: aarch64 [builds]\",\"html_url\":\"https://x/issues/160\",\"state\":\"closed\"," \
    "\"state_reason\":\"completed\",\"labels\":[{\"name\":\"priority: low\",\"color\":\"0e8a16\"}]}"
#define PULL                                                                                                           \
    "{\"number\":305,\"title\":\"Players\",\"html_url\":\"https://x/pull/305\",\"state\":\"closed\",\"labels\":[],"    \
    "\"pull_request\":{\"url\":\"x\"}}"
static void look(void) {
    static const char* const colors[] = {"b60205", "d93f0b", "fbca04", "0075ca", "d4c5f9", "ededed", "5c3317"};
    static const char* const squares[] = {"\xF0\x9F\x9F\xA5", "\xF0\x9F\x9F\xA7", "\xF0\x9F\x9F\xA8",
                                          "\xF0\x9F\x9F\xA6", "\xF0\x9F\x9F\xAA", "\xE2\xAC\x9C",
                                          "\xF0\x9F\x9F\xAB"};
    cJSON *pull = cJSON_Parse(PULL), *issue = cJSON_Parse(ISSUE);
    char* s;
    int i;
    for (i = 0; i < 7; ++i) SAME(rei_square(colors[i]), squares[i]);
    cJSON_ReplaceItemInObject(cJSON_GetObjectItem(pull, "pull_request"), "url", cJSON_CreateString("x"));
    cJSON_AddStringToObject(cJSON_GetObjectItem(pull, "pull_request"), "merged_at", "2026");
    SAME(rei_state(pull), "merged");
    cJSON_DeleteItemFromObject(cJSON_GetObjectItem(pull, "pull_request"), "merged_at");
    cJSON_AddNullToObject(cJSON_GetObjectItem(pull, "pull_request"), "merged_at");
    SAME(rei_state(pull), "closed");
    cJSON_ReplaceItemInObject(pull, "state", cJSON_CreateString("open"));
    cJSON_AddTrueToObject(pull, "draft");
    SAME(rei_state(pull), "draft");
    cJSON_ReplaceItemInObject(issue, "state_reason", cJSON_CreateString("not_planned"));
    SAME(rei_state(issue), "not planned");
    s = rei_escape("a*b_[c]`|~<x>\\");
    SAME(s, "a\\*b\\_\\[c\\]\\`\\|\\~\\<x\\>\\\\");
    free(s);
    s = rei_excerpt("hi <!-- hidden --> there\r\n\n\n\n![pic](u) <img src=x> <DETAILS>x</details>", 300);
    SAME(s, "> hi  there\n>\n> [image] [image] [details]");
    free(s);
    s = rei_excerpt("one two three", 7);
    SAME(s, "> one...");
    free(s);
    s = rei_excerpt("```c\ncode that goes on", 12);
    SAME(s, "> ```c\n> code...\n> ```");
    free(s);
    s = rei_excerpt("\xc3\xa1\xc3\xa1\xc3\xa1 b", 3); /* letters, not bytes */
    SAME(s, "> \xc3\xa1\xc3\xa1\xc3\xa1...");
    free(s);
    s = rei_excerpt("", 300);
    SAME(s, "");
    free(s);
    cJSON_Delete(pull);
    cJSON_Delete(issue);
}

/* ---- link ---- */
static Rg g;
static Link l;
static void link_start(void) {
    forget();
    now_is = 0;
    rg_init(&g, "o/r", NULL, fake, NULL);
    lk_init(&l, &g);
}
static void link_end(void) {
    lk_free(&l);
    rg_free(&g);
}
static int edits;
static char* edited;
static int gone_too;
static int fake_edit(void* ctx, u64 channel, u64 message, const cJSON* embed) {
    (void)ctx;
    CHECK(channel == 7 && message == 70);
    ++edits;
    free(edited);
    edited = rei_strdup(jstr(embed, "description") ? jstr(embed, "description") : "");
    return gone_too ? 404 : 0;
}
static void link_tests(void) {
    const cJSON* items[2];
    cJSON *issue = cJSON_Parse(ISSUE), *pull = cJSON_Parse(PULL), *e, *saved;
    int numbers[REI_MOST], n;
    char* text;
    SAME(
        cJSON_GetStringValue(cJSON_GetObjectItem(e = lk_reply((items[0] = issue, items), 1), "description")),
        "**Issue [#160](https://x/issues/160)**: todo: aarch64 \\[builds\\]  (done)\n\xF0\x9F\x9F\xA9 `priority: low`");
    CHECK(jnum(e, "color") == 0x8250DF);
    cJSON_Delete(e);
    items[1] = pull;
    e = lk_reply(items, 2);
    CHECK(jnum(e, "color") == 0x6E7781);
    cJSON_Delete(e);

    /* a pull request is asked whether it merged; an open one asked again after LK_CACHE, a closed one after
       LK_SETTLED */
    link_start();
    gh("repos/o/r/issues/305", PULL);
    gh("repos/o/r/pulls/305", "{\"merged_at\":\"2026-10-17\",\"draft\":false}");
    SAME(rei_state(lk_item(&l, 305)), "merged");
    answer("GET", "https://api.github.com/repos/o/r/issues/160", 200, ISSUE);
    lk_item(&l, 160);
    now_is += LK_CACHE + 1;
    lk_item(&l, 160);
    CHECK(times_asked("GET", "https://api.github.com/repos/o/r/issues/160") == 1);
    now_is += LK_SETTLED;
    lk_item(&l, 160);
    CHECK(times_asked("GET", "https://api.github.com/repos/o/r/issues/160") == 2);
    link_end();

    link_start();
    text = rei_strdup(ISSUE);
    memcpy(strstr(text, "\"closed\""), "\"open\"  ", 8);
    gh("repos/o/r/issues/160", text);
    lk_item(&l, 160);
    lk_item(&l, 160);
    now_is += LK_CACHE + 1;
    lk_item(&l, 160);
    CHECK(times_asked("GET", "https://api.github.com/repos/o/r/issues/160") == 2);
    /* a reply kept and edited when its item changes; GitHub's unknown numbers left out */
    e = lk_answer(&l, "#160 and #999", numbers, &n);
    CHECK(e && n == 1 && numbers[0] == 160);
    HAS(jstr(e, "description"), "#160");
    CHECK(!strstr(jstr(e, "description"), "999"));
    lk_keep(&l, 7, 70, numbers, n, e);
    cJSON_Delete(e);
    edits = 0;
    lk_refresh(&l, fake_edit, NULL);
    CHECK(edits == 0); /* nothing changed */
    gh("repos/o/r/issues/160", ISSUE);
    now_is += LK_CACHE;
    lk_refresh(&l, fake_edit, NULL);
    CHECK(edits == 1);
    HAS(edited, "(done)");
    CHECK(!lk_answer(&l, "#999 only", numbers, &n) && !lk_answer(&l, "nothing here", numbers, &n));
    /* the state: what GitHub said and the replies, ids whole */
    lk_keep(&l, 1234567890123456789ULL, 9876543210987654321ULL, numbers, 1, issue);
    saved = lk_save(&l);
    {
        char* written = cJSON_PrintUnformatted(saved);
        Rg g2;
        Link l2;
        rg_init(&g2, "o/r", NULL, fake, NULL);
        lk_init(&l2, &g2);
        cJSON* parsed = cJSON_Parse(written);
        lk_load(&l2, parsed);
        cJSON_Delete(parsed);
        CHECK(l2.nreplies == 2 && l2.replies[1].channel == 1234567890123456789ULL &&
              l2.replies[1].message == 9876543210987654321ULL);
        {
            int before = nasked;
            CHECK(lk_item(&l2, 160) && nasked == before); /* from the file, not asked */
        }
        lk_free(&l2);
        rg_free(&g2);
        free(written);
    }
    cJSON_Delete(saved);
    /* too old to edit: dropped */
    now_is += LK_AGE;
    lk_refresh(&l, fake_edit, NULL);
    CHECK(l.nreplies == 0);
    free(text);
    link_end();

    /* a reply whose message is gone (404) is dropped */
    link_start();
    gh("repos/o/r/issues/160", ISSUE);
    numbers[0] = 160;
    e = lk_reply((items[0] = issue, items), 1);
    cJSON_ReplaceItemInObject(e, "description", cJSON_CreateString("old"));
    lk_keep(&l, 7, 70, numbers, 1, e);
    cJSON_Delete(e);
    gone_too = 1;
    lk_refresh(&l, fake_edit, NULL);
    gone_too = 0;
    CHECK(l.nreplies == 0);
    /* at most LK_TRACK replies */
    for (n = 0; n < LK_TRACK + 3; ++n) lk_keep(&l, 1, (u64)n, numbers, 1, issue);
    CHECK(l.nreplies == LK_TRACK && l.replies[0].message == 3);
    link_end();
    cJSON_Delete(issue);
    cJSON_Delete(pull);
}

#define NIGHTLY                                                                                                        \
    "{\"name\":\"Nightly 2026-10-17 (44aa300)\",\"tag_name\":\"nightly-2026-10-17-44aa300\","                          \
    "\"html_url\":\"https://n/nightly-2026-10-17-44aa300\",\"prerelease\":false,\"assets\":["                          \
    "{\"name\":\"ScarletNX-windows-x86_64.zip\",\"size\":83886080,\"browser_download_url\":\"https://n/win\"},"        \
    "{\"name\":\"ScarletNX-linux-x86_64.AppImage\",\"size\":52428800,\"browser_download_url\":\"https://n/lin\"},"     \
    "{\"name\":\"ScarletNX-linux-x86_64.AppImage.zsync\",\"size\":156672,\"browser_download_url\":\"https://n/zs\"}]}"
static void commands(void) {
    int numbers[REI_MOST], n;
    cJSON* e;
    link_start();
    gh("repos/o/nightly/releases/latest", NIGHTLY);
    gh("repos/o/nightly/releases/tags/pr-355",
       "{\"name\":\"PR #355: Android menu files\",\"tag_name\":\"pr-355\",\"html_url\":\"https://n/pr-355\","
       "\"prerelease\":true,\"assets\":[]}");
    e = lk_answer(&l, "!nightly", numbers, &n);
    CHECK(n == 0);
    SAME(jstr(e, "title"), "Nightly 2026-10-17 (44aa300)");
    SAME(jstr(e, "url"), "https://n/nightly-2026-10-17-44aa300");
    /* the cards' look: a field a package, in their order, each its kind linked and its size; a file the cards do not
       list (the AppImage's .zsync) left out; the mascot beside them */
    {
        const cJSON *fields = cJSON_GetObjectItemCaseSensitive(e, "fields"), *f0 = cJSON_GetArrayItem(fields, 0),
                    *f1 = cJSON_GetArrayItem(fields, 1);
        char* printed = cJSON_PrintUnformatted(e);
        CHECK(cJSON_GetArraySize(fields) == 2 && !jstr(e, "description"));
        SAME(jstr(f0, "name"), "Linux x86-64");
        SAME(jstr(f0, "value"), "[AppImage](https://n/lin)\n52.4 MB");
        SAME(jstr(f1, "name"), "Windows x86-64");
        SAME(jstr(f1, "value"), "[zip](https://n/win)\n83.9 MB");
        CHECK(printed && !strstr(printed, "zsync") && !strstr(printed, "https://n/zs"));
        SAME(jstr(cJSON_GetObjectItemCaseSensitive(e, "thumbnail"), "url"),
             "https://raw.githubusercontent.com/o/r/main/dist/icons/hicolor/128x128/apps/wimsy.png");
        free(printed);
    }
    cJSON_Delete(e);
    e = lk_answer(&l, "!nightly", numbers, &n);
    CHECK(times_asked("GET", "https://api.github.com/repos/o/nightly/releases/latest") == 1);
    cJSON_Delete(e);
    e = lk_answer(&l, "!release", numbers, &n);
    CHECK(e && !strncmp(jstr(e, "description"), "No version is released yet. The newest nightly:", 47));
    SAME(jstr(e, "url"), "https://n/nightly-2026-10-17-44aa300");
    cJSON_Delete(e);
    e = lk_answer(&l, "!release #355", numbers, &n);
    SAME(jstr(e, "url"), "https://n/pr-355");
    CHECK(jnum(e, "color") == 0x6E7781);
    HAS(jstr(e, "description"), "No downloads on it yet.");
    cJSON_Delete(e);
    e = lk_answer(&l, "!release #354", numbers, &n);
    CHECK(e && !strncmp(jstr(e, "description"), "No builds for #354", 18));
    cJSON_Delete(e);
    CHECK(!lk_answer(&l, "try !nightly later", numbers, &n) && !lk_answer(&l, "!nightlies", numbers, &n));
    link_end();
    link_start();
    gh("repos/o/r/releases/latest",
       "{\"name\":\"ScarletNX 0.1.0\",\"tag_name\":\"v0.1.0\",\"html_url\":\"https://x/v0.1.0\",\"assets\":[]}");
    e = lk_answer(&l, "  !Release ", numbers, &n);
    SAME(jstr(e, "url"), "https://x/v0.1.0");
    CHECK(jnum(e, "color") == REI_OPEN);
    cJSON_Delete(e);
    link_end();
    rg_init(&g, "o/r", "p/builds", fake, NULL);
    SAME(g.nightly, "p/builds");
    rg_free(&g);
}

/* ---- GitHub's ETags and pages ---- */
static void github_tests(void) {
    cJSON* got;
    int status, i;
    link_start();
    gh("repos/o/r/thing", "{\"n\":1}");
    fixtures[nfixtures - 1].etag = rei_strdup("\"e1\"");
    got = rg_get(&g, "repos/o/r/thing", &status);
    CHECK(status == 200 && jnum(got, "n") == 1);
    cJSON_Delete(got);
    got = rg_get(&g, "repos/o/r/thing", &status); /* asked with its ETag: 304, the kept answer */
    CHECK(status == 304 && jnum(got, "n") == 1 && g.unchanged == 1);
    cJSON_Delete(got);
    got = rg_get(&g, "repos/o/r/none", &status);
    CHECK(!got && status == 404);
    {
        Buf page = {0};
        buf_add(&page, "[");
        for (i = 0; i < 100; ++i) buf_printf(&page, "%s{\"i\":%d}", i ? "," : "", i);
        buf_add(&page, "]");
        gh("repos/o/r/list?per_page=100&page=1", page.text);
        gh("repos/o/r/list?per_page=100&page=2", "[{\"i\":100}]");
        buf_free(&page);
    }
    got = rg_pages(&g, "repos/o/r/list", NULL);
    CHECK(cJSON_GetArraySize(got) == 101 && jnum(cJSON_GetArrayItem(got, 100), "i") == 100);
    cJSON_Delete(got);
    gh("repos/o/r/runs?x=1&per_page=100&page=1", "{\"workflow_runs\":[{\"id\":5}]}");
    got = rg_pages(&g, "repos/o/r/runs?x=1", "workflow_runs");
    CHECK(cJSON_GetArraySize(got) == 1);
    cJSON_Delete(got);
    CHECK(!rg_pages(&g, "repos/o/r/missing", NULL));
    link_end();
}

/* ---- Discord's REST ---- */
static double slept;
static void no_sleep(double s) { slept += s; }
static void rest_tests(void) {
    Dc d;
    u64 ids[4];
    int webhook[4], mine[4];
    cJSON* e = cJSON_Parse("{\"description\":\"x\"}");
    forget();
    dc_init(&d, fake, NULL);
    d.sleep = no_sleep;
    answer("POST", DC_API "channels/7/messages", 200, "{\"id\":\"1234567890123456789\"}");
    CHECK(dc_post(&d, 7, e, 99) == 1234567890123456789ULL);
    HAS(posted_body, "\"message_reference\":{\"message_id\":\"99\"");
    HAS(posted_body, "\"allowed_mentions\":{\"parse\":[],\"replied_user\":false}");
    answer("PATCH", DC_API "channels/7/messages/70", 404, "{\"code\":10008}");
    CHECK(dc_edit(&d, 7, 70, e) == 404);
    answer("PATCH", DC_API "channels/7/messages/71", 429, "{\"retry_after\":0.25}");
    slept = 0;
    CHECK(dc_edit(&d, 7, 71, e) == 429 && times_asked("PATCH", DC_API "channels/7/messages/71") == DC_TRIES);
    CHECK(slept > 0.99 && slept < 1.01); /* four waits of 0.25 s */
    answer("DELETE", DC_API "channels/7/messages/72", 204, "");
    CHECK(dc_remove(&d, 7, 72) == 0);
    answer("DELETE", DC_API "channels/7/messages/73", 403, "{\"code\":50013}");
    CHECK(dc_remove(&d, 7, 73) == 403);
    answer("GET", DC_API "users/@me", 200, "{\"id\":\"42\"}");
    answer("GET", DC_API "channels/7/messages?limit=4", 200,
           "[{\"id\":\"9\",\"webhook_id\":\"1\",\"author\":{\"id\":\"5\"}},{\"id\":\"8\",\"author\":{\"id\":\"42\"}},"
           "{\"id\":\"7\",\"author\":{\"id\":\"3\"}}]");
    CHECK(dc_history(&d, 7, 0, ids, webhook, mine, 4) == 3);
    CHECK(ids[0] == 9 && webhook[0] && !mine[0] && ids[1] == 8 && !webhook[1] && mine[1] && !webhook[2] && !mine[2]);
    CHECK(dc_history(&d, 8, 0, ids, webhook, mine, 4) == -1);
    cJSON_Delete(e);
    forget();
}

/* ---- the cards ---- */
typedef struct {
    u64 channel, message;
    char* description;
    int removed;
} Sent;
static Sent sent[256];
static int nsent, cd_edits, cd_removes, edit_gone, remove_fails;
static u64 next_id = 1000;
static u64 c_post(void* ctx, u64 channel, const cJSON* embed) {
    (void)ctx;
    sent[nsent].channel = channel;
    sent[nsent].message = ++next_id;
    sent[nsent].description = cJSON_PrintUnformatted(embed);
    return sent[nsent++].message;
}
static Sent* sent_of(u64 message) {
    int i;
    for (i = 0; i < nsent; ++i)
        if (sent[i].message == message) return &sent[i];
    return NULL;
}
static int c_edit(void* ctx, u64 channel, u64 message, const cJSON* embed) {
    Sent* s = sent_of(message);
    (void)ctx;
    (void)channel;
    ++cd_edits;
    if (edit_gone || !s || s->removed) return 404;
    free(s->description);
    s->description = cJSON_PrintUnformatted(embed);
    return 0;
}
static int c_remove(void* ctx, u64 channel, u64 message) {
    Sent* s = sent_of(message);
    (void)ctx;
    (void)channel;
    ++cd_removes;
    if (remove_fails) return remove_fails; /* Discord's status */
    if (s) s->removed = 1;
    return 0;
}
static int history_calls;
static int c_history(void* ctx, u64 channel, u64 before, u64* ids, int* webhook, int* mine, int max) {
    (void)ctx;
    (void)max;
    ++history_calls;
    if (channel != 1 || before) return 0;
    ids[0] = 51; /* an old webhook card */
    webhook[0] = 1;
    mine[0] = 0;
    ids[1] = 52; /* someone talking */
    webhook[1] = 0;
    mine[1] = 0;
    return 2;
}
static const char* issue_label = "kind: bug";
static void cards_fixtures(const char* issue_state, const char* closed_at) {
    char* issue = rei_format(
        "{\"number\":12,\"title\":\"Crash on start\",\"html_url\":\"https://github.com/o/r/issues/12\",\"state\":\"%s\","
        "\"state_reason\":%s,\"closed_at\":%s,\"comments\":1,\"body\":\"It crashes <!-- x -->here.\","
        "\"labels\":[{\"name\":\"%s\",\"color\":\"d73a4a\"}],\"assignees\":[],\"user\":{\"login\":\"ana\","
        "\"html_url\":\"https://github.com/ana\"},\"type\":{\"name\":\"Bug\"}}",
        issue_state, strcmp(issue_state, "open") ? "\"completed\"" : "null", closed_at, issue_label);
    char* listing = rei_format("[%s]", issue);
    gh("repos/o/r/issues/12", issue);
    gh("repos/o/r/issues/12/comments?per_page=100&page=1",
       "[{\"user\":{\"login\":\"bot\",\"type\":\"Bot\"},\"body\":\"beep\",\"created_at\":\"2026-10-20T10:00:00Z\"},"
       "{\"user\":{\"login\":\"bia\",\"type\":\"User\"},\"body\":\"Same **here**\",\"html_url\":\"https://c/1\","
       "\"created_at\":\"2026-10-20T09:00:00Z\"}]");
    answer("POST", "https://api.github.com/graphql", 200,
           "{\"data\":{\"repository\":{\"issue\":{\"closedByPullRequestsReferences\":{\"nodes\":[{\"number\":13,"
           "\"url\":\"https://github.com/o/r/pull/13\"}]}}}}}");
    gh("repos/o/r/issues?state=open&per_page=100&page=1", strcmp(issue_state, "open") ? "[]" : listing);
    free(issue);
    free(listing);
    gh("repos/o/nightly/releases?per_page=100&page=1", "[]");
    gh("repos/o/r/actions/workflows/build.yml/runs?per_page=20", "{\"workflow_runs\":[]}");
    gh("repos/o/r/commits/main", "{\"sha\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}");
}
#define COMMENTS_DAN                                                                                                   \
    "[{\"user\":{\"login\":\"bia\",\"type\":\"User\"},\"body\":\"Same\",\"html_url\":\"https://c/1\","                 \
    "\"created_at\":\"2026-10-20T09:00:00Z\"},{\"user\":{\"login\":\"dan\",\"type\":\"User\"},\"body\":\"Mine too\","  \
    "\"html_url\":\"https://c/5\",\"created_at\":\"2026-10-20T11:00:00Z\"}]"
/* the issue's comments: one by who, at when */
static void comment_by(const char* who, const char* when) {
    char* list = rei_format("[{\"user\":{\"login\":\"%s\",\"type\":\"User\"},\"body\":\"hi\",\"html_url\":"
                            "\"https://c/%s\",\"created_at\":\"%s\"}]",
                            who, who, when);
    gh("repos/o/r/issues/12/comments?per_page=100&page=1", list);
    free(list);
}
/* a round 30 s on, GitHub listing these issues as updated since the last */
static void updated(Cards* c, const char* listed) {
    char* listing =
        rei_format("repos/o/r/issues?state=all&sort=updated&direction=desc&since=%s&per_page=100&page=1", c->since);
    gh(listing, listed);
    free(listing);
    now_is += 30;
    cd_round(c);
}
static void cards_tests(void) {
    Cards c;
    CdDiscord dc = {NULL, c_post, c_edit, c_remove, c_history};
    char* listing;
    forget();
    now_is = 1760954400; /* 2025-10-20T10:00:00Z */
    rg_init(&g, "o/r", NULL, fake, NULL);
    cd_init(&c, &g, dc, 0, 1, 3);
    cards_fixtures("open", "null");
    /* the first round: the webhook's old card deleted, talk left; the open issue's card posted, nothing asked of
       the closed ones */
    cd_round(&c);
    CHECK(history_calls == 1 && cd_removes == 1 && sent_of(51) == NULL);
    CHECK(!times_asked("GET", "https://api.github.com/repos/o/r/issues?state=closed&per_page=100&page=1"));
    CHECK(nsent == 1 && sent[0].channel == 1);
    HAS(sent[0].description, "\"title\":\"#12: Crash on start\"");
    HAS(sent[0].description,
        "**Open**\\n**Bug** | \xF0\x9F\x9F\xA5 `kind: bug` | fixed by [#13](https://github.com/o/r/pull/13)");
    HAS(sent[0].description, "> It crashes here.");
    HAS(sent[0].description, "**Latest comment**, by **bia** ([on GitHub](https://c/1))\\n> Same **here**");
    HAS(sent[0].description, "\"footer\":{\"text\":\"1 comment, edited at each change and comment\"}");
    HAS(sent[0].description, "\"author\":{\"name\":\"ana\"");
    CHECK(!strstr(sent[0].description, "beep"));
    /* a second round with nothing new: no edit, no post */
    listing =
        rei_format("repos/o/r/issues?state=all&sort=updated&direction=desc&since=%s&per_page=100&page=1", c.since);
    gh(listing, "[]");
    free(listing);
    now_is += 30;
    cd_round(&c);
    CHECK(nsent == 1 && cd_edits == 0 && history_calls == 1);
    /* listed as updated, drawn the same: no edit either */
    listing =
        rei_format("repos/o/r/issues?state=all&sort=updated&direction=desc&since=%s&per_page=100&page=1", c.since);
    gh(listing, "[{\"number\":12}]");
    free(listing);
    now_is += 30;
    cd_round(&c);
    CHECK(nsent == 1 && cd_edits == 0 && !c.cards[0].dirty);
    /* a label changed: edited where it is */
    issue_label = "kind: crash";
    cards_fixtures("open", "null");
    updated(&c, "[{\"number\":12}]");
    CHECK(nsent == 1 && cd_edits == 1 && !sent[0].removed);
    HAS(sent[0].description, "`kind: crash`");
    /* a person's comment: to the end of the channel, the new card posted before the old one is deleted */
    gh("repos/o/r/issues/12/comments?per_page=100&page=1", COMMENTS_DAN);
    updated(&c, "[{\"number\":12}]");
    CHECK(nsent == 2 && sent[0].removed && !sent[1].removed && c.cards[0].message == sent[1].message);
    CHECK(cd_edits == 1 && cd_removes == 2 && !c.cards[0].stale);
    HAS(sent[1].description, "by **dan**");
    HAS(sent[1].description, "`kind: crash`");
    /* closed: to the end as it closed, deleted a day after */
    issue_label = "kind: bug";
    cards_fixtures("closed", "\"2025-10-20T10:01:00Z\"");
    gh("repos/o/r/issues/12/comments?per_page=100&page=1", COMMENTS_DAN);
    updated(&c, "[{\"number\":12}]");
    CHECK(nsent == 3 && sent[1].removed && !sent[2].removed && cd_edits == 1 && cd_removes == 3);
    HAS(sent[2].description, "**Closed** as done");
    now_is = 1760954460 + CD_DAY;
    updated(&c, "[]");
    CHECK(sent[2].removed && cd_removes == 4);
    {
        cJSON* saved = cd_save(&c);
        char* text = cJSON_PrintUnformatted(saved);
        HAS(text, "\"key\":\"issue-12\",\"message\":\"0\"");
        HAS(text, "\"gone\":true");
        free(text);
        cJSON_Delete(saved);
    }
    /* reopened: a card again, at the end; the next round leaves it */
    cards_fixtures("open", "null");
    gh("repos/o/r/issues/12/comments?per_page=100&page=1", COMMENTS_DAN);
    updated(&c, "[{\"number\":12}]");
    CHECK(nsent == 4 && sent[3].channel == 1 && !sent[3].removed && !c.cards[0].gone && !c.cards[0].closed);
    updated(&c, "[{\"number\":12}]");
    CHECK(nsent == 4 && cd_edits == 1 && cd_removes == 4);
    /* a card deleted by hand on Discord: posted again at its next change */
    edit_gone = 1;
    issue_label = "kind: crash";
    cards_fixtures("open", "null");
    gh("repos/o/r/issues/12/comments?per_page=100&page=1", COMMENTS_DAN);
    updated(&c, "[{\"number\":12}]");
    edit_gone = 0;
    CHECK(nsent == 5 && c.cards[0].message == sent[4].message);
    HAS(sent[4].description, "`kind: crash`");
    gh("repos/o/r/issues/12/comments?per_page=100&page=1",
       "[{\"user\":{\"login\":\"cid\",\"type\":\"User\"},\"body\":\"newer\",\"html_url\":\"https://c/2\","
       "\"created_at\":\"2026-10-21T09:00:00Z\"}]");
    updated(&c, "[{\"number\":12}]");
    CHECK(nsent == 6 && sent[4].removed);
    HAS(sent[5].description, "by **cid**");
    /* the old card not deleted at a move: kept as stale, deleted by a later round, even with nothing new */
    remove_fails = 500;
    comment_by("eve", "2026-10-22T09:00:00Z");
    updated(&c, "[{\"number\":12}]");
    CHECK(nsent == 7 && c.cards[0].message == sent[6].message && c.cards[0].stale == sent[5].message);
    CHECK(!sent[5].removed && !c.cards[0].dirty);
    updated(&c, "[]");
    CHECK(c.cards[0].stale == sent[5].message);
    remove_fails = 0;
    updated(&c, "[]");
    CHECK(sent[5].removed && !c.cards[0].stale && nsent == 7);
    /* no move while one stays: the activity waits, the card dirty, until the old one is gone (404: deleted by hand) */
    remove_fails = 500;
    comment_by("fay", "2026-10-23T09:00:00Z");
    updated(&c, "[{\"number\":12}]");
    CHECK(nsent == 8 && c.cards[0].stale == sent[6].message);
    comment_by("gus", "2026-10-24T09:00:00Z");
    updated(&c, "[{\"number\":12}]");
    CHECK(nsent == 8 && c.cards[0].dirty && c.cards[0].stale == sent[6].message);
    remove_fails = 404;
    updated(&c, "[]");
    remove_fails = 0;
    CHECK(!c.cards[0].stale && nsent == 9 && c.cards[0].message == sent[8].message && !c.cards[0].dirty);
    HAS(sent[8].description, "by **gus**");
    /* GitHub not answering: the card left as it was drawn, and drawn at the next round */
    {
        int before = nsent, edits_before = cd_edits;
        answer("GET", "https://api.github.com/repos/o/r/issues/12", 502, "{}");
        listing =
            rei_format("repos/o/r/issues?state=all&sort=updated&direction=desc&since=%s&per_page=100&page=1", c.since);
        gh(listing, "[{\"number\":12}]");
        free(listing);
        now_is += 30;
        cd_round(&c);
        CHECK(nsent == before && cd_edits == edits_before);
        CHECK(c.ncards == 1 && c.cards[0].dirty);
        /* the issue there but who fixes it not (GraphQL): no card from half of it either */
        cards_fixtures("open", "null");
        gh("repos/o/r/issues/12/comments?per_page=100&page=1", "[]");
        answer("POST", "https://api.github.com/graphql", 502, "{}");
        now_is += 30;
        cd_round(&c);
        CHECK(nsent == before && cd_edits == edits_before && c.cards[0].dirty);
    }
    {
        cJSON* saved = cd_save(&c);
        Cards again;
        cd_init(&again, &g, dc, 0, 1, 3);
        cd_load(&again, saved);
        CHECK(again.ncards == 1 && again.cards[0].message == c.cards[0].message && again.purged_issues);
        SAME(again.since, c.since);
        SAME(again.head, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        SAME(again.cards[0].moved, c.cards[0].moved);
        CHECK(*again.cards[0].moved && again.cards[0].stale == c.cards[0].stale);
        cd_free(&again);
        cJSON_Delete(saved);
    }
    /* cards kept before their activity was: an issue's drawn again once, where it is, to learn what moves it; a
       nightly's never moves */
    {
        int before = nsent, removes = cd_removes;
        char* old = rei_format("{\"cards\":[{\"key\":\"issue-12\",\"message\":\"%llu\",\"drawn\":\"x\",\"closed\":0,"
                               "\"gone\":false,\"dirty\":false},{\"key\":\"nightly-abc\",\"message\":\"5\",\"drawn\":"
                               "\"y\",\"closed\":0,\"gone\":false,\"dirty\":false}],\"since\":\"%s\",\"head\":\"%s\","
                               "\"purged_issues\":true}",
                               (unsigned long long)c.cards[0].message, c.since, c.head);
        cJSON* saved = cJSON_Parse(old);
        Cards again;
        cd_init(&again, &g, dc, 0, 1, 3);
        cd_load(&again, saved);
        CHECK(again.ncards == 2 && again.cards[0].dirty && !*again.cards[0].moved && !again.cards[1].dirty);
        cards_fixtures("open", "null");
        updated(&again, "[]");
        CHECK(nsent == before && cd_removes == removes && again.cards[0].message == c.cards[0].message);
        CHECK(*again.cards[0].moved && !again.cards[0].dirty);
        cd_free(&again);
        cJSON_Delete(saved);
        free(old);
    }
    cd_free(&c);
    rg_free(&g);
    forget();
}

/* pull request 7 drawn, as a round draws it: its releases asked again */
static void drawn_pr(Cards* c) {
    int open;
    double closed;
    cJSON_Delete(c->nightlies);
    c->nightlies = NULL;
    cJSON_Delete(cd_pr(c, 7, &open, &closed));
}
/* its push's Build run in that status, then drawn */
static void run_is(Cards* c, const char* status) {
    char* runs = rei_format("{\"workflow_runs\":[{\"id\":9,\"event\":\"pull_request\",\"status\":%s,\"html_url\":"
                            "\"https://r/9\",\"created_at\":\"2026-10-20T11:00:00Z\"}]}",
                            status);
    gh("repos/o/r/actions/workflows/build.yml/runs?head_sha=1111111111111111111111111111111111111111&per_page=100&"
       "page=1",
       runs);
    free(runs);
    drawn_pr(c);
}
/* a pull request's card: its state, its commits, its builds */
static void pr_card(void) {
    Cards c;
    CdDiscord dc = {NULL, c_post, c_edit, c_remove, c_history};
    cJSON* e;
    int open;
    double closed;
    char* text;
    forget();
    rg_init(&g, "o/r", NULL, fake, NULL);
    cd_init(&c, &g, dc, 2, 0, 0);
    gh("repos/o/r/pulls/7",
       "{\"number\":7,\"title\":\"Faster [blit]\",\"html_url\":\"https://github.com/o/r/pull/7\",\"state\":\"open\","
       "\"draft\":false,\"merged\":false,\"comments\":0,\"review_comments\":0,\"labels\":[],\"assignees\":[],"
       "\"user\":{\"login\":\"ana\",\"html_url\":\"https://github.com/ana\",\"avatar_url\":\"https://a/1\"},"
       "\"head\":{\"sha\":\"1111111111111111111111111111111111111111\",\"repo\":{\"full_name\":\"o/r\"}},"
       "\"base\":{\"sha\":\"2222222222222222222222222222222222222222\"}}");
    gh("repos/o/r/pulls/7/commits?per_page=100&page=1",
       "[{\"commit\":{\"message\":\"[gpu] First\\n\\nbody\"}},{\"commit\":{\"message\":\"[gpu] Second *one*\"}}]");
    gh("repos/o/r/pulls/7/reviews?per_page=100&page=1", "[]");
    gh("repos/o/r/compare/2222222222222222222222222222222222222222...1111111111111111111111111111111111111111",
       "{\"merge_base_commit\":{\"sha\":\"3333333333333333333333333333333333333333\"}}");
    gh("repos/o/nightly/releases?per_page=100&page=1",
       "[{\"tag_name\":\"nightly-2026-10-20-3333333\",\"html_url\":\"https://n/1\",\"published_at\":\"2026-10-20T10:00:00Z\"},"
       "{\"tag_name\":\"pr-7\",\"html_url\":\"https://n/pr-7\",\"published_at\":\"2026-10-20T11:00:00Z\","
       "\"body\":\"built from /commits/1111111111111111111111111111111111111111\",\"assets\":[{\"name\":"
       "\"ScarletNX-windows-x86_64.zip\",\"size\":12345678,\"browser_download_url\":\"https://n/w\"},"
       "{\"name\":\"ScarletNX-linux-ppc64.AppImage\",\"size\":2345678,\"browser_download_url\":\"https://n/p\"}]}]");
    gh("repos/o/r/actions/workflows/build.yml/runs?head_sha=1111111111111111111111111111111111111111&per_page=100&page=1",
       "{\"workflow_runs\":[{\"id\":9,\"event\":\"pull_request\",\"status\":\"in_progress\",\"html_url\":\"https://r/9\","
       "\"created_at\":\"2026-10-20T11:00:00Z\"}]}");
    gh("repos/o/r/actions/runs/9/jobs?per_page=100&page=1",
       "{\"jobs\":[{\"name\":\"Linux AppImage (x86_64)\",\"status\":\"in_progress\",\"html_url\":\"https://j/1\"},"
       "{\"name\":\"Android APK\",\"status\":\"completed\",\"conclusion\":\"failure\",\"html_url\":\"https://j/2\"},"
       "{\"name\":\"Linux AppImage (aarch64)\",\"status\":\"completed\",\"conclusion\":\"success\"},"
       "{\"name\":\"macOS and iOS (arm64)\",\"status\":\"queued\",\"html_url\":\"https://j/3\"},"
       "{\"name\":\"Linux (ppc64, cross)\",\"status\":\"completed\",\"conclusion\":\"failure\","
       "\"html_url\":\"https://j/4\"}]}");
    answer("POST", "https://api.github.com/graphql", 200,
           "{\"data\":{\"repository\":{\"pullRequest\":{\"closingIssuesReferences\":{\"nodes\":[]}}}}}");
    e = cd_pr(&c, 7, &open, &closed);
    CHECK(e && open && !closed);
    text = cJSON_PrintUnformatted(e);
    HAS(text, "\"title\":\"PR #7: Faster [blit]\"");
    HAS(text,
        "**Open** at [`1111111`](https://github.com/o/r/pull/7/commits/1111111111111111111111111111111111111111), "
        "on main at [`3333333`](https://github.com/o/r/commit/3333333333333333333333333333333333333333) "
        "([its nightly](https://n/1)).");
    HAS(text, "**2 commits**, the newest first\\n- \\\\[gpu\\\\] Second \\\\*one\\\\*\\n- \\\\[gpu\\\\] First");
    HAS(text, "{\"name\":\"Linux x86-64\",\"value\":\"Building\\n[log](https://j/1)\",\"inline\":true}");
    HAS(text, "{\"name\":\"Linux ARM64\",\"value\":\"Built\\nbeing published\",\"inline\":true}");
    HAS(text, "{\"name\":\"Windows x86-64\",\"value\":\"[zip](https://n/w)\\n12.3 MB\",\"inline\":true}");
    HAS(text, "{\"name\":\"Android\",\"value\":\"Failed\\n[log](https://j/2)\",\"inline\":true}");
    HAS(text, "{\"name\":\"macOS ARM64\",\"value\":\"Waiting\",\"inline\":true}"); /* its job queued */
    HAS(text, "{\"name\":\"iOS ARM64\",\"value\":\"Waiting\",\"inline\":true}");
    HAS(text, "{\"name\":\"Linux PowerPC64 LE\",\"value\":\"Waiting\",\"inline\":true},{\"name\":\"Linux "
              "PowerPC64 BE\",\"value\":\"[AppImage (Linux programs)](https://n/p)\\n2.3 MB\",\"inline\":true},"
              "{\"name\":\"Linux RISC-V 64\"");
    HAS(text, "{\"name\":\"Linux RISC-V 64\",\"value\":\"Waiting\",\"inline\":true},{\"name\":\"Linux "
              "LoongArch64\",\"value\":\"Waiting\",\"inline\":true},{\"name\":\"Windows x86-64\"");
    /* the jobs that make no file after the packages: what their tests did */
    HAS(text, "{\"name\":\"iOS ARM64\",\"value\":\"Waiting\",\"inline\":true},{\"name\":\"Linux PowerPC64 BE "
              "ELFv1 tests\",\"value\":\"Tests failed\\n[log](https://j/4)\",\"inline\":true},{\"name\":\"Build\"");
    HAS(text, "{\"name\":\"Build\",\"value\":\"[Running](https://r/9)\",\"inline\":true}");
    HAS(text, "{\"name\":\"Builds\",\"value\":\"[All files](https://n/pr-7)\",\"inline\":true}");
    HAS(text, "\"color\":2066493"); /* GitHub's green: open */
    free(text);
    cJSON_Delete(e);
    /* what moves it to the end: its push's builds published or failed (not a job's progress, a run again or
       cancelled), a comment, a draft, a push; not a label */
    {
        char was[17];
        const char* pr7 =
            "{\"number\":7,\"title\":\"Faster [blit]\",\"html_url\":\"https://github.com/o/r/pull/7\",\"state\":\"open\","
            "\"draft\":%s,\"merged\":false,\"comments\":%d,\"review_comments\":0,\"labels\":[%s],\"assignees\":[],"
            "\"user\":{\"login\":\"ana\",\"html_url\":\"https://github.com/ana\",\"avatar_url\":\"https://a/1\"},"
            "\"head\":{\"sha\":\"%s\",\"repo\":{\"full_name\":\"o/r\"}},\"base\":{\"sha\":"
            "\"2222222222222222222222222222222222222222\"}}";
        char* json;
        /* the push before's builds, this one's running */
        gh("repos/o/nightly/releases?per_page=100&page=1",
           "[{\"tag_name\":\"pr-7\",\"html_url\":\"https://n/pr-7\",\"published_at\":\"2026-10-20T11:00:00Z\","
           "\"body\":\"built from /commits/0000000000000000000000000000000000000000\",\"assets\":[]}]");
        c.builds[0] = 0;
        drawn_pr(&c);
        snprintf(was, sizeof was, "%s", c.activity);
        CHECK(*was && !*c.builds);
        gh("repos/o/r/actions/runs/9/jobs?per_page=100&page=1",
           "{\"jobs\":[{\"name\":\"Linux AppImage (x86_64)\",\"status\":\"completed\",\"conclusion\":\"success\"}]}");
        drawn_pr(&c);
        SAME(c.activity, was); /* a job done */
        run_is(&c, "\"completed\",\"conclusion\":\"cancelled\"");
        SAME(c.activity, was); /* cancelled, none failed before */
        run_is(&c, "\"completed\",\"conclusion\":\"failure\"");
        CHECK(strcmp(c.activity, was)); /* failed */
        SAME(c.builds, "failed 1111111111111111111111111111111111111111");
        snprintf(was, sizeof was, "%s", c.activity);
        run_is(&c, "\"in_progress\"");
        SAME(c.activity, was); /* run again */
        run_is(&c, "\"completed\",\"conclusion\":\"cancelled\"");
        SAME(c.activity, was); /* cancelled */
        gh("repos/o/nightly/releases?per_page=100&page=1",
           "[{\"tag_name\":\"pr-7\",\"html_url\":\"https://n/pr-7\",\"published_at\":\"2026-10-20T12:00:00Z\","
           "\"body\":\"built from /commits/1111111111111111111111111111111111111111\",\"assets\":[]}]");
        drawn_pr(&c);
        CHECK(strcmp(c.activity, was)); /* published */
        SAME(c.builds, "published 1111111111111111111111111111111111111111");
        snprintf(was, sizeof was, "%s", c.activity);
        json = rei_format(pr7, "false", 1, "", "1111111111111111111111111111111111111111");
        gh("repos/o/r/pulls/7", json);
        free(json);
        gh("repos/o/r/issues/7/comments?per_page=100&page=1",
           "[{\"user\":{\"login\":\"bia\",\"type\":\"User\"},\"body\":\"Works\",\"html_url\":\"https://c/7\","
           "\"created_at\":\"2026-10-20T12:00:00Z\"}]");
        drawn_pr(&c);
        CHECK(strcmp(c.activity, was)); /* a comment */
        snprintf(was, sizeof was, "%s", c.activity);
        json = rei_format(pr7, "true", 1, "", "1111111111111111111111111111111111111111");
        gh("repos/o/r/pulls/7", json);
        free(json);
        drawn_pr(&c);
        CHECK(strcmp(c.activity, was)); /* a draft */
        snprintf(was, sizeof was, "%s", c.activity);
        json = rei_format(pr7, "true", 1, "{\"name\":\"x\"}", "1111111111111111111111111111111111111111");
        gh("repos/o/r/pulls/7", json);
        free(json);
        drawn_pr(&c);
        SAME(c.activity, was); /* a label: where it is */
        /* a push: its builds none yet, the push before's outcome not its own; then its own published */
        json = rei_format(pr7, "true", 1, "{\"name\":\"x\"}", "4444444444444444444444444444444444444444");
        gh("repos/o/r/pulls/7", json);
        free(json);
        gh("repos/o/r/compare/2222222222222222222222222222222222222222...4444444444444444444444444444444444444444",
           "{\"merge_base_commit\":{\"sha\":\"3333333333333333333333333333333333333333\"}}");
        gh("repos/o/r/actions/workflows/build.yml/runs?head_sha=4444444444444444444444444444444444444444&per_page=100&"
           "page=1",
           "{\"workflow_runs\":[]}");
        drawn_pr(&c);
        CHECK(strcmp(c.activity, was) && !*c.builds); /* a push */
        snprintf(was, sizeof was, "%s", c.activity);
        gh("repos/o/nightly/releases?per_page=100&page=1",
           "[{\"tag_name\":\"pr-7\",\"html_url\":\"https://n/pr-7\",\"published_at\":\"2026-10-20T13:00:00Z\","
           "\"body\":\"built from /commits/4444444444444444444444444444444444444444\",\"assets\":[]}]");
        drawn_pr(&c);
        CHECK(strcmp(c.activity, was)); /* its own published */
        SAME(c.builds, "published 4444444444444444444444444444444444444444");
        json = rei_format(pr7, "true", 1, "{\"name\":\"x\"}", "5555555555555555555555555555555555555555");
        gh("repos/o/r/pulls/7", json);
        free(json);
        gh("repos/o/r/compare/2222222222222222222222222222222222222222...5555555555555555555555555555555555555555",
           "{\"merge_base_commit\":{\"sha\":\"3333333333333333333333333333333333333333\"}}");
        gh("repos/o/r/actions/workflows/build.yml/runs?head_sha=5555555555555555555555555555555555555555&per_page=100&"
           "page=1",
           "{\"workflow_runs\":[]}");
        drawn_pr(&c);
        snprintf(was, sizeof was, "%s", c.activity);
        json = rei_format(pr7, "true", 1, "{\"name\":\"x\"}", "6666666666666666666666666666666666666666");
        gh("repos/o/r/pulls/7", json);
        free(json);
        gh("repos/o/r/compare/2222222222222222222222222222222222222222...6666666666666666666666666666666666666666",
           "{\"merge_base_commit\":{\"sha\":\"3333333333333333333333333333333333333333\"}}");
        gh("repos/o/r/actions/workflows/build.yml/runs?head_sha=6666666666666666666666666666666666666666&per_page=100&"
           "page=1",
           "{\"workflow_runs\":[]}");
        drawn_pr(&c);
        CHECK(strcmp(c.activity, was) && !*c.builds); /* a push, no builds on either side */
        /* in a round: the card's outcome carried into its drawing and out of it, and kept in the state */
        {
            Cards r;
            cJSON *saved, *back;
            char *state, *kept, failed_moved[17], running_moved[17];
            u64 message = c_post(NULL, 2, NULL);
            int before;
            json = rei_format(pr7, "false", 1, "", "1111111111111111111111111111111111111111");
            gh("repos/o/r/pulls/7", json);
            free(json);
            gh("repos/o/nightly/releases?per_page=100&page=1", "[]");
            c.builds[0] = 0;
            run_is(&c, "\"in_progress\"");
            snprintf(running_moved, sizeof running_moved, "%s", c.activity);
            run_is(&c, "\"completed\",\"conclusion\":\"failure\"");
            snprintf(failed_moved, sizeof failed_moved, "%s", c.activity);
            gh("repos/o/r/issues?state=all&sort=updated&direction=desc&since=2026-10-20T10:00:00Z&per_page=100&page=1",
               "[]");
            gh("repos/o/r/actions/workflows/build.yml/runs?per_page=20", "{\"workflow_runs\":[]}");
            gh("repos/o/r/commits/main", "{\"sha\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}");
            /* failed before, running again now: no move, the outcome kept */
            run_is(&c, "\"in_progress\"");
            state =
                rei_format("{\"cards\":[{\"key\":\"pr-7\",\"message\":\"%llu\",\"drawn\":\"x\",\"moved\":\"%s\","
                           "\"builds\":\"failed 1111111111111111111111111111111111111111\",\"dirty\":true}],"
                           "\"since\":\"2026-10-20T10:00:00Z\",\"head\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                           "\"purged_prs\":true}",
                           (unsigned long long)message, failed_moved);
            saved = cJSON_Parse(state);
            cd_init(&r, &g, dc, 2, 0, 0);
            cd_load(&r, saved);
            before = nsent;
            now_is += 30;
            cd_round(&r);
            CHECK(nsent == before && r.cards[0].message == message && !r.cards[0].dirty);
            SAME(r.cards[0].builds, "failed 1111111111111111111111111111111111111111");
            cd_free(&r);
            cJSON_Delete(saved);
            free(state);
            /* running before, failed now: moved, the outcome learnt and saved */
            run_is(&c, "\"completed\",\"conclusion\":\"failure\"");
            state = rei_format("{\"cards\":[{\"key\":\"pr-7\",\"message\":\"%llu\",\"drawn\":\"x\",\"moved\":\"%s\","
                               "\"builds\":\"\",\"dirty\":true}],\"since\":\"2026-10-20T10:00:00Z\",\"head\":"
                               "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"purged_prs\":true}",
                               (unsigned long long)message, running_moved);
            saved = cJSON_Parse(state);
            cd_init(&r, &g, dc, 2, 0, 0);
            cd_load(&r, saved);
            now_is += 30;
            cd_round(&r);
            CHECK(nsent == before + 1 && r.cards[0].message == sent[nsent - 1].message);
            SAME(r.cards[0].builds, "failed 1111111111111111111111111111111111111111");
            back = cd_save(&r);
            kept = cJSON_PrintUnformatted(back);
            HAS(kept, "\"builds\":\"failed 1111111111111111111111111111111111111111\"");
            free(kept);
            cJSON_Delete(back);
            cd_free(&r);
            cJSON_Delete(saved);
            free(state);
        }
    }
    /* GraphQL failing: no card from what is missing */
    answer("POST", "https://api.github.com/graphql", 502, "{}");
    CHECK(!cd_pr(&c, 7, &open, &closed) && c.failed);
    cd_free(&c);
    rg_free(&g);
    forget();
}

/* ---- the state file ---- */
static void state_tests(void) {
    char path[] = "/tmp/rei-state-XXXXXX";
    int fd = mkstemp(path);
    cJSON *s = cJSON_Parse("{\"a\":1}"), *back;
    FILE* f;
    CHECK(fd >= 0);
    close(fd);
    CHECK(rei_save(path, s) == 0);
    back = rei_load(path);
    CHECK(jnum(back, "a") == 1);
    cJSON_Delete(back);
    f = fopen(path, "w");
    fputs("{not json", f);
    fclose(f);
    back = rei_load(path); /* said in the log, and an empty state */
    CHECK(cJSON_IsObject(back) && !back->child);
    cJSON_Delete(back);
    remove(path);
    back = rei_load(path);
    CHECK(cJSON_IsObject(back) && !back->child);
    cJSON_Delete(back);
    back = rei_load(NULL);
    CHECK(cJSON_IsObject(back));
    cJSON_Delete(back);
    cJSON_Delete(s);
}

/* ---- the gateway's events, read from their JSON ---- */
static int message(const char* json, EvMessage* m) { return ev_message(json, strlen(json), m); }
static char* ready_user(const char* json) { return ev_ready_user(json, strlen(json)); }
static void event_tests(void) {
    EvMessage m;
    Buf b = {0};
    size_t before;
    const char* framed =
        "{\"id\":\"7\",\"channel_id\":\"8\",\"content\":\"#1\",\"author\":{\"id\":\"1\"}},\"s\":5,\"op\":0}";
    char* user;
    /* a person's, its strings unescaped (Concord left them as sent: \" and é in the text) */
    CHECK(message("{\"id\":\"1430000000000000000\",\"channel_id\":\"555555555555555555\",\"content\":\"he said "
                  "\\\"#160\\\" \\\\ caf\\u00e9 \\ud83d\\ude00\\n\",\"author\":{\"id\":\"1\",\"username\":\"ana\","
                  "\"bot\":false},\"embeds\":[],\"member\":{\"roles\":[]}}",
                  &m));
    CHECK(m.id == 1430000000000000000ULL && m.channel == 555555555555555555ULL);
    SAME(m.content, "he said \"#160\" \\ caf\xc3\xa9 \xf0\x9f\x98\x80\n");
    ev_message_free(&m);
    CHECK(!m.content && !m.id);
    /* a reply: its own author counts, not the one it answers */
    CHECK(message(
        "{\"id\":\"2\",\"channel_id\":\"3\",\"content\":\"#5\",\"author\":{\"id\":\"1\"},\"referenced_message\":"
        "{\"id\":\"9\",\"channel_id\":\"3\",\"content\":\"card\",\"author\":{\"id\":\"4\",\"bot\":true}}}",
        &m));
    SAME(m.content, "#5");
    ev_message_free(&m);
    CHECK(!message("{\"id\":\"9\",\"channel_id\":\"3\",\"content\":\"#5\",\"author\":{\"id\":\"4\",\"bot\":true},"
                   "\"referenced_message\":{\"id\":\"2\",\"channel_id\":\"3\",\"author\":{\"id\":\"1\"}}}",
                   &m) &&
          !m.content);
    /* a webhook's (the old cards), one with no author, ids that are no snowflakes, no JSON: never answered */
    CHECK(!message(
              "{\"id\":\"2\",\"channel_id\":\"3\",\"content\":\"#5\",\"author\":{\"id\":\"1\"},\"webhook_id\":\"6\"}",
              &m) &&
          !m.content && !m.id && !m.channel);
    CHECK(!message("{\"id\":\"2\",\"channel_id\":\"3\",\"content\":\"#5\"}", &m));
    CHECK(!message("{\"id\":\"2\",\"channel_id\":\"3\",\"content\":\"#5\",\"author\":null}", &m));
    CHECK(!message("{\"id\":\"2a\",\"channel_id\":\"3\",\"content\":\"#5\",\"author\":{\"id\":\"1\"}}", &m));
    CHECK(!message("{\"id\":2,\"channel_id\":\"3\",\"content\":\"#5\",\"author\":{\"id\":\"1\"}}", &m));
    CHECK(!message("{\"id\":\"-2\",\"channel_id\":\"3\",\"content\":\"#5\",\"author\":{\"id\":\"1\"}}", &m));
    CHECK(!message("{\"id\":\"2\",\"content\":\"#5\",\"author\":{\"id\":\"1\"}}", &m));
    CHECK(!message("{\"id\":\"2\",\"channel_id\":\"3\",", &m) && !message("", &m) && !message("[1]", &m));
    /* the event as the gateway hands it: its object, then the rest of the frame, read only as far as told */
    CHECK(!ev_message(framed, (size_t)(strstr(framed, "\"author\"") - framed), &m));
    CHECK(ev_message(framed, (size_t)(strstr(framed, ",\"s\"") - framed), &m));
    CHECK(m.id == 7 && m.channel == 8);
    SAME(m.content, "#1");
    ev_message_free(&m);
    /* files alone: no text */
    CHECK(
        message("{\"id\":\"2\",\"channel_id\":\"3\",\"author\":{\"id\":\"1\"},\"attachments\":[{\"id\":\"4\"}]}", &m));
    SAME(m.content, "");
    ev_message_free(&m);
    /* a long one, as the messages of 2 to 3 KB that crashed Concord's decoder */
    buf_add(&b,
            "{\"id\":\"2\",\"channel_id\":\"3\",\"author\":{\"id\":\"1\"},\"embeds\":[{\"type\":\"video\",\"provider\":"
            "{\"name\":\"x\"},\"video\":{\"url\":\"https://v\",\"width\":1280}}],\"content\":\"");
    before = b.len;
    while (b.len - before < 4000) buf_add(&b, "word ");
    buf_add(&b, "#160\"}");
    CHECK(message(b.text, &m));
    CHECK(m.content && strlen(m.content) == b.len - before - 2 && strstr(m.content, " #160"));
    ev_message_free(&m);
    buf_free(&b);
    /* READY's user */
    user = ready_user("{\"v\":10,\"user\":{\"id\":\"1\",\"username\":\"ReiWimsy\",\"bot\":true},\"session_id\":\"s\"}");
    SAME(user, "ReiWimsy");
    free(user);
    CHECK(!ready_user("{\"user\":{\"username\":7}}") && !ready_user("{\"v\":10}") && !ready_user("nope"));
}
/* the messages waiting: oldest first, across the end of the ring; a full queue refuses one, which stays its caller's */
static void queue_tests(void) {
    static EvQueue q;
    EvMessage m;
    int i, ordered = 1, taken = 1;
    ev_queue_init(&q);
    for (i = 0; i < EV_WAITING; ++i) {
        m.id = (u64)i + 1;
        m.channel = 3;
        m.content = rei_format("#%d", i + 1);
        taken &= ev_push(&q, &m) && !m.content;
    }
    CHECK(taken);
    m.id = 999;
    m.channel = 3;
    m.content = rei_strdup("#999");
    CHECK(!ev_push(&q, &m) && m.id == 999);
    SAME(m.content, "#999");
    ev_message_free(&m);
    /* half out, as many in: the ring wraps */
    for (i = 0; i < EV_WAITING / 2; ++i) {
        ev_pop(&q, &m);
        ordered &= m.id == (u64)i + 1;
        ev_message_free(&m);
        m.id = (u64)(EV_WAITING + i + 1);
        m.content = rei_format("#%d", EV_WAITING + i + 1);
        ev_push(&q, &m);
    }
    for (i = EV_WAITING / 2; i < EV_WAITING + EV_WAITING / 2; ++i) {
        char want[16];
        ev_pop(&q, &m);
        sprintf(want, "#%d", i + 1);
        ordered &= m.id == (u64)i + 1 && !strcmp(m.content, want);
        ev_message_free(&m);
    }
    CHECK(ordered && q.n == 0);
}

int main(void) {
    rei_now = clock_now;
    refs();
    look();
    link_tests();
    commands();
    github_tests();
    rest_tests();
    cards_tests();
    pr_card();
    state_tests();
    event_tests();
    queue_tests();
    printf("reiwimsy: %d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
