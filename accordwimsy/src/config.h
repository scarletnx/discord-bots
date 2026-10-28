/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The env file's settings, KEY=value a line (# a comment): the README's table */
#ifndef AW_CONFIG_H
#define AW_CONFIG_H
#include <stddef.h>

typedef struct {
    char forgejo[256];       /* ACCORDWIMSY_FORGEJO: the Forgejo's address, its API under /api/v1 */
    char forgejo_token[512]; /* ACCORDWIMSY_FORGEJO_TOKEN */
    char forgejo_user[128];  /* ACCORDWIMSY_FORGEJO_USER: whose the token is, for git's push */
    char org[128];           /* ACCORDWIMSY_ORG: the GitHub organization */
    char forgejo_org[128];   /* ACCORDWIMSY_FORGEJO_ORG: the Forgejo's, the GitHub one's name by default */
    char github_token[512];  /* GITHUB_TOKEN */
    char github_api[256];    /* ACCORDWIMSY_GITHUB_API */
    char github[256];        /* ACCORDWIMSY_GITHUB: where its repositories clone from */
    char state[1024];        /* ACCORDWIMSY_STATE: state.json, the lock and mirrors/ */
    char git[256];           /* ACCORDWIMSY_GIT */
} AwConfig;

/* the file at path read into c, the defaults first: 0, or nonzero with why (a key it does not know, a value too long,
   one needed and missing) */
int aw_config_read(AwConfig* c, const char* path, char* why, size_t size);
#endif
