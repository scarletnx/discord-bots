/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "git.h"
#include "aw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* git args (args[0] "git"), quiet unless loud, with the GitHub's and the Forgejo's Authorization headers for their
   addresses: its exit status, or -1 when it could not start */
static int git(const AwConfig* c, char* const* args, int quiet) {
    pid_t child;
    int status;
    fflush(NULL);
    child = fork();
    if (child < 0) return -1;
    if (!child) {
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
        if (quiet && !freopen("/dev/null", "w", stdout)) _exit(126);
        if (strchr(c->git, '/'))
            execv(c->git, args);
        else
            execvp(c->git, args);
        fprintf(stderr, "accordwimsy: cannot run %s\n", c->git);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) continue;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
static char* mirror_of(const AwConfig* c, const char* name) { return aw_format("%s/mirrors/%s.git", c->state, name); }
int aw_mirror(const AwConfig* c, const char* name, const char* from, const char* to) {
    char *dir = mirror_of(c, name), *git_dir = aw_format("--git-dir=%s", dir);
    char heads[] = "+refs/heads/*:refs/heads/*", tags[] = "+refs/tags/*:refs/tags/*";
    struct stat st;
    int r = 0;
    if (stat(dir, &st)) {
        char* init[] = {"git", "init", "--bare", "--quiet", dir, NULL};
        r = git(c, init, 1);
        if (r) aw_warn("git init --bare %s: status %d", dir, r);
    }
    if (!r) {
        char* fetch[] = {"git", git_dir, "fetch", "--prune", "--quiet", (char*)from, heads, tags, NULL};
        r = git(c, fetch, 1);
        if (r) aw_warn("%s: git fetch from %s: status %d", name, from, r);
    }
    if (!r) {
        char* any[] = {"git", git_dir, "show-ref", "--heads", "--tags", "--quiet", NULL};
        char* push[] = {"git", git_dir, "push", "--prune", "--quiet", (char*)to, heads, tags, NULL};
        if (git(c, any, 1)) /* a repository with no commit yet: git pushes nothing, and says so as a failure */
            aw_log("%s: no branch or tag on GitHub yet, nothing to push", name);
        else if ((r = git(c, push, 1)) != 0)
            aw_warn("%s: git push to %s: status %d", name, to, r);
    }
    free(git_dir);
    free(dir);
    return r;
}
int aw_has_branch(const AwConfig* c, const char* name, const char* branch) {
    char *dir = mirror_of(c, name), *git_dir = aw_format("--git-dir=%s", dir), *ref = aw_format("refs/heads/%s", branch);
    char* verify[] = {"git", git_dir, "rev-parse", "--verify", "--quiet", ref, NULL};
    int has = git(c, verify, 1) == 0;
    free(ref);
    free(git_dir);
    free(dir);
    return has;
}
