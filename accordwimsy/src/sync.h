/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* One pass: each repository of the GitHub organization, the new ones too, the same on the Forgejo. Its branches and
   tags (git.h); its labels; its issues and pull requests by their numbers, which the Forgejo then gives the same (a
   number GitHub has no more, deleted or moved away, a closed issue saying so); a pull request as a pull request into
   the same branch, from its head (GitHub keeps every one's, a fork's too) pushed as a branch of its own while it is
   open, merged by GitHub's merge commit, closed or reopened as there; as an issue saying why only when it cannot be
   one (its head or the branch it went into gone from GitHub); the comments.
   Each says its GitHub author and links its GitHub page; an edit on GitHub (its updated_at) edits it. Releases stay
   on GitHub: the repository's Releases tab is turned off on every pass. */
#ifndef AW_SYNC_H
#define AW_SYNC_H
#include "aw.h"
#include "config.h"

typedef struct {
    const AwConfig* config;
    AwFetch fetch;
    void* ctx;
    cJSON* state;  /* state.json: per repository, what was made of it */
    long writes;   /* the Forgejo's POST, PATCH and PUT, for the log */
    int failures;  /* repositories a pass stopped on */
    char* state_path;
} Aw;

/* every repository, state.json written after each: 0, or 1 when one or more stopped (each said why) */
int aw_sync(Aw* a);
#endif
