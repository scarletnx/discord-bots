/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "look.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char* rei_state(const cJSON* item) {
    const cJSON* pull = cJSON_GetObjectItemCaseSensitive(item, "pull_request");
    const char* state = jstr(item, "state");
    if (cJSON_IsObject(pull)) {
        if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(pull, "merged_at"))) return "merged";
        if (state && !strcmp(state, "closed")) return "closed";
        return jbool(item, "draft") ? "draft" : "open";
    }
    if (state && !strcmp(state, "open")) return "open";
    state = jstr(item, "state_reason");
    return state && !strcmp(state, "completed") ? "done" : "not planned";
}
int rei_state_color(const char* state) {
    if (!strcmp(state, "open")) return REI_OPEN;
    if (!strcmp(state, "draft")) return REI_DRAFT;
    if (!strcmp(state, "merged")) return REI_MERGED;
    if (!strcmp(state, "closed")) return REI_CLOSED;
    if (!strcmp(state, "done")) return REI_DONE;
    return REI_NOT_PLANNED;
}

/* Python's colorsys.rgb_to_hls */
static void hls(double r, double g, double b, double* h, double* l, double* s) {
    double maxc = fmax(r, fmax(g, b)), minc = fmin(r, fmin(g, b)), range = maxc - minc;
    *l = (maxc + minc) / 2;
    if (maxc == minc) {
        *h = *s = 0;
        return;
    }
    *s = *l <= 0.5 ? range / (maxc + minc) : range / (2 - maxc - minc);
    {
        double rc = (maxc - r) / range, gc = (maxc - g) / range, bc = (maxc - b) / range;
        *h = r == maxc ? bc - gc : g == maxc ? 2 + rc - bc : 4 + gc - rc;
        *h = fmod(*h / 6, 1);
        if (*h < 0) *h += 1;
    }
}
const char* rei_square(const char* rrggbb) {
    static const int hues[] = {0, 30, 50, 120, 210, 275};
    static const char* const squares[] = {"\xF0\x9F\x9F\xA5", "\xF0\x9F\x9F\xA7", "\xF0\x9F\x9F\xA8",
                                          "\xF0\x9F\x9F\xA9", "\xF0\x9F\x9F\xA6", "\xF0\x9F\x9F\xAA"};
    double c[3], h, l, s, best = 1e9;
    int i, pick = 0;
    for (i = 0; i < 3; ++i) {
        char two[3] = {rrggbb[2 * i], rrggbb[2 * i + 1], 0};
        c[i] = (double)strtol(two, NULL, 16) / 255;
    }
    hls(c[0], c[1], c[2], &h, &l, &s);
    if (l < 0.2 || l > 0.92 || s < 0.2) return l >= 0.5 ? "\xE2\xAC\x9C" : "\xE2\xAC\x9B"; /* white, black */
    h *= 360;
    if (h >= 10 && h <= 45 && l < 0.3) return "\xF0\x9F\x9F\xAB"; /* brown */
    for (i = 0; i < 6; ++i) {
        double d = fabs(h - hues[i]);
        if (360 - d < d) d = 360 - d;
        if (d < best) best = d, pick = i;
    }
    return squares[pick];
}
static int hex6(const char* s) {
    int i;
    if (!s || strlen(s) != 6) return 0;
    for (i = 0; i < 6; ++i)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}
char* rei_chip(const cJSON* label) {
    const char *color = jstr(label, "color"), *name = jstr(label, "name");
    if (!name) name = "";
    return hex6(color) ? rei_format("%s `%s`", rei_square(color), name) : rei_format("`%s`", name);
}
char* rei_chips(const cJSON* item) {
    const cJSON* label;
    Buf b = {0};
    cJSON_ArrayForEach(label, cJSON_GetObjectItemCaseSensitive(item, "labels")) {
        char* chip = rei_chip(label);
        if (b.len) buf_add(&b, " ");
        buf_add(&b, chip);
        free(chip);
    }
    return buf_take(&b);
}
char* rei_escape(const char* text) {
    Buf b = {0};
    for (; text && *text; ++text) {
        if (strchr("\\`*_[]<>|~", *text)) buf_add(&b, "\\");
        buf_addn(&b, text, 1);
    }
    return buf_take(&b);
}

size_t rei_letters(const char* s) {
    size_t n = 0;
    for (; *s; ++s) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}
size_t rei_bytes_of(const char* s, size_t n) {
    const char* p = s;
    while (*p && n) {
        ++p;
        while (((unsigned char)*p & 0xC0) == 0x80) ++p;
        --n;
    }
    return (size_t)(p - s);
}

/* s with each piece from open to close (close included) put as with; case only for ASCII when nocase */
static char* drop(const char* s, const char* open, const char* close, const char* with, int nocase) {
    Buf b = {0};
    size_t no = strlen(open), nc = strlen(close);
    while (*s) {
        int at = nocase ? !strncasecmp(s, open, no) : !strncmp(s, open, no);
        const char* end = NULL;
        if (at) {
            const char* p = s + no;
            for (; *p; ++p)
                if (nocase ? !strncasecmp(p, close, nc) : !strncmp(p, close, nc)) {
                    end = p + nc;
                    break;
                }
        }
        if (end) {
            buf_add(&b, with);
            s = end;
        } else
            buf_addn(&b, s++, 1);
    }
    return buf_take(&b);
}
/* ![alt](url) and <img ...> as [image] */
static char* images(const char* s) {
    Buf b = {0};
    while (*s) {
        if (s[0] == '!' && s[1] == '[') {
            const char* p = s + 2;
            while (*p && *p != ']') ++p;
            if (p[0] == ']' && p[1] == '(') {
                const char* q = p + 2;
                while (*q && *q != ')') ++q;
                if (*q == ')') {
                    buf_add(&b, "[image]");
                    s = q + 1;
                    continue;
                }
            }
        }
        if (!strncmp(s, "<img", 4) && !(isalnum((unsigned char)s[4]) || s[4] == '_')) {
            const char* p = strchr(s + 4, '>');
            if (p) {
                buf_add(&b, "[image]");
                s = p + 1;
                continue;
            }
        }
        buf_addn(&b, s++, 1);
    }
    return buf_take(&b);
}
char* rei_excerpt(const char* text, size_t most) {
    char *a = drop(text ? text : "", "<!--", "-->", "", 0), *c = images(a),
         *d = drop(c, "<details>", "</details>", "[details]", 1);
    Buf b = {0}, q = {0};
    const char *p, *start, *end;
    size_t nl = 0, fences = 0;
    free(a);
    free(c);
    for (p = d; *p; ++p) { /* no \r, and three newlines or more as two */
        if (*p == '\r') continue;
        if (*p == '\n') {
            if (++nl <= 2) buf_add(&b, "\n");
            continue;
        }
        nl = 0;
        buf_addn(&b, p, 1);
    }
    free(d);
    start = b.text ? b.text : "";
    while (isspace((unsigned char)*start)) ++start;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) --end;
    {
        char* t = rei_format("%.*s", (int)(end - start), start);
        buf_free(&b);
        if (rei_letters(t) > most) {
            size_t cut = rei_bytes_of(t, most);
            char *space, *shorter;
            t[cut] = 0;
            space = strrchr(t, ' ');
            if (space) *space = 0;
            cut = strlen(t);
            while (cut && isspace((unsigned char)t[cut - 1])) t[--cut] = 0;
            shorter = rei_format("%s...", t);
            free(t);
            t = shorter;
        }
        for (p = t; (p = strstr(p, "```")); p += 3) ++fences;
        if (fences % 2) {
            char* closed = rei_format("%s\n```", t);
            free(t);
            t = closed;
        }
        for (p = t; *p;) {
            size_t n = strcspn(p, "\n");
            if (q.len) buf_add(&q, "\n");
            if (n) {
                buf_add(&q, "> ");
                buf_addn(&q, p, n);
            } else
                buf_add(&q, ">");
            p += n + (p[n] == '\n');
        }
        free(t);
    }
    return buf_take(&q);
}
