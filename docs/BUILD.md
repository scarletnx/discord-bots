<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Build

Both programs are C99, built with BSD make or GNU make, a C compiler and pkg-config's flags for libcurl:

| System | What to install |
|---|---|
| FreeBSD | `pkg install git gmake pkgconf curl ca_root_nss` |
| Debian, Ubuntu | `apt install build-essential pkg-config libcurl4-openssl-dev git` |

## AccordWimsy

    make -C accordwimsy          # accordwimsy/accordwimsy
    make -C accordwimsy test     # its tests: a fake GitHub and Forgejo, git on local bare repositories

It uses ReiWimsy's copy of cJSON (`rei/third_party/cjson`), so it builds from a whole checkout. At run time it needs
`git` (or FreeBSD's `git-lite`) and the system's CA certificates.

## ReiWimsy

ReiWimsy links [Concord](https://github.com/Cogmasters/concord) 3.0.1 statically, with a fix it does not have yet
(`dist/freebsd/concord-heartbeat-send.patch`: a heartbeat that cannot be sent leaves its gateway retrying every
millisecond, never reconnecting). Concord at the commit `dist/freebsd/build.sh` checks, with that fix, installed into a
folder of its own:

    git clone --depth 1 --branch v3.0.1 --recurse-submodules --shallow-submodules \
        https://github.com/Cogmasters/concord.git /tmp/concord
    git -C /tmp/concord apply "$PWD/dist/freebsd/concord-heartbeat-send.patch"
    gmake -C /tmp/concord && gmake -C /tmp/concord install PREFIX=/opt/concord SHAREDIR=/opt/concord/share
    make -C rei CONCORD=/opt/concord      # rei/reiwimsy
    make -C rei test                      # its tests: no Discord, no network, no Concord

## The FreeBSD packages

On the FreeBSD release the jails run (15.1 here), with the network, from a checkout:

    sh dist/freebsd/build.sh [OUT]        # OUT: ./packages when not given

It clones Concord at its pinned commits (`CONCORD_COMMIT`, and `REFLECTC_COMMIT` for its reflect-c submodule) and
refuses others, applies the fix, runs Concord's `unit-gateway-lifecycle` test, builds both programs from a copy of
the checkout with their own tests first, and writes with `pkg create`:

| Package | Its files | Depends on |
|---|---|---|
| `reiwimsy-<VERSION>.pkg` | `/usr/local/libexec/reiwimsy`, `/usr/local/etc/rc.d/reiwimsy`, `/usr/local/etc/reiwimsy.env.sample` | curl, ca_root_nss |
| `accordwimsy-<VERSION>.pkg` | `/usr/local/bin/accordwimsy`, `/usr/local/etc/accordwimsy/accordwimsy.env.sample`, `/usr/local/share/examples/accordwimsy/{accordwimsy.cron,accordwimsy.newsyslog}` | curl, git-lite or git, ca_root_nss |

`VERSION`, at the top of the checkout, is both packages' version; a dependency is recorded at the version the building
machine has installed. On install each one's post-install script makes its user, and its env file from the sample
when there is none (mode 600); AccordWimsy's also makes `/var/db/accordwimsy` (700, its user's) and
`/var/log/accordwimsy` with its log files. A package changes nothing it did not install: an env file filled in stays,
and so do the users when it is deleted. What a package holds: `pkg info -F <file> -l`, its scripts `pkg info -F
<file> -D`.

A FreeBSD port of AccordWimsy, `devel/accordwimsy`, builds from a commit of this repository in noirjail.sh's ports
overlay, for a poudriere repository: what an isolated jail installs from.
