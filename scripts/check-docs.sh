#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# Refuses a doc that tells history: a date (YYYY-MM-DD) in README.md or docs/*.md, outside code blocks and inline code.
# A doc says what is; when and how it came to be goes in the commit message.
#
#   sh scripts/check-docs.sh [DIR]      DIR: the checkout when not given
set -eu
cd "${1:-$(dirname "$0")/..}"
# awk: each file's lines outside ``` fences and indented blocks, inline code taken out, that hold a date
awk '
FNR == 1 { fence = 0 }
/^```/ { fence = !fence; next }
fence || /^(    |\t)/ { next }
{
	line = $0
	gsub(/`[^`]*`/, "", line)
	if (line ~ /(^|[^0-9])20[0-9][0-9]-[01][0-9]-[0-3][0-9]([^0-9]|$)/) {
		printf "%s:%d: %s\n", FILENAME, FNR, $0
		bad = 1
	}
}
END { exit bad }' README.md docs/*.md || {
	echo "a doc says what is, not when it came to be: the date and the story go in the commit message" >&2
	exit 1
}
