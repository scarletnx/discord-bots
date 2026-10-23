/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ReiWimsy: the replies to messages (link.h) and the cards (cards.h) of a GitHub repository on Discord.

    REIWIMSY_TOKEN=... REIWIMSY_REPO=OWNER/NAME [REIWIMSY_NIGHTLY=OWNER/NAME] [REIWIMSY_CHANNELS=id,id]
    [GITHUB_TOKEN=...] [REIWIMSY_CARDS_PRS=id] [REIWIMSY_CARDS_ISSUES=id] [REIWIMSY_CARDS_NIGHTLY=id]
    [REIWIMSY_STATE=file] reiwimsy

Concord's gateway brings the events, read here from their JSON (event.h), never decoded by Concord; a person's message
waits for the thread that answers them, one at a time. A thread of its own draws the cards every ROUND seconds and
edits the replies whose items changed every LK_REFRESH. The cards need GITHUB_TOKEN (GitHub allows
60 requests an hour without one), and so does the replies' editing. linkbot's LINKBOT_TOKEN, LINKBOT_REPO,
LINKBOT_NIGHTLY, LINKBOT_CHANNELS and LINKBOT_STATE are read for the REIWIMSY_ ones while those are unset, said once */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <discord.h> /* Concord's */

#include "cards.h"
#include "event.h"
#include "http.h"
#include "link.h"
#include "rest.h"
#include "state.h"

#define ROUND    30
#define CHANNELS 64

static struct {
    const char *token, *repo, *nightly, *github, *state;
    u64 channels[CHANNELS];
    int nchannels;
    u64 prs, issues, nightlies;
} env;
static RhAuth github_auth, discord_auth;
static Rg link_gh, cards_gh;
static Link replies;
static Cards cards;
static Dc dc;
static cJSON* cards_saved; /* the cards as the last round left them, for rei_save */
static pthread_mutex_t link_lock = PTHREAD_MUTEX_INITIALIZER, save_lock = PTHREAD_MUTEX_INITIALIZER;
static EvQueue waiting;

static const char* const KNOWN[] = {"REIWIMSY_TOKEN",        "REIWIMSY_REPO",         "REIWIMSY_NIGHTLY",
                                    "REIWIMSY_CHANNELS",     "REIWIMSY_STATE",        "REIWIMSY_CARDS_PRS",
                                    "REIWIMSY_CARDS_ISSUES", "REIWIMSY_CARDS_NIGHTLY"};

static void stop(const char* fmt, const char* value) {
    fputs("reiwimsy: ", stderr);
    fprintf(stderr, fmt, value);
    fputc('\n', stderr);
    exit(1);
}
/* name's value, else linkbot's old one, said once; NULL when neither is set */
static const char* setting(const char* name, const char* old) {
    const char *v = getenv(name), *was = old ? getenv(old) : NULL;
    if (v && *v) {
        if (was && *was) rei_log("%s and %s are both set: %s=%s is taken", old, name, name, v);
        return v;
    }
    if (was && *was) {
        rei_log("%s is read as %s: rename it in the env file", old, name);
        return was;
    }
    return NULL;
}
static int repo_name(const char* s) {
    const char* slash = strchr(s, '/');
    size_t i;
    if (!slash || slash == s || !slash[1] || strchr(slash + 1, '/')) return 0;
    for (i = 0; s[i]; ++i)
        if (!(isalnum((unsigned char)s[i]) || strchr("._-/", s[i]))) return 0;
    return 1;
}
static u64 channel_id(const char* name) {
    const char* v = getenv(name);
    size_t i;
    if (!v || !*v) return 0;
    for (i = 0; v[i]; ++i)
        if (!isdigit((unsigned char)v[i])) {
            fprintf(stderr, "reiwimsy: %s=%s is not a channel id (Developer Mode, Copy Channel ID: its digits)\n", name,
                    v);
            exit(1);
        }
    return strtoull(v, NULL, 10);
}
static void read_env(void) {
    extern char** environ;
    char** e;
    const char* list;
    size_t k;
    for (e = environ; *e; ++e) { /* a key of ours misspelt would be a setting quietly not taken */
        int known = 0;
        if (strncmp(*e, "REIWIMSY_", 9)) continue;
        for (k = 0; k < sizeof KNOWN / sizeof *KNOWN; ++k)
            known |= !strncmp(*e, KNOWN[k], strlen(KNOWN[k])) && (*e)[strlen(KNOWN[k])] == '=';
        if (!known)
            stop("%s: no such setting (REIWIMSY_TOKEN, _REPO, _NIGHTLY, _CHANNELS, _STATE, _CARDS_PRS, "
                 "_CARDS_ISSUES, _CARDS_NIGHTLY)",
                 *e);
    }
    env.token = setting("REIWIMSY_TOKEN", "LINKBOT_TOKEN");
    env.repo = setting("REIWIMSY_REPO", "LINKBOT_REPO");
    if (!env.token || !env.repo) stop("REIWIMSY_TOKEN and REIWIMSY_REPO (OWNER/NAME) are needed%s", "");
    if (!repo_name(env.repo)) stop("REIWIMSY_REPO=%s is not OWNER/NAME", env.repo);
    env.nightly = setting("REIWIMSY_NIGHTLY", "LINKBOT_NIGHTLY");
    if (env.nightly && !repo_name(env.nightly)) stop("REIWIMSY_NIGHTLY=%s is not OWNER/NAME", env.nightly);
    env.state = setting("REIWIMSY_STATE", "LINKBOT_STATE");
    env.github = getenv("GITHUB_TOKEN");
    if (env.github && !*env.github) env.github = NULL;
    for (list = setting("REIWIMSY_CHANNELS", "LINKBOT_CHANNELS"); list && *list;) {
        size_t n = strcspn(list, ",");
        char id[32];
        if (n && n < sizeof id) {
            memcpy(id, list, n);
            id[n] = 0;
            for (k = 0; k < n; ++k)
                if (!isdigit((unsigned char)id[k])) stop("REIWIMSY_CHANNELS has %s, which is not a channel id", id);
            if (env.nchannels == CHANNELS) stop("REIWIMSY_CHANNELS names more than %s channels", "64");
            env.channels[env.nchannels++] = strtoull(id, NULL, 10);
        }
        list += n + (list[n] == ',');
    }
    env.prs = channel_id("REIWIMSY_CARDS_PRS");
    env.issues = channel_id("REIWIMSY_CARDS_ISSUES");
    env.nightlies = channel_id("REIWIMSY_CARDS_NIGHTLY");
    if ((env.prs || env.issues || env.nightlies) && !env.github)
        stop("REIWIMSY_CARDS_* need GITHUB_TOKEN: GitHub allows 60 requests an hour without one%s", "");
}

static void save_all(void) {
    cJSON* state = cJSON_CreateObject();
    pthread_mutex_lock(&save_lock);
    pthread_mutex_lock(&link_lock);
    cJSON_AddItemToObject(state, "link", lk_save(&replies));
    pthread_mutex_unlock(&link_lock);
    if (cards_saved) cJSON_AddItemToObject(state, "cards", cJSON_Duplicate(cards_saved, 1));
    rei_save(env.state, state);
    pthread_mutex_unlock(&save_lock);
    cJSON_Delete(state);
}

/* the cards' Discord, through dc */
static u64 card_post(void* ctx, u64 channel, const cJSON* embed) { return dc_post(ctx, channel, embed, 0); }
static int card_edit(void* ctx, u64 channel, u64 message, const cJSON* embed) {
    return dc_edit(ctx, channel, message, embed);
}
static int card_remove(void* ctx, u64 channel, u64 message) { return dc_remove(ctx, channel, message); }
static int card_history(void* ctx, u64 channel, u64 before, u64* ids, int* webhook, int* mine, int max) {
    return dc_history(ctx, channel, before, ids, webhook, mine, max);
}
static int reply_edit(void* ctx, u64 channel, u64 message, const cJSON* embed) {
    (void)ctx;
    return dc_edit(&dc, channel, message, embed);
}

static void* rounds(void* unused) {
    double refreshed = rei_now();
    (void)unused;
    for (;;) {
        double start = rei_now();
        if (env.prs || env.issues || env.nightlies) {
            cJSON* now;
            cd_round(&cards);
            now = cd_save(&cards);
            pthread_mutex_lock(&save_lock);
            cJSON_Delete(cards_saved);
            cards_saved = now;
            pthread_mutex_unlock(&save_lock);
        }
        if (env.github && rei_now() - refreshed >= LK_REFRESH) {
            pthread_mutex_lock(&link_lock);
            lk_refresh(&replies, reply_edit, NULL);
            pthread_mutex_unlock(&link_lock);
            refreshed = rei_now();
        }
        save_all();
        if (rei_now() - start < ROUND) sleep((unsigned)(ROUND - (rei_now() - start)));
    }
    return NULL;
}

/* each waiting message answered, one at a time: an answer asks GitHub, which takes a while */
static void* answers(void* unused) {
    (void)unused;
    for (;;) {
        EvMessage m;
        int numbers[REI_MOST], n = 0;
        cJSON* e;
        u64 id;
        ev_pop(&waiting, &m);
        pthread_mutex_lock(&link_lock);
        e = lk_answer(&replies, m.content, numbers, &n);
        pthread_mutex_unlock(&link_lock);
        if (e && (id = dc_post(&dc, m.channel, e, m.id)) && n) {
            pthread_mutex_lock(&link_lock);
            lk_keep(&replies, m.channel, id, numbers, n, e);
            pthread_mutex_unlock(&link_lock);
            save_all();
        }
        cJSON_Delete(e);
        ev_message_free(&m);
    }
    return NULL;
}
static void start(void* (*run)(void*), const char* what) {
    pthread_t thread;
    int code = pthread_create(&thread, NULL, run, NULL);
    if (code) stop("the thread of %s", rei_format("%s did not start: %s", what, strerror(code)));
    pthread_detach(thread);
}

/* READY, at each new session: the threads started at the first */
static void ready(const char* json, size_t size) {
    static int started;
    char* user = ev_ready_user(json, size);
    if (user)
        rei_log("up as %s for %s; cards: pull requests %s, issues %s, nightlies %s", user, env.repo,
                env.prs ? "on" : "off", env.issues ? "on" : "off", env.nightlies ? "on" : "off");
    else
        rei_log("up for %s, its READY naming no user; cards: pull requests %s, issues %s, nightlies %s", env.repo,
                env.prs ? "on" : "off", env.issues ? "on" : "off", env.nightlies ? "on" : "off");
    free(user);
    if (!env.github) rei_log("no GITHUB_TOKEN: its replies stay as sent");
    if (!started) {
        start(rounds, "the cards and the replies' edits");
        start(answers, "the answers");
        started = 1;
    }
}
/* MESSAGE_CREATE: a person's message in a channel the replies answer in waits for its answer; a bot's or a webhook's
   (a card) never gets one */
static void take(const char* json, size_t size) {
    EvMessage m;
    int i, here = !env.nchannels;
    if (!ev_message(json, size, &m)) return;
    for (i = 0; i < env.nchannels; ++i) here |= env.channels[i] == m.channel;
    if (here && !ev_push(&waiting, &m))
        rei_log("%d messages wait for an answer already: message %llu in channel %llu gets none", EV_WAITING,
                (unsigned long long)m.id, (unsigned long long)m.channel);
    ev_message_free(&m);
}
/* Concord's gateway thread, before Concord decodes anything: each event the bot reads is read here, from its JSON, and
   none is left to Concord, whose decoder of MESSAGE_CREATE crashed the bot (SIGSEGV, SIGBUS) on messages of 2 to 3
   KB. READY's session is Concord's own before this */
static enum discord_event_scheduler scheduler(struct discord* client, const char data[], size_t size,
                                              enum discord_gateway_events event) {
    (void)client;
    if (event == DISCORD_EV_READY) ready(data, size);
    if (event == DISCORD_EV_MESSAGE_CREATE) take(data, size);
    return DISCORD_EVENT_IGNORE;
}

int main(void) {
    struct discord* client;
    struct discord_config config;
    CdDiscord cdd;
    cJSON* saved;
    CCORDcode code;
    read_env();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (env.github) snprintf(github_auth.authorization, sizeof github_auth.authorization, "Bearer %s", env.github);
    github_auth.accept = "application/vnd.github+json";
    snprintf(discord_auth.authorization, sizeof discord_auth.authorization, "Bot %s", env.token);
    rg_init(&link_gh, env.repo, env.nightly, rh_fetch, &github_auth);
    rg_init(&cards_gh, env.repo, env.nightly, rh_fetch, &github_auth);
    lk_init(&replies, &link_gh);
    dc_init(&dc, rh_fetch, &discord_auth);
    { /* the icon beside each download, the application's emojis: a card or a reply drawn without them says so */
        cJSON* emojis = dc_emojis(&dc);
        cd_icons(emojis);
        cJSON_Delete(emojis);
    }
    cdd.ctx = &dc;
    cdd.post = card_post;
    cdd.edit = card_edit;
    cdd.remove = card_remove;
    cdd.history = card_history;
    cd_init(&cards, &cards_gh, cdd, env.prs, env.issues, env.nightlies);
    saved = rei_load(env.state);
    lk_load(&replies, cJSON_GetObjectItemCaseSensitive(saved, "link"));
    cd_load(&cards, cJSON_GetObjectItemCaseSensitive(saved, "cards"));
    cards_saved = cd_save(&cards);
    cJSON_Delete(saved);
    ev_queue_init(&waiting);
    memset(&config, 0, sizeof config);
    config.token = (char*)env.token;
    config.log.level = LOGMOD_LEVEL_WARN; /* Concord's traces name every event: only what goes wrong is logged */
    client = discord_from_config(&config);
    if (!client) stop("Concord could not start a client with REIWIMSY_TOKEN%s", "");
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS | DISCORD_GATEWAY_GUILD_MESSAGES |
                                    DISCORD_GATEWAY_DIRECT_MESSAGES | DISCORD_GATEWAY_MESSAGE_CONTENT);
    discord_set_event_scheduler(client, scheduler);
    code = discord_run(client);
    rei_log("the gateway stopped: %s", discord_strerror(code, client));
    discord_cleanup(client);
    ccord_global_cleanup();
    curl_global_cleanup();
    return 1; /* daemon(8) starts it again */
}
