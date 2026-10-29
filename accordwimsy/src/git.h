/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A repository's branches and tags through a bare mirror of them: fetched from GitHub, pushed to the Forgejo */
#ifndef AW_GIT_H
#define AW_GIT_H
#include "config.h"

/* The Forgejo's branch of GitHub's pull request number, which its pull request is made from: GitHub keeps every pull
   request's head in its own repository (refs/pull/<number>/head), a fork's too */
#define AW_PULL_BRANCH "accordwimsy/pull/%d"
/* Where the Forgejo keeps the old tip of a branch (or a tag) GitHub rewrote: AW_KEPT_BRANCH/branch/<name>/<stamp> */
#define AW_KEPT_BRANCH "accordwimsy/kept"

/* mirrors/<name>.git made when there is none, then from's branches and tags fetched into it (pruned of what from has
   not), its pull requests' heads as refs/pull/<n>/head, and the branches and tags pushed to to, which loses nothing:
   a rewritten branch's (or a moved tag's) old tip is kept there as AW_KEPT_BRANCH/.../<stamp>, said, and nothing is
   deleted. *stale (owned): what to has under refs/heads and refs/tags that from has not, a ref a line (never an
   AW_PULL_BRANCH one), for aw_delete_refs once it has been gone long enough; *pull_branches (owned): the AW_PULL_BRANCH
   branches to has, a ref a line. The tokens go in git's environment, never its arguments. 0, or git's status, said */
int aw_mirror(const AwConfig* c, const char* name, const char* from, const char* to, const char* stamp, char** stale,
              char** pull_branches);
/* refs (a ref a line) deleted from to: 0, or git's status, said */
int aw_delete_refs(const AwConfig* c, const char* name, const char* to, const char* refs);
/* 1 when the mirror of name has the branch */
int aw_has_branch(const AwConfig* c, const char* name, const char* branch);
/* 1 when the mirror of name has GitHub's pull request number's head */
int aw_has_pull(const AwConfig* c, const char* name, int number);
/* 1 when from has ref now, 0 when it has not, -1 when it could not be asked (said) */
int aw_remote_has(const AwConfig* c, const char* name, const char* from, const char* ref);
/* the head of pull request number pushed to to as its AW_PULL_BRANCH, made or moved there: 0, or git's status, said */
int aw_push_pull(const AwConfig* c, const char* name, const char* to, int number);
#endif
