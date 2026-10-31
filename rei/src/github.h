/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GitHub's REST and GraphQL APIs through a fetch it is given (http.c's libcurl, or the tests' fixtures). Every GET is
   kept with its ETag and asked again with If-None-Match: an answer of 304 costs nothing of GitHub's 5000 an hour */
#ifndef REI_GITHUB_H
#define REI_GITHUB_H
#include "rei.h"

typedef struct {
    int status;  /* HTTP's, 0 when no answer came */
    char* body;  /* owned, or NULL */
    char* etag;  /* owned, or NULL */
    char* error; /* owned: why no answer came, or NULL */
} RgAnswer;
/* one request: method (GET or POST) url, body for a POST, If-None-Match etag when given */
typedef void (*RgFetch)(void* ctx, const char* method, const char* url, const char* body, const char* etag,
                        RgAnswer* out);

typedef struct {
    char* url;
    char* etag;
    char* body;
    double when;
} RgKept;
typedef struct {
    char repo[128];    /* OWNER/NAME: the issues and pull requests */
    char nightly[128]; /* OWNER/NAME: the nightlies' and the pull requests' builds */
    RgFetch fetch;
    void* ctx;
    RgKept* kept;
    int nkept, capkept;
    long calls, unchanged; /* requests made, and the 304s among them */
} Rg;

void rg_init(Rg* g, const char* repo, const char* nightly, RgFetch fetch, void* ctx);
void rg_free(Rg* g);
/* GET https://api.github.com/<path>: its JSON (owned), or NULL; *status (when given) the answer's HTTP status, 404 for
   none there. A failure is said in the log with the path and the status */
cJSON* rg_get(Rg* g, const char* path, int* status);
/* every item of a listing, 100 a page; key names the array of a listing that is an object ("workflow_runs", "jobs"),
   NULL for one that is an array. An array (owned), or NULL when a page failed */
cJSON* rg_pages(Rg* g, const char* path, const char* key);
/* a GraphQL query's "data" (owned), or NULL */
cJSON* rg_graphql(Rg* g, const char* query);
/* the most GETs kept; the oldest go first */
#define RG_KEPT 4000
#endif
