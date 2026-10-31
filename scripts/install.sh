#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# Installs or upgrades packages dist/freebsd/build.sh made into a Bastille jail, from the host it runs on (as root):
#
#   sh scripts/install.sh JAIL PACKAGE...      e.g. sh scripts/install.sh discordbots packages/reiwimsy-1.0.0.pkg
#
# Each package's dependencies come from the jail's own package repository (pkg install), the package itself is added
# from its file (pkg add), and a service of it that was running is restarted on the new program. A service is never
# turned on here: its env file is filled in first (docs/INSTALL.md). A jail of a guard group (noirjail.sh) trips on the
# processes this starts: pause the group first, and let it learn after.
set -eu

[ $# -ge 2 ] || { echo "usage: sh install.sh JAIL PACKAGE..." >&2; exit 2; }
jail=$1
shift
if ! jls -j "$jail" jid > /dev/null 2>&1; then
	echo "$jail is not running: start it (bastille start $jail), or make it (docs/SETUP.md)" >&2
	exit 1
fi
root=$(jls -j "$jail" path)

for file in "$@"; do
	[ -f "$file" ] || { echo "$file: no such package file" >&2; exit 1; }
	name=$(pkg query -F "$file" %n)
	deps=$(pkg query -F "$file" %dn)
	[ -z "$deps" ] || jexec "$jail" pkg install -y $deps
	base=$(basename "$file")
	install -m 644 "$file" "$root/tmp/$base"
	jexec "$jail" pkg add -f "/tmp/$base"
	rm "$root/tmp/$base"
	if [ -x "$root/usr/local/etc/rc.d/$name" ] && jexec "$jail" service "$name" status > /dev/null 2>&1; then
		jexec "$jail" service "$name" restart
	fi
	echo "$jail: $(jexec "$jail" pkg query '%n %v' "$name") installed"
done
