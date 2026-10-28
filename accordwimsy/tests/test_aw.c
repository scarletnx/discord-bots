/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AccordWimsy's gate test: a fake GitHub (its answers by URL) and a fake Forgejo (a little of its API, with the state
   a real one keeps: repositories, the issues' numbers, labels, comments), git for real on bare repositories in a
   temporary folder. No network. */
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

/* ---- the fake Forgejo ---- */
typedef struct {
    int number;
    char *title, *body, *state;
    int labels[8], nlabels;
} Issue;
typedef struct {
    int id, issue;
    char* body;
} Comment;
typedef struct {
    char name[64], branch[64], description[128];
    int private_, releases;
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
static Repo* repo_of(const char* name) {
    int i;
    for (i = 0; i < nrepos; ++i)
        if (!strcmp(repos[i].name, name)) return &repos[i];
    return NULL;
}
static Issue* issue_of(Repo* r, int number) { return number >= 1 && number <= r->nissues ? &r->issues[number - 1] : NULL; }
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
        out->status = 201;
        out->body = aw_format("{\"number\":%d,\"state\":\"open\"}", is->number);
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
static char* refs(const char* side, const char* name) {
    char *command = aw_format("git --git-dir=%s/%s/org/%s.git for-each-ref --format='%%(refname) %%(objectname)'", root,
                              side, name);
    FILE* p = popen(command, "r");
    Buf b = {0};
    char line[256];
    while (p && fgets(line, sizeof line, p)) buf_add(&b, line);
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
static int writes_total(void) { return writes[0] + writes[1] + writes[2]; }

int main(void) {
    AwConfig c;
    char *env, why[512], *gh_refs, *fj_refs;
    cJSON* state = cJSON_CreateObject();
    Repo* game;
    long written;
    int before;
    char template[] = "/tmp/accordwimsy-test.XXXXXX";
    if (!mkdtemp(template)) return 1;
    /* no one's git settings: a commit signed through a pinentry, an insteadOf */
    setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
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

    /* GitHub: the repositories, a game with main, a branch and a tag, and an empty private one */
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
              "--allow-empty -m one && git branch feature && git tag v1 && git remote add origin %s/gh/org/game.git && "
              "git push --quiet origin main feature v1",
              root, root));
    github("orgs/org/repos?type=all&sort=full_name",
           "[{\"name\":\"empty\",\"private\":true,\"description\":\"\",\"default_branch\":\"main\"},"
           "{\"name\":\"game\",\"private\":false,\"description\":\"A game\",\"default_branch\":\"main\","
           "\"html_url\":\"https://github.com/org/game\"}]");
    github("repos/org/empty/labels?", "[]");
    github("repos/org/empty/issues?state=all&sort=updated&direction=asc", "[]");
    github("repos/org/empty/issues/comments?sort=updated&direction=asc", "[]");
    github("repos/org/game/labels?", "[{\"name\":\"bug\",\"color\":\"d73a4a\",\"description\":\"It is broken\"}]");
    github("repos/org/game/issues?state=all&sort=updated&direction=asc", issues_v1);
    github("repos/org/game/pulls/2", "{\"head\":{\"label\":\"bob:fix-sound\"},\"base\":{\"ref\":\"main\"}}");
    github("repos/org/game/issues/comments?sort=updated&direction=asc", comments_v1);

    /* the first pass: both made, git the same, every number, the pull request as an issue saying so */
    CHECK(pass(&c, state, &written) == 0);
    game = repo_of("game");
    CHECK(repo_of("empty") && repo_of("empty")->private_ && !repo_of("empty")->releases && !repo_of("empty")->branch[0]);
    CHECK(game && !game->private_ && !game->releases && !strcmp(game->branch, "main"));
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
                                           "2026-10-02, from `bob:fix-sound` into `main`, merged. Its changes: "
                                           "https://github.com/org/game/pull/2/files"));
        CHECK(!strcmp(game->issues[1].state, "closed"));
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

    /* a Forgejo whose next number is not GitHub's: said, the repository stopped, the state where it was */
    github("repos/org/game/issues?state=all&sort=updated&direction=asc&since=2026-10-06T10:00:00Z",
           "[{\"number\":6,\"title\":\"Six\",\"state\":\"open\",\"body\":\"6\","
           "\"html_url\":\"https://github.com/org/game/issues/6\",\"user\":{\"login\":\"ada\"},"
           "\"created_at\":\"2026-10-07T09:00:00Z\",\"updated_at\":\"2026-10-07T09:00:00Z\",\"labels\":[]}]");
    skew = 1;
    CHECK(pass(&c, state, NULL) == 1);
    CHECK(jnum(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(state, "repos"), "game"), "next") == 6);
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
    free(env);
    printf("test_aw: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
