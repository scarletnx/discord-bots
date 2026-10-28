/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "config.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* key;
    size_t at, size; /* where in AwConfig, and its room */
    const char* fallback;
} Key;
#define ROOM(field) offsetof(AwConfig, field), sizeof(((AwConfig*)0)->field)
static const Key keys[] = {
    {"ACCORDWIMSY_FORGEJO", ROOM(forgejo), ""},
    {"ACCORDWIMSY_FORGEJO_TOKEN", ROOM(forgejo_token), ""},
    {"ACCORDWIMSY_FORGEJO_USER", ROOM(forgejo_user), "accordwimsy"},
    {"ACCORDWIMSY_ORG", ROOM(org), "scarletnx"},
    {"ACCORDWIMSY_FORGEJO_ORG", ROOM(forgejo_org), ""},
    {"GITHUB_TOKEN", ROOM(github_token), ""},
    {"ACCORDWIMSY_GITHUB_API", ROOM(github_api), "https://api.github.com"},
    {"ACCORDWIMSY_GITHUB", ROOM(github), "https://github.com"},
    {"ACCORDWIMSY_STATE", ROOM(state), "/var/db/accordwimsy"},
    {"ACCORDWIMSY_GIT", ROOM(git), "/usr/local/bin/git"},
};
#define KEYS (sizeof keys / sizeof *keys)

static char* field(AwConfig* c, const Key* k) { return (char*)c + k->at; }
static void trim_slash(char* s) {
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') s[--n] = 0;
}
int aw_config_read(AwConfig* c, const char* path, char* why, size_t size) {
    FILE* f;
    char line[2048];
    size_t i;
    int number = 0;
    memset(c, 0, sizeof *c);
    for (i = 0; i < KEYS; ++i) snprintf(field(c, &keys[i]), keys[i].size, "%s", keys[i].fallback);
    f = fopen(path, "r");
    if (!f) {
        snprintf(why, size, "%s: %s", path, strerror(errno));
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        char *s = line, *eq, *end;
        ++number;
        while (*s == ' ' || *s == '\t') ++s;
        end = s + strlen(s);
        while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!*s || *s == '#') continue;
        eq = strchr(s, '=');
        if (!eq) {
            snprintf(why, size, "%s:%d: \"%.60s\" is no KEY=value", path, number, s);
            fclose(f);
            return 1;
        }
        *eq = 0;
        for (i = 0; i < KEYS && strcmp(keys[i].key, s); ++i) continue;
        if (i == KEYS) {
            snprintf(why, size, "%s:%d: %.60s is no key of AccordWimsy's (README: its settings)", path, number, s);
            fclose(f);
            return 1;
        }
        if (strlen(eq + 1) >= keys[i].size) {
            snprintf(why, size, "%s:%d: %s is %zu characters, past the %zu it takes", path, number, s, strlen(eq + 1),
                     keys[i].size - 1);
            fclose(f);
            return 1;
        }
        snprintf(field(c, &keys[i]), keys[i].size, "%s", eq + 1);
    }
    fclose(f);
    if (!c->forgejo_org[0]) snprintf(c->forgejo_org, sizeof c->forgejo_org, "%s", c->org);
    trim_slash(c->forgejo);
    trim_slash(c->github_api);
    trim_slash(c->github);
    for (i = 0; i < KEYS; ++i)
        if (!field(c, &keys[i])[0]) {
            snprintf(why, size, "%s: %s is needed, and empty", path, keys[i].key);
            return 1;
        }
    return 0;
}
