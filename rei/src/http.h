/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* HTTPS through libcurl, as github.h's RgFetch: the requests to GitHub and to Discord's REST API */
#ifndef REI_HTTP_H
#define REI_HTTP_H
#include "github.h"

typedef struct {
    char authorization[512]; /* "Bearer <token>" for GitHub, "Bot <token>" for Discord; "" for none */
    const char* accept;      /* GitHub's media type, or NULL */
} RhAuth;
/* an RgFetch, ctx an RhAuth */
void rh_fetch(void* ctx, const char* method, const char* url, const char* body, const char* etag, RgAnswer* out);
#endif
