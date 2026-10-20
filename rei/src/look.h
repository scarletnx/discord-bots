/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How an issue or a pull request reads in a reply and on a card: its state, its labels as GitHub's pills, text quoted
   or shown as it is. Lengths are of letters (UTF-8 code points), as Discord counts them */
#ifndef REI_LOOK_H
#define REI_LOOK_H
#include "rei.h"

/* GitHub's colours: open, draft, merged, closed; an issue done, or not planned */
#define REI_OPEN        0x1F883D
#define REI_DRAFT       0x6E7781
#define REI_MERGED      0x8250DF
#define REI_CLOSED      0xCF222E
#define REI_DONE        0x8250DF
#define REI_NOT_PLANNED 0x6E7781

/* an issue's or a pull request's state as GitHub words it: open, draft, merged, closed; done, not planned */
const char* rei_state(const cJSON* item);
int rei_state_color(const char* state);
/* the coloured square nearest a colour (RRGGBB): a grey by its lightness, a dark orange brown, else by hue */
const char* rei_square(const char* rrggbb);
/* a label as GitHub's pill, after the square of its colour; owned */
char* rei_chip(const cJSON* label);
/* every label of item as chips, a space between; owned, "" for none */
char* rei_chips(const cJSON* item);
/* text shown as it is in Discord's markdown: \ ` * _ [ ] < > | ~ escaped; owned */
char* rei_escape(const char* text);

size_t rei_letters(const char* s);
/* the bytes of s's first n letters */
size_t rei_bytes_of(const char* s, size_t n);
/* a comment or a description as a card quotes it: no hidden comments, an image as [image], <details> as [details],
   cut by a word after most letters, each line after "> "; owned */
char* rei_excerpt(const char* text, size_t most);
#endif
