/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

#define AGENT "accordwimsy (https://github.com/scarletnx/discord-bots)"

static size_t body_of(char* data, size_t size, size_t n, void* to) {
    buf_addn(to, data, size * n);
    return size * n;
}
void aw_http(void* ctx, const char* method, const char* url, const char* auth, const char* body, AwAnswer* out) {
    CURL* curl = curl_easy_init();
    struct curl_slist* headers = NULL;
    Buf got = {0};
    char *line, error[CURL_ERROR_SIZE] = "";
    CURLcode done;
    long status = 0;
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (!curl) {
        out->error = aw_strdup("libcurl could not start a request");
        return;
    }
    if (auth) {
        line = aw_format("Authorization: %s", auth);
        headers = curl_slist_append(headers, line);
        free(line);
    }
    headers = curl_slist_append(headers, "Accept: application/json");
    if (body) headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, AGENT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_of);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &got);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    done = curl_easy_perform(curl);
    if (done == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out->status = (int)status;
        out->body = buf_take(&got);
    } else {
        out->error = aw_format("%s %s: %s", method, url, *error ? error : curl_easy_strerror(done));
        buf_free(&got);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}
