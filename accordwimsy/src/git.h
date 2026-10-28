/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A repository's branches and tags through a bare mirror of them: fetched from GitHub, pushed to the Forgejo */
#ifndef AW_GIT_H
#define AW_GIT_H
#include "config.h"

/* mirrors/<name>.git made when there is none, then from's branches and tags fetched into it and pushed to to, each
   side pruned of what the other has not (never refs/pull/: the Forgejo's own). The tokens go in git's environment,
   never its arguments. 0, or git's status, said */
int aw_mirror(const AwConfig* c, const char* name, const char* from, const char* to);
/* 1 when the mirror of name has the branch */
int aw_has_branch(const AwConfig* c, const char* name, const char* branch);
#endif
