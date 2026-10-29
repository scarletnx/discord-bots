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
/* the mirror's branches and tags pushed to to, and each one to has that the mirror has not deleted there by its name:
   never an AW_PULL_BRANCH one, nor refs/pull/. Not a --prune push: over HTTP it deleted the AW_PULL_BRANCH branches
   too, whatever negative refspec left them out, and the Forgejo closed their pull requests (git.scarletnx.dev,
   2026-10-28). 0, or git's status, said */
static int push_mirror(const AwConfig* c, const char* name, char* git_dir, const char* to) {
    char heads[] = "+refs/heads/*:refs/heads/*", tags[] = "+refs/tags/*:refs/tags/*", format[] = "--format=%(refname)",
         rh[] = "refs/heads", rt[] = "refs/tags";
    char* ls[] = {"git", "ls-remote", "--heads", "--tags", (char*)to, NULL};
    char* each[] = {"git", git_dir, "for-each-ref", format, rh, rt, NULL};
    char *theirs, *mine = NULL, *ours = NULL, *line, *next, **push;
    size_t n = 0, lines = 0, i;
    int r;
    if (!(theirs = git_out(c, ls, &r)) || !(mine = git_out(c, each, &r))) {
        aw_warn("%s: the refs of %s, or of the mirror, could not be listed (git: status %d)", name, to, r);
        free(theirs);
        return r ? r : 1;
    }
    ours = aw_format("\n%s", mine); /* each of the mirror's refs between two newlines */
    for (line = theirs; *line; ++line) lines += *line == '\n';
    push = calloc(lines + 8, sizeof *push);
    push[n++] = "git", push[n++] = git_dir, push[n++] = "push", push[n++] = "--quiet", push[n++] = (char*)to;
    push[n++] = heads, push[n++] = tags;
    for (line = theirs; *line; line = next) { /* "<object>\t<ref>\n" */
        char *tab = strchr(line, '\t'), *end = strchr(line, '\n'), *ref, *between;
        next = end ? end + 1 : line + strlen(line);
        if (!tab || (end && tab > end)) continue;
        ref = aw_format("%.*s", (int)(end ? end - tab - 1 : (long)strlen(tab + 1)), tab + 1);
        between = aw_format("\n%s\n", ref);
        if ((!strncmp(ref, "refs/heads/", 11) || !strncmp(ref, "refs/tags/", 10)) &&
            strncmp(ref, "refs/heads/accordwimsy/", 23) && !strstr(ref, "^{}") && !strstr(ours, between))
            push[n++] = aw_format(":%s", ref);
        free(between);
        free(ref);
    }
    r = git(c, push, 1);
    if (r) aw_warn("%s: git push to %s: status %d", name, to, r);
    for (i = 7; i < n; ++i) free(push[i]);
    free(push);
    free(ours);
    free(mine);
    free(theirs);
    return r;
}
int aw_mirror(const AwConfig* c, const char* name, const char* from, const char* to) {
    char *dir = mirror_of(c, name), *git_dir = aw_format("--git-dir=%s", dir);
    char heads[] = "+refs/heads/*:refs/heads/*", tags[] = "+refs/tags/*:refs/tags/*",
         pulls[] = "+refs/pull/*/head:refs/pull/*/head";
    struct stat st;
    int r = 0;
    if (stat(dir, &st)) {
        char* init[] = {"git", "init", "--bare", "--quiet", dir, NULL};
        r = git(c, init, 1);
        if (r) aw_warn("git init --bare %s: status %d", dir, r);
    }
    if (!r) {
        char* fetch[] = {"git", git_dir, "fetch", "--prune", "--quiet", (char*)from, heads, tags, pulls, NULL};
        r = git(c, fetch, 1);
        if (r) aw_warn("%s: git fetch from %s: status %d", name, from, r);
    }
    if (!r) {
        char* any[] = {"git", git_dir, "show-ref", "--heads", "--tags", "--quiet", NULL};
        if (git(c, any, 1)) /* a repository with no commit yet: git pushes nothing, and says so as a failure */
            aw_log("%s: no branch or tag on GitHub yet, nothing to push", name);
        else
            r = push_mirror(c, name, git_dir, to);
    }
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
