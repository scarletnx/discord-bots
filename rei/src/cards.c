/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "cards.h"
#include "look.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SERVER       "https://github.com"
/* a nightly's colours: building, published, a package failed, not published */
#define BUILDING     0x1F6FEB
#define SCARLET      0xB0102A
#define AMBER        0xD29922
#define CLOSED       0x6E7681
/* the mascot beside each card */
#define WIMSY        "https://raw.githubusercontent.com/%s/main/dist/icons/hicolor/128x128/apps/wimsy.png"
/* what a card lists, at most, before "and N more": a nightly's changes, a pull request's commits; a text's letters */
#define MOST_LINES   10
#define MOST_COMMITS 6
#define MOST_TEXT    300
/* Discord's limits: an embed's description, and its whole text */
#define DESCRIPTION  4096
#define EMBED        6000
/* what lets a fork's pull request be published (ScarletNX's scripts/publish-builds.py) */
#define LABEL        "publish builds"

/* each release file, in the order the cards list them: the job of ScarletNX's build.yml that makes it (macOS's and
   iOS's share one runner), and what it is for, as scripts/nightly-notes.py's SYSTEMS has it */
static const struct {
    const char *file, *job, *system, *arch, *kind;
} PACKAGES[] = {
    {"ScarletNX-linux-x86_64.AppImage", "Linux AppImage (x86_64)", "Linux", "x86-64", "AppImage"},
    {"ScarletNX-linux-aarch64.AppImage", "Linux AppImage (aarch64)", "Linux", "ARM64", "AppImage"},
    {"ScarletNX-linux-ppc64le.AppImage", "Linux AppImage (ppc64le)", "Linux", "PowerPC64 LE", "AppImage"},
    {"ScarletNX-linux-ppc64.AppImage", "Linux AppImage (ppc64)", "Linux", "PowerPC64 BE", "AppImage (Linux programs)"},
    {"ScarletNX-linux-riscv64.AppImage", "Linux AppImage (riscv64)", "Linux", "RISC-V 64", "AppImage"},
    {"ScarletNX-linux-loongarch64.AppImage", "Linux AppImage (loongarch64)", "Linux", "LoongArch64", "AppImage"},
    {"ScarletNX-windows-x86_64.zip", "Windows (Clang + LLVM)", "Windows", "x86-64", "zip"},
    {"ScarletNX-android-universal.apk", "Android APK", "Android", "ARM64, x86-64", "APK"},
    {"ScarletNX-macos-arm64.zip", "macOS and iOS (arm64)", "macOS", "ARM64", "zip"},
    {"ScarletNX-ios-arm64.ipa", "macOS and iOS (arm64)", "iOS", "ARM64", "IPA"},
};
#define NPACKAGES (int)(sizeof PACKAGES / sizeof *PACKAGES)
/* Build's jobs that make no release file, shown after the packages: each job, as build.yml names it, and what it
   tests */
static const struct {
    const char *job, *field;
} CHECKS[] = {{"Linux (ppc64, cross)", "Linux PowerPC64 BE ELFv1 tests"}};
#define NCHECKS (int)(sizeof CHECKS / sizeof *CHECKS)

static const char* S(const char* s) { return s ? s : ""; }
/* FNV-1a of text, as 16 hex digits */
static void fnv(const char* text, char out[17]) {
    unsigned long long h = 1469598103934665603ULL;
    const unsigned char* p;
    for (p = (const unsigned char*)text; *p; ++p) h = (h ^ *p) * 1099511628211ULL;
    snprintf(out, 17, "%016llx", h);
}
/* what moves a card to the end of its channel, hashed into c->activity */
static void activity(Cards* c, const char* state, const cJSON* comment, const char* builds, const char* head) {
    char* text =
        rei_format("%s|%s|%s|%s|%s", state, S(jstr(comment, "html_url")), S(jstr(comment, "created_at")), builds, head);
    fnv(text, c->activity);
    free(text);
}

/* ---- time: GitHub's ISO 8601 (2026-10-20T18:30:00Z) and back ---- */
static long long days_from_civil(int y, int m, int d) { /* Howard Hinnant's, for the proleptic Gregorian calendar */
    int era;
    unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}
static double seconds_of(const char* iso) {
    int y, mo, d, h, mi, s;
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    return (double)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
}
static void iso_of(double when, char out[32]) {
    time_t t = (time_t)when;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* ---- what GitHub says ---- */
static cJSON* need(Cards* c, cJSON* got) {
    if (!got) c->failed = 1;
    return got;
}
static cJSON* get(Cards* c, const char* fmt, const char* a, const char* b) {
    char* path = rei_format(fmt, a, b);
    cJSON* got = need(c, rg_get(c->gh, path, NULL));
    free(path);
    return got;
}
static cJSON* pages(Cards* c, const char* path, const char* key) { return need(c, rg_pages(c->gh, path, key)); }

static int newer_published(const void* a, const void* b) {
    const char *x = S(jstr(*(const cJSON* const*)a, "published_at")),
               *y = S(jstr(*(const cJSON* const*)b, "published_at"));
    return strcmp(y, x);
}
/* the nightly repository's releases, the newest published first, once a round */
static const cJSON* nightlies(Cards* c) {
    if (!c->nightlies) {
        char* path = rei_format("repos/%s/releases", c->gh->nightly);
        cJSON *all = pages(c, path, NULL), *sorted = cJSON_CreateArray();
        int n = cJSON_GetArraySize(all), i = 0;
        const cJSON** list = malloc((size_t)(n ? n : 1) * sizeof *list);
        const cJSON* r;
        free(path);
        if (!all) {
            free(list);
            cJSON_Delete(sorted);
            return NULL;
        }
        cJSON_ArrayForEach(r, all) list[i++] = r;
        qsort(list, (size_t)n, sizeof *list, newer_published);
        for (i = 0; i < n; ++i) cJSON_AddItemToArray(sorted, cJSON_Duplicate(list[i], 1));
        free(list);
        cJSON_Delete(all);
        c->nightlies = sorted;
    }
    return c->nightlies;
}
static const cJSON* release_tagged(Cards* c, const char* tag) {
    const cJSON* r;
    cJSON_ArrayForEach(r, nightlies(c)) if (!strcmp(S(jstr(r, "tag_name")), tag)) return r;
    return NULL;
}
static int prefix(const char* s, const char* p) { return !strncmp(s, p, strlen(p)); }
static int suffix(const char* s, const char* x) {
    size_t n = strlen(s), m = strlen(x);
    return n >= m && !strcmp(s + n - m, x);
}

/* the newest Build run of sha, of a push or a pull request: owned, or NULL */
static cJSON* build_of(Cards* c, const char* sha) {
    char* path = rei_format("repos/%s/actions/workflows/build.yml/runs?head_sha=%s", c->gh->repo, sha);
    cJSON *runs = pages(c, path, "workflow_runs"), *best = NULL;
    const cJSON* r;
    free(path);
    cJSON_ArrayForEach(r, runs) {
        const char* event = S(jstr(r, "event"));
        if (strcmp(event, "push") && strcmp(event, "pull_request")) continue;
        if (!best || strcmp(S(jstr(r, "created_at")), S(jstr(best, "created_at"))) > 0) best = (cJSON*)r;
    }
    best = best ? cJSON_Duplicate(best, 1) : NULL;
    cJSON_Delete(runs);
    return best;
}
static cJSON* jobs_of(Cards* c, const cJSON* run) {
    char *path, id[24];
    cJSON* jobs;
    if (!run) return cJSON_CreateArray();
    snprintf(id, sizeof id, "%.0f", jnum(run, "id"));
    path = rei_format("repos/%s/actions/runs/%s/jobs", c->gh->repo, id);
    jobs = pages(c, path, "jobs");
    free(path);
    return jobs ? jobs : cJSON_CreateArray();
}

/* a person's comment: not a bot's, and with words */
static int by_person(const cJSON* item) {
    const char* type = jstr(cJSON_GetObjectItemCaseSensitive(item, "user"), "type");
    const char* body = S(jstr(item, "body"));
    while (isspace((unsigned char)*body)) ++body;
    return !(type && !strcmp(type, "Bot")) && *body;
}
/* the last page of a listing with a person's comment on it, from the last page back; owned array */
static cJSON* last_of(Cards* c, const char* path, int total) {
    int page;
    for (page = (total + 99) / 100; page > 0; --page) {
        char* at = rei_format("%s?per_page=100&page=%d", path, page);
        cJSON *got = need(c, rg_get(c->gh, at, NULL)), *people = cJSON_CreateArray();
        const cJSON* item;
        free(at);
        cJSON_ArrayForEach(item, got) if (by_person(item)) cJSON_AddItemToArray(people, cJSON_Duplicate(item, 1));
        cJSON_Delete(got);
        if (cJSON_GetArraySize(people)) return people;
        cJSON_Delete(people);
    }
    return cJSON_CreateArray();
}
static void newest(cJSON** best, const cJSON* list, const char* when) {
    const cJSON* item;
    cJSON_ArrayForEach(item, list) {
        const char* at = jstr(item, when);
        if (at && (!*best || strcmp(at, S(jstr(*best, "created_at"))) > 0)) {
            cJSON_Delete(*best);
            *best = cJSON_Duplicate(item, 1);
            cJSON_DeleteItemFromObjectCaseSensitive(*best, "created_at");
            cJSON_AddStringToObject(*best, "created_at", at);
        }
    }
}
/* the newest comment of an issue or a pull request (its reviews and review comments too) a person wrote; owned */
static cJSON* latest_comment(Cards* c, int number, int comments, const cJSON* pr) {
    char* path = rei_format("repos/%s/issues/%d/comments", c->gh->repo, number);
    cJSON *best = NULL, *list = last_of(c, path, comments);
    free(path);
    newest(&best, list, "created_at");
    cJSON_Delete(list);
    if (pr) {
        const cJSON* r;
        cJSON* people = cJSON_CreateArray();
        path = rei_format("repos/%s/pulls/%d/reviews", c->gh->repo, number);
        list = pages(c, path, NULL);
        free(path);
        cJSON_ArrayForEach(r, list) if (jstr(r, "submitted_at") && by_person(r))
            cJSON_AddItemToArray(people, cJSON_Duplicate(r, 1));
        cJSON_Delete(list);
        newest(&best, people, "submitted_at");
        cJSON_Delete(people);
        path = rei_format("repos/%s/pulls/%d/comments", c->gh->repo, number);
        list = last_of(c, path, (int)jnum(pr, "review_comments"));
        free(path);
        newest(&best, list, "created_at");
        cJSON_Delete(list);
    }
    return best;
}
/* the issues a pull request closes, or the pull requests that close an issue: "[#N](url), ..." owned, "" for none */
static char* linked(Cards* c, int number, int pr) {
    const char *field = pr ? "pullRequest" : "issue",
               *refs = pr ? "closingIssuesReferences" : "closedByPullRequestsReferences";
    size_t owner = strcspn(c->gh->repo, "/");
    char* query =
        rei_format("query{repository(owner:\"%.*s\",name:\"%s\"){%s(number:%d){%s(first:10){nodes{number url}}}}}",
                   (int)owner, c->gh->repo, c->gh->repo + owner + 1, field, number, refs);
    cJSON* data = need(c, rg_graphql(c->gh, query));
    const cJSON *nodes = jpath(jpath(jpath(data, "repository"), field), refs), *node;
    Buf b = {0};
    free(query);
    cJSON_ArrayForEach(node, cJSON_GetObjectItemCaseSensitive(nodes, "nodes")) {
        buf_printf(&b, "%s[#%d](%s)", b.len ? ", " : "", (int)jnum(node, "number"), S(jstr(node, "url")));
    }
    cJSON_Delete(data);
    return buf_take(&b);
}

/* ---- drawing ---- */
static void plural(Buf* b, int count, const char* word) {
    buf_printf(b, "%d %s%s", count, word, count == 1 ? "" : "s");
}
/* what a card says of its item besides its state: its type, its labels, who it is assigned to, its milestone, and the
   issues or pull requests it is linked to */
static char* meta(const cJSON* item, const char* kind, const char* links, const char* word) {
    Buf b = {0};
    char* chips = rei_chips(item);
    const cJSON *people = cJSON_GetObjectItemCaseSensitive(item, "assignees"), *p,
                *milestone = cJSON_GetObjectItemCaseSensitive(item, "milestone");
    if (kind && *kind) buf_printf(&b, "**%s**", kind);
    if (*chips) buf_printf(&b, "%s%s", b.len ? " | " : "", chips);
    free(chips);
    if (cJSON_GetArraySize(people)) {
        int first = 1;
        buf_printf(&b, "%sassigned to ", b.len ? " | " : "");
        cJSON_ArrayForEach(p, people) {
            buf_printf(&b, "%s[%s](%s)", first ? "" : ", ", S(jstr(p, "login")), S(jstr(p, "html_url")));
            first = 0;
        }
    }
    if (cJSON_IsObject(milestone))
        buf_printf(&b, "%smilestone [%s](%s)", b.len ? " | " : "", S(jstr(milestone, "title")),
                   S(jstr(milestone, "html_url")));
    if (links && *links) buf_printf(&b, "%s%s %s", b.len ? " | " : "", word, links);
    return buf_take(&b);
}
static void comment_lines(Buf* b, const cJSON* comment) {
    char* quoted;
    if (!comment) return;
    quoted = rei_excerpt(jstr(comment, "body"), MOST_TEXT);
    buf_printf(b, "\n\n**Latest comment**, by **%s** ([on GitHub](%s))\n%s",
               S(jstr(cJSON_GetObjectItemCaseSensitive(comment, "user"), "login")), S(jstr(comment, "html_url")),
               quoted);
    free(quoted);
}
/* "- line" for the first most, then "- and N more" linked */
static void listed(Buf* b, char** lines, int n, const char* more, int most) {
    int i;
    for (i = 0; i < n && i < most; ++i) buf_printf(b, "\n- %s", lines[i]);
    if (n > most) buf_printf(b, "\n- and [%d more](%s)", n - most, more);
}
static void field(cJSON* fields, const char* name, const char* value) {
    cJSON* f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "name", name);
    cJSON_AddStringToObject(f, "value", value);
    cJSON_AddBoolToObject(f, "inline", 1);
    cJSON_AddItemToArray(fields, f);
}
static void capitalized(Buf* b, const char* word) {
    if (!*word) return;
    buf_printf(b, "%c%s", toupper((unsigned char)*word), word + 1);
}
static const cJSON* named(const cJSON* list, const char* key, const char* name) {
    const cJSON *item, *found = NULL;
    cJSON_ArrayForEach(item, list) if (!strcmp(S(jstr(item, key)), name)) found = item; /* the last, as a dict keeps */
    return found;
}
/* a field per release file: published, its link and size; else what its job does; built: what a built one waits for.
   Then a field per job of CHECKS: what its tests did */
static void packages(cJSON* fields, const cJSON* jobs, const cJSON* release, const char* built) {
    int i;
    for (i = 0; i < NPACKAGES; ++i) {
        const cJSON *job = named(jobs, "name", PACKAGES[i].job),
                    *asset = named(cJSON_GetObjectItemCaseSensitive(release, "assets"), "name", PACKAGES[i].file);
        const char *status = S(jstr(job, "status")), *conclusion = S(jstr(job, "conclusion"));
        Buf v = {0};
        char* name;
        if (asset)
            buf_printf(&v, "[%s](%s)\n%.1f MB", PACKAGES[i].kind, S(jstr(asset, "browser_download_url")),
                       jnum(asset, "size") / 1e6);
        else if (!job || !strcmp(status, "queued") || !strcmp(status, "waiting") || !strcmp(status, "pending") ||
                 !strcmp(status, "requested"))
            buf_add(&v, "Waiting");
        else if (strcmp(status, "completed"))
            buf_printf(&v, "Building\n[log](%s)", S(jstr(job, "html_url")));
        else if (!strcmp(conclusion, "success"))
            buf_printf(&v, built && *built ? "Built\n%s" : "Built", S(built));
        else if (!strcmp(conclusion, "cancelled") || !strcmp(conclusion, "skipped"))
            capitalized(&v, conclusion);
        else
            buf_printf(&v, "Failed\n[log](%s)", S(jstr(job, "html_url")));
        name = strchr(PACKAGES[i].arch, ',') ? rei_strdup(PACKAGES[i].system)
                                             : rei_format("%s %s", PACKAGES[i].system, PACKAGES[i].arch);
        field(fields, name, v.text);
        free(name);
        buf_free(&v);
    }
    for (i = 0; i < NCHECKS; ++i) {
        const cJSON* job = named(jobs, "name", CHECKS[i].job);
        const char *status = S(jstr(job, "status")), *conclusion = S(jstr(job, "conclusion"));
        Buf v = {0};
        if (!job || !strcmp(status, "queued") || !strcmp(status, "waiting") || !strcmp(status, "pending") ||
            !strcmp(status, "requested"))
            buf_add(&v, "Waiting");
        else if (strcmp(status, "completed"))
            buf_printf(&v, "Testing\n[log](%s)", S(jstr(job, "html_url")));
        else if (!strcmp(conclusion, "success"))
            buf_add(&v, "Tests passed");
        else if (!strcmp(conclusion, "cancelled") || !strcmp(conclusion, "skipped"))
            capitalized(&v, conclusion);
        else
            buf_printf(&v, "Tests failed\n[log](%s)", S(jstr(job, "html_url")));
        field(fields, CHECKS[i].field, v.text);
        buf_free(&v);
    }
}
static int failed_jobs(const cJSON* jobs) {
    const cJSON* j;
    cJSON_ArrayForEach(j, jobs) {
        int i, ours = 0;
        const char* conclusion = S(jstr(j, "conclusion"));
        for (i = 0; i < NPACKAGES; ++i) ours |= !strcmp(S(jstr(j, "name")), PACKAGES[i].job);
        if (ours && !strcmp(S(jstr(j, "status")), "completed") && strcmp(conclusion, "success") &&
            strcmp(conclusion, "skipped"))
            return 1;
    }
    return 0;
}
/* the embed, its description cut by whole lines, the last first, to what Discord takes: what it lists gives way, the
   fields stay. Takes description, fields and author */
static cJSON* embed(Cards* c, const char* title, const char* url, char* description, cJSON* fields, int color,
                    const char* footer, cJSON* author) {
    cJSON *e = cJSON_CreateObject(), *thumb, *f;
    char *t = rei_format("%.*s", (int)rei_bytes_of(title, 256), title), *picture, when[32];
    long room =
        EMBED - (long)rei_letters(t) - (long)rei_letters(S(footer)) - (long)rei_letters(S(jstr(author, "name")));
    cJSON_ArrayForEach(f, fields) room -= (long)(rei_letters(S(jstr(f, "name"))) + rei_letters(S(jstr(f, "value"))));
    if (room > DESCRIPTION) room = DESCRIPTION;
    if (room < 0) room = 0;
    while (rei_letters(description) > (size_t)room) {
        char* nl = strrchr(description, '\n');
        if (nl)
            *nl = 0;
        else
            description[rei_bytes_of(description, (size_t)room)] = 0;
    }
    cJSON_AddStringToObject(e, "title", t);
    cJSON_AddStringToObject(e, "url", url);
    cJSON_AddStringToObject(e, "description", description);
    cJSON_AddNumberToObject(e, "color", color);
    picture = rei_format(WIMSY, c->gh->repo);
    thumb = cJSON_AddObjectToObject(e, "thumbnail");
    cJSON_AddStringToObject(thumb, "url", picture);
    iso_of(rei_now(), when);
    cJSON_AddStringToObject(e, "timestamp", when);
    if (cJSON_GetArraySize(fields))
        cJSON_AddItemToObject(e, "fields", fields);
    else
        cJSON_Delete(fields);
    if (footer && *footer) cJSON_AddStringToObject(cJSON_AddObjectToObject(e, "footer"), "text", footer);
    if (author) cJSON_AddItemToObject(e, "author", author);
    free(t);
    free(picture);
    free(description);
    return e;
}
static cJSON* author_of(const cJSON* user) {
    cJSON* a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "name", S(jstr(user, "login")));
    cJSON_AddStringToObject(a, "url", S(jstr(user, "html_url")));
    if (jstr(user, "avatar_url")) cJSON_AddStringToObject(a, "icon_url", jstr(user, "avatar_url"));
    return a;
}
static int labeled(const cJSON* item, const char* name) {
    const cJSON* l;
    cJSON_ArrayForEach(
        l, cJSON_GetObjectItemCaseSensitive(item, "labels")) if (!strcmp(S(jstr(l, "name")), name)) return 1;
    return 0;
}
/* a commit message's first line, shown as it is; owned */
static char* subject(const char* message) {
    char *first = rei_format("%.*s", (int)strcspn(S(message), "\n"), S(message)), *shown = rei_escape(first);
    free(first);
    return shown;
}

cJSON* cd_pr(Cards* c, int number, int* open, double* closed_at) {
    char num[16], *url = rei_format(SERVER "/%s", c->gh->repo), *about, *links, *title;
    cJSON *pr, *commits, *comment, *fields = cJSON_CreateArray(), *e;
    const cJSON* head_obj;
    const char *head, *state;
    char** subjects;
    int n = 0, i, color = REI_CLOSED, draft;
    Buf b = {0};
    snprintf(num, sizeof num, "%d", number);
    c->failed = 0;
    *open = 0;
    *closed_at = 0;
    pr = get(c, "repos/%s/pulls/%s", c->gh->repo, num);
    if (!pr) {
        free(url);
        cJSON_Delete(fields);
        return NULL;
    }
    head_obj = cJSON_GetObjectItemCaseSensitive(pr, "head");
    head = S(jstr(head_obj, "sha"));
    state = S(jstr(pr, "state"));
    draft = jbool(pr, "draft");
    {
        char* path = rei_format("repos/%s/pulls/%d/commits", c->gh->repo, number);
        commits = pages(c, path, NULL);
        free(path);
    }
    subjects = malloc((size_t)(cJSON_GetArraySize(commits) + 1) * sizeof *subjects);
    for (i = cJSON_GetArraySize(commits) - 1; i >= 0; --i) /* the newest first */
        subjects[n++] =
            subject(jstr(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(commits, i), "commit"), "message"));
    comment = latest_comment(c, number, (int)jnum(pr, "comments"), pr);
    if (jbool(pr, "merged")) {
        const char* merged = S(jstr(pr, "merge_commit_sha"));
        buf_printf(&b, "**Merged** into main at [`%.7s`](%s/commit/%s): the next nightly has it.", merged, url, merged);
        color = REI_MERGED;
        *closed_at = seconds_of(jstr(pr, "closed_at"));
    } else if (!strcmp(state, "closed")) {
        buf_add(&b, "**Closed** without a merge.");
        *closed_at = seconds_of(jstr(pr, "closed_at"));
    } else {
        char* path = rei_format("repos/%s/compare/%s...%s", c->gh->repo,
                                S(jstr(cJSON_GetObjectItemCaseSensitive(pr, "base"), "sha")), head);
        cJSON *cmp = need(c, rg_get(c->gh, path, NULL)), *run = NULL, *jobs;
        const char* base = S(jstr(cJSON_GetObjectItemCaseSensitive(cmp, "merge_base_commit"), "sha"));
        const cJSON *r, *release, *current = NULL;
        char base7[8], tail[16], *fork_name, *pr_tag, *built;
        const char* its = "";
        Buf v = {0};
        free(path);
        *open = 1;
        snprintf(base7, sizeof base7, "%.7s", base);
        snprintf(tail, sizeof tail, "-%s", base7);
        cJSON_ArrayForEach(r, nightlies(c)) if (prefix(S(jstr(r, "tag_name")), "nightly-") &&
                                                suffix(S(jstr(r, "tag_name")), tail)) {
            its = S(jstr(r, "html_url"));
            break;
        }
        buf_printf(&b, "**%s** at [`%.7s`](%s/pull/%d/commits/%s), on main at [`%s`](%s/commit/%s)",
                   draft ? "Draft" : "Open", head, url, number, head, base7, url, base);
        if (*its) buf_printf(&b, " ([its nightly](%s))", its);
        buf_add(&b, ".");
        run = build_of(c, head);
        jobs = jobs_of(c, run);
        pr_tag = rei_format("pr-%d", number);
        release = release_tagged(c, pr_tag);
        free(pr_tag);
        if (release) {
            char* commits_of = rei_format("/commits/%s", head);
            if (strstr(S(jstr(release, "body")), commits_of)) current = release;
            free(commits_of);
        }
        fork_name = rei_strdup(S(jstr(cJSON_GetObjectItemCaseSensitive(head_obj, "repo"), "full_name")));
        built = rei_strdup(strcmp(fork_name, c->gh->repo) && !labeled(pr, LABEL) ? "waits for the team's label"
                                                                                 : "being published");
        packages(fields, jobs, current, built);
        /* the outcome that moves the card: published or failed, kept while its push's builds run again or are
           cancelled */
        if (current)
            snprintf(c->builds, sizeof c->builds, "published %s", head);
        else if (run && !strcmp(S(jstr(run, "status")), "completed") &&
                 (!strcmp(S(jstr(run, "conclusion")), "failure") || !strcmp(S(jstr(run, "conclusion")), "timed_out") ||
                  !strcmp(S(jstr(run, "conclusion")), "startup_failure")))
            snprintf(c->builds, sizeof c->builds, "failed %s", head);
        else {
            char* mine = rei_format(" %s", head);
            if (!suffix(c->builds, mine)) c->builds[0] = 0; /* another push's */
            free(mine);
        }
        if (!run)
            buf_add(&v, "Not started");
        else if (strcmp(S(jstr(run, "status")), "completed"))
            buf_printf(&v, "[Running](%s)", S(jstr(run, "html_url")));
        else {
            const char* conclusion = S(jstr(run, "conclusion"));
            buf_add(&v, "[");
            if (!strcmp(conclusion, "success"))
                buf_add(&v, "Passed");
            else
                capitalized(&v, conclusion);
            buf_printf(&v, "](%s)", S(jstr(run, "html_url")));
        }
        field(fields, "Build", v.text);
        buf_free(&v);
        if (current)
            buf_printf(&v, "[All files](%s)", S(jstr(current, "html_url")));
        else if (release)
            buf_printf(&v, "[The push before](%s)", S(jstr(release, "html_url")));
        else
            buf_add(&v, "None yet");
        field(fields, "Builds", v.text);
        buf_free(&v);
        color = draft ? REI_DRAFT : REI_OPEN;
        free(fork_name);
        free(built);
        cJSON_Delete(cmp);
        cJSON_Delete(run);
        cJSON_Delete(jobs);
    }
    links = linked(c, number, 1);
    about = meta(pr, NULL, links, "closes");
    if (*about) buf_printf(&b, "\n%s", about);
    if (n) {
        char* more = rei_format("%s/commits", S(jstr(pr, "html_url")));
        buf_add(&b, "\n\n**");
        plural(&b, n, "commit");
        buf_add(&b, "**, the newest first");
        listed(&b, subjects, n, more, MOST_COMMITS);
        free(more);
    }
    comment_lines(&b, comment);
    title = rei_format("PR #%d: %s", number, S(jstr(pr, "title")));
    e = embed(c, title, S(jstr(pr, "html_url")), buf_take(&b), fields, color, "Edited at each push, comment and build",
              author_of(cJSON_GetObjectItemCaseSensitive(pr, "user")));
    activity(c,
             jbool(pr, "merged")        ? "merged"
             : !strcmp(state, "closed") ? "closed"
             : draft                    ? "draft"
                                        : "open",
             comment, c->builds, head);
    for (i = 0; i < n; ++i) free(subjects[i]);
    free(subjects);
    free(title);
    free(about);
    free(links);
    free(url);
    cJSON_Delete(commits);
    cJSON_Delete(comment);
    cJSON_Delete(pr);
    if (c->failed) {
        cJSON_Delete(e);
        return NULL;
    }
    return e;
}

cJSON* cd_issue(Cards* c, int number, int* open, double* closed_at) {
    char num[16], *about, *links, *quoted, *title, *footer_text;
    cJSON *issue, *comment, *e;
    const char *reason, *kind;
    int color;
    Buf b = {0}, f = {0};
    snprintf(num, sizeof num, "%d", number);
    c->failed = 0;
    *open = 0;
    *closed_at = 0;
    issue = get(c, "repos/%s/issues/%s", c->gh->repo, num);
    if (!issue) return NULL;
    reason = jstr(issue, "state_reason");
    if (!strcmp(S(jstr(issue, "state")), "open")) {
        buf_add(&b, "**Open**");
        color = REI_OPEN;
        *open = 1;
    } else {
        *closed_at = seconds_of(jstr(issue, "closed_at"));
        if (reason && !strcmp(reason, "completed")) {
            buf_add(&b, "**Closed** as done");
            color = REI_DONE;
        } else {
            const char* r;
            buf_add(&b, "**Closed** as ");
            for (r = reason ? reason : "closed"; *r; ++r) buf_addn(&b, *r == '_' ? " " : r, 1);
            color = REI_NOT_PLANNED;
        }
    }
    kind = S(jstr(cJSON_GetObjectItemCaseSensitive(issue, "type"), "name"));
    links = linked(c, number, 0);
    about = meta(issue, kind, links, "fixed by");
    if (*about) buf_printf(&b, "\n%s", about);
    quoted = rei_excerpt(jstr(issue, "body"), MOST_TEXT);
    buf_printf(&b, "\n\n%s", *quoted ? quoted : "*No description.*");
    comment = latest_comment(c, number, (int)jnum(issue, "comments"), NULL);
    comment_lines(&b, comment);
    plural(&f, (int)jnum(issue, "comments"), "comment");
    buf_add(&f, ", edited at each change and comment");
    footer_text = buf_take(&f);
    title = rei_format("#%d: %s", number, S(jstr(issue, "title")));
    e = embed(c, title, S(jstr(issue, "html_url")), buf_take(&b), cJSON_CreateArray(), color, footer_text,
              author_of(cJSON_GetObjectItemCaseSensitive(issue, "user")));
    {
        char* state = rei_format("%s %s", S(jstr(issue, "state")), S(reason));
        activity(c, state, comment, "", "");
        free(state);
    }
    free(title);
    free(footer_text);
    free(quoted);
    free(about);
    free(links);
    cJSON_Delete(comment);
    cJSON_Delete(issue);
    if (c->failed) {
        cJSON_Delete(e);
        return NULL;
    }
    return e;
}

/* the first 40-hex run of text, or "" */
static void commit_in(const char* text, char out[41]) {
    const char* p;
    out[0] = 0;
    for (p = S(text); *p; ++p) {
        int n = 0;
        while (isxdigit((unsigned char)p[n]) && !isupper((unsigned char)p[n])) ++n;
        if (n >= 40) {
            memcpy(out, p, 40);
            out[40] = 0;
            return;
        }
        if (n) p += n - 1;
    }
}
/* a subject that is all of "Merge pull request #N from OWNER:BRANCH": *pr its N */
static int merge_of(const char* subject, unsigned* pr) {
    const char *p = subject, *digits;
    if (strncmp(p, "Merge pull request #", 20)) return 0;
    p += 20;
    digits = p;
    *pr = 0;
    while (isdigit((unsigned char)*p)) *pr = *pr * 10 + (unsigned)(*p++ - '0');
    if (p == digits || strncmp(p, " from ", 6)) return 0;
    p += 6;
    if (!*p) return 0;
    for (; *p; ++p)
        if (isspace((unsigned char)*p)) return 0;
    return 1;
}
/* What main got from previous to sha, newest first, as lines: a merged pull request by its title, any other commit by
   its subject (main's first parents, as git log --first-parent). Their count, or -1 when previous is no ancestor */
static int changes(Cards* c, const char* previous, const char* sha, char*** lines) {
    char* path = rei_format("repos/%s/compare/%s...%s", c->gh->repo, previous, sha);
    cJSON* cmp = need(c, rg_get(c->gh, path, NULL));
    const cJSON* commits = cJSON_GetObjectItemCaseSensitive(cmp, "commits");
    const char* status = S(jstr(cmp, "status"));
    char at[41];
    int n = 0, cap = 0;
    *lines = NULL;
    free(path);
    if (strcmp(status, "ahead")) {
        cJSON_Delete(cmp);
        return -1;
    }
    snprintf(at, sizeof at, "%s", sha);
    while (strcmp(at, previous)) {
        const cJSON *commit = NULL, *k;
        const char *message, *body;
        char *line, *first, *text;
        unsigned pr = 0;
        cJSON_ArrayForEach(k, commits) if (!strcmp(S(jstr(k, "sha")), at)) commit = k;
        if (!commit) break; /* past what the comparison lists */
        message = S(jstr(cJSON_GetObjectItemCaseSensitive(commit, "commit"), "message"));
        first = rei_format("%.*s", (int)strcspn(message, "\n"), message);
        body = message + strlen(first);
        if (merge_of(first, &pr)) {
            const char* p = body;
            text = NULL;
            while (*p) { /* the title: the body's first line with words */
                size_t len = strcspn(p, "\n");
                const char* s = p;
                while (s < p + len && isspace((unsigned char)*s)) ++s;
                if (s < p + len) {
                    const char* e = p + len;
                    while (e > s && isspace((unsigned char)e[-1])) --e;
                    text = rei_format("%.*s", (int)(e - s), s);
                    break;
                }
                p += len + (p[len] == '\n');
            }
            if (!text) text = rei_strdup(first);
        } else {
            pr = 0;
            text = rei_strdup(first);
        }
        {
            char* shown = rei_escape(text);
            line = pr ? rei_format("%s ([#%u](" SERVER "/%s/pull/%u))", shown, pr, c->gh->repo, pr)
                      : rei_format("%s ([`%.7s`](" SERVER "/%s/commit/%s))", shown, at, c->gh->repo, at);
            free(shown);
        }
        if (n == cap) {
            cap = cap ? 2 * cap : 16;
            *lines = realloc(*lines, (size_t)cap * sizeof **lines);
        }
        (*lines)[n++] = line;
        free(first);
        free(text);
        snprintf(at, sizeof at, "%s",
                 S(jstr(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(commit, "parents"), 0), "sha")));
    }
    cJSON_Delete(cmp);
    return n;
}
/* whether sha is an ancestor of newer, which is not sha */
static int ancestor(Cards* c, const char* sha, const char* newer) {
    char* path;
    cJSON* cmp;
    int is;
    if (!strcmp(sha, newer)) return 0;
    path = rei_format("repos/%s/compare/%s...%s", c->gh->repo, sha, newer);
    cmp = need(c, rg_get(c->gh, path, NULL));
    is = !strcmp(S(jstr(cmp, "status")), "ahead");
    free(path);
    cJSON_Delete(cmp);
    return is;
}
cJSON* cd_nightly(Cards* c, const char* sha) {
    char *url = rei_format(SERVER "/%s", c->gh->repo), *commit = rei_format("%s/commit/%s", url, sha), tail[16],
         previous[41] = "", date[11] = "", *link, *title, *footer;
    cJSON *head, *run, *jobs, *fields = cJSON_CreateArray(), *e;
    const cJSON *r, *own = NULL, *older_first = NULL;
    const char* built = "";
    int color = BUILDING, past_own = 0, n, i;
    char** lines;
    Buf b = {0};
    c->failed = 0;
    c->activity[0] = 0;
    head = get(c, "repos/%s/commits/%s", c->gh->repo, sha);
    snprintf(date, sizeof date, "%s", S(jstr(jpath(head, "commit.committer"), "date")));
    snprintf(tail, sizeof tail, "-%.7s", sha);
    nightlies(c);
    cJSON_ArrayForEach(r, c->nightlies) if (prefix(S(jstr(r, "tag_name")), "nightly-") &&
                                            suffix(S(jstr(r, "tag_name")), tail)) {
        own = r;
        break;
    }
    /* the nightlies before its own (all of them while it has none): the commit the newest of them was built from */
    cJSON_ArrayForEach(r, c->nightlies) {
        char from[41];
        if (!prefix(S(jstr(r, "tag_name")), "nightly-")) continue;
        if (own && !past_own) {
            past_own = r == own;
            continue;
        }
        if (!older_first) older_first = r;
        commit_in(jstr(r, "body"), from);
        if (*from && strcmp(from, sha)) {
            snprintf(previous, sizeof previous, "%s", from);
            break;
        }
    }
    run = build_of(c, sha);
    jobs = jobs_of(c, run);
    link = rei_strdup(commit);
    if (own) {
        buf_printf(&b, "Built from main at [`%.7s`](%s), committed %s.", sha, commit, date);
        free(link);
        link = rei_strdup(S(jstr(own, "html_url")));
        color = SCARLET;
    } else if (!run || strcmp(S(jstr(run, "status")), "completed"))
        buf_printf(&b, "Building main at [`%.7s`](%s), committed %s.", sha, commit, date);
    else if (strcmp(S(jstr(run, "conclusion")), "success")) {
        buf_printf(&b, "Not published: the build of [`%.7s`](%s) failed ([run](%s)).", sha, commit,
                   S(jstr(run, "html_url")));
        color = AMBER;
        built = "not published";
    } else if (*previous && older_first && ancestor(c, sha, previous)) {
        buf_printf(&b, "Not published: [%s](%s), of a newer commit, came first.", S(jstr(older_first, "name")),
                   S(jstr(older_first, "html_url")));
        color = CLOSED;
        built = "not published";
    } else {
        buf_printf(&b, "Built from main at [`%.7s`](%s), committed %s: being published.", sha, commit, date);
        built = "being published";
    }
    n = *previous ? changes(c, previous, sha, &lines) : -1;
    if (n > 0) {
        char* compare = rei_format("%s/compare/%s...%s", url, previous, sha);
        buf_printf(&b, "\n\n**Changes** ([`%.7s...%.7s`](%s))", previous, sha, compare);
        listed(&b, lines, n, compare, MOST_LINES);
        free(compare);
    }
    for (i = 0; i < n; ++i) free(lines[i]);
    if (n >= 0) free(lines);
    packages(fields, jobs, own, built);
    {
        char *source = rei_format("[`%.7s`](%s)", sha, commit),
             *every = rei_format("[%s](" SERVER "/%s/releases)", c->gh->nightly, c->gh->nightly);
        field(fields, "Source", source);
        field(fields, "Every nightly", every);
        free(source);
        free(every);
    }
    if (failed_jobs(jobs) && !own) color = AMBER;
    title = rei_format("Nightly %s (%.7s)", date, sha);
    footer = rei_format("The newest %d nightlies are kept", CD_KEEP);
    e = embed(c, title, link, buf_take(&b), fields, color, footer, NULL);
    free(title);
    free(footer);
    free(link);
    free(url);
    free(commit);
    cJSON_Delete(head);
    cJSON_Delete(run);
    cJSON_Delete(jobs);
    if (c->failed) {
        cJSON_Delete(e);
        return NULL;
    }
    return e;
}

/* ---- the hash of what a card shows ---- */
void cd_hash(const cJSON* embed, char out[17]) {
    cJSON* shown = cJSON_Duplicate(embed, 1);
    char* text;
    cJSON_DeleteItemFromObjectCaseSensitive(shown, "timestamp");
    text = cJSON_PrintUnformatted(shown);
    fnv(text, out);
    free(text);
    cJSON_Delete(shown);
}

/* ---- the rounds ---- */
void cd_init(Cards* c, Rg* gh, CdDiscord dc, u64 prs, u64 issues, u64 nightly) {
    memset(c, 0, sizeof *c);
    c->gh = gh;
    c->dc = dc;
    c->prs = prs;
    c->issues = issues;
    c->nightly = nightly;
}
void cd_free(Cards* c) {
    free(c->cards);
    free(c->releases);
    cJSON_Delete(c->nightlies);
    memset(c, 0, sizeof *c);
}
static CdCard* card_of(Cards* c, const char* key, int make) {
    int i;
    for (i = 0; i < c->ncards; ++i)
        if (!strcmp(c->cards[i].key, key)) return &c->cards[i];
    if (!make) return NULL;
    if (c->ncards == c->capcards) {
        int cap = c->capcards ? 2 * c->capcards : 64;
        CdCard* more = realloc(c->cards, (size_t)cap * sizeof *more);
        if (!more) abort();
        c->cards = more;
        c->capcards = cap;
    }
    memset(&c->cards[c->ncards], 0, sizeof *c->cards);
    snprintf(c->cards[c->ncards].key, sizeof c->cards[c->ncards].key, "%s", key);
    return &c->cards[c->ncards++];
}
static void dirty(Cards* c, const char* key) { card_of(c, key, 1)->dirty = 1; }
static u64 channel_of(const Cards* c, const char* key) {
    return prefix(key, "pr-") ? c->prs : prefix(key, "issue-") ? c->issues : c->nightly;
}
static int kept(const Cards* c, u64 message) {
    int i;
    for (i = 0; i < c->ncards; ++i)
        if (c->cards[i].message == message) return 1;
    return 0;
}

/* the first round on a channel: the cards a webhook posted there, and our messages no card keeps, deleted */
static void purge(Cards* c, u64 channel, int* done) {
    u64 before = 0, ids[100];
    int webhook[100], mine[100], seen = 0, deleted = 0, n, i;
    if (!channel || *done) return;
    do {
        n = c->dc.history(c->dc.ctx, channel, before, ids, webhook, mine, 100);
        if (n < 0) {
            rei_log("channel %llu: its messages could not be read: its old cards are deleted next round",
                    (unsigned long long)channel);
            return;
        }
        for (i = 0; i < n; ++i) {
            int said;
            if (!webhook[i] && !(mine[i] && !kept(c, ids[i]))) continue;
            said = c->dc.remove(c->dc.ctx, channel, ids[i]);
            if (!said || said == 404)
                ++deleted;
            else
                rei_log("channel %llu: message %llu could not be deleted: Discord answered %d (Manage Messages?)",
                        (unsigned long long)channel, (unsigned long long)ids[i], said);
        }
        seen += n;
        if (n) before = ids[n - 1];
    } while (n == 100 && seen < CD_HISTORY);
    *done = 1;
    rei_log("channel %llu: %d old cards deleted of the %d newest messages", (unsigned long long)channel, deleted, seen);
}

/* a pull request's or an issue's card key, when its channel is set; "" otherwise */
static void key_of(const Cards* c, const cJSON* item, char key[56]) {
    int pr = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(item, "pull_request"));
    key[0] = 0;
    if (pr ? c->prs : c->issues) snprintf(key, 56, "%s-%d", pr ? "pr" : "issue", (int)jnum(item, "number"));
}
static int mark_listing(Cards* c, const char* path) {
    cJSON* items = rg_pages(c->gh, path, NULL);
    const cJSON* item;
    if (!items) return 0;
    cJSON_ArrayForEach(item, items) {
        char key[56];
        key_of(c, item, key);
        if (*key) dirty(c, key);
    }
    cJSON_Delete(items);
    return 1;
}
/* the issues and pull requests updated since the last round; the first, every open one: a card for one already closed
   would only wait for its day to be deleted */
static void mark_updated(Cards* c, double start) {
    char* path;
    int ok;
    if (!c->prs && !c->issues) return;
    if (!*c->since)
        path = rei_format("repos/%s/issues?state=open", c->gh->repo);
    else
        path = rei_format("repos/%s/issues?state=all&sort=updated&direction=desc&since=%s", c->gh->repo, c->since);
    ok = mark_listing(c, path);
    free(path);
    if (ok) iso_of(start - 60, c->since); /* a minute back: GitHub's clock and ours may differ */
}

static int by_job(const void* a, const void* b) { return strcmp(*(const char* const*)a, *(const char* const*)b); }
/* a run's state and its jobs', as a hash */
static void run_sig(const cJSON* run, const cJSON* jobs, char out[17]) {
    int n = cJSON_GetArraySize(jobs), i = 0;
    char** each = malloc((size_t)(n ? n : 1) * sizeof *each);
    const cJSON* j;
    cJSON* all = cJSON_CreateArray();
    cJSON_AddItemToArray(all, cJSON_CreateString(S(jstr(run, "status"))));
    cJSON_AddItemToArray(all, cJSON_CreateString(S(jstr(run, "conclusion"))));
    cJSON_ArrayForEach(j, jobs) each[i++] =
        rei_format("%s|%s|%s", S(jstr(j, "name")), S(jstr(j, "status")), S(jstr(j, "conclusion")));
    qsort(each, (size_t)n, sizeof *each, by_job);
    for (i = 0; i < n; ++i) {
        cJSON_AddItemToArray(all, cJSON_CreateString(each[i]));
        free(each[i]);
    }
    free(each);
    {
        cJSON* wrap = cJSON_CreateObject();
        cJSON_AddItemToObject(wrap, "run", all);
        cd_hash(wrap, out);
        cJSON_Delete(wrap);
    }
}
/* the card a Build run's packages go on: pr-N or nightly-SHA, "" for none */
static void target_of(Cards* c, const cJSON* run, char key[56]) {
    const char *event = S(jstr(run, "event")), *sha = S(jstr(run, "head_sha"));
    const cJSON* pulls = cJSON_GetObjectItemCaseSensitive(run, "pull_requests");
    key[0] = 0;
    if (!strcmp(event, "push") && !strcmp(S(jstr(run, "head_branch")), "main") &&
        !strcmp(S(jstr(cJSON_GetObjectItemCaseSensitive(run, "head_repository"), "full_name")), c->gh->repo))
        snprintf(key, 56, "nightly-%s", sha);
    else if (!strcmp(event, "pull_request")) {
        if (cJSON_GetArraySize(pulls))
            snprintf(key, 56, "pr-%d", (int)jnum(cJSON_GetArrayItem(pulls, 0), "number"));
        else { /* a fork's run names none */
            char* path = rei_format("repos/%s/pulls?state=open", c->gh->repo);
            cJSON* open = rg_pages(c->gh, path, NULL);
            const cJSON* p;
            free(path);
            cJSON_ArrayForEach(p, open) if (!strcmp(S(jstr(cJSON_GetObjectItemCaseSensitive(p, "head"), "sha")), sha)) {
                snprintf(key, 56, "pr-%d", (int)jnum(p, "number"));
                break;
            }
            cJSON_Delete(open);
        }
    }
}
/* the builds whose jobs changed since the last round: their cards */
static void mark_builds(Cards* c, int first) {
    char* path = rei_format("repos/%s/actions/workflows/build.yml/runs?per_page=%d", c->gh->repo, CD_RUNS);
    cJSON* got = rg_get(c->gh, path, NULL);
    const cJSON* run;
    CdRun now[CD_RUNS];
    int n = 0, i;
    free(path);
    if (!got) return;
    cJSON_ArrayForEach(run, cJSON_GetObjectItemCaseSensitive(got, "workflow_runs")) {
        long long id = (long long)jnum(run, "id");
        const char* before = NULL;
        char sig[17], key[56];
        cJSON* jobs;
        if (n == CD_RUNS) break;
        for (i = 0; i < c->nruns; ++i)
            if (c->runs[i].id == id) before = c->runs[i].sig;
        /* a run that ended before the last round has nothing new */
        if (before && !strcmp(S(jstr(run, "status")), "completed") && before[0] == 'Z') {
            now[n].id = id;
            snprintf(now[n++].sig, sizeof now->sig, "%s", before);
            continue;
        }
        jobs = jobs_of(c, run);
        run_sig(run, jobs, sig);
        cJSON_Delete(jobs);
        if (!strcmp(S(jstr(run, "status")), "completed")) sig[0] = 'Z'; /* ended, no hex digit: never looked at again */
        if (!first && (!before || strcmp(before, sig))) {
            target_of(c, run, key);
            if (*key && channel_of(c, key) && (prefix(key, "pr-") || card_of(c, key, 0))) dirty(c, key);
        }
        now[n].id = id;
        snprintf(now[n++].sig, sizeof now->sig, "%s", sig);
    }
    memcpy(c->runs, now, (size_t)n * sizeof *now);
    c->nruns = n;
    cJSON_Delete(got);
}

/* the nightly releases changed since the last round: their pull requests' and nightlies' cards */
static void mark_releases(Cards* c, int first) {
    const cJSON* r;
    CdRelease* now;
    int n = 0, i;
    if (!nightlies(c)) return;
    now = malloc((size_t)(cJSON_GetArraySize(c->nightlies) + 1) * sizeof *now);
    cJSON_ArrayForEach(r, c->nightlies) {
        const char *tag = S(jstr(r, "tag_name")), *before = NULL;
        cJSON* shown = cJSON_CreateObject();
        const cJSON* a;
        cJSON* assets = cJSON_AddArrayToObject(shown, "assets");
        char sig[17];
        cJSON_AddStringToObject(shown, "published_at", S(jstr(r, "published_at")));
        cJSON_AddStringToObject(shown, "body", S(jstr(r, "body")));
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(r, "assets")) {
            char* one = rei_format("%s %.0f", S(jstr(a, "name")), jnum(a, "size"));
            cJSON_AddItemToArray(assets, cJSON_CreateString(one));
            free(one);
        }
        cd_hash(shown, sig);
        cJSON_Delete(shown);
        for (i = 0; i < c->nreleases; ++i)
            if (!strcmp(c->releases[i].tag, tag)) before = c->releases[i].sig;
        if (!first && (!before || strcmp(before, sig))) {
            char key[56];
            if (prefix(tag, "pr-") && c->prs) {
                snprintf(key, sizeof key, "%s", tag);
                if (card_of(c, key, 0)) dirty(c, key);
            } else if (prefix(tag, "nightly-")) {
                const char* sha7 = strrchr(tag, '-') + 1;
                for (i = 0; i < c->ncards; ++i)
                    if (prefix(c->cards[i].key, "nightly-") && prefix(c->cards[i].key + 8, sha7)) c->cards[i].dirty = 1;
            }
        }
        snprintf(now[n].tag, sizeof now->tag, "%s", tag);
        snprintf(now[n++].sig, sizeof now->sig, "%s", sig);
    }
    free(c->releases);
    c->releases = now;
    c->nreleases = c->capreleases = n;
}

/* main's new commits since the last round, its first parents, each a nightly's card (the newest ten at most) */
static void mark_head(Cards* c) {
    char* path;
    cJSON* got;
    const char* sha;
    if (!c->nightly) return;
    path = rei_format("repos/%s/commits/main", c->gh->repo);
    got = rg_get(c->gh, path, NULL);
    free(path);
    sha = S(jstr(got, "sha"));
    if (strlen(sha) == 40 && *c->head && strcmp(sha, c->head)) {
        char* cmp_path = rei_format("repos/%s/compare/%s...%s", c->gh->repo, c->head, sha);
        cJSON* cmp = rg_get(c->gh, cmp_path, NULL);
        const cJSON* commits = cJSON_GetObjectItemCaseSensitive(cmp, "commits");
        char at[41], key[56];
        int made = 0;
        free(cmp_path);
        snprintf(at, sizeof at, "%s", sha);
        while (strcmp(at, c->head) && made < 10) {
            const cJSON *commit = NULL, *k;
            cJSON_ArrayForEach(k, commits) if (!strcmp(S(jstr(k, "sha")), at)) commit = k;
            snprintf(key, sizeof key, "nightly-%s", at);
            dirty(c, key);
            ++made;
            if (!commit) break; /* not in the comparison (main forced back): only the newest */
            snprintf(at, sizeof at, "%s",
                     S(jstr(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(commit, "parents"), 0), "sha")));
        }
        cJSON_Delete(cmp);
    }
    if (strlen(sha) == 40) snprintf(c->head, sizeof c->head, "%s", sha);
    cJSON_Delete(got);
}

/* a card's message from before its move deleted: 1 when it is gone */
static int unstale(Cards* c, CdCard* card, u64 channel) {
    int said = c->dc.remove(c->dc.ctx, channel, card->stale);
    if (said && said != 404) {
        rei_log("%s: message %llu, the one before its move, not deleted: Discord answered %d: tried again next round",
                card->key, (unsigned long long)card->stale, said);
        return 0;
    }
    card->stale = 0;
    return 1;
}
/* each card to draw, posted, edited, or left as it is */
static void draw(Cards* c, CdCard* card, double now) {
    u64 channel = channel_of(c, card->key);
    int open = 1, said;
    double closed_at = 0;
    char hash[17];
    cJSON* e;
    if (!channel) {
        card->dirty = 0;
        return;
    }
    if (prefix(card->key, "pr-")) {
        snprintf(c->builds, sizeof c->builds, "%s", card->builds);
        e = cd_pr(c, atoi(card->key + 3), &open, &closed_at);
        if (e) snprintf(card->builds, sizeof card->builds, "%s", c->builds);
    } else if (prefix(card->key, "issue-"))
        e = cd_issue(c, atoi(card->key + 6), &open, &closed_at);
    else
        e = cd_nightly(c, card->key + 8);
    if (!e) {
        rei_log("%s: not drawn, GitHub did not give all it shows: tried again next round", card->key);
        return;
    }
    if (open) {
        card->closed = 0;
        card->gone = 0;
    } else if (!card->closed)
        card->closed = closed_at ? closed_at : now;
    cd_hash(e, hash);
    if (card->stale && !unstale(c, card, channel)) {
        cJSON_Delete(e);
        return;
    }
    if (!*card->moved) /* seen first: what moves it from now on */
        snprintf(card->moved, sizeof card->moved, "%s", c->activity);
    if (card->message && strcmp(c->activity, card->moved)) {
        /* to the end of its channel: the new one posted before the old one is deleted, so it is never missing */
        u64 id = c->dc.post(c->dc.ctx, channel, e);
        if (!id) {
            rei_log("%s: not moved to the end, not posted: tried again next round", card->key);
            cJSON_Delete(e);
            return;
        }
        card->stale = card->message;
        card->message = id;
        snprintf(card->moved, sizeof card->moved, "%s", c->activity);
        snprintf(card->drawn, sizeof card->drawn, "%s", hash);
        unstale(c, card, channel);
    }
    if (card->message && strcmp(hash, card->drawn)) {
        said = c->dc.edit(c->dc.ctx, channel, card->message, e);
        if (said == 404) /* deleted on Discord: a new one, while it may have one */
            card->message = 0;
        else if (said) {
            rei_log("%s: message %llu not edited: Discord answered %d: tried again next round", card->key,
                    (unsigned long long)card->message, said);
            cJSON_Delete(e);
            return;
        }
    }
    if (!card->message && (open || (!card->gone && now - card->closed < CD_DAY))) {
        u64 id = c->dc.post(c->dc.ctx, channel, e);
        if (!id) {
            rei_log("%s: not posted: tried again next round", card->key);
            cJSON_Delete(e);
            return;
        }
        card->message = id;
        snprintf(card->moved, sizeof card->moved, "%s", c->activity);
    }
    snprintf(card->drawn, sizeof card->drawn, "%s", hash);
    card->dirty = 0;
    cJSON_Delete(e);
}
/* the cards of pull requests and issues closed over a day ago deleted; what is left of a week ago forgotten */
static void sweep(Cards* c, double now) {
    int i, nightly = 0;
    for (i = 0; i < c->ncards; ++i) {
        CdCard* card = &c->cards[i];
        if (card->stale && !card->dirty) unstale(c, card, channel_of(c, card->key)); /* a dirty one: by its draw */
        if (card->closed && card->message && now - card->closed >= CD_DAY) {
            int said = c->dc.remove(c->dc.ctx, channel_of(c, card->key), card->message);
            if (!said || said == 404) {
                card->message = 0;
                card->gone = 1;
            } else
                rei_log("%s: message %llu not deleted: Discord answered %d: tried again next round", card->key,
                        (unsigned long long)card->message, said);
        }
    }
    for (i = c->ncards - 1; i >= 0; --i) {
        CdCard* card = &c->cards[i];
        int forget = card->closed && !card->message && now - card->closed >= CD_DAY + 7 * 86400;
        if (prefix(card->key, "nightly-") && !card->dirty && ++nightly > 2 * CD_KEEP) forget = 1;
        if (forget) c->cards[i] = c->cards[--c->ncards];
    }
}

void cd_round(Cards* c) {
    double start = rei_now();
    int first = !*c->since && !c->nruns && !c->nreleases, i;
    cJSON_Delete(c->nightlies);
    c->nightlies = NULL;
    purge(c, c->prs, &c->purged_prs);
    purge(c, c->issues, &c->purged_issues);
    mark_updated(c, start);
    mark_builds(c, first);
    mark_releases(c, first);
    mark_head(c);
    for (i = 0; i < c->ncards; ++i)
        if (c->cards[i].dirty) draw(c, &c->cards[i], start);
    sweep(c, start);
}

cJSON* cd_save(const Cards* c) {
    cJSON *saved = cJSON_CreateObject(), *cards = cJSON_AddArrayToObject(saved, "cards"),
          *runs = cJSON_AddArrayToObject(saved, "runs"), *releases = cJSON_AddArrayToObject(saved, "releases");
    int i;
    for (i = 0; i < c->ncards; ++i) {
        const CdCard* k = &c->cards[i];
        cJSON* one = cJSON_CreateObject();
        char id[24];
        snprintf(id, sizeof id, "%llu", (unsigned long long)k->message);
        cJSON_AddStringToObject(one, "key", k->key);
        cJSON_AddStringToObject(one, "message", id);
        cJSON_AddStringToObject(one, "drawn", k->drawn);
        cJSON_AddStringToObject(one, "moved", k->moved);
        cJSON_AddStringToObject(one, "builds", k->builds);
        snprintf(id, sizeof id, "%llu", (unsigned long long)k->stale);
        cJSON_AddStringToObject(one, "stale", id);
        cJSON_AddNumberToObject(one, "closed", k->closed);
        cJSON_AddBoolToObject(one, "gone", k->gone);
        cJSON_AddBoolToObject(one, "dirty", k->dirty);
        cJSON_AddItemToArray(cards, one);
    }
    for (i = 0; i < c->nruns; ++i) {
        cJSON* one = cJSON_CreateObject();
        char id[24];
        snprintf(id, sizeof id, "%lld", c->runs[i].id);
        cJSON_AddStringToObject(one, "id", id);
        cJSON_AddStringToObject(one, "sig", c->runs[i].sig);
        cJSON_AddItemToArray(runs, one);
    }
    for (i = 0; i < c->nreleases; ++i) {
        cJSON* one = cJSON_CreateObject();
        cJSON_AddStringToObject(one, "tag", c->releases[i].tag);
        cJSON_AddStringToObject(one, "sig", c->releases[i].sig);
        cJSON_AddItemToArray(releases, one);
    }
    cJSON_AddStringToObject(saved, "since", c->since);
    cJSON_AddStringToObject(saved, "head", c->head);
    cJSON_AddBoolToObject(saved, "purged_prs", c->purged_prs);
    cJSON_AddBoolToObject(saved, "purged_issues", c->purged_issues);
    return saved;
}
void cd_load(Cards* c, const cJSON* saved) {
    const cJSON* one;
    cJSON_ArrayForEach(one, cJSON_GetObjectItemCaseSensitive(saved, "cards")) {
        CdCard* k;
        if (!jstr(one, "key")) continue;
        k = card_of(c, jstr(one, "key"), 1);
        k->message = strtoull(S(jstr(one, "message")), NULL, 10);
        snprintf(k->drawn, sizeof k->drawn, "%s", S(jstr(one, "drawn")));
        snprintf(k->moved, sizeof k->moved, "%s", S(jstr(one, "moved")));
        snprintf(k->builds, sizeof k->builds, "%s", S(jstr(one, "builds")));
        k->stale = strtoull(S(jstr(one, "stale")), NULL, 10);
        k->closed = jnum(one, "closed");
        k->gone = jbool(one, "gone");
        /* a card kept before its activity was: drawn again once, to learn it, so its next activity moves it */
        k->dirty = jbool(one, "dirty") || (k->message && !*k->moved && !prefix(k->key, "nightly-"));
    }
    cJSON_ArrayForEach(one, cJSON_GetObjectItemCaseSensitive(saved, "runs")) {
        if (c->nruns == CD_RUNS || !jstr(one, "id")) continue;
        c->runs[c->nruns].id = strtoll(jstr(one, "id"), NULL, 10);
        snprintf(c->runs[c->nruns++].sig, sizeof c->runs->sig, "%s", S(jstr(one, "sig")));
    }
    cJSON_ArrayForEach(one, cJSON_GetObjectItemCaseSensitive(saved, "releases")) {
        if (!jstr(one, "tag")) continue;
        if (c->nreleases == c->capreleases) {
            int cap = c->capreleases ? 2 * c->capreleases : 32;
            CdRelease* more = realloc(c->releases, (size_t)cap * sizeof *more);
            if (!more) abort();
            c->releases = more;
            c->capreleases = cap;
        }
        snprintf(c->releases[c->nreleases].tag, sizeof c->releases->tag, "%s", jstr(one, "tag"));
        snprintf(c->releases[c->nreleases++].sig, sizeof c->releases->sig, "%s", S(jstr(one, "sig")));
    }
    snprintf(c->since, sizeof c->since, "%s", S(jstr(saved, "since")));
    snprintf(c->head, sizeof c->head, "%s", S(jstr(saved, "head")));
    c->purged_prs = jbool(saved, "purged_prs");
    c->purged_issues = jbool(saved, "purged_issues");
}
