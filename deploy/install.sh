#!/bin/sh
# Installs or updates the Discord bots in their Bastille jail, on the host it runs on (as root):
#
#   sh install.sh [JAIL]          default: discordbots
#
# The jail is made once, with Bastille or the host's own tool for jails, for example:
#   bastille create discordbots 15.1-RELEASE <ip> <interface>
# The bots only go out: Discord, GitHub, xAI.
#
# Run from this folder, which holds reiwimsy.rc, xuji.rc and the .env examples, with ../rei and ../xuji beside it.
# ReiWimsy is built in the jail: Concord (CONCORD_VERSION, cloned with its reflect-c submodule, both checked against
# their pinned commits: its Makefile runs git submodule, which a release tarball cannot) once into
# /usr/local/reiwimsy-concord, then ../rei, its tests run before it is installed. Again it changes only what changed:
# the jail and its users are made once, Concord built once a version, the code and the services copied, and an env
# file that is there is left as it is (the tokens are in it). A service starts only once its env file is filled in:
# service reiwimsy start, service xuji start, inside the jail (bastille cmd JAIL service NAME start).
#
# linkbot, the Python bot ReiWimsy replaces, is taken over once: stopped, its linkbot_enable set to NO, its env file's
# LINKBOT_ keys copied as REIWIMSY_ ones into reiwimsy.env. Its files stay where they are (/usr/local/etc/linkbot.env,
# /var/db/linkbot): its state's ids are JSON numbers, which C's double would round, so ReiWimsy starts its own.
set -eu

CONCORD_VERSION=3.0.1
CONCORD_COMMIT=d7f224b70c9dd0b1aa4997f025d1322641611977
REFLECTC_COMMIT=cad2211cd2b912813f38c0c0c7a34160cc67ca97
CONCORD_URL=https://github.com/Cogmasters/concord.git

jail=${1:-discordbots}
here=$(cd "$(dirname "$0")" && pwd)
root=/usr/local/bastille/jails/$jail/root
concord=/usr/local/reiwimsy-concord

if ! jls -j "$jail" jid >/dev/null 2>&1; then
	echo "$jail is not running: make it (bastille create), or start it (bastille start $jail)" >&2
	exit 1
fi
bastille pkg "$jail" install -y py312-discord.py curl gmake pkgconf git

# Concord, once a version
if [ "$(cat "$root$concord/VERSION" 2>/dev/null)" != "$CONCORD_VERSION" ]; then
	src=/usr/local/src/concord-$CONCORD_VERSION
	install -d "$root/usr/local/src"
	[ -d "$root$src" ] || jexec "$jail" git -c advice.detachedHead=false clone -q --depth 1 \
		--branch "v$CONCORD_VERSION" --recurse-submodules --shallow-submodules "$CONCORD_URL" "$src"
	got=$(jexec "$jail" git -C "$src" rev-parse HEAD)
	sub=$(jexec "$jail" git -C "$src/reflect-c" rev-parse HEAD)
	if [ "$got" != "$CONCORD_COMMIT" ] || [ "$sub" != "$REFLECTC_COMMIT" ]; then
		echo "$src is at $got with reflect-c at $sub, not $CONCORD_COMMIT and $REFLECTC_COMMIT: not built" >&2
		exit 1
	fi
	jexec "$jail" sh -c "cd $src && gmake && gmake install PREFIX=$concord SHAREDIR=$concord/share"
	echo "$CONCORD_VERSION" > "$root$concord/VERSION"
	echo "Concord $CONCORD_VERSION built into $concord"
fi

# ReiWimsy: its code copied over the last one, built, its tests run, installed
install -d "$root/usr/local/src/reiwimsy"
(cd "$here/../rei" && tar cf - --exclude '*.o' Makefile src tests/test_rei.c third_party) |
	(cd "$root/usr/local/src/reiwimsy" && tar xf -)
jexec "$jail" sh -c "cd /usr/local/src/reiwimsy && make clean >/dev/null && make test && make CONCORD=$concord"
install -d "$root/usr/local/libexec" "$root/usr/local/etc/rc.d"
install -m 755 "$root/usr/local/src/reiwimsy/reiwimsy" "$root/usr/local/libexec/reiwimsy"
install -m 555 "$here/reiwimsy.rc" "$root/usr/local/etc/rc.d/reiwimsy"
bastille cmd "$jail" pw usershow reiwimsy >/dev/null 2>&1 ||
	bastille cmd "$jail" pw useradd reiwimsy -c "ReiWimsy Discord bot" -d /nonexistent -s /usr/sbin/nologin

# linkbot taken over, once
if [ -e "$root/usr/local/etc/linkbot.env" ] && [ ! -e "$root/usr/local/etc/reiwimsy.env" ]; then
	jexec "$jail" service linkbot onestop || true
	bastille sysrc "$jail" linkbot_enable=NO >/dev/null
	sed 's/^LINKBOT_/REIWIMSY_/' "$root/usr/local/etc/linkbot.env" > "$root/usr/local/etc/reiwimsy.env"
	chmod 600 "$root/usr/local/etc/reiwimsy.env"
	echo "linkbot: stopped, linkbot_enable=NO; its env file copied to reiwimsy.env with REIWIMSY_ keys"
	echo "linkbot: /usr/local/etc/linkbot.env and /var/db/linkbot are left as they were (ReiWimsy keeps its own state)"
fi
if [ ! -e "$root/usr/local/etc/reiwimsy.env" ]; then
	install -m 600 "$here/reiwimsy.env.example" "$root/usr/local/etc/reiwimsy.env"
	echo "reiwimsy: fill in $root/usr/local/etc/reiwimsy.env, then: bastille cmd $jail service reiwimsy start"
fi
# on at its first install; a NO set since (the bot left off) stays
jexec "$jail" sysrc -n reiwimsy_enable >/dev/null 2>&1 || bastille sysrc "$jail" reiwimsy_enable=YES >/dev/null

# Xuji, as it was
bastille cmd "$jail" pw usershow xuji >/dev/null 2>&1 ||
	bastille cmd "$jail" pw useradd xuji -c "xuji Discord bot" -d /nonexistent -s /usr/sbin/nologin
install -m 755 "$here/../xuji/xuji.py" "$root/usr/local/libexec/xuji.py"
install -m 555 "$here/xuji.rc" "$root/usr/local/etc/rc.d/xuji"
if [ ! -e "$root/usr/local/etc/xuji.env" ]; then
	install -m 600 "$here/xuji.env.example" "$root/usr/local/etc/xuji.env"
	echo "xuji: fill in $root/usr/local/etc/xuji.env, then: bastille cmd $jail service xuji start"
fi
jexec "$jail" sysrc -n xuji_enable >/dev/null 2>&1 || bastille sysrc "$jail" xuji_enable=YES >/dev/null
echo "$jail: installed"
