/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "http.h"
#include <ctype.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define AGENT "DiscordBot (https://github.com/scarletnx/discord-bots, 1) reiwimsy"

static size_t body_of(char* data, size_t size, size_t n, void* to) {
    buf_addn(to, data, size * n);
    return size * n;
}
static size_t header_of(char* data, size_t size, size_t n, void* to) {
    char** etag = to;
    size_t len = size * n;
    if (len > 5 && !strncasecmp(data, "etag:", 5)) {
        const char *s = data + 5, *e = data + len;
        while (s < e && isspace((unsigned char)*s)) ++s;
        while (e > s && isspace((unsigned char)e[-1])) --e;
        free(*etag);
        *etag = rei_format("%.*s", (int)(e - s), s);
    }
    return len;
}
void rh_fetch(void* ctx, const char* method, const char* url, const char* body, const char* etag, RgAnswer* out) {
    const RhAuth* auth = ctx;
    CURL* curl = curl_easy_init();
    struct curl_slist* headers = NULL;
    Buf got = {0};
    char *line, error[CURL_ERROR_SIZE] = "";
    CURLcode done;
    long status = 0;
    memset(out, 0, sizeof *out);
    if (!curl) {
        out->error = rei_strdup("libcurl could not start a request");
        return;
    }
    if (auth->authorization[0]) {
        line = rei_format("Authorization: %s", auth->authorization);
        headers = curl_slist_append(headers, line);
        free(line);
    }
    if (auth->accept) {
        line = rei_format("Accept: %s", auth->accept);
        headers = curl_slist_append(headers, line);
        free(line);
    }
    if (etag) {
        line = rei_format("If-None-Match: %s", etag);
        headers = curl_slist_append(headers, line);
        free(line);
    }
    if (body) headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, AGENT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_of);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &got);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_of);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out->etag);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); /* a timeout in a thread, without SIGALRM */
    done = curl_easy_perform(curl);
    if (done == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out->status = (int)status;
        out->body = buf_take(&got);
    } else {
        out->error = rei_format("%s %s: %s", method, url, *error ? error : curl_easy_strerror(done));
        buf_free(&got);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}
