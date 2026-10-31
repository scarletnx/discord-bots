#!/bin/sh
# Installs or updates ReiWimsy in its Bastille jail, on the host it runs on (as root):
#
#   sh install.sh [JAIL]          default: discordbots
#
# The jail is made once, with Bastille or the host's own tool for jails, for example:
#   bastille create discordbots 15.1-RELEASE <ip> <interface>
# The bot only goes out: Discord, GitHub.
#
# Run from this folder, which holds reiwimsy.rc and its .env example, with ../rei beside it.
# ReiWimsy is built in the jail: Concord (CONCORD_VERSION, cloned with its reflect-c submodule, both checked against
# their pinned commits: its Makefile runs git submodule, which a release tarball cannot), with the fixes CONCORD_PATCHES
# names over it (each one Concord does not have yet, its tests CONCORD_TESTS run before Concord is installed) once into
# /usr/local/reiwimsy-concord, then ../rei, its tests run before it is installed. Again it changes only what changed:
# the jail and its user are made once, Concord built once a version and set of patches, the code and the service
# copied, and an env file that is there is left as it is (the tokens are in it). The service starts only once its
# env file is filled in: service reiwimsy start, inside the jail (bastille cmd JAIL service reiwimsy start).
#
# linkbot, the Python bot ReiWimsy replaces, is taken over once: stopped, its linkbot_enable set to NO, its env file's
# LINKBOT_ keys copied as REIWIMSY_ ones into reiwimsy.env. Its files stay where they are (/usr/local/etc/linkbot.env,
# /var/db/linkbot): its state's ids are JSON numbers, which C's double would round, so ReiWimsy starts its own.
set -eu

CONCORD_VERSION=3.0.1
CONCORD_COMMIT=d7f224b70c9dd0b1aa4997f025d1322641611977
REFLECTC_COMMIT=cad2211cd2b912813f38c0c0c7a34160cc67ca97
CONCORD_URL=https://github.com/Cogmasters/concord.git
# applied in this order over CONCORD_COMMIT, from this folder: a heartbeat that cannot be sent (a connection gone
# without a CLOSE) left the gateway retrying it every millisecond, never reconnecting
CONCORD_PATCHES=concord-heartbeat-send.patch
CONCORD_TESTS=unit-gateway-lifecycle

jail=${1:-discordbots}
here=$(cd "$(dirname "$0")" && pwd)
root=/usr/local/bastille/jails/$jail/root
concord=/usr/local/reiwimsy-concord

if ! jls -j "$jail" jid >/dev/null 2>&1; then
	echo "$jail is not running: make it (bastille create), or start it (bastille start $jail)" >&2
	exit 1
fi
bastille pkg "$jail" install -y curl gmake pkgconf git

# Concord, once a version and set of patches: its VERSION file names both
patches=$(cd "$here" && cat $CONCORD_PATCHES | sha256)
[ -n "$patches" ] || { echo "the SHA-256 of $CONCORD_PATCHES could not be taken" >&2; exit 1; }
build=$CONCORD_VERSION+$(echo "$patches" | cut -c1-12)
if [ "$(cat "$root$concord/VERSION" 2>/dev/null)" != "$build" ]; then
	src=/usr/local/src/concord-$build
	install -d "$root/usr/local/src"
	[ -d "$root$src" ] || jexec "$jail" git -c advice.detachedHead=false clone -q --depth 1 \
		--branch "v$CONCORD_VERSION" --recurse-submodules --shallow-submodules "$CONCORD_URL" "$src"
	got=$(jexec "$jail" git -C "$src" rev-parse HEAD)
	sub=$(jexec "$jail" git -C "$src/reflect-c" rev-parse HEAD)
	if [ "$got" != "$CONCORD_COMMIT" ] || [ "$sub" != "$REFLECTC_COMMIT" ]; then
		echo "$src is at $got with reflect-c at $sub, not $CONCORD_COMMIT and $REFLECTC_COMMIT: not built" >&2
		exit 1
	fi
	# on its own clean checkout, once: a run stopped after them does not apply them twice
	if [ ! -e "$root$src/.patched" ]; then
		for patch in $CONCORD_PATCHES; do
			jexec "$jail" git -C "$src" apply < "$here/$patch"
		done
		: > "$root$src/.patched"
	fi
	# its tests link libcurl, which FreeBSD keeps under /usr/local: pkg-config says where (the library's own Makefile
	# adds /usr/local/include, its tests' does not)
	jexec "$jail" sh -c "cd $src && gmake && CFLAGS=\"\$(pkg-config --cflags libcurl)\" gmake -C test $CONCORD_TESTS \
		LDLIBS=\"-ldiscord \$(pkg-config --libs libcurl)\""
	for t in $CONCORD_TESTS; do
		jexec "$jail" sh -c "cd $src/test && ./$t > $t.log 2>&1" || {
			echo "Concord's $t failed in $src/test; the end of its $t.log:" >&2
			tail -n 20 "$root$src/test/$t.log" >&2
			exit 1
		}
	done
	jexec "$jail" sh -c "cd $src && gmake install PREFIX=$concord SHAREDIR=$concord/share"
	echo "$build" > "$root$concord/VERSION"
	echo "Concord $build built into $concord, its tests $CONCORD_TESTS passed"
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
echo "$jail: installed"
