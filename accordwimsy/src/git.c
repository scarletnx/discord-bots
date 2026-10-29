/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "git.h"
#include "aw.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* the child's side of a git run: the GitHub's and the Forgejo's Authorization headers for their addresses, no one's
   settings, then git args (args[0] "git") in its place */
static void exec_git(const AwConfig* c, char* const* args) {
    char *github = aw_format("x-access-token:%s", c->github_token), *forgejo = aw_format("%s:%s", c->forgejo_user,
                                                                                           c->forgejo_token);
    char *g64 = aw_base64(github), *f64 = aw_base64(forgejo);
    char *gkey = aw_format("http.%s/.extraheader", c->github), *fkey = aw_format("http.%s/.extraheader", c->forgejo);
    char *gval = aw_format("Authorization: Basic %s", g64), *fval = aw_format("Authorization: Basic %s", f64);
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1); /* no one's settings: an insteadOf, a signing, a hook */
    setenv("GIT_CONFIG_COUNT", "2", 1);
    setenv("GIT_CONFIG_KEY_0", gkey, 1);
    setenv("GIT_CONFIG_VALUE_0", gval, 1);
    setenv("GIT_CONFIG_KEY_1", fkey, 1);
    setenv("GIT_CONFIG_VALUE_1", fval, 1);
    if (strchr(c->git, '/'))
        execv(c->git, args);
    else
        execvp(c->git, args);
    fprintf(stderr, "accordwimsy: cannot run %s\n", c->git);
    _exit(127);
}
static int status_of(pid_t child) {
    int status;
    while (waitpid(child, &status, 0) < 0) continue;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
/* git args, quiet unless loud: its exit status, or -1 when it could not start */
static int git(const AwConfig* c, char* const* args, int quiet) {
    pid_t child;
    fflush(NULL);
    child = fork();
    if (child < 0) return -1;
    if (!child) {
        if (quiet && !freopen("/dev/null", "w", stdout)) _exit(126);
        exec_git(c, args);
    }
    return status_of(child);
}
/* what git args prints on its standard output (owned), or NULL when it fails or cannot start; *status its status */
static char* git_out(const AwConfig* c, char* const* args, int* status) {
    int fds[2];
    pid_t child;
    Buf b = {0};
    char chunk[4096];
    ssize_t n;
    fflush(NULL);
    *status = -1;
    if (pipe(fds)) return NULL;
    child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (!child) {
        close(fds[0]);
        if (dup2(fds[1], 1) < 0) _exit(126);
        close(fds[1]);
        exec_git(c, args);
    }
    close(fds[1]);
    while ((n = read(fds[0], chunk, sizeof chunk)) != 0) {
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) break;
        buf_addn(&b, chunk, (size_t)n);
    }
    close(fds[0]);
    *status = status_of(child);
    if (*status) {
        buf_free(&b);
        return NULL;
    }
    return buf_take(&b);
}
static char* mirror_of(const AwConfig* c, const char* name) { return aw_format("%s/mirrors/%s.git", c->state, name); }
/* the mirror's (git_dir) branches and tags, "<object> <ref>" a line: owned, or NULL */
static char* mirror_refs(const AwConfig* c, char* git_dir) {
    char format[] = "--format=%(objectname) %(refname)", rh[] = "refs/heads", rt[] = "refs/tags";
    char* each[] = {"git", git_dir, "for-each-ref", format, rh, rt, NULL};
    int status;
    return git_out(c, each, &status);
}
/* the object ref has among refs ("<object><sep><ref>" lines) into object: 1, or 0 when refs has no ref */
static int object_of(const char* refs, const char* ref, char sep, char object[80]) {
    const char* line;
    size_t n = strlen(ref);
    for (line = refs; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : line + strlen(line)) {
        const char* at = strchr(line, sep);
        if (at && at < (strchr(line, '\n') ? strchr(line, '\n') : line + strlen(line)) && !strncmp(at + 1, ref, n) &&
            (at[1 + n] == '\n' || !at[1 + n])) {
            snprintf(object, 80, "%.*s", (int)(at - line), line);
            return 1;
        }
    }
    return 0;
}
/* whether from moved to to rewrites its history: 0 when to holds from, 1 when it does not or git cannot say (said) */
static int rewritten(const AwConfig* c, const char* name, char* git_dir, char* from, char* to) {
    char ancestor[] = "--is-ancestor";
    char* base[] = {"git", git_dir, "merge-base", ancestor, from, to, NULL};
    int r = git(c, base, 1);
    if (r > 1) aw_warn("%s: git merge-base %.12s %.12s: status %d, taken as rewritten", name, from, to, r);
    return r != 0;
}
/* the mirror's branches and tags pushed to to, never deleting (no --prune: over HTTP it deleted the AW_PULL_BRANCH
   branches too, whatever negative refspec left them out, and the Forgejo closed their pull requests, git.scarletnx.dev
   2026-10-28). A branch whose new tip does not hold the old one, or a tag moved, keeps its old tip there as
   AW_KEPT_BRANCH, said. *stale: what to has under refs/heads and refs/tags that the mirror has not, a ref a line (an
   AW_KEPT_BRANCH one too; never an AW_PULL_BRANCH one). 0, or git's status, said */
static int push_mirror(const AwConfig* c, const char* name, char* git_dir, const char* to, const char* stamp,
                       const char* before, char** stale) {
    char heads[] = "+refs/heads/*:refs/heads/*", tags[] = "+refs/tags/*:refs/tags/*", quiet[] = "--quiet",
         verb[] = "push", list[] = "ls-remote", h[] = "--heads", t[] = "--tags";
    char* ls[] = {"git", list, h, t, (char*)to, NULL};
    char *after = mirror_refs(c, git_dir), *theirs = NULL, **push, object[80];
    const char *line, *end;
    size_t n = 0, lines = 0, i;
    int r = 0;
    Buf out = {0};
    if (!after) {
        aw_warn("%s: the mirror's refs could not be listed", name);
        return 1;
    }
    for (line = before; *line; ++line) lines += *line == '\n';
    push = calloc(lines + 8, sizeof *push);
    push[n++] = "git", push[n++] = git_dir, push[n++] = verb, push[n++] = quiet, push[n++] = (char*)to;
    push[n++] = heads, push[n++] = tags;
    for (line = before; *line; line = *end ? end + 1 : end) { /* "<object> <ref>": the mirror before the fetch */
        const char* space = strchr(line, ' ');
        char *was, *ref;
        end = strchr(line, '\n') ? strchr(line, '\n') : line + strlen(line);
        if (!space || space > end) continue;
        was = aw_format("%.*s", (int)(space - line), line);
        ref = aw_format("%.*s", (int)(end - space - 1), space + 1);
        if (object_of(after, ref, ' ', object) && strcmp(was, object) &&
            (!strncmp(ref, "refs/tags/", 10) || rewritten(c, name, git_dir, was, object))) {
            int tag = !strncmp(ref, "refs/tags/", 10);
            char* kept =
                aw_format("refs/heads/" AW_KEPT_BRANCH "/%s/%s/%s", tag ? "tag" : "branch", ref + (tag ? 10 : 11), stamp);
            aw_warn("%s: %s was rewritten on GitHub: its old tip %.12s is kept here as %s", name, ref, was, kept + 11);
            push[n++] = aw_format("%s:%s", was, kept);
            free(kept);
        }
        free(ref);
        free(was);
    }
    if ((r = git(c, push, 1)) != 0)
        aw_warn("%s: git push to %s: status %d", name, to, r);
    else if (!(theirs = git_out(c, ls, &r))) {
        aw_warn("%s: git ls-remote %s: status %d", name, to, r);
        r = r ? r : 1;
    } else
        for (line = theirs; *line; line = *end ? end + 1 : end) { /* "<object>\t<ref>" */
            const char* tab = strchr(line, '\t');
            char* ref;
            end = strchr(line, '\n') ? strchr(line, '\n') : line + strlen(line);
            if (!tab || tab > end) continue;
            ref = aw_format("%.*s", (int)(end - tab - 1), tab + 1);
            if ((!strncmp(ref, "refs/heads/", 11) || !strncmp(ref, "refs/tags/", 10)) &&
                strncmp(ref, "refs/heads/accordwimsy/pull/", 28) && !strstr(ref, "^{}") &&
                !object_of(after, ref, ' ', object))
                buf_printf(&out, "%s\n", ref);
            free(ref);
        }
    *stale = r ? NULL : buf_take(&out);
    buf_free(&out);
    for (i = 7; i < n; ++i) free(push[i]);
    free(push);
    free(theirs);
    free(after);
    return r;
}
int aw_mirror(const AwConfig* c, const char* name, const char* from, const char* to, const char* stamp, char** stale) {
    char *dir = mirror_of(c, name), *git_dir = aw_format("--git-dir=%s", dir), *before = NULL;
    char heads[] = "+refs/heads/*:refs/heads/*", tags[] = "+refs/tags/*:refs/tags/*",
         pulls[] = "+refs/pull/*/head:refs/pull/*/head", no_gc[] = "-c", gc_off[] = "gc.auto=0";
    struct stat st;
    int r = 0;
    *stale = NULL;
    if (stat(dir, &st)) {
        char* init[] = {"git", "init", "--bare", "--quiet", dir, NULL};
        r = git(c, init, 1);
        if (r) aw_warn("git init --bare %s: status %d", dir, r);
    }
    if (!r && !(before = mirror_refs(c, git_dir))) {
        aw_warn("%s: the mirror's refs could not be listed", name);
        r = 1;
    }
    if (!r) { /* no gc while it fetches: a rewritten branch's old tip stays in the mirror until it is kept */
        char* fetch[] = {"git",   no_gc,       gc_off, git_dir, "fetch", "--prune",
                         "--quiet", (char*)from, heads,  tags,    pulls,   NULL};
        r = git(c, fetch, 1);
        if (r) aw_warn("%s: git fetch from %s: status %d", name, from, r);
    }
    if (!r) {
        char* any[] = {"git", git_dir, "show-ref", "--heads", "--tags", "--quiet", NULL};
        char* gc[] = {"git", git_dir, "gc", "--auto", "--quiet", NULL};
        if (git(c, any, 1)) { /* a repository with no commit yet: git pushes nothing, and says so as a failure */
            aw_log("%s: no branch or tag on GitHub yet, nothing to push", name);
            *stale = aw_strdup("");
        } else
            r = push_mirror(c, name, git_dir, to, stamp, before, stale);
        if (!r && git(c, gc, 1)) aw_warn("%s: git gc --auto in the mirror failed", name);
    }
    free(before);
    free(git_dir);
    free(dir);
    return r;
}
/* refs (a ref a line) deleted from to: 0, or git's status, said */
int aw_delete_refs(const AwConfig* c, const char* name, const char* to, const char* refs) {
    char *dir = mirror_of(c, name), *git_dir = aw_format("--git-dir=%s", dir), **push;
    const char *line, *end;
    size_t n = 0, lines = 0, i;
    int r;
    for (line = refs; *line; ++line) lines += *line == '\n';
    push = calloc(lines + 8, sizeof *push);
    push[n++] = "git", push[n++] = git_dir, push[n++] = "push", push[n++] = "--quiet", push[n++] = (char*)to;
    for (line = refs; *line; line = *end ? end + 1 : end) {
        end = strchr(line, '\n') ? strchr(line, '\n') : line + strlen(line);
        if (end > line) push[n++] = aw_format(":%.*s", (int)(end - line), line);
    }
    r = n > 5 ? git(c, push, 1) : 0;
    if (r) aw_warn("%s: git push deleting from %s: status %d", name, to, r);
    for (i = 5; i < n; ++i) free(push[i]);
    free(push);
    free(git_dir);
    free(dir);
    return r;
}
static int has_ref(const AwConfig* c, const char* name, char* ref) {
    char *dir = mirror_of(c, name), *git_dir = aw_format("--git-dir=%s", dir);
    char* verify[] = {"git", git_dir, "rev-parse", "--verify", "--quiet", ref, NULL};
    int has = git(c, verify, 1) == 0;
    free(ref);
    free(git_dir);
    free(dir);
    return has;
}
int aw_has_branch(const AwConfig* c, const char* name, const char* branch) {
    return has_ref(c, name, aw_format("refs/heads/%s", branch));
}
int aw_has_pull(const AwConfig* c, const char* name, int number) {
    return has_ref(c, name, aw_format("refs/pull/%d/head", number));
}
int aw_remote_has(const AwConfig* c, const char* name, const char* from, const char* ref) {
    char* ls[] = {"git", "ls-remote", "--exit-code", (char*)from, (char*)ref, NULL};
    int r = git(c, ls, 1); /* 2: no such ref */
    if (r == 0 || r == 2) return r == 0;
    aw_warn("%s: git ls-remote %s %s: status %d", name, from, ref, r);
    return -1;
}
int aw_push_pull(const AwConfig* c, const char* name, const char* to, int number) {
    char *dir = mirror_of(c, name), *git_dir = aw_format("--git-dir=%s", dir),
         *spec = aw_format("+refs/pull/%d/head:refs/heads/" AW_PULL_BRANCH, number, number);
    char* push[] = {"git", git_dir, "push", "--quiet", (char*)to, spec, NULL};
    int r = git(c, push, 1);
    if (r) aw_warn("%s: git push of pull request #%d's head to %s: status %d", name, number, to, r);
    free(spec);
    free(git_dir);
    free(dir);
    return r;
}
