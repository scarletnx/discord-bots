/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* HTTP(S) through libcurl, as aw.h's AwFetch: GitHub's API and the Forgejo's */
#ifndef AW_HTTP_H
#define AW_HTTP_H
#include "aw.h"

/* an AwFetch; ctx unused */
void aw_http(void* ctx, const char* method, const char* url, const char* auth, const char* body, AwAnswer* out);
#endif
