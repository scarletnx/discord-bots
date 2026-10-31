#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# Builds the FreeBSD packages of ReiWimsy and AccordWimsy from this checkout, on the FreeBSD release they are for:
#
#   sh dist/freebsd/build.sh [OUT]      OUT: ./packages when not given
#
# Writes OUT/reiwimsy-<VERSION>.pkg and OUT/accordwimsy-<VERSION>.pkg, VERSION from the file at the top of the
# checkout. Needs git, gmake, pkgconf, curl and ca_root_nss installed (pkg install git gmake pkgconf curl ca_root_nss)
# and the network, for Concord. Each program's tests run before it is packaged, and Concord's own before it is used.
# ReiWimsy links Concord statically: Concord CONCORD_VERSION at CONCORD_COMMIT, its reflect-c submodule at
# REFLECTC_COMMIT, the fixes CONCORD_PATCHES names applied over it in that order. A package depends on the packages
# this machine has installed of curl and ca_root_nss (and git, for AccordWimsy), at the versions it has.
set -eu

CONCORD_VERSION=3.0.1
CONCORD_COMMIT=d7f224b70c9dd0b1aa4997f025d1322641611977
REFLECTC_COMMIT=cad2211cd2b912813f38c0c0c7a34160cc67ca97
CONCORD_URL=https://github.com/Cogmasters/concord.git
# a heartbeat that cannot be sent (a connection gone without a CLOSE) leaves Concord's gateway retrying it every
# millisecond, never reconnecting
CONCORD_PATCHES=concord-heartbeat-send.patch
CONCORD_TESTS=unit-gateway-lifecycle

WWW=https://github.com/scarletnx/wimsy-core
MAINTAINER=noreply@scarletnx.dev

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)

if [ "$(uname -s)" != FreeBSD ]; then
	echo "build.sh makes FreeBSD packages: run it on FreeBSD, not on $(uname -s)" >&2
	exit 1
fi
for tool in git gmake pkg-config pkg; do
	command -v "$tool" > /dev/null || { echo "$tool is not installed: pkg install git gmake pkgconf" >&2; exit 1; }
done
version=$(cat "$top/VERSION")
case "$version" in
'' | *[!0-9.]*)
	echo "$top/VERSION says \"$version\", not a version of digits and dots" >&2
	exit 1
	;;
esac
mkdir -p "${1:-packages}"
out=$(cd "${1:-packages}" && pwd)
work=$(mktemp -d -t wimsy-pkg)
trap 'rm -rf "$work"' EXIT

# The packages <name>... this machine has, as a manifest's deps entries; stops on one it lacks
deps() {
	for d in "$@"; do
		pkg query '"%n": { origin: "%o", version: "%v" },' "$d" 2> /dev/null | grep . ||
			{ echo "$d is not installed here, and the package would depend on it: pkg install $d" >&2; exit 1; }
	done
}

# Writes the package <name> from $work/stage/<name>: its manifest made of <comment>, <origin>, the description in
# <name>.desc, the deps entries <deps> and the post-install script <name>.post-install, its files those plist names
# (<name>.plist, relative to /usr/local)
package() {
	_name=$1
	_comment=$2
	_origin=$3
	_deps=$4
	{
		printf 'name: "%s"\nversion: "%s"\norigin: "%s"\n' "$_name" "$version" "$_origin"
		printf 'comment: "%s"\nwww: "%s"\nmaintainer: "%s"\n' "$_comment" "$WWW" "$MAINTAINER"
		printf 'prefix: "/usr/local"\nlicenselogic: "single"\nlicenses: ["GPLv3+"]\n'
		printf 'desc: <<EOD\n'
		cat "$here/$_name.desc"
		printf 'EOD\n'
		printf 'deps: {\n%s\n}\n' "$_deps"
		printf 'scripts: {\npost-install: <<EOD\n'
		cat "$here/$_name.post-install"
		printf 'EOD\n}\n'
	} > "$work/$_name.manifest"
	pkg create -M "$work/$_name.manifest" -r "$work/stage/$_name" -p "$here/$_name.plist" -o "$out"
	echo "$out/$_name-$version.pkg"
}

# Concord, from its pinned commits, with its fixes, its tests passed
git -c advice.detachedHead=false clone -q --depth 1 --branch "v$CONCORD_VERSION" --recurse-submodules \
	--shallow-submodules "$CONCORD_URL" "$work/concord"
got=$(git -C "$work/concord" rev-parse HEAD)
sub=$(git -C "$work/concord/reflect-c" rev-parse HEAD)
if [ "$got" != "$CONCORD_COMMIT" ] || [ "$sub" != "$REFLECTC_COMMIT" ]; then
	echo "Concord v$CONCORD_VERSION is at $got with reflect-c at $sub, not $CONCORD_COMMIT and $REFLECTC_COMMIT" >&2
	exit 1
fi
for patch in $CONCORD_PATCHES; do
	git -C "$work/concord" apply "$here/$patch"
done
# its tests link libcurl, which FreeBSD keeps under /usr/local: pkg-config says where (Concord's own Makefile adds
# /usr/local/include, its tests' does not)
(cd "$work/concord" && gmake -s && CFLAGS="$(pkg-config --cflags libcurl)" gmake -s -C test $CONCORD_TESTS \
	LDLIBS="-ldiscord $(pkg-config --libs libcurl)")
for t in $CONCORD_TESTS; do
	(cd "$work/concord/test" && "./$t" > "$t.log" 2>&1) || {
		echo "Concord's $t failed; the end of its log:" >&2
		tail -n 20 "$work/concord/test/$t.log" >&2
		exit 1
	}
done
(cd "$work/concord" && gmake -s install PREFIX="$work/concord-prefix" SHAREDIR="$work/concord-prefix/share")

# The programs, built and tested from a copy, so the checkout keeps no objects
cp -R "$top/rei" "$top/accordwimsy" "$work/"
(cd "$work/rei" && make -s clean && make -s test && make -s CONCORD="$work/concord-prefix")
(cd "$work/accordwimsy" && make -s clean && make -s test)

s=$work/stage/reiwimsy/usr/local
install -d "$s/libexec" "$s/etc/rc.d"
install -m 755 "$work/rei/reiwimsy" "$s/libexec/reiwimsy"
install -m 555 "$here/reiwimsy.rc" "$s/etc/rc.d/reiwimsy"
install -m 644 "$here/reiwimsy.env.sample" "$s/etc/reiwimsy.env.sample"
# an assignment: a dependency missing stops the script, which an argument's $(...) would not
rdeps=$(deps curl ca_root_nss)
package reiwimsy "ScarletNX's Discord bot: links issues and pull requests, keeps cards" net-im/reiwimsy "$rdeps"

s=$work/stage/accordwimsy/usr/local
install -d "$s/bin" "$s/etc/accordwimsy" "$s/share/examples/accordwimsy"
install -m 755 "$work/accordwimsy/accordwimsy" "$s/bin/accordwimsy"
install -m 644 "$here/accordwimsy.env.sample" "$s/etc/accordwimsy/accordwimsy.env.sample"
install -m 644 "$here/accordwimsy.cron" "$s/share/examples/accordwimsy/accordwimsy.cron"
install -m 644 "$here/accordwimsy.newsyslog" "$s/share/examples/accordwimsy/accordwimsy.newsyslog"
# git or git-lite, whichever this machine has: AccordWimsy runs either
gitpkg=$(pkg query %n git-lite git 2> /dev/null | head -n 1)
[ -n "$gitpkg" ] || { echo "neither git-lite nor git is installed: pkg install git-lite" >&2; exit 1; }
adeps=$(deps curl "$gitpkg" ca_root_nss)
package accordwimsy "A GitHub organization kept the same on a Forgejo" devel/accordwimsy "$adeps"
