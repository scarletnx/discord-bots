/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "sync.h"
#include "git.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            if (got || status == 200) aw_warn("GET %s: no list came", path);
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

/* ---- state.json: { "repos": { NAME: { "next", "issues_since", "comments_since", "seen", "comments" } } } ---- */
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
/* what the repository says on GitHub, and its Releases tab off: releases stay on GitHub */
static int patch_repo(Aw* a, const cJSON* repo) {
    const char *name = jstr(repo, "name"), *branch = jstr(repo, "default_branch");
    char* path = aw_format("repos/%s/%s", a->config->forgejo_org, name);
    cJSON *edit = cJSON_CreateObject(), *got;
    cJSON_AddBoolToObject(edit, "has_releases", 0);
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
/* the text on the Forgejo: where it is from, then GitHub's body */
static char* issue_body(Aw* a, const char* name, const cJSON* issue) {
    const cJSON* pr = cJSON_GetObjectItemCaseSensitive(issue, "pull_request");
    const char *who = jstr(cJSON_GetObjectItemCaseSensitive(issue, "user"), "login"), *body = jstr(issue, "body");
    const char* url = jstr(issue, "html_url");
    char when[11];
    Buf b = {0};
    day(jstr(issue, "created_at"), when);
    if (pr) {
        char* path = aw_format("repos/%s/%s/pulls/%.0f", a->config->org, name, jnum(issue, "number"));
        int status;
        cJSON* pull = github(a, path, &status);
        const char *head = pull ? jstr(cJSON_GetObjectItemCaseSensitive(pull, "head"), "label") : NULL,
                   *base = pull ? jstr(cJSON_GetObjectItemCaseSensitive(pull, "base"), "ref") : NULL;
        const char* state = jstr(pr, "merged_at")             ? "merged"
                            : !strcmp(jstr(issue, "state") ? jstr(issue, "state") : "", "open") ? "open"
                                                                                                 : "closed";
        buf_printf(&b, "> From GitHub: pull request [#%.0f](%s) by **@%s**, %s, from `%s` into `%s`, %s. Its changes: "
                       "%s/files\n\n",
                   jnum(issue, "number"), url ? url : "", who ? who : "ghost", when, head ? head : "?",
                   base ? base : "?", state, url ? url : "");
        cJSON_Delete(pull);
        free(path);
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
/* number made on the Forgejo, from issue or (NULL) as a number GitHub has no more: it must come back as number */
static int create(Aw* a, const char* name, int number, const cJSON* issue, const cJSON* ids) {
    char* path = aw_format("repos/%s/%s/issues", a->config->forgejo_org, name);
    cJSON *make = cJSON_CreateObject(), *got;
    int closed = !issue || strcmp(jstr(issue, "state") ? jstr(issue, "state") : "", "open"), rc;
    char* body = issue ? issue_body(a, name, issue)
                       : aw_format("> GitHub #%d is not there: deleted, or moved to another repository.", number);
    char* title = issue ? aw_strdup(jstr(issue, "title")) : aw_format("GitHub #%d (not there)", number);
    cJSON_AddStringToObject(make, "title", title);
    cJSON_AddStringToObject(make, "body", body);
    if (issue) cJSON_AddItemToObject(make, "labels", label_ids(issue, ids));
    cJSON_AddBoolToObject(make, "closed", closed);
    got = forgejo(a, "POST", path, make, 201, NULL);
    rc = !got;
    if (got && (int)jnum(got, "number") != number) {
        aw_warn("%s: GitHub #%d came out as the Forgejo's #%.0f: the numbers no longer match, %s stops here (the "
                "Forgejo's issues must start empty)",
                name, number, jnum(got, "number"), name);
        rc = 1;
    } else if (got && closed && strcmp(jstr(got, "state") ? jstr(got, "state") : "", "closed"))
        rc = set_state(a, name, number, 1);
    cJSON_Delete(got);
    cJSON_Delete(make);
    free(body);
    free(title);
    free(path);
    return rc;
}
static int update(Aw* a, const char* name, int number, const cJSON* issue, const cJSON* ids) {
    char *path = aw_format("repos/%s/%s/issues/%d", a->config->forgejo_org, name, number),
         *labels_path = aw_format("repos/%s/%s/issues/%d/labels", a->config->forgejo_org, name, number);
    char* body = issue_body(a, name, issue);
    cJSON *edit = cJSON_CreateObject(), *tags = cJSON_CreateObject(), *got;
    int rc;
    cJSON_AddStringToObject(edit, "title", jstr(issue, "title") ? jstr(issue, "title") : "");
    cJSON_AddStringToObject(edit, "body", body);
    cJSON_AddStringToObject(edit, "state", strcmp(jstr(issue, "state") ? jstr(issue, "state") : "", "open") ? "closed" : "open");
    got = forgejo(a, "PATCH", path, edit, 201, NULL);
    rc = !got;
    cJSON_Delete(got);
    if (!rc) {
        cJSON_AddItemToObject(tags, "labels", label_ids(issue, ids));
        got = forgejo(a, "PUT", labels_path, tags, 200, NULL);
        rc = !got;
        cJSON_Delete(got);
    }
    cJSON_Delete(edit);
    cJSON_Delete(tags);
    free(body);
    free(path);
    free(labels_path);
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
            rc = create(a, name, next, NULL, ids);
            if (!rc) aw_log("%s: #%d made, as GitHub has it no more", name, next++);
        }
        if (rc) break;
        if (number == next) {
            rc = create(a, name, number, sorted[i], ids);
            if (!rc) next++;
        } else if (!jstr(seen, key) || strcmp(jstr(seen, key), updated ? updated : ""))
            rc = update(a, name, number, sorted[i], ids);
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
    char *from = aw_format("%s/%s/%s.git", a->config->github, a->config->org, name),
         *to = aw_format("%s/%s/%s.git", a->config->forgejo, a->config->forgejo_org, name);
    cJSON *st = child(repos, name), *ids = cJSON_CreateObject();
    int rc = ensure_repo(a, repo) || aw_mirror(a->config, name, from, to) || patch_repo(a, repo) ||
             labels(a, name, ids) || issues(a, name, st, ids) || comments(a, name, st);
    save(a);
    cJSON_Delete(ids);
    free(from);
    free(to);
    return rc;
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
    aw_log("%d repositories, %ld write(s) on the Forgejo%s", cJSON_GetArraySize(list), a->writes,
           a->failures ? ", some stopped" : "");
    cJSON_Delete(list);
    return a->failures != 0;
}
