/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "sync.h"
#include "git.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the two APIs ---- */
/* a request's JSON answer (owned), or NULL; *status its HTTP status. A failure (no answer, or a status but want's and
   404 when missing is asked for) is said with the method, the URL and the start of what came */
static cJSON* request(Aw* a, const char* method, const char* url, const char* auth, const cJSON* body, int* status) {
    AwAnswer got;
    char* text = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON* json;
    a->fetch(a->ctx, method, url, auth, text, &got);
    free(text);
    if (strcmp(method, "GET")) a->writes++;
    *status = got.status;
    if (got.error) {
        aw_warn("%s", got.error);
        aw_answer_free(&got);
        return NULL;
    }
    json = got.body ? cJSON_Parse(got.body) : NULL;
    if (got.status >= 300 && got.status != 404)
        aw_warn("%s %s: HTTP %d: %.200s", method, url, got.status, got.body ? got.body : "");
    aw_answer_free(&got);
    return json;
}
static char* github_auth(const Aw* a) { return aw_format("Bearer %s", a->config->github_token); }
static char* forgejo_auth(const Aw* a) { return aw_format("token %s", a->config->forgejo_token); }
/* GET <github api>/<path>: 2xx's JSON, else NULL */
static cJSON* github(Aw* a, const char* path, int* status) {
    char *url = aw_format("%s/%s", a->config->github_api, path), *auth = github_auth(a);
    cJSON* json = request(a, "GET", url, auth, NULL, status);
    free(url);
    free(auth);
    if (*status < 200 || *status >= 300) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}
/* every page of a GitHub list, path ending in '?' or holding one, 100 an answer: one array, or NULL */
static cJSON* github_all(Aw* a, const char* path) {
    cJSON* all = cJSON_CreateArray();
    int page, status;
    for (page = 1;; ++page) {
        char* paged = aw_format("%s%sper_page=100&page=%d", path, strchr(path, '?')[1] ? "&" : "", page);
        cJSON *got = github(a, paged, &status), *item;
        int n = 0;
        free(paged);
        if (!cJSON_IsArray(got)) {
            aw_warn("GET %s: no list came (HTTP %d)", path, status);
            cJSON_Delete(got);
            cJSON_Delete(all);
            return NULL;
        }
        while ((item = cJSON_DetachItemFromArray(got, 0)) != NULL) {
            cJSON_AddItemToArray(all, item);
            ++n;
        }
        cJSON_Delete(got);
        if (n < 100) return all;
    }
}
/* method <forgejo>/api/v1/<path> with body: its JSON, NULL when status is not want */
static cJSON* forgejo(Aw* a, const char* method, const char* path, const cJSON* body, int want, int* status) {
    char *url = aw_format("%s/api/v1/%s", a->config->forgejo, path), *auth = forgejo_auth(a);
    int got;
    cJSON* json = request(a, method, url, auth, body, &got);
    free(url);
    free(auth);
    if (status) *status = got;
    if (got != want) {
        if (got == 404 && status) { /* asked for: the caller looks at it */
        } else if (got && got < 300)
            aw_warn("%s %s: HTTP %d where %d was due", method, path, got, want);
        cJSON_Delete(json);
        return NULL;
    }
    return json ? json : cJSON_CreateObject();
}
/* every page of a Forgejo list, 50 an answer */
static cJSON* forgejo_all(Aw* a, const char* path) {
    cJSON* all = cJSON_CreateArray();
    int page;
    for (page = 1;; ++page) {
        char* paged = aw_format("%s%slimit=50&page=%d", path, strchr(path, '?') ? "&" : "?", page);
        cJSON *got = forgejo(a, "GET", paged, NULL, 200, NULL), *item;
        int n = 0;
        free(paged);
        if (!cJSON_IsArray(got)) {
            cJSON_Delete(got);
            cJSON_Delete(all);
            return NULL;
        }
        while ((item = cJSON_DetachItemFromArray(got, 0)) != NULL) {
            cJSON_AddItemToArray(all, item);
            ++n;
        }
        cJSON_Delete(got);
        if (n < 50) return all;
    }
}

/* ---- state.json: { "repos": { NAME: { "next", "issues_since", "comments_since", "seen", "comments", "pulls", "gone",
   "ok", "missing" } } }; "pulls": the numbers made as pull requests, each with the state it was left in here: "open",
   "closed", "merged"; "gone": each ref the Forgejo has and GitHub has no more, with when it was first seen so; "ok":
   when the repository's last pass went through; "missing": since when GitHub has not listed it */
static cJSON* child(cJSON* o, const char* key) {
    cJSON* c = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsObject(c)) {
        cJSON_DeleteItemFromObjectCaseSensitive(o, key);
        cJSON_AddItemToObject(o, key, c = cJSON_CreateObject());
    }
    return c;
}
static void set_string(cJSON* o, const char* key, const char* value) {
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
    cJSON_AddStringToObject(o, key, value);
}
static void set_number(cJSON* o, const char* key, double value) {
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
    cJSON_AddNumberToObject(o, key, value);
}
static int save(Aw* a) {
    char *text = cJSON_PrintUnformatted(a->state), *temporary = aw_format("%s.new", a->state_path);
    FILE* f = fopen(temporary, "wb");
    int failed = !f || fputs(text, f) == EOF;
    if (f && fclose(f)) failed = 1;
    if (!failed && rename(temporary, a->state_path)) failed = 1;
    if (failed) aw_warn("%s: cannot be written: the next pass makes again what this one made", a->state_path);
    free(temporary);
    free(text);
    return failed;
}

/* ---- the repository and its labels ---- */
static int ensure_repo(Aw* a, const cJSON* repo) {
    const char* name = jstr(repo, "name");
    char* path = aw_format("repos/%s/%s", a->config->forgejo_org, name);
    int status, rc = 0;
    cJSON* got = forgejo(a, "GET", path, NULL, 200, &status);
    if (!got && status == 404) {
        cJSON* make = cJSON_CreateObject();
        char* orgs = aw_format("orgs/%s/repos", a->config->forgejo_org);
        cJSON_AddStringToObject(make, "name", name);
        cJSON_AddBoolToObject(make, "private", jbool(repo, "private"));
        cJSON_AddStringToObject(make, "description", jstr(repo, "description") ? jstr(repo, "description") : "");
        got = forgejo(a, "POST", orgs, make, 201, NULL);
        if (got) aw_log("%s: made on the Forgejo", name);
        cJSON_Delete(make);
        free(orgs);
    }
    rc = !got;
    cJSON_Delete(got);
    free(path);
    return rc;
}
/* what the repository says on GitHub, its Releases tab off (releases stay on GitHub), its pull requests on and a pull
   request markable as merged by a commit of its own (GitHub's merge commit, AW_PULL_BRANCH's pull requests) */
static int patch_repo(Aw* a, const cJSON* repo) {
    const char *name = jstr(repo, "name"), *branch = jstr(repo, "default_branch");
    char* path = aw_format("repos/%s/%s", a->config->forgejo_org, name);
    cJSON *edit = cJSON_CreateObject(), *got;
    cJSON_AddBoolToObject(edit, "has_releases", 0);
    cJSON_AddBoolToObject(edit, "has_pull_requests", 1);
    cJSON_AddBoolToObject(edit, "allow_manual_merge", 1);
    cJSON_AddBoolToObject(edit, "private", jbool(repo, "private"));
    cJSON_AddStringToObject(edit, "description", jstr(repo, "description") ? jstr(repo, "description") : "");
    cJSON_AddStringToObject(edit, "website", jstr(repo, "html_url") ? jstr(repo, "html_url") : "");
    if (branch && aw_has_branch(a->config, name, branch)) cJSON_AddStringToObject(edit, "default_branch", branch);
    got = forgejo(a, "PATCH", path, edit, 200, NULL);
    cJSON_Delete(edit);
    free(path);
    if (!got) return 1;
    cJSON_Delete(got);
    return 0;
}
/* GitHub's labels on the Forgejo, made or edited where they differ: name -> the Forgejo's id, in ids */
static int labels(Aw* a, const char* name, cJSON* ids) {
    char *gh_path = aw_format("repos/%s/%s/labels?", a->config->org, name),
         *fj_path = aw_format("repos/%s/%s/labels", a->config->forgejo_org, name);
    cJSON *theirs = github_all(a, gh_path), *ours = theirs ? forgejo_all(a, fj_path) : NULL, *l, *o;
    int rc = !theirs || !ours;
    for (l = theirs && ours ? theirs->child : NULL; l && !rc; l = l->next) {
        const char *label = jstr(l, "name"), *color = jstr(l, "color"), *about = jstr(l, "description");
        char* hex = aw_format("#%s", color ? color : "ededed");
        for (o = ours->child; o && strcmp(jstr(o, "name") ? jstr(o, "name") : "", label); o = o->next) continue;
        if (!o || strcmp(jstr(o, "color") ? jstr(o, "color") : "", hex) ||
            strcmp(jstr(o, "description") ? jstr(o, "description") : "", about ? about : "")) {
            cJSON *want = cJSON_CreateObject(), *got;
            char* at = o ? aw_format("%s/%.0f", fj_path, jnum(o, "id")) : NULL;
            cJSON_AddStringToObject(want, "name", label);
            cJSON_AddStringToObject(want, "color", hex);
            cJSON_AddStringToObject(want, "description", about ? about : "");
            got = o ? forgejo(a, "PATCH", at, want, 200, NULL) : forgejo(a, "POST", fj_path, want, 201, NULL);
            if (got) cJSON_AddNumberToObject(ids, label, jnum(got, "id"));
            rc = !got;
            cJSON_Delete(got);
            cJSON_Delete(want);
            free(at);
        } else
            cJSON_AddNumberToObject(ids, label, jnum(o, "id"));
        free(hex);
    }
    cJSON_Delete(theirs);
    cJSON_Delete(ours);
    free(gh_path);
    free(fj_path);
    return rc;
}

/* ---- issues and pull requests ---- */
static void day(const char* stamp, char out[11]) { snprintf(out, 11, "%.10s", stamp ? stamp : ""); }
/* t (seconds since the epoch) as format, in UTC, into out */
static void when_of(long t, const char* format, char* out, size_t size) {
    time_t at = (time_t)t;
    struct tm tm;
    if (!gmtime_r(&at, &tm) || !strftime(out, size, format, &tm)) snprintf(out, size, "%ld", t);
}
/* GitHub's pull request part of an issue of its list, NULL for an issue */
static const cJSON* pull_request(const cJSON* issue) { return cJSON_GetObjectItemCaseSensitive(issue, "pull_request"); }
/* what GitHub says of a pull request: "merged", "open" or "closed" */
static const char* pull_state(const cJSON* issue) {
    return jstr(pull_request(issue), "merged_at")                                ? "merged"
           : !strcmp(jstr(issue, "state") ? jstr(issue, "state") : "", "open") ? "open"
                                                                                : "closed";
}
/* the text on the Forgejo: where it is from, then GitHub's body. A pull request (pull, GitHub's) says its branches and
   its state; one made as an issue (note: why) says why, and where its changes are */
static char* issue_body(const cJSON* issue, const cJSON* pull, const char* note) {
    const char *who = jstr(cJSON_GetObjectItemCaseSensitive(issue, "user"), "login"), *body = jstr(issue, "body");
    const char* url = jstr(issue, "html_url");
    char when[11];
    Buf b = {0};
    day(jstr(issue, "created_at"), when);
    if (pull_request(issue)) {
        const char *head = pull ? jstr(cJSON_GetObjectItemCaseSensitive(pull, "head"), "label") : NULL,
                   *base = pull ? jstr(cJSON_GetObjectItemCaseSensitive(pull, "base"), "ref") : NULL;
        buf_printf(&b, "> From GitHub: pull request [#%.0f](%s) by **@%s**, %s, from `%s` into `%s`, %s.",
                   jnum(issue, "number"), url ? url : "", who ? who : "ghost", when, head ? head : "?",
                   base ? base : "?", pull_state(issue));
        if (note) buf_printf(&b, " Made here as an issue: %s. Its changes: %s/files", note, url ? url : "");
        buf_add(&b, "\n\n");
    } else
        buf_printf(&b, "> From GitHub: [#%.0f](%s), opened by **@%s** on %s.\n\n", jnum(issue, "number"),
                   url ? url : "", who ? who : "ghost", when);
    buf_add(&b, body && *body ? body : "(no description)");
    return buf_take(&b);
}
static cJSON* label_ids(const cJSON* issue, const cJSON* ids) {
    cJSON *out = cJSON_CreateArray(), *l;
    for (l = cJSON_GetObjectItemCaseSensitive(issue, "labels") ? cJSON_GetObjectItemCaseSensitive(issue, "labels")->child : NULL;
         l; l = l->next) {
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(ids, jstr(l, "name") ? jstr(l, "name") : "");
        if (cJSON_IsNumber(id)) cJSON_AddItemToArray(out, cJSON_CreateNumber(id->valuedouble));
    }
    return out;
}
static int set_state(Aw* a, const char* name, int number, int closed) {
    char* path = aw_format("repos/%s/%s/issues/%d", a->config->forgejo_org, name, number);
    cJSON *edit = cJSON_CreateObject(), *got;
    cJSON_AddStringToObject(edit, "state", closed ? "closed" : "open");
    got = forgejo(a, "PATCH", path, edit, 201, NULL);
    cJSON_Delete(edit);
    free(path);
    cJSON_Delete(got);
    return !got;
}
/* the Forgejo's number, made: it must come back as GitHub's, else the repository stops here */
static int same_number(const char* name, int number, const cJSON* got) {
    if ((int)jnum(got, "number") == number) return 1;
    aw_warn("%s: GitHub #%d came out as the Forgejo's #%.0f: the numbers no longer match, %s stops here (the "
            "Forgejo's issues must start empty)",
            name, number, jnum(got, "number"), name);
    return 0;
}
/* number made on the Forgejo as an issue, from issue (pull, GitHub's, and note for a pull request made as one) or
   (NULL) as a number GitHub has no more: it must come back as number, which *made then says, whatever comes after */
static int create_issue(Aw* a, const char* name, int number, const cJSON* issue, const cJSON* pull, const char* note,
                        const cJSON* ids, int* made) {
    char* path = aw_format("repos/%s/%s/issues", a->config->forgejo_org, name);
    cJSON *make = cJSON_CreateObject(), *got;
    int closed = !issue || strcmp(jstr(issue, "state") ? jstr(issue, "state") : "", "open"), rc;
    char* body = issue ? issue_body(issue, pull, note)
                       : aw_format("> GitHub #%d is not there: deleted, or moved to another repository.", number);
    char* title = issue ? aw_strdup(jstr(issue, "title")) : aw_format("GitHub #%d (not there)", number);
    cJSON_AddStringToObject(make, "title", title);
    cJSON_AddStringToObject(make, "body", body);
    if (issue) cJSON_AddItemToObject(make, "labels", label_ids(issue, ids));
    cJSON_AddBoolToObject(make, "closed", closed);
    got = forgejo(a, "POST", path, make, 201, NULL);
    rc = !got;
    if (got && !same_number(name, number, got))
        rc = 1;
    else if (got) {
        *made = 1;
        if (closed && strcmp(jstr(got, "state") ? jstr(got, "state") : "", "closed"))
            rc = set_state(a, name, number, 1);
    }
    cJSON_Delete(got);
    cJSON_Delete(make);
    free(body);
    free(title);
    free(path);
    return rc;
}
/* labels as GitHub's on the Forgejo's number, an issue or a pull request */
static int put_labels(Aw* a, const char* name, int number, const cJSON* issue, const cJSON* ids) {
    char* path = aw_format("repos/%s/%s/issues/%d/labels", a->config->forgejo_org, name, number);
    cJSON *tags = cJSON_CreateObject(), *got;
    int rc;
    cJSON_AddItemToObject(tags, "labels", label_ids(issue, ids));
    got = forgejo(a, "PUT", path, tags, 200, NULL);
    rc = !got;
    cJSON_Delete(got);
    cJSON_Delete(tags);
    free(path);
    return rc;
}
/* GitHub's pull request number of name (owned), or NULL, said */
static cJSON* github_pull(Aw* a, const char* name, int number) {
    char* path = aw_format("repos/%s/%s/pulls/%d", a->config->org, name, number);
    int status;
    cJSON* pull = github(a, path, &status);
    if (!pull) aw_warn("%s: GitHub's pull request #%d could not be read (HTTP %d)", name, number, status);
    free(path);
    return pull;
}
/* why GitHub's pull request number cannot be one here (owned), or NULL: a pull request is made from its head, which
   GitHub keeps, into its base, which must be a branch still. *ref (owned, or NULL) the one the mirror has not */
static char* pull_note(Aw* a, const char* name, int number, const cJSON* pull, char** ref) {
    const char* base = jstr(cJSON_GetObjectItemCaseSensitive(pull, "base"), "ref");
    *ref = NULL;
    if (!aw_has_pull(a->config, name, number)) {
        *ref = aw_format("refs/pull/%d/head", number);
        return aw_strdup("its head is not on GitHub");
    }
    if (!base || !aw_has_branch(a->config, name, base)) {
        *ref = base ? aw_format("refs/heads/%s", base) : NULL;
        return aw_format("the branch it went into, %s, is not on GitHub any more", base ? base : "?");
    }
    return NULL;
}
/* an issue, or a pull request made as one (why said again), edited as GitHub has it */
static int update_issue(Aw* a, const char* name, int number, const cJSON* issue, const cJSON* ids) {
    char *path = aw_format("repos/%s/%s/issues/%d", a->config->forgejo_org, name, number), *note = NULL, *body, *ref;
    cJSON *pull = NULL, *edit = cJSON_CreateObject(), *got;
    int rc = 0;
    if (pull_request(issue)) {
        rc = !(pull = github_pull(a, name, number));
        if (pull && !(note = pull_note(a, name, number, pull, &ref)))
            note = aw_strdup("it was made so on an earlier pass");
        if (pull) free(ref);
    }
    body = issue_body(issue, pull, note);
    cJSON_AddStringToObject(edit, "title", jstr(issue, "title") ? jstr(issue, "title") : "");
    cJSON_AddStringToObject(edit, "body", body);
    cJSON_AddStringToObject(edit, "state",
                            strcmp(jstr(issue, "state") ? jstr(issue, "state") : "", "open") ? "closed" : "open");
    if (!rc) {
        got = forgejo(a, "PATCH", path, edit, 201, NULL);
        rc = !got;
        cJSON_Delete(got);
    }
    if (!rc) rc = put_labels(a, name, number, issue, ids);
    cJSON_Delete(edit);
    cJSON_Delete(pull);
    free(note);
    free(body);
    free(path);
    return rc;
}

/* ---- pull requests ---- */
static char* github_git(const Aw* a, const char* name) {
    return aw_format("%s/%s/%s.git", a->config->github, a->config->org, name);
}
static char* forgejo_git(const Aw* a, const char* name) {
    return aw_format("%s/%s/%s.git", a->config->forgejo, a->config->forgejo_org, name);
}
/* POST pulls/<number>/merge, marking it merged by GitHub's merge commit, which the base branch here must hold: 1 when
   done, else 0 and *status the HTTP status */
static int merge_pull(Aw* a, const char* name, int number, const cJSON* pull, int* status) {
    const char* commit = jstr(pull, "merge_commit_sha");
    char* path = aw_format("repos/%s/%s/pulls/%d/merge", a->config->forgejo_org, name, number);
    cJSON *merge = cJSON_CreateObject(), *got;
    cJSON_AddStringToObject(merge, "Do", "manually-merged");
    cJSON_AddStringToObject(merge, "MergeCommitID", commit ? commit : "");
    got = forgejo(a, "POST", path, merge, 200, status);
    cJSON_Delete(merge);
    free(path);
    if (!got) return 0;
    cJSON_Delete(got);
    return 1;
}
/* the Forgejo's AW_PULL_BRANCH of number gone, once its pull request is no more open: the pull request keeps its own
   copy of the head (refs/pull/<number>/head). Missing already is fine */
static void drop_branch(Aw* a, const char* name, int number) {
    char* path = aw_format("repos/%s/%s/branches/" AW_PULL_BRANCH, a->config->forgejo_org, name, number);
    int status;
    cJSON_Delete(forgejo(a, "DELETE", path, NULL, 204, &status));
    free(path);
}
/* the Forgejo's pull request number brought from the state it was left in (pulls) to GitHub's (want): reopened from
   its head when GitHub's is open again or merged (a closed one cannot be marked merged), marked merged by GitHub's
   merge commit, or closed; its AW_PULL_BRANCH goes once it is no more open. A merged one stays merged. A merge commit
   the Forgejo refuses (409: the base branch here does not hold it, pushed over since) leaves it closed, said; any
   other failure stops, for the next pass. 0, or 1 (said) */
static int settle(Aw* a, const char* name, int number, const cJSON* pull, const char* want, cJSON* pulls) {
    const char *had, *now, *commit = jstr(pull, "merge_commit_sha"),
                          *base = jstr(cJSON_GetObjectItemCaseSensitive(pull, "base"), "ref");
    char key[16], *to;
    int rc = 0, status;
    snprintf(key, sizeof key, "%d", number);
    had = jstr(pulls, key);
    now = !had || !strcmp(had, "open") ? "open" : !strcmp(had, "merged") ? "merged" : "closed";
    if (!strcmp(now, want)) return 0;
    if (!strcmp(now, "merged")) {
        aw_warn("%s: #%d is merged here and %s on GitHub: it stays merged", name, number, want);
        return 0;
    }
    if (!strcmp(now, "closed")) {
        to = forgejo_git(a, name);
        rc = aw_push_pull(a->config, name, to, number) || set_state(a, name, number, 0);
        free(to);
        if (rc) return 1;
        set_string(pulls, key, now = "open");
    }
    if (!strcmp(want, "merged")) {
        if (merge_pull(a, name, number, pull, &status))
            now = "merged";
        else if (status == 409) {
            aw_warn("%s: #%d is merged on GitHub by %s, which %s here does not hold: it is closed here instead", name,
                    number, commit ? commit : "no commit", base ? base : "its base");
            rc = set_state(a, name, number, 1);
            now = "closed";
        } else {
            aw_warn("%s: #%d could not be marked merged (HTTP %d): the next pass", name, number, status);
            rc = 1;
        }
    } else if (!strcmp(want, "closed")) {
        rc = set_state(a, name, number, 1);
        now = "closed";
    }
    if (rc) return 1;
    set_string(pulls, key, now);
    if (strcmp(now, "open")) drop_branch(a, name, number);
    return 0;
}
/* number made on the Forgejo as a pull request from GitHub's (pull): its head pushed as AW_PULL_BRANCH, the pull
   request into the same base, then its state as GitHub's. It must come back as number, which *made then says, whatever
   comes after */
static int make_pull(Aw* a, const char* name, int number, const cJSON* issue, const cJSON* pull, const cJSON* ids,
                     cJSON* pulls, int* made) {
    char *to = forgejo_git(a, name), *path = aw_format("repos/%s/%s/pulls", a->config->forgejo_org, name),
         *head = aw_format(AW_PULL_BRANCH, number), *body = issue_body(issue, pull, NULL), key[16];
    cJSON *make = cJSON_CreateObject(), *got = NULL;
    int rc = aw_push_pull(a->config, name, to, number);
    snprintf(key, sizeof key, "%d", number);
    if (!rc) {
        cJSON_AddStringToObject(make, "title", jstr(issue, "title") ? jstr(issue, "title") : "");
        cJSON_AddStringToObject(make, "body", body);
        cJSON_AddStringToObject(make, "head", head);
        cJSON_AddStringToObject(make, "base", jstr(cJSON_GetObjectItemCaseSensitive(pull, "base"), "ref"));
        cJSON_AddItemToObject(make, "labels", label_ids(issue, ids));
        got = forgejo(a, "POST", path, make, 201, NULL);
        rc = !got || !same_number(name, number, got);
    }
    if (!rc) {
        *made = 1;
        set_string(pulls, key, "open");
        rc = settle(a, name, number, pull, pull_state(issue), pulls);
    }
    cJSON_Delete(got);
    cJSON_Delete(make);
    free(body);
    free(head);
    free(path);
    free(to);
    return rc;
}
/* a pull request made here as one, edited as GitHub has it: its title and text, its base while open, its labels, its
   head moved, then its state */
static int update_pull(Aw* a, const char* name, int number, const cJSON* issue, const cJSON* ids, cJSON* pulls) {
    cJSON *pull = github_pull(a, name, number), *edit = cJSON_CreateObject(), *got;
    char *path = aw_format("repos/%s/%s/pulls/%d", a->config->forgejo_org, name, number), *to = forgejo_git(a, name),
         *body = issue_body(issue, pull, NULL), key[16];
    const char *want = pull_state(issue), *base = jstr(cJSON_GetObjectItemCaseSensitive(pull, "base"), "ref");
    int rc = !pull, open_here;
    snprintf(key, sizeof key, "%d", number);
    open_here = !jstr(pulls, key) || !strcmp(jstr(pulls, key), "open");
    cJSON_AddStringToObject(edit, "title", jstr(issue, "title") ? jstr(issue, "title") : "");
    cJSON_AddStringToObject(edit, "body", body);
    /* a new base on GitHub, while it is open and the branch is there to go into */
    if (open_here && !strcmp(want, "open") && base && aw_has_branch(a->config, name, base))
        cJSON_AddStringToObject(edit, "base", base);
    if (!rc) {
        got = forgejo(a, "PATCH", path, edit, 201, NULL);
        rc = !got;
        cJSON_Delete(got);
    }
    if (!rc) rc = put_labels(a, name, number, issue, ids);
    if (!rc && open_here && strcmp(want, "closed")) rc = aw_push_pull(a->config, name, to, number); /* new commits */
    if (!rc) rc = settle(a, name, number, pull, want, pulls);
    cJSON_Delete(edit);
    cJSON_Delete(pull);
    free(body);
    free(to);
    free(path);
    return rc;
}

/* number made on the Forgejo from GitHub's issue (NULL: a number GitHub has no more): a pull request as one, or as an
   issue saying why when it cannot be: what the mirror has not, GitHub has not either. One GitHub has after all came
   after this pass's fetch: it stops here, for the next pass. *made once the Forgejo has number: a failure after it
   (its state, its merge) leaves it to the next pass's update, as it would make it twice */
static int create(Aw* a, const char* name, int number, const cJSON* issue, const cJSON* ids, cJSON* st, int* made) {
    cJSON* pull;
    char *note, *ref, *from;
    int rc, has = 0;
    *made = 0;
    if (!issue || !pull_request(issue)) return create_issue(a, name, number, issue, NULL, NULL, ids, made);
    if (!(pull = github_pull(a, name, number))) return 1;
    note = pull_note(a, name, number, pull, &ref);
    if (ref) {
        from = github_git(a, name);
        has = aw_remote_has(a->config, name, from, ref);
        if (has == 1) aw_log("%s: #%d's %s came after this pass's fetch: the next pass makes it", name, number, ref);
        free(from);
        free(ref);
    }
    if (has)
        rc = 1;
    else if (note) {
        aw_log("%s: #%d, a pull request, made as an issue: %s", name, number, note);
        rc = create_issue(a, name, number, issue, pull, note, ids, made);
    } else
        rc = make_pull(a, name, number, issue, pull, ids, child(st, "pulls"), made);
    free(note);
    cJSON_Delete(pull);
    return rc;
}
/* number edited on the Forgejo as GitHub's issue has it, as a pull request when it was made one */
static int update(Aw* a, const char* name, int number, const cJSON* issue, const cJSON* ids, cJSON* st) {
    char key[16];
    snprintf(key, sizeof key, "%d", number);
    if (pull_request(issue) && jstr(child(st, "pulls"), key))
        return update_pull(a, name, number, issue, ids, child(st, "pulls"));
    return update_issue(a, name, number, issue, ids);
}
/* whether line is one of list's lines */
static int has_line(const char* list, const char* line) {
    size_t n = strlen(line);
    const char* at;
    for (at = list; (at = strstr(at, line)) != NULL; at += n)
        if ((at == list || at[-1] == '\n') && (at[n] == '\n' || !at[n])) return 1;
    return 0;
}
/* what the Forgejo has and GitHub has no more (stale, a ref a line, from aw_mirror): each one stays the grace from when
   it was first seen gone, said then, and is deleted after it; one on GitHub again before that just stays. An
   AW_KEPT_BRANCH one counts from when it was kept, which was said already. 0, or 1 (said) */
static int grace(Aw* a, const char* name, cJSON* st, const char* stale) {
    static const char kept_prefix[] = "refs/heads/" AW_KEPT_BRANCH "/";
    cJSON *gone = child(st, "gone"), *g, *next;
    const char *line, *end;
    char when[16], *to;
    Buf due = {0};
    int rc = 0;
    if (!stale) return 0;
    for (g = gone->child; g; g = next) {
        next = g->next;
        if (has_line(stale, g->string)) continue;
        if (strncmp(g->string, kept_prefix, sizeof kept_prefix - 1))
            aw_log("%s: %s is on GitHub again: it stays here", name, g->string);
        cJSON_DeleteItemFromObjectCaseSensitive(gone, g->string);
    }
    for (line = stale; *line; line = *end ? end + 1 : end) {
        const cJSON* since;
        char* ref;
        end = strchr(line, '\n') ? strchr(line, '\n') : line + strlen(line);
        if (end == line) continue;
        ref = aw_format("%.*s", (int)(end - line), line);
        if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(gone, ref))) {
            set_number(gone, ref, a->now);
            when_of(a->now + a->config->grace, "%Y-%m-%d", when, sizeof when);
            if (a->config->grace && strncmp(ref, kept_prefix, sizeof kept_prefix - 1))
                aw_warn("%s: %s is gone from GitHub: it stays here until %s, then it is deleted", name, ref, when);
        }
        since = cJSON_GetObjectItemCaseSensitive(gone, ref);
        if (a->now - (long)since->valuedouble >= a->config->grace) buf_printf(&due, "%s\n", ref);
        free(ref);
    }
    if (due.len) {
        to = forgejo_git(a, name);
        rc = aw_delete_refs(a->config, name, to, due.text);
        for (line = due.text; !rc && *line; line = *end ? end + 1 : end) {
            char* ref;
            end = strchr(line, '\n') ? strchr(line, '\n') : line + strlen(line);
            ref = aw_format("%.*s", (int)(end - line), line);
            when_of((long)jnum(gone, ref), "%Y-%m-%d", when, sizeof when);
            if (a->config->grace)
                aw_log("%s: %s deleted here, gone from GitHub since %s", name, ref, when);
            else
                aw_warn("%s: %s deleted here: GitHub has it no more", name, ref);
            cJSON_DeleteItemFromObjectCaseSensitive(gone, ref);
            free(ref);
        }
        free(to);
    }
    buf_free(&due);
    return rc;
}
/* the pull requests left open here as the Forgejo has them: one it closed under the sync gets its head pushed again
   and is reopened, GitHub's state coming on with its next edit, and one it merged is recorded so; one still open whose
   AW_PULL_BRANCH is gone (pulls: the ones the Forgejo has, NULL when not known) gets it pushed again. A --prune push
   deleted those branches, 2026-10-28. 0, or 1 (said) */
static int keep_open(Aw* a, const char* name, cJSON* st, const char* pulls_here) {
    cJSON *pulls = child(st, "pulls"), *p, *open, *q;
    char *path, *to;
    int rc = 0, *lost, nlost = 0, *bare, nbare = 0, i;
    for (p = pulls->child; p && !(cJSON_IsString(p) && !strcmp(p->valuestring, "open")); p = p->next) continue;
    if (!p) return 0;
    path = aw_format("repos/%s/%s/pulls?state=open", a->config->forgejo_org, name);
    open = forgejo_all(a, path);
    free(path);
    if (!open) return 1;
    lost = calloc((size_t)cJSON_GetArraySize(pulls) + 1, sizeof *lost);
    bare = calloc((size_t)cJSON_GetArraySize(pulls) + 1, sizeof *bare);
    for (p = pulls->child; p; p = p->next) {
        int number = atoi(p->string), here = 0;
        char* branch;
        if (!cJSON_IsString(p) || strcmp(p->valuestring, "open")) continue;
        for (q = open->child; q && !here; q = q->next) here = (int)jnum(q, "number") == number;
        branch = aw_format("refs/heads/" AW_PULL_BRANCH, number);
        if (!here)
            lost[nlost++] = number;
        else if (pulls_here && !has_line(pulls_here, branch))
            bare[nbare++] = number;
        free(branch);
    }
    to = forgejo_git(a, name);
    for (i = 0; i < nbare && !rc; ++i) {
        aw_log("%s: #%d, open, had its branch gone here: pushed again", name, bare[i]);
        rc = aw_push_pull(a->config, name, to, bare[i]);
    }
    for (i = 0; i < nlost && !rc; ++i) {
        char key[16], *at = aw_format("repos/%s/%s/pulls/%d", a->config->forgejo_org, name, lost[i]);
        cJSON* got = forgejo(a, "GET", at, NULL, 200, NULL);
        snprintf(key, sizeof key, "%d", lost[i]);
        rc = !got;
        if (got && jbool(got, "merged")) {
            aw_log("%s: #%d, left open here, is merged here: recorded so", name, lost[i]);
            set_string(pulls, key, "merged");
        } else if (got) {
            aw_log("%s: #%d, left open here, was closed under the sync: its head pushed again, reopened", name, lost[i]);
            rc = aw_push_pull(a->config, name, to, lost[i]) || set_state(a, name, lost[i], 0);
        }
        cJSON_Delete(got);
        free(at);
    }
    free(to);
    free(bare);
    free(lost);
    cJSON_Delete(open);
    return rc;
}
static int by_number(const void* x, const void* y) {
    double a = jnum(*(cJSON* const*)x, "number"), b = jnum(*(cJSON* const*)y, "number");
    return a < b ? -1 : a > b;
}
/* the issues and pull requests updated since the last pass, the new ones made in their numbers' order */
static int issues(Aw* a, const char* name, cJSON* st, const cJSON* ids) {
    const char* since = jstr(st, "issues_since");
    char* path = aw_format("repos/%s/%s/issues?state=all&sort=updated&direction=asc%s%s", a->config->org, name,
                           since ? "&since=" : "", since ? since : "");
    cJSON *list = github_all(a, path), *seen = child(st, "seen"), *item;
    cJSON** sorted;
    int next = jnum(st, "next") >= 1 ? (int)jnum(st, "next") : 1, count = list ? cJSON_GetArraySize(list) : 0, i, rc = !list;
    int made;
    char latest[32] = "";
    free(path);
    sorted = calloc((size_t)count + 1, sizeof *sorted);
    for (i = 0, item = list ? list->child : NULL; item; item = item->next) sorted[i++] = item;
    qsort(sorted, (size_t)count, sizeof *sorted, by_number);
    for (i = 0; i < count && !rc; ++i) {
        int number = (int)jnum(sorted[i], "number");
        const char* updated = jstr(sorted[i], "updated_at");
        char key[16];
        snprintf(key, sizeof key, "%d", number);
        while (!rc && next < number) { /* a number GitHub has no more */
            rc = create(a, name, next, NULL, ids, st, &made);
            if (made) aw_log("%s: #%d made, as GitHub has it no more", name, next++);
        }
        if (rc) break;
        if (number == next) {
            rc = create(a, name, number, sorted[i], ids, st, &made);
            if (made) next++;
        } else if (!jstr(seen, key) || strcmp(jstr(seen, key), updated ? updated : ""))
            rc = update(a, name, number, sorted[i], ids, st);
        if (!rc) {
            set_string(seen, key, updated ? updated : "");
            if (updated && strcmp(updated, latest) > 0) snprintf(latest, sizeof latest, "%s", updated);
        }
    }
    set_number(st, "next", next);
    if (!rc && latest[0]) set_string(st, "issues_since", latest);
    if (count && !rc) aw_log("%s: %d issue(s) and pull request(s) looked at, up to #%d", name, count, next - 1);
    free(sorted);
    cJSON_Delete(list);
    return rc;
}

/* ---- comments ---- */
static char* comment_body(const cJSON* c) {
    const char *who = jstr(cJSON_GetObjectItemCaseSensitive(c, "user"), "login"), *body = jstr(c, "body");
    char when[11];
    day(jstr(c, "created_at"), when);
    return aw_format("> From GitHub: **@%s** on %s, [the comment](%s).\n\n%s", who ? who : "ghost", when,
                     jstr(c, "html_url") ? jstr(c, "html_url") : "", body ? body : "");
}
static int comments(Aw* a, const char* name, cJSON* st) {
    const char* since = jstr(st, "comments_since");
    char* path = aw_format("repos/%s/%s/issues/comments?sort=updated&direction=asc%s%s", a->config->org, name,
                           since ? "&since=" : "", since ? since : "");
    cJSON *list = github_all(a, path), *kept = child(st, "comments"), *c;
    int next = (int)jnum(st, "next"), rc = !list, made = 0, edited = 0;
    char latest[32] = "";
    free(path);
    for (c = list ? list->child : NULL; c && !rc; c = c->next) {
        const char *issue_url = jstr(c, "issue_url"), *updated = jstr(c, "updated_at"), *slash;
        int number = issue_url && (slash = strrchr(issue_url, '/')) ? atoi(slash + 1) : 0;
        char key[32], *body = comment_body(c);
        cJSON *was, *want = cJSON_CreateObject(), *got;
        snprintf(key, sizeof key, "%.0f", jnum(c, "id"));
        was = cJSON_GetObjectItemCaseSensitive(kept, key);
        cJSON_AddStringToObject(want, "body", body);
        if (number < 1 || number >= next) {
            aw_warn("%s: comment %s is of #%d, which the Forgejo has not yet: the next pass", name, key, number);
            rc = 1;
        } else if (!was) {
            char* at = aw_format("repos/%s/%s/issues/%d/comments", a->config->forgejo_org, name, number);
            got = forgejo(a, "POST", at, want, 201, NULL);
            if (got) {
                cJSON* entry = cJSON_CreateObject();
                cJSON_AddNumberToObject(entry, "id", jnum(got, "id"));
                cJSON_AddStringToObject(entry, "at", updated ? updated : "");
                cJSON_AddItemToObject(kept, key, entry);
                ++made;
            }
            rc = !got;
            cJSON_Delete(got);
            free(at);
        } else if (strcmp(jstr(was, "at") ? jstr(was, "at") : "", updated ? updated : "")) {
            char* at = aw_format("repos/%s/%s/issues/comments/%.0f", a->config->forgejo_org, name, jnum(was, "id"));
            got = forgejo(a, "PATCH", at, want, 200, NULL);
            if (got) {
                set_string(was, "at", updated ? updated : "");
                ++edited;
            }
            rc = !got;
            cJSON_Delete(got);
            free(at);
        }
        if (!rc && updated && strcmp(updated, latest) > 0) snprintf(latest, sizeof latest, "%s", updated);
        cJSON_Delete(want);
        free(body);
    }
    if (!rc && latest[0]) set_string(st, "comments_since", latest);
    if (made || edited) aw_log("%s: %d comment(s) made, %d edited", name, made, edited);
    cJSON_Delete(list);
    return rc;
}

/* ---- a pass ---- */
static int one(Aw* a, const cJSON* repo, cJSON* repos) {
    const char* name = jstr(repo, "name");
    char *from = github_git(a, name), *to = forgejo_git(a, name), *stale = NULL, *pulls = NULL, stamp[32];
    cJSON *st = child(repos, name), *ids = cJSON_CreateObject();
    int rc;
    if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(st, "missing"))) {
        aw_log("%s: on GitHub again", name);
        cJSON_DeleteItemFromObjectCaseSensitive(st, "missing");
    }
    when_of(a->now, "%Y%m%d-%H%M%S", stamp, sizeof stamp);
    rc = ensure_repo(a, repo) || aw_mirror(a->config, name, from, to, stamp, &stale, &pulls) ||
         patch_repo(a, repo) || labels(a, name, ids) || grace(a, name, st, stale) || keep_open(a, name, st, pulls) ||
         issues(a, name, st, ids) || comments(a, name, st);
    if (!rc) set_number(st, "ok", a->now);
    save(a);
    cJSON_Delete(ids);
    free(stale);
    free(pulls);
    free(from);
    free(to);
    return rc;
}
/* each repository GitHub listed, with when its last pass went through (0: none yet), "<name> <seconds since the
   epoch>" a line, written over a->config->fresh: the host reads it to tell a copy that lags. The file is made by
   the operator, in a directory only root writes, so that what root reads cannot be swapped for another file */
static void write_fresh(Aw* a, const cJSON* list, const cJSON* repos) {
    FILE* f = fopen(a->config->fresh, "w");
    const cJSON* repo;
    Buf b = {0};
    int failed;
    if (!f) {
        aw_warn("%s: %s: the host cannot tell whether the copy is up to date", a->config->fresh, strerror(errno));
        return;
    }
    for (repo = list->child; repo; repo = repo->next) {
        const char* name = jstr(repo, "name");
        if (name) buf_printf(&b, "%s %.0f\n", name, jnum(cJSON_GetObjectItemCaseSensitive(repos, name), "ok"));
    }
    failed = b.len && fwrite(b.text, 1, b.len, f) != b.len;
    if (fclose(f)) failed = 1;
    if (failed) aw_warn("%s: cannot be written: the host cannot tell whether the copy is up to date", a->config->fresh);
    buf_free(&b);
}
/* the repositories state.json has and GitHub listed no more (list): each said once, its copy here left as it is */
static void missing(Aw* a, const cJSON* list, cJSON* repos) {
    cJSON* st;
    const cJSON* repo;
    for (st = repos->child; st; st = st->next) {
        for (repo = list->child; repo && strcmp(jstr(repo, "name") ? jstr(repo, "name") : "", st->string);
             repo = repo->next)
            continue;
        if (repo || cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(st, "missing"))) continue;
        set_number(st, "missing", a->now);
        aw_warn("%s is not on GitHub any more (deleted, made private to the token, or moved): its copy here stays as "
                "it is",
                st->string);
    }
}
int aw_sync(Aw* a) {
    char* path = aw_format("orgs/%s/repos?type=all&sort=full_name", a->config->org);
    cJSON *list = github_all(a, path), *repo, *repos = child(a->state, "repos");
    free(path);
    if (!list) {
        aw_warn("the repositories of %s could not be listed: nothing done", a->config->org);
        return 1;
    }
    for (repo = list->child; repo; repo = repo->next) {
        if (!jstr(repo, "name")) continue;
        if (one(a, repo, repos)) {
            aw_warn("%s: stopped, the rest of it on the next pass", jstr(repo, "name"));
            a->failures++;
        }
    }
    missing(a, list, repos);
    save(a);
    write_fresh(a, list, repos);
    aw_log("%d repositories, %ld write(s) on the Forgejo%s", cJSON_GetArraySize(list), a->writes,
           a->failures ? ", some stopped" : "");
    cJSON_Delete(list);
    return a->failures != 0;
}

void aw_status(const Aw* a) {
    const cJSON *repos = cJSON_GetObjectItemCaseSensitive(a->state, "repos"), *r, *g;
    char at[32], until[16];
    if (!repos || !repos->child) {
        printf("nothing synced yet (%s)\n", a->state_path);
        return;
    }
    for (r = repos->child; r; r = r->next) {
        long ok = (long)jnum(r, "ok");
        if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(r, "missing"))) {
            when_of((long)jnum(r, "missing"), "%Y-%m-%d", at, sizeof at);
            printf("%s: not on GitHub since %s, its copy here left as it is\n", r->string, at);
        } else if (ok) {
            when_of(ok, "%Y-%m-%d %H:%M UTC", at, sizeof at);
            printf("%s: last good pass %s, %ld minute(s) ago\n", r->string, at, (a->now - ok) / 60);
        } else
            printf("%s: no good pass yet\n", r->string);
        const cJSON* gone = cJSON_GetObjectItemCaseSensitive(r, "gone");
        for (g = gone ? gone->child : NULL; g; g = g->next) {
            int kept = !strncmp(g->string, "refs/heads/" AW_KEPT_BRANCH "/", strlen("refs/heads/" AW_KEPT_BRANCH "/"));
            when_of((long)g->valuedouble, "%Y-%m-%d", at, sizeof at);
            when_of((long)g->valuedouble + a->config->grace, "%Y-%m-%d", until, sizeof until);
            printf("  %s: %s %s, deleted here on %s\n", g->string, kept ? "kept since" : "gone from GitHub since", at,
                   until);
        }
    }
}
