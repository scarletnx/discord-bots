/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AccordWimsy: the repositories of a GitHub organization, the same on a Forgejo, a pass at a time (cron)

     accordwimsy sync [ENV FILE]     default /usr/local/etc/accordwimsy/accordwimsy.env

   One pass at a time: a second one, while one goes, says so and does nothing. */
#include "aw.h"
#include "config.h"
#include "http.h"
#include "sync.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENV_FILE "/usr/local/etc/accordwimsy/accordwimsy.env"

static cJSON* load(const char* path) {
    FILE* f = fopen(path, "rb");
    Buf b = {0};
    char chunk[4096];
    size_t n;
    cJSON* state;
    if (!f) {
        if (errno != ENOENT) aw_warn("%s: %s: the pass starts from nothing made", path, strerror(errno));
        return cJSON_CreateObject();
    }
    while ((n = fread(chunk, 1, sizeof chunk, f))) buf_addn(&b, chunk, n);
    fclose(f);
    state = cJSON_Parse(b.text ? b.text : "");
    if (!cJSON_IsObject(state)) {
        aw_warn("%s: its %zu bytes are no JSON object: stopping, as starting from nothing would make every issue again",
                path, b.len);
        exit(1);
    }
    buf_free(&b);
    return state;
}
int main(int argc, char** argv) {
    AwConfig config;
    Aw a;
    char why[1024], *lock_path, *mirrors;
    int lock, rc;
    if (argc < 2 || strcmp(argv[1], "sync") || argc > 3) {
        fprintf(stderr, "usage: accordwimsy sync [ENV FILE]   (default %s)\n", ENV_FILE);
        return 2;
    }
    if (aw_config_read(&config, argc == 3 ? argv[2] : ENV_FILE, why, sizeof why)) {
        aw_warn("%s", why);
        return 2;
    }
    mirrors = aw_format("%s/mirrors", config.state);
    if ((mkdir(config.state, 0700) && errno != EEXIST) || (mkdir(mirrors, 0700) && errno != EEXIST)) {
        aw_warn("%s: %s", mirrors, strerror(errno));
        return 1;
    }
    free(mirrors);
    lock_path = aw_format("%s/lock", config.state);
    lock = open(lock_path, O_RDWR | O_CREAT, 0600);
    if (lock < 0) {
        aw_warn("%s: %s", lock_path, strerror(errno));
        return 1;
    }
    struct flock whole;
    memset(&whole, 0, sizeof whole);
    whole.l_type = F_WRLCK;
    whole.l_whence = SEEK_SET;
    if (fcntl(lock, F_SETLK, &whole)) {
        aw_log("a pass is going already (%s held): this one does nothing", lock_path);
        return 0;
    }
    free(lock_path);
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
        aw_warn("libcurl could not start");
        return 1;
    }
    memset(&a, 0, sizeof a);
    a.config = &config;
    a.fetch = aw_http;
    a.state_path = aw_format("%s/state.json", config.state);
    a.state = load(a.state_path);
    rc = aw_sync(&a);
    cJSON_Delete(a.state);
    free(a.state_path);
    curl_global_cleanup();
    close(lock);
    return rc;
}
