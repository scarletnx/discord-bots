/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AccordWimsy's gate test: a fake GitHub (its answers by URL) and a fake Forgejo (a little of its API, with the state
   a real one keeps: repositories, the issues' numbers, labels, comments, pull requests and what it refuses of them),
   git for real on bare repositories in a temporary folder. No network. */
#include "aw.h"
#include "config.h"
#include "git.h"
#include "sync.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            ++failures;                                                                                                \
            fprintf(stderr, "test_aw.c:%d: %s\n", __LINE__, #x);                                                      \
        }                                                                                                              \
    } while (0)

static char root[256];
#define GH_API "https://gh.test"
#define FJ     "file://%s/fj"

/* ---- the fake GitHub: an answer by path, the paging taken off (page 2 on, nothing) ---- */
static struct {
    char* path;
    char* body;
} gh[64];
static int ngh;
static char* gh_asked[512];
static int ngh_asked;
static void github(const char* path, const char* body) {
    int i;
    for (i = 0; i < ngh && strcmp(gh[i].path, path); ++i) continue;
    if (i == ngh) gh[ngh++].path = aw_strdup(path);
    else free(gh[i].body);
    gh[i].body = aw_strdup(body);
}
static int github_asked(const char* part) {
    int i, n = 0;
    for (i = 0; i < ngh_asked; ++i) n += strstr(gh_asked[i], part) != NULL;
    return n;
}

/* a git command's status, what it prints dropped */
static int quiet(const char* fmt, ...) {
    char command[2048];
    va_list a;
    int n;
    va_start(a, fmt);
    n = vsnprintf(command, sizeof command, fmt, a);
    va_end(a);
    snprintf(command + n, sizeof command - (size_t)n, " >/dev/null 2>&1");
    return system(command);
}
/* the commit side's ("gh" or "fj") repository name has at ref, or "" */
static char* rev(const char* side, const char* name, const char* ref) {
    char *command = aw_format("git --git-dir=%s/%s/org/%s.git rev-parse --verify --quiet %s 2>/dev/null", root, side,
                              name, ref),
         line[64] = "";
    FILE* p = popen(command, "r");
    if (p) {
        if (!fgets(line, sizeof line, p)) line[0] = 0;
        pclose(p);
    }
    line[strcspn(line, "\n")] = 0;
    free(command);
    return aw_strdup(line);
}

/* ---- the fake Forgejo ---- */
typedef struct {
    int number;
    char *title, *body, *state;
    int labels[8], nlabels;
    int pull, merged; /* a pull request: from head into base; merged by merge */
    char head[64], base[64], merge[48];
} Issue;
typedef struct {
    int id, issue;
    char* body;
} Comment;
typedef struct {
    char name[64], branch[64], description[128];
    int private_, releases, pulls, manual; /* manual: a pull request can be marked merged by a commit */
    Issue issues[32];
    int nissues;
    struct {
        int id;
        char name[64], color[16], description[64];
    } labels[8];
    int nlabels;
    Comment comments[32];
    int ncomments;
} Repo;
static Repo repos[4];
static int nrepos, next_id = 1000, writes[4], skew; /* POST PATCH PUT DELETE; skew: a number the Forgejo gets wrong */
static int merge_fails; /* the next merges failing, as a Forgejo that answers 500 */
static Repo* repo_of(const char* name) {
    int i;
    for (i = 0; i < nrepos; ++i)
        if (!strcmp(repos[i].name, name)) return &repos[i];
    return NULL;
}
static Issue* issue_of(Repo* r, int number) { return number >= 1 && number <= r->nissues ? &r->issues[number - 1] : NULL; }
static int has_branch(const Repo* r, const char* branch) {
    return !quiet("git --git-dir=%s/fj/org/%s.git rev-parse --verify --quiet refs/heads/%s", root, r->name, branch);
}
/* the next number, an issue or a pull request: open, whatever it was asked to be */
static Issue* new_issue(Repo* r, const cJSON* in) {
    Issue* is = &r->issues[r->nissues++];
    const cJSON* l;
    memset(is, 0, sizeof *is);
    is->number = r->nissues + skew;
    is->title = aw_strdup(jstr(in, "title"));
    is->body = aw_strdup(jstr(in, "body"));
    is->state = aw_strdup("open"); /* as a Forgejo may take "closed" no further: the program closes it after */
    for (l = cJSON_GetObjectItemCaseSensitive(in, "labels") ? cJSON_GetObjectItemCaseSensitive(in, "labels")->child : NULL;
         l && is->nlabels < 8; l = l->next)
        is->labels[is->nlabels++] = (int)l->valuedouble;
    return is;
}
static void set(char** field, const cJSON* o, const char* key) {
    if (!jstr(o, key)) return;
    free(*field);
    *field = aw_strdup(jstr(o, key));
}
static void forgejo(const char* method, const char* path, const cJSON* in, AwAnswer* out) {
    char name[64] = "", rest[256] = "";
    int number = 0, id = 0;
    Repo* r;
    out->status = 404;
    if (!strcmp(method, "POST")) writes[0]++;
    if (!strcmp(method, "PATCH")) writes[1]++;
    if (!strcmp(method, "PUT")) writes[2]++;
    if (!strcmp(method, "DELETE")) writes[3]++;
    if (!strcmp(method, "POST") && !strcmp(path, "orgs/org/repos")) { /* a repository made */
        r = &repos[nrepos++];
        memset(r, 0, sizeof *r);
        snprintf(r->name, sizeof r->name, "%s", jstr(in, "name"));
        r->private_ = jbool(in, "private");
        r->releases = 1;
        {
            char* make = aw_format("git init --bare --quiet %s/fj/org/%s.git", root, r->name);
            if (system(make)) fprintf(stderr, "test_aw: %s failed\n", make);
            free(make);
        }
        out->status = 201;
        out->body = aw_strdup("{}");
        return;
    }
    if (sscanf(path, "repos/org/%63[^/?]%255s", name, rest) < 1 || !(r = repo_of(name))) return;
    if (!rest[0]) {
        if (!strcmp(method, "GET")) {
            out->status = 200;
            out->body = aw_strdup("{}");
        } else if (!strcmp(method, "PATCH")) {
            r->releases = jbool(in, "has_releases");
            r->private_ = jbool(in, "private");
            r->pulls = jbool(in, "has_pull_requests");
            r->manual = jbool(in, "allow_manual_merge");
            if (jstr(in, "default_branch")) snprintf(r->branch, sizeof r->branch, "%s", jstr(in, "default_branch"));
            snprintf(r->description, sizeof r->description, "%s", jstr(in, "description") ? jstr(in, "description") : "");
            out->status = 200;
            out->body = aw_strdup("{}");
        }
        return;
    }
    if (!strncmp(rest, "/labels", 7)) {
        if (!strcmp(method, "GET")) {
            Buf b = {0};
            int i;
            buf_add(&b, "[");
            for (i = 0; strstr(rest, "page=1") && i < r->nlabels; ++i)
                buf_printf(&b, "%s{\"id\":%d,\"name\":\"%s\",\"color\":\"%s\",\"description\":\"%s\"}", i ? "," : "",
                           r->labels[i].id, r->labels[i].name, r->labels[i].color, r->labels[i].description);
            buf_add(&b, "]");
            out->status = 200;
            out->body = buf_take(&b);
        } else if (!strcmp(method, "POST")) {
            int i = r->nlabels++;
            r->labels[i].id = next_id++;
            snprintf(r->labels[i].name, 64, "%s", jstr(in, "name"));
            snprintf(r->labels[i].color, 16, "%s", jstr(in, "color"));
            snprintf(r->labels[i].description, 64, "%s", jstr(in, "description"));
            out->status = 201;
            out->body = aw_format("{\"id\":%d}", r->labels[i].id);
        }
        return;
    }
    if (!strcmp(rest, "/issues") && !strcmp(method, "POST")) {
        Issue* is = new_issue(r, in);
        out->status = 201;
        out->body = aw_format("{\"number\":%d,\"state\":\"open\"}", is->number);
        return;
    }
    if (!strcmp(rest, "/pulls") && !strcmp(method, "POST")) { /* both branches there, else 404; one, 422 */
        const char *head = jstr(in, "head"), *base = jstr(in, "base");
        Issue* is;
        int i;
        if (!r->pulls || !head || !base || !has_branch(r, head) || !has_branch(r, base)) return;
        out->status = 422;
        if (!strcmp(head, base)) return;
        out->status = 409; /* one open already, from that head into that base */
        for (i = 0; i < r->nissues; ++i)
            if (r->issues[i].pull && !strcmp(r->issues[i].state, "open") && !strcmp(r->issues[i].head, head) &&
                !strcmp(r->issues[i].base, base))
                return;
        is = new_issue(r, in);
        is->pull = 1;
        snprintf(is->head, sizeof is->head, "%s", head);
        snprintf(is->base, sizeof is->base, "%s", base);
        quiet("git --git-dir=%s/fj/org/%s.git update-ref refs/pull/%d/head refs/heads/%s", root, r->name, is->number,
              head);
        out->status = 201;
        out->body = aw_format("{\"number\":%d}", is->number);
        return;
    }
    if (sscanf(rest, "/pulls/%d", &number) == 1) {
        Issue* is = issue_of(r, number);
        const char* tail = strchr(rest + 7, '/');
        if (!is || !is->pull) return;
        if (!tail && !strcmp(method, "PATCH")) {
            const char* base = jstr(in, "base");
            if (base && strcmp(base, is->base)) { /* a new base: a branch there, and not merged yet */
                if (is->merged || !has_branch(r, base)) return;
                snprintf(is->base, sizeof is->base, "%s", base);
            }
            set(&is->title, in, "title");
            set(&is->body, in, "body");
            out->status = 201;
            out->body = aw_strdup("{}");
        } else if (tail && !strcmp(tail, "/merge") && !strcmp(method, "POST")) {
            /* marked merged by a commit: open (a closed one is not found), the style allowed, the commit whole and
               in the base branch ("Wrong commit ID", 409) */
            const char* commit = jstr(in, "MergeCommitID");
            if (strcmp(is->state, "open")) return;
            out->status = 500;
            if (merge_fails && merge_fails--) return;
            out->status = 422;
            if (!jstr(in, "Do") || strcmp(jstr(in, "Do"), "manually-merged")) return;
            out->status = 405;
            if (is->merged || !r->manual) return;
            out->status = 409;
            if (!commit || strlen(commit) != 40 ||
                quiet("git --git-dir=%s/fj/org/%s.git merge-base --is-ancestor %s refs/heads/%s", root, r->name, commit,
                      is->base))
                return;
            is->merged = 1;
            snprintf(is->merge, sizeof is->merge, "%s", commit);
            free(is->state);
            is->state = aw_strdup("closed");
            out->status = 200;
        }
        return;
    }
    if (!strncmp(rest, "/branches/", 10) && !strcmp(method, "DELETE")) {
        if (has_branch(r, rest + 10) &&
            !quiet("git --git-dir=%s/fj/org/%s.git update-ref -d refs/heads/%s", root, r->name, rest + 10))
            out->status = 204;
        return;
    }
    if (sscanf(rest, "/issues/comments/%d", &id) == 1 && !strcmp(method, "PATCH")) {
        int i;
        for (i = 0; i < r->ncomments && r->comments[i].id != id; ++i) continue;
        if (i == r->ncomments) return;
        set(&r->comments[i].body, in, "body");
        out->status = 200;
        out->body = aw_strdup("{}");
        return;
    }
    if (sscanf(rest, "/issues/%d", &number) == 1) {
        Issue* is = issue_of(r, number);
        const char* tail = strchr(rest + 8, '/');
        if (!is) return;
        if (!tail && !strcmp(method, "PATCH")) {
            if (is->pull && jstr(in, "state") && strcmp(jstr(in, "state"), is->state)) {
                out->status = 412; /* a merged one keeps its state */
                if (is->merged) return;
                out->status = 409; /* reopened from its head, which must be there */
                if (!strcmp(jstr(in, "state"), "open") && !has_branch(r, is->head)) return;
            }
            set(&is->title, in, "title");
            set(&is->body, in, "body");
            set(&is->state, in, "state");
            out->status = 201;
            out->body = aw_strdup("{}");
        } else if (tail && !strcmp(tail, "/labels") && !strcmp(method, "PUT")) {
            const cJSON* l;
            is->nlabels = 0;
            for (l = cJSON_GetObjectItemCaseSensitive(in, "labels")->child; l && is->nlabels < 8; l = l->next)
                is->labels[is->nlabels++] = (int)l->valuedouble;
            out->status = 200;
            out->body = aw_strdup("[]");
        } else if (tail && !strcmp(tail, "/comments") && !strcmp(method, "POST")) {
            Comment* c = &r->comments[r->ncomments++];
            c->id = next_id++;
            c->issue = number;
            c->body = aw_strdup(jstr(in, "body"));
            out->status = 201;
            out->body = aw_format("{\"id\":%d}", c->id);
        }
    }
}
static void fake(void* ctx, const char* method, const char* url, const char* auth, const char* body, AwAnswer* out) {
    char fj[300];
    (void)ctx;
    memset(out, 0, sizeof *out);
    snprintf(fj, sizeof fj, FJ "/api/v1/", root);
    if (!strncmp(url, GH_API "/", strlen(GH_API) + 1)) {
        const char* path = url + strlen(GH_API) + 1;
        char* bare = aw_strdup(path);
        char* paging = strstr(bare, "per_page=100&page=");
        int i, page = paging ? atoi(paging + 18) : 1;
        if (ngh_asked < 512) gh_asked[ngh_asked++] = aw_strdup(path);
        if (!auth || strcmp(auth, "Bearer gh-token")) fprintf(stderr, "test_aw: GitHub asked without its token\n");
        if (paging) { /* what the program asked for, its paging taken off */
            *paging = 0;
            if (paging > bare && paging[-1] == '&') paging[-1] = 0;
        }
        for (i = 0; i < ngh && strcmp(gh[i].path, bare); ++i) continue;
        out->status = i < ngh ? 200 : 404;
        out->body = aw_strdup(i < ngh && page == 1 ? gh[i].body : i < ngh ? "[]" : "{\"message\":\"Not Found\"}");
        free(bare);
        return;
    }
    if (!strncmp(url, fj, strlen(fj))) {
        cJSON* in = body ? cJSON_Parse(body) : NULL;
        if (!auth || strcmp(auth, "token fj-token")) fprintf(stderr, "test_aw: the Forgejo asked without its token\n");
        forgejo(method, url + strlen(fj), in, out);
        cJSON_Delete(in);
        return;
    }
    out->error = aw_format("%s %s: no such host here", method, url);
}

/* ---- git on the fake GitHub's side ---- */
static int sh(const char* fmt, ...) {
    char command[2048];
    va_list a;
    int status;
    va_start(a, fmt);
    vsnprintf(command, sizeof command, fmt, a);
    va_end(a);
    status = system(command);
    if (status) fprintf(stderr, "test_aw: %s: status %d\n", command, status);
    return status;
}
/* side's refs of name, the pull requests' own left out (GitHub's refs/pull/, the Forgejo's and its AW_PULL_BRANCH) */
static char* refs(const char* side, const char* name) {
    char *command = aw_format("git --git-dir=%s/%s/org/%s.git for-each-ref --format='%%(refname) %%(objectname)'", root,
                              side, name);
    FILE* p = popen(command, "r");
    Buf b = {0};
    char line[256];
    while (p && fgets(line, sizeof line, p))
        if (strncmp(line, "refs/pull/", 10) && strncmp(line, "refs/heads/accordwimsy/", 23)) buf_add(&b, line);
    if (p) pclose(p);
    free(command);
    return buf_take(&b);
}

static const char* issues_v1 =
    "[{\"number\":4,\"title\":\"Crash on start\",\"state\":\"closed\",\"body\":\"It crashes.\","
    "\"html_url\":\"https://github.com/org/game/issues/4\",\"user\":{\"login\":\"ada\"},"
    "\"created_at\":\"2026-10-03T10:00:00Z\",\"updated_at\":\"2026-10-04T10:00:00Z\",\"labels\":[]},"
    "{\"number\":1,\"title\":\"Sound is off\",\"state\":\"open\",\"body\":\"No sound.\","
    "\"html_url\":\"https://github.com/org/game/issues/1\",\"user\":{\"login\":\"ada\"},"
    "\"created_at\":\"2026-10-01T10:00:00Z\",\"updated_at\":\"2026-10-05T10:00:00Z\",\"labels\":[{\"name\":\"bug\"}]},"
    "{\"number\":2,\"title\":\"Fix the sound\",\"state\":\"closed\",\"body\":\"Fixes #1.\","
    "\"html_url\":\"https://github.com/org/game/pull/2\",\"user\":{\"login\":\"bob\"},"
    "\"created_at\":\"2026-10-02T10:00:00Z\",\"updated_at\":\"2026-10-02T12:00:00Z\",\"labels\":[],"
    "\"pull_request\":{\"merged_at\":\"2026-10-02T12:00:00Z\"}}]";
static const char* comments_v1 =
    "[{\"id\":100,\"issue_url\":\"https://gh.test/repos/org/game/issues/1\",\"body\":\"Same here.\","
    "\"html_url\":\"https://github.com/org/game/issues/1#issuecomment-100\",\"user\":{\"login\":\"cy\"},"
    "\"created_at\":\"2026-10-01T11:00:00Z\",\"updated_at\":\"2026-10-01T11:00:00Z\"},"
    "{\"id\":101,\"issue_url\":\"https://gh.test/repos/org/game/issues/2\",\"body\":\"Looks right.\","
    "\"html_url\":\"https://github.com/org/game/pull/2#issuecomment-101\",\"user\":{\"login\":\"ada\"},"
    "\"created_at\":\"2026-10-02T11:00:00Z\",\"updated_at\":\"2026-10-02T11:00:00Z\"}]";

/* GitHub's pulls/<number> of game: its head's label, its base, its merge commit (NULL: not merged) */
static void github_pull(int number, const char* head, const char* base, const char* merge) {
    char *path = aw_format("repos/org/game/pulls/%d", number),
         *body = aw_format("{\"head\":{\"label\":\"%s\"},\"base\":{\"ref\":\"%s\"},\"merge_commit_sha\":%s%s%s}", head,
                           base, merge ? "\"" : "", merge ? merge : "null", merge ? "\"" : "");
    github(path, body);
    free(path);
    free(body);
}
/* pull request number by eve in GitHub's issues list (merged: when, or NULL) */
static char* pull_entry(int number, const char* title, const char* state, const char* merged, const char* updated,
                        const char* labels) {
    return aw_format("{\"number\":%d,\"title\":\"%s\",\"state\":\"%s\",\"body\":\"Pull %d.\",\"html_url\":"
                     "\"https://github.com/org/game/pull/%d\",\"user\":{\"login\":\"eve\"},\"created_at\":"
                     "\"2026-10-07T00:00:00Z\",\"updated_at\":\"%s\",\"labels\":%s,"
                     "\"pull_request\":{\"merged_at\":%s%s%s}}",
                     number, title, state, number, number, updated, labels, merged ? "\"" : "",
                     merged ? merged : "null", merged ? "\"" : "");
}
/* game's issues list since then: n entries, each owned */
static void issues_since(const char* since, int n, ...) {
    char *path = aw_format("repos/org/game/issues?state=all&sort=updated&direction=asc&since=%s", since), *list;
    Buf b = {0};
    va_list a;
    int i;
    buf_add(&b, "[");
    va_start(a, n);
    for (i = 0; i < n; ++i) {
        char* entry = va_arg(a, char*);
        buf_printf(&b, "%s%s", i ? "," : "", entry);
        free(entry);
    }
    va_end(a);
    buf_add(&b, "]");
    list = buf_take(&b);
    github(path, list);
    free(list);
    free(path);
}
/* the state game's pull request number was left in, from state.json */
static const char* pull_state_of(const cJSON* state, const char* number) {
    const cJSON* game = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(state, "repos"), "game");
    return jstr(cJSON_GetObjectItemCaseSensitive(game, "pulls"), number);
}

/* game's next number, from state.json */
static int next_of(const cJSON* state) {
    const cJSON* game = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(state, "repos"), "game");
    return (int)jnum(game, "next");
}

static int pass(AwConfig* c, cJSON* state, long* written) {
    Aw a;
    int rc;
    memset(&a, 0, sizeof a);
    a.config = c;
    a.fetch = fake;
    a.state = state;
    a.state_path = aw_format("%s/state/state.json", root);
    rc = aw_sync(&a);
    if (written) *written = a.writes;
    free(a.state_path);
    return rc;
}
static int writes_total(void) { return writes[0] + writes[1] + writes[2] + writes[3]; }

int main(void) {
    AwConfig c;
    char *env, why[512], *gh_refs, *fj_refs, *merged2;
    cJSON* state = cJSON_CreateObject();
    Repo* game;
    long written;
    int before;
    char template[] = "/tmp/accordwimsy-test.XXXXXX";
    if (!mkdtemp(template)) return 1;
    /* no one's git settings: a commit signed through a pinentry, an insteadOf */
    setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    setenv("GIT_AUTHOR_NAME", "T", 1);
    setenv("GIT_AUTHOR_EMAIL", "t@t", 1);
    setenv("GIT_COMMITTER_NAME", "T", 1);
    setenv("GIT_COMMITTER_EMAIL", "t@t", 1);
    snprintf(root, sizeof root, "%s", template);

    /* the env file: every key it knows, the defaults, and the refusals */
    env = aw_format("%s/aw.env", root);
    {
        FILE* f = fopen(env, "w");
        fprintf(f, "# a comment\n\nACCORDWIMSY_FORGEJO=" FJ "/\nACCORDWIMSY_FORGEJO_TOKEN=fj-token\nGITHUB_TOKEN=gh-token\n"
                   "ACCORDWIMSY_ORG=org\nACCORDWIMSY_GITHUB_API=" GH_API "\nACCORDWIMSY_GITHUB=file://%s/gh\n"
                   "ACCORDWIMSY_STATE=%s/state\nACCORDWIMSY_GIT=git\n",
                root, root, root);
        fclose(f);
    }
    CHECK(!aw_config_read(&c, env, why, sizeof why));
    CHECK(!strcmp(c.forgejo_org, "org") && !strcmp(c.forgejo_user, "accordwimsy") && c.forgejo[strlen(c.forgejo) - 1] != '/');
    {
        AwConfig bad;
        char* wrong = aw_format("%s/bad.env", root);
        FILE* f = fopen(wrong, "w");
        fprintf(f, "ACCORDWIMSY_FORGEJO=x\nACCORDWIMSY_FORGEJO_TOKEN=t\nGITHUB_TOKN=t\n");
        fclose(f);
        CHECK(aw_config_read(&bad, wrong, why, sizeof why) && strstr(why, "bad.env:3: GITHUB_TOKN is no key"));
        f = fopen(wrong, "w");
        fprintf(f, "ACCORDWIMSY_FORGEJO_TOKEN=t\nGITHUB_TOKEN=t\n");
        fclose(f);
        CHECK(aw_config_read(&bad, wrong, why, sizeof why) && strstr(why, "ACCORDWIMSY_FORGEJO is needed, and empty"));
        f = fopen(wrong, "w");
        fprintf(f, "ACCORDWIMSY_FORGEJO=x\nno equals here\n");
        fclose(f);
        CHECK(aw_config_read(&bad, wrong, why, sizeof why) && strstr(why, "bad.env:2: \"no equals here\" is no KEY=value"));
        free(wrong);
    }
    CHECK(!strcmp(aw_base64("x-access-token:t"), "eC1hY2Nlc3MtdG9rZW46dA=="));

    /* GitHub: the repositories, a game with main, a branch and a tag, pull request #2 merged into main (its head kept
       as refs/pull/2/head, as GitHub keeps every one's), and an empty private one */
    mkdir(c.state, 0700);
    {
        char* mirrors = aw_format("%s/mirrors", c.state);
        mkdir(mirrors, 0700);
        free(mirrors);
    }
    CHECK(!sh("mkdir -p %s/gh/org %s/fj/org %s/work && git init --bare --quiet %s/gh/org/game.git && "
              "git init --bare --quiet %s/gh/org/empty.git",
              root, root, root, root, root));
    CHECK(!sh("cd %s/work && git init --quiet -b main && git -c user.name=T -c user.email=t@t commit --quiet "
              "--allow-empty -m one && git branch feature && git tag v1 && git switch --quiet -c fix-sound && "
              "git commit --quiet --allow-empty -m fix && git switch --quiet main && "
              "git merge --quiet --no-ff -m 'Merge #2' fix-sound && git remote add origin %s/gh/org/game.git && "
              "git push --quiet origin main feature v1 fix-sound:refs/pull/2/head",
              root, root));
    merged2 = rev("gh", "game", "refs/heads/main");
    github("orgs/org/repos?type=all&sort=full_name",
           "[{\"name\":\"empty\",\"private\":true,\"description\":\"\",\"default_branch\":\"main\"},"
           "{\"name\":\"game\",\"private\":false,\"description\":\"A game\",\"default_branch\":\"main\","
           "\"html_url\":\"https://github.com/org/game\"}]");
    github("repos/org/empty/labels?", "[]");
    github("repos/org/empty/issues?state=all&sort=updated&direction=asc", "[]");
    github("repos/org/empty/issues/comments?sort=updated&direction=asc", "[]");
    github("repos/org/game/labels?", "[{\"name\":\"bug\",\"color\":\"d73a4a\",\"description\":\"It is broken\"}]");
    github("repos/org/game/issues?state=all&sort=updated&direction=asc", issues_v1);
    github_pull(2, "bob:fix-sound", "main", merged2);
    github("repos/org/game/issues/comments?sort=updated&direction=asc", comments_v1);

    /* the first pass: both made, git the same, every number, the pull request as one, marked merged by GitHub's merge
       commit, its branch here gone with it. Its merge fails once (500): the repository stops with #2 made, and the
       next pass marks it merged rather than make it again */
    merge_fails = 1;
    CHECK(pass(&c, state, NULL) == 1);
    CHECK(next_of(state) == 3);
    CHECK(pass(&c, state, &written) == 0);
    game = repo_of("game");
    CHECK(repo_of("empty") && repo_of("empty")->private_ && !repo_of("empty")->releases && !repo_of("empty")->branch[0]);
    CHECK(game && !game->private_ && !game->releases && !strcmp(game->branch, "main") && game->pulls && game->manual);
    gh_refs = refs("gh", "game");
    fj_refs = refs("fj", "game");
    CHECK(strstr(gh_refs, "refs/heads/feature") && strstr(gh_refs, "refs/tags/v1") && !strcmp(gh_refs, fj_refs));
    free(gh_refs);
    free(fj_refs);
    CHECK(game && game->nissues == 4 && game->nlabels == 1 && !strcmp(game->labels[0].color, "#d73a4a"));
    if (game && game->nissues == 4) {
        CHECK(!strcmp(game->issues[0].title, "Sound is off") && !strcmp(game->issues[0].state, "open"));
        CHECK(strstr(game->issues[0].body, "> From GitHub: [#1](https://github.com/org/game/issues/1), opened by "
                                           "**@ada** on 2026-10-01.\n\nNo sound."));
        CHECK(game->issues[0].nlabels == 1 && game->issues[0].labels[0] == game->labels[0].id);
        CHECK(strstr(game->issues[1].body, "pull request [#2](https://github.com/org/game/pull/2) by **@bob**, "
                                           "2026-10-02, from `bob:fix-sound` into `main`, merged.\n\nFixes #1."));
        CHECK(game->issues[1].pull && !strcmp(game->issues[1].head, "accordwimsy/pull/2") &&
              !strcmp(game->issues[1].base, "main"));
        CHECK(game->issues[1].merged && !strcmp(game->issues[1].merge, merged2) &&
              !strcmp(game->issues[1].state, "closed"));
        CHECK(!has_branch(game, "accordwimsy/pull/2") && pull_state_of(state, "2") &&
              !strcmp(pull_state_of(state, "2"), "merged"));
        CHECK(!strcmp(game->issues[2].title, "GitHub #3 (not there)") && !strcmp(game->issues[2].state, "closed"));
        CHECK(!strcmp(game->issues[3].state, "closed") && strstr(game->issues[3].body, "It crashes."));
    }
    CHECK(game && game->ncomments == 2 && game->comments[0].issue == 1 &&
          strstr(game->comments[0].body, "> From GitHub: **@cy** on 2026-10-01, [the comment](https://github.com/org/"
                                         "game/issues/1#issuecomment-100).\n\nSame here."));
    CHECK(jnum(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(state, "repos"), "game"), "next") == 5);
    {
        char* saved = aw_format("%s/state/state.json", root);
        struct stat st;
        CHECK(!stat(saved, &st) && st.st_size > 0);
        free(saved);
    }

    /* the second pass, nothing changed: GitHub asked from where the first stopped, the Forgejo written to only for
       its Releases tab, turned off on every pass */
    ngh_asked = 0;
    github("repos/org/game/issues?state=all&sort=updated&direction=asc&since=2026-10-05T10:00:00Z",
           "[{\"number\":1,\"title\":\"Sound is off\",\"state\":\"open\",\"body\":\"No sound.\","
           "\"html_url\":\"https://github.com/org/game/issues/1\",\"user\":{\"login\":\"ada\"},"
           "\"created_at\":\"2026-10-01T10:00:00Z\",\"updated_at\":\"2026-10-05T10:00:00Z\",\"labels\":[{\"name\":\"bug\"}]}]");
    github("repos/org/game/issues/comments?sort=updated&direction=asc&since=2026-10-02T11:00:00Z",
           "[{\"id\":101,\"issue_url\":\"https://gh.test/repos/org/game/issues/2\",\"body\":\"Looks right.\","
           "\"html_url\":\"https://github.com/org/game/pull/2#issuecomment-101\",\"user\":{\"login\":\"ada\"},"
           "\"created_at\":\"2026-10-02T11:00:00Z\",\"updated_at\":\"2026-10-02T11:00:00Z\"}]");
    before = writes_total();
    CHECK(pass(&c, state, &written) == 0);
    CHECK(writes_total() - before == 2 && written == 2);
    CHECK(github_asked("issues?state=all&sort=updated&direction=asc&since=2026-10-05T10:00:00Z") == 1);
    CHECK(github_asked("issues/comments?sort=updated&direction=asc&since=2026-10-02T11:00:00Z") == 1);

    /* edits and new ones: #1 retitled, #5 new, a comment edited, one new, the branch deleted on GitHub */
    github("repos/org/game/issues?state=all&sort=updated&direction=asc&since=2026-10-05T10:00:00Z",
           "[{\"number\":1,\"title\":\"Sound is off on the title screen\",\"state\":\"closed\",\"body\":\"No sound.\","
           "\"html_url\":\"https://github.com/org/game/issues/1\",\"user\":{\"login\":\"ada\"},"
           "\"created_at\":\"2026-10-01T10:00:00Z\",\"updated_at\":\"2026-10-06T10:00:00Z\",\"labels\":[]},"
           "{\"number\":5,\"title\":\"Faster loading\",\"state\":\"open\",\"body\":\"\","
           "\"html_url\":\"https://github.com/org/game/issues/5\",\"user\":{\"login\":\"cy\"},"
           "\"created_at\":\"2026-10-06T09:00:00Z\",\"updated_at\":\"2026-10-06T09:00:00Z\",\"labels\":[]}]");
    github("repos/org/game/issues/comments?sort=updated&direction=asc&since=2026-10-02T11:00:00Z",
           "[{\"id\":100,\"issue_url\":\"https://gh.test/repos/org/game/issues/1\",\"body\":\"Same here, on 2.0.\","
           "\"html_url\":\"https://github.com/org/game/issues/1#issuecomment-100\",\"user\":{\"login\":\"cy\"},"
           "\"created_at\":\"2026-10-01T11:00:00Z\",\"updated_at\":\"2026-10-06T11:00:00Z\"},"
           "{\"id\":102,\"issue_url\":\"https://gh.test/repos/org/game/issues/5\",\"body\":\"Yes please.\","
           "\"html_url\":\"https://github.com/org/game/issues/5#issuecomment-102\",\"user\":{\"login\":\"ada\"},"
           "\"created_at\":\"2026-10-06T12:00:00Z\",\"updated_at\":\"2026-10-06T12:00:00Z\"}]");
    CHECK(!sh("git --git-dir=%s/gh/org/game.git branch --quiet -D feature", root));
    CHECK(pass(&c, state, NULL) == 0);
    if (game && game->nissues == 5) {
        CHECK(!strcmp(game->issues[0].title, "Sound is off on the title screen") &&
              !strcmp(game->issues[0].state, "closed") && game->issues[0].nlabels == 0);
        CHECK(!strcmp(game->issues[4].title, "Faster loading") && strstr(game->issues[4].body, "(no description)"));
    } else
        CHECK(!"#5 made");
    CHECK(game && game->ncomments == 3 && strstr(game->comments[0].body, "Same here, on 2.0.") &&
          game->comments[2].issue == 5);
    fj_refs = refs("fj", "game");
    CHECK(!strstr(fj_refs, "refs/heads/feature") && strstr(fj_refs, "refs/heads/main") && strstr(fj_refs, "refs/tags/v1"));
    free(fj_refs);

    /* pull requests: #6 from a fork's branch, which GitHub has only as refs/pull/6/head, open, labelled; #7 closed; #8
       into a branch gone and #9 with no head kept, issues saying why; #10 merged by a commit main does not hold (pushed
       over since), closed here, said */
    CHECK(!sh("cd %s/work && git switch --quiet -c faster main && git commit --quiet --allow-empty -m faster && "
              "git switch --quiet -c try main && git commit --quiet --allow-empty -m try && "
              "git switch --quiet -c lost main && git commit --quiet --allow-empty -m lost && "
              "git switch --quiet main && "
              "git push --quiet origin faster:refs/pull/6/head try:refs/pull/7/head try:refs/pull/8/head "
              "lost:refs/pull/10/head",
              root));
    {
        char* lost = rev("gh", "game", "refs/pull/10/head");
        github_pull(6, "eve:faster", "main", NULL);
        github_pull(7, "eve:try", "main", NULL);
        github_pull(8, "eve:try", "old-release", NULL);
        github_pull(9, "eve:gone", "main", NULL);
        github_pull(10, "eve:lost", "main", lost);
        free(lost);
    }
    issues_since("2026-10-06T10:00:00Z", 5,
                 pull_entry(6, "Faster loading", "open", NULL, "2026-10-07T01:00:00Z", "[{\"name\":\"bug\"}]"),
                 pull_entry(7, "Try this", "closed", NULL, "2026-10-07T02:00:00Z", "[]"),
                 pull_entry(8, "Backport", "closed", NULL, "2026-10-07T03:00:00Z", "[]"),
                 pull_entry(9, "Gone", "open", NULL, "2026-10-07T04:00:00Z", "[]"),
                 pull_entry(10, "Lost", "closed", "2026-10-07T05:00:00Z", "2026-10-07T05:00:00Z", "[]"));
    github("repos/org/game/issues/comments?sort=updated&direction=asc&since=2026-10-06T12:00:00Z", "[]");
    {
        /* #6 opened while the pass goes, after its fetch: a git whose fetch leaves refs/pull/6/head out. The
           repository stops there, #6 not made, and the next pass makes it */
        char* late = aw_format("%s/late-git", root);
        FILE* f = fopen(late, "w");
        fprintf(f, "#!/bin/sh\ngit \"$@\" || exit\n"
                   "if [ \"$2\" = fetch ]; then git \"$1\" update-ref -d refs/pull/6/head; fi\n");
        fclose(f);
        chmod(late, 0755);
        snprintf(c.git, sizeof c.git, "%s", late);
        CHECK(pass(&c, state, NULL) == 1);
        CHECK(game && game->nissues == 5 &&
              next_of(state) == 6);
        snprintf(c.git, sizeof c.git, "git");
        free(late);
    }
    CHECK(pass(&c, state, NULL) == 0);
    if (game && game->nissues == 10) {
        Issue* is = game->issues;
        char *theirs = rev("gh", "game", "refs/pull/6/head"),
             *ours = rev("fj", "game", "refs/heads/accordwimsy/pull/6");
        CHECK(is[5].pull && !strcmp(is[5].state, "open") && !strcmp(is[5].head, "accordwimsy/pull/6") &&
              !strcmp(is[5].base, "main") && is[5].nlabels == 1 && is[5].labels[0] == game->labels[0].id);
        CHECK(theirs[0] && !strcmp(theirs, ours));
        CHECK(strstr(is[5].body, "from `eve:faster` into `main`, open.\n\nPull 6.") &&
              !strstr(is[5].body, "as an issue"));
        CHECK(is[6].pull && !is[6].merged && !strcmp(is[6].state, "closed") && !has_branch(game, "accordwimsy/pull/7"));
        CHECK(!is[7].pull && !strcmp(is[7].state, "closed") &&
              strstr(is[7].body, "into `old-release`, closed. Made here as an issue: the branch it went into, "
                                 "old-release, is not on GitHub any more. Its changes: "
                                 "https://github.com/org/game/pull/8/files\n\nPull 8."));
        CHECK(!is[8].pull && !strcmp(is[8].state, "open") &&
              strstr(is[8].body, "Made here as an issue: its head is not on GitHub."));
        CHECK(is[9].pull && !is[9].merged && !strcmp(is[9].state, "closed") &&
              !has_branch(game, "accordwimsy/pull/10"));
        free(theirs);
        free(ours);
    } else
        CHECK(!"#6 to #10 made");
    CHECK(pull_state_of(state, "6") && !strcmp(pull_state_of(state, "6"), "open"));
    CHECK(pull_state_of(state, "7") && !strcmp(pull_state_of(state, "7"), "closed"));
    CHECK(pull_state_of(state, "10") && !strcmp(pull_state_of(state, "10"), "closed"));
    CHECK(!pull_state_of(state, "8") && !pull_state_of(state, "9"));
    gh_refs = refs("gh", "game");
    fj_refs = refs("fj", "game");
    CHECK(!strcmp(gh_refs, fj_refs));
    free(gh_refs);
    free(fj_refs);

    /* #7 reopened on GitHub: reopened here from its head, pushed again; #6, not updated, keeps its branch through the
       pass's prune of the branches GitHub has not */
    issues_since("2026-10-07T05:00:00Z", 1, pull_entry(7, "Try this", "open", NULL, "2026-10-08T01:00:00Z", "[]"));
    CHECK(pass(&c, state, NULL) == 0);
    if (game && game->nissues == 10) {
        char *theirs = rev("gh", "game", "refs/pull/6/head"),
             *ours = rev("fj", "game", "refs/heads/accordwimsy/pull/6");
        CHECK(!strcmp(game->issues[6].state, "open") && has_branch(game, "accordwimsy/pull/7"));
        CHECK(theirs[0] && !strcmp(theirs, ours));
        free(theirs);
        free(ours);
    }
    CHECK(pull_state_of(state, "7") && !strcmp(pull_state_of(state, "7"), "open"));

    /* #6 pushed to and retitled, #7 closed again: the branch moved, the title too; #7's branch gone */
    CHECK(!sh("cd %s/work && git switch --quiet faster && git commit --quiet --allow-empty -m 'faster, again' && "
              "git switch --quiet main && git push --quiet origin faster:refs/pull/6/head",
              root));
    issues_since("2026-10-08T01:00:00Z", 2,
                 pull_entry(6, "Faster loading, all of it", "open", NULL, "2026-10-09T01:00:00Z", "[]"),
                 pull_entry(7, "Try this", "closed", NULL, "2026-10-09T02:00:00Z", "[]"));
    CHECK(pass(&c, state, NULL) == 0);
    if (game && game->nissues == 10) {
        char *theirs = rev("gh", "game", "refs/pull/6/head"),
             *ours = rev("fj", "game", "refs/heads/accordwimsy/pull/6");
        CHECK(!strcmp(game->issues[5].title, "Faster loading, all of it") && game->issues[5].nlabels == 0);
        CHECK(theirs[0] && !strcmp(theirs, ours));
        CHECK(!strcmp(game->issues[6].state, "closed") && !has_branch(game, "accordwimsy/pull/7"));
        free(theirs);
        free(ours);
    }

    /* #6 merged on GitHub: marked merged here by the merge commit main now holds, its branch gone */
    CHECK(!sh("cd %s/work && git merge --quiet --no-ff -m 'Merge #6' faster && git push --quiet origin main", root));
    {
        char* merged6 = rev("gh", "game", "refs/heads/main");
        github_pull(6, "eve:faster", "main", merged6);
        issues_since("2026-10-09T02:00:00Z", 1,
                     pull_entry(6, "Faster loading, all of it", "closed", "2026-10-10T01:00:00Z",
                                "2026-10-10T01:00:00Z", "[]"));
        CHECK(pass(&c, state, NULL) == 0);
        CHECK(game && game->nissues == 10 && game->issues[5].merged && !strcmp(game->issues[5].merge, merged6) &&
              !strcmp(game->issues[5].state, "closed") && !has_branch(game, "accordwimsy/pull/6"));
        CHECK(pull_state_of(state, "6") && !strcmp(pull_state_of(state, "6"), "merged"));
        free(merged6);
    }

    /* a Forgejo whose next number is not GitHub's: said, the repository stopped, the state where it was */
    github("repos/org/game/issues?state=all&sort=updated&direction=asc&since=2026-10-10T01:00:00Z",
           "[{\"number\":11,\"title\":\"Eleven\",\"state\":\"open\",\"body\":\"11\","
           "\"html_url\":\"https://github.com/org/game/issues/11\",\"user\":{\"login\":\"ada\"},"
           "\"created_at\":\"2026-10-11T09:00:00Z\",\"updated_at\":\"2026-10-11T09:00:00Z\",\"labels\":[]}]");
    skew = 1;
    CHECK(pass(&c, state, NULL) == 1);
    CHECK(next_of(state) == 11);
    skew = 0;

    /* the program itself, while another pass holds the lock: it says so and does nothing */
    {
        char *lock = aw_format("%s/lock", c.state), *command = aw_format("./accordwimsy sync %s 2>&1", env), line[256] = "";
        int fd = open(lock, O_RDWR | O_CREAT, 0600);
        struct flock whole;
        FILE* p;
        memset(&whole, 0, sizeof whole);
        whole.l_type = F_WRLCK;
        whole.l_whence = SEEK_SET;
        CHECK(fd >= 0 && !fcntl(fd, F_SETLK, &whole));
        p = popen(command, "r");
        CHECK(p && fgets(line, sizeof line, p) && strstr(line, "a pass is going already"));
        CHECK(p && pclose(p) == 0);
        close(fd);
        free(lock);
        free(command);
    }
    {
        FILE* p = popen("./accordwimsy 2>&1", "r");
        char line[256] = "";
        CHECK(p && fgets(line, sizeof line, p) && strstr(line, "usage: accordwimsy sync"));
        CHECK(p && WEXITSTATUS(pclose(p)) == 2);
    }

    sh("rm -rf %s", root);
    cJSON_Delete(state);
    free(merged2);
    free(env);
    printf("test_aw: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
