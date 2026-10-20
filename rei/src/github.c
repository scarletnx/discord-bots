/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "github.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API "https://api.github.com/"

void rg_init(Rg* g, const char* repo, const char* nightly, RgFetch fetch, void* ctx) {
    memset(g, 0, sizeof *g);
    snprintf(g->repo, sizeof g->repo, "%s", repo);
    if (nightly && *nightly)
        snprintf(g->nightly, sizeof g->nightly, "%s", nightly);
    else {
        size_t owner = strcspn(repo, "/");
        snprintf(g->nightly, sizeof g->nightly, "%.*s/nightly", (int)owner, repo);
    }
    g->fetch = fetch;
    g->ctx = ctx;
}
static void answer_free(RgAnswer* a) {
    free(a->body);
    free(a->etag);
    free(a->error);
    memset(a, 0, sizeof *a);
}
void rg_free(Rg* g) {
    int i;
    for (i = 0; i < g->nkept; ++i) {
        free(g->kept[i].url);
        free(g->kept[i].etag);
        free(g->kept[i].body);
    }
    free(g->kept);
    g->kept = NULL;
    g->nkept = g->capkept = 0;
}

static RgKept* kept_of(Rg* g, const char* url) {
    int i;
    for (i = 0; i < g->nkept; ++i)
        if (!strcmp(g->kept[i].url, url)) return &g->kept[i];
    return NULL;
}
/* url's answer kept with its etag, the oldest dropped past RG_KEPT */
static void keep(Rg* g, const char* url, const char* etag, const char* body) {
    RgKept* k = kept_of(g, url);
    if (!k) {
        if (g->nkept == RG_KEPT) {
            int oldest = 0, i;
            for (i = 1; i < g->nkept; ++i)
                if (g->kept[i].when < g->kept[oldest].when) oldest = i;
            free(g->kept[oldest].url);
            free(g->kept[oldest].etag);
            free(g->kept[oldest].body);
            g->kept[oldest] = g->kept[--g->nkept];
        }
        if (g->nkept == g->capkept) {
            int cap = g->capkept ? 2 * g->capkept : 64;
            RgKept* more = realloc(g->kept, (size_t)cap * sizeof *more);
            if (!more) return;
            g->kept = more;
            g->capkept = cap;
        }
        k = &g->kept[g->nkept++];
        k->url = rei_strdup(url);
    } else {
        free(k->etag);
        free(k->body);
    }
    k->etag = rei_strdup(etag);
    k->body = rei_strdup(body);
    k->when = rei_now();
}

cJSON* rg_get(Rg* g, const char* path, int* status) {
    char* url = rei_format(API "%s", path);
    RgKept* k = kept_of(g, url);
    RgAnswer a = {0};
    cJSON* found = NULL;
    g->fetch(g->ctx, "GET", url, NULL, k ? k->etag : NULL, &a);
    ++g->calls;
    if (status) *status = a.status;
    if (a.status == 304 && k) {
        ++g->unchanged;
        k->when = rei_now();
        found = cJSON_Parse(k->body);
    } else if (a.status >= 200 && a.status < 300 && a.body) {
        found = cJSON_Parse(a.body);
        if (!found)
            rei_log("GitHub: %s answered %d with %zu bytes that are not JSON", path, a.status, strlen(a.body));
        else if (a.etag)
            keep(g, url, a.etag, a.body);
    } else if (a.status != 404)
        rei_log("GitHub: %s answered %d: %.200s", path, a.status, a.error ? a.error : a.body ? a.body : "nothing");
    answer_free(&a);
    free(url);
    return found;
}

cJSON* rg_pages(Rg* g, const char* path, const char* key) {
    cJSON* all = cJSON_CreateArray();
    int page;
    for (page = 1;; ++page) {
        char* at = rei_format("%s%sper_page=100&page=%d", path, strchr(path, '?') ? "&" : "?", page);
        cJSON* got = rg_get(g, at, NULL);
        cJSON* items = key ? cJSON_GetObjectItemCaseSensitive(got, key) : got;
        int n;
        free(at);
        if (!cJSON_IsArray(items)) {
            cJSON_Delete(got);
            cJSON_Delete(all);
            return NULL;
        }
        n = cJSON_GetArraySize(items);
        while (cJSON_GetArraySize(items)) cJSON_AddItemToArray(all, cJSON_DetachItemFromArray(items, 0));
        cJSON_Delete(got);
        if (n < 100) return all;
    }
}

cJSON* rg_graphql(Rg* g, const char* query) {
    cJSON* request = cJSON_CreateObject();
    char* body;
    RgAnswer a = {0};
    cJSON *found = NULL, *data = NULL;
    cJSON_AddStringToObject(request, "query", query);
    body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    g->fetch(g->ctx, "POST", API "graphql", body, NULL, &a);
    ++g->calls;
    if (a.status == 200 && a.body) found = cJSON_Parse(a.body);
    data = found ? cJSON_DetachItemFromObjectCaseSensitive(found, "data") : NULL;
    if (!cJSON_IsObject(data))
        rei_log("GitHub: GraphQL answered %d: %.200s", a.status, a.error ? a.error : a.body ? a.body : "nothing");
    if (data && !cJSON_IsObject(data)) {
        cJSON_Delete(data);
        data = NULL;
    }
    cJSON_Delete(found);
    answer_free(&a);
    free(body);
    return data;
}
