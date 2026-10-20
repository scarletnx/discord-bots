/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a message asks for: the #N it names, or a command of its own */
#ifndef REI_REFS_H
#define REI_REFS_H

#define REI_MOST 5 /* the numbers a reply links, at most */

/* the numbers text names as #160 (issue #160, PR #160 too), in order, each once, at most REI_MOST: not inside a word, a
   path or a link, nor in code (`...` or ```...```); a bare 160 is talk. Their count */
int rei_references(const char* text, int numbers[REI_MOST]);

/* the whole message as a command: !nightly, !release, or !release #N (*number); any case, spaces around */
enum { REI_NO_COMMAND, REI_NIGHTLY, REI_RELEASE, REI_RELEASE_PR };
int rei_command(const char* text, int* number);
#endif
