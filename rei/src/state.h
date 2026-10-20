/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The state file, JSON: what the replies and the cards keep across a restart (link.h's and cards.h's saves) */
#ifndef REI_STATE_H
#define REI_STATE_H
#include "rei.h"

/* path's JSON object (owned); an empty one when path is NULL or missing, and when it cannot be read or is no JSON
   object, which the log says with what it starts from */
cJSON* rei_load(const char* path);
/* state written to path.new, then put over path: 0, or -1 said in the log */
int rei_save(const char* path, const cJSON* state);
#endif
