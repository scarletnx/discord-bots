#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# check-docs.sh against docs made here: a date in the text fails, one in code passes, and so does no date.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
mkdir "$t/docs"
fails=0

# Writes <text> as README.md and docs/A.md of a fresh doc tree; expects check-docs.sh to give <want> (0 or 1)
case_() {
	printf '%b' "$2" > "$t/README.md"
	printf '# A\n' > "$t/docs/A.md"
	got=0
	sh "$here/check-docs.sh" "$t" > /dev/null 2>&1 || got=$?
	if [ "$got" -eq "$3" ]; then echo "ok   $1"; else echo "FAIL $1: exit $got, not $3"; fails=$((fails + 1)); fi
}

case_ "a date in the text fails" "# R\n\nThe bots moved on 2026-10-30.\n" 1
case_ "a date in a fenced block passes" "# R\n\n\`\`\`\nlog-2026-10-28.txt\n\`\`\`\n" 0
case_ "a date in an indented block passes" "# R\n\n    ls log-2026-10-28.txt\n" 0
case_ "a date in inline code passes" "# R\n\nA log is \`console-2026-10-28.log\`.\n" 0
case_ "a version is no date" "# R\n\nConcord 3.0.1 and Gatus 5.38.0.\n" 0
case_ "no date passes" "# R\n\nWhat it does.\n" 0
[ "$fails" -eq 0 ] || exit 1
