<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Setup

## Index

* [The host and its tool](#the-host-and-its-tool)
* [The jails](#the-jails)
* [ReiWimsy's jail](#reiwimsys-jail)
* [The forge](#the-forge)
  * [The guard](#the-guard)
  * [Forgejo](#forgejo)
  * [The start page](#the-start-page)
  * [AccordWimsy's jail](#accordwimsys-jail)
* [The status page](#the-status-page)
* [Another machine: what changes](#another-machine-what-changes)
* [The secrets, and where each lives](#the-secrets-and-where-each-lives)

How ScarletNX's services run, to make them again elsewhere: the Discord bot, the forge that mirrors the GitHub
organization, and the status page. Installing the two programs themselves: [Install](./INSTALL.md).

## The host and its tool

One FreeBSD host runs every jail with [Bastille](https://github.com/BastilleBSD/bastille), thin jails of
15.1-RELEASE. The jails are made and kept by [noirjail.sh](https://accord.dravee.dev/DraVee/noirjail.sh)
(Apache-2.0): Bastille underneath, pf rules for what each jail may reach, Cloudflare tunnels, and a guard that stops a
group of jails when they run something they never ran before. Its settings sit in
`/usr/local/etc/noirjail/jail-inventory.json`, set with `noirjail.sh config set NAME VALUE`. The commands below are
noirjail.sh's; each one's own help says what Bastille and pf do under it.

## The jails

| Jail | Runs | Network | Reaches |
|---|---|---|---|
| `discordbots` | ReiWimsy | the host's LAN bridge | Discord, GitHub |
| `gitsync` | AccordWimsy, from cron | isolated: a /30 of `ISOLATED_NET` with NAT | DNS, GitHub on 443 (pf table `github`), `gitscarlet` on 3000 |
| `gitscarlet` | Forgejo, on 3000 | isolated | DNS only; reached by `gitedge` and `gitsync` on 3000 |
| `gitedge` | cloudflared, the forge's tunnel | isolated | Cloudflare's edge (7844, TCP and UDP), `gitscarlet` on 3000 |
| `status` | Gatus and cloudflared, the status page's tunnel | the host's LAN bridge | the addresses it checks, Cloudflare |

An isolated jail asks `ISOLATED_DNS` for names and reaches no package repository: its packages come from the host's
(noirjail.sh's `jail pkg` mounts them in).

## ReiWimsy's jail

    bastille create discordbots 15.1-RELEASE <ip> <interface>

Then the package and its settings: [Install](./INSTALL.md#reiwimsy).

## The forge

A copy of the GitHub organization on [Forgejo](https://forgejo.org): code, issues and pull requests, kept the same
every 10 minutes by AccordWimsy; releases stay on GitHub. Three isolated jails, each doing one thing, so the one that
holds the tokens to GitHub is not the one that answers the internet.

    noirjail.sh jail create --isolated --name gitsync --release 15.1-RELEASE
    noirjail.sh firewall jail gitsync egress allow --to table:github --port 443
    noirjail.sh firewall jail gitsync egress allow --to jail:gitscarlet --port 3000

`gitscarlet` and `gitedge` are made the same way, with `--name gitscarlet` and `--name gitedge`. Forgejo goes into
`gitscarlet` with `noirjail.sh forgejo install --url gitscarlet --fqdn <the forge's name> --db sqlite`, which leaves
it no egress but DNS, and lets in port 3000 from `gitedge` and `gitsync` alone. cloudflared goes into `gitedge` with
`noirjail.sh cloudflared install --url gitedge`, its tunnel's token read from a file (`--token-file`), and the tunnel's
public name routes to `http://<gitscarlet's address>:3000`.

### The guard

    noirjail.sh guard add --group <the forge's name> gitscarlet gitedge gitsync
    noirjail.sh guard learn <the forge's name>

The guard learns what the three jails run, listen on and hold in `/usr/local`, `/etc` and `/var/cron/tabs`. A process,
a socket or a file it never learned trips the whole group: the three jails stopped, the evidence kept, the name taken
off Cloudflare, a mail sent. Any change to them therefore goes:

    noirjail.sh guard pause <the forge's name> --minutes 15
    # the change: a package, a file under /usr/local, a restart
    noirjail.sh guard learn <the forge's name>
    noirjail.sh guard pause <the forge's name> --minutes 1

Nothing starts a process in them otherwise: no `jexec`, no `bastille cmd`, no `pkg -j`. They are read from the host,
under `/usr/local/bastille/jails/<jail>/root`, and `pkg -c <that root> query` reads their packages.

### Forgejo

What `noirjail.sh forgejo install` sets in `app.ini`: `ROOT_URL` and `DOMAIN` from its `--fqdn`; `HTTP_ADDR` the
jail's own address, port 3000; no SSH; offline mode; registration closed, and repositories made only by an admin; no
push mirrors; `[git] PULL_REQUEST_PUSH_MESSAGE = false`; migrations only from github.com and api.github.com; Actions,
packages, federation and the update check off; `[security] DISABLE_WEBHOOKS = true`; mail off until
`FORGEJO_MAIL_FROM` is set. Its three secrets (`SECRET_KEY`, `INTERNAL_TOKEN`, `JWT_SECRET`) are made in the jail by
`forgejo generate secret` and never leave it; `app.ini` is root:git 640 in a root:git 750 directory. Cloudflare
challenges every client that is no browser on the forge's name, so repositories come in through `gitsync`, never by a
clone from outside.

### The start page

`dist/forge/`: Forgejo's start page (`templates/home.tmpl`: getting started, the bots, the repositories) and its
images (`public/assets/img`: the logo, the favicons, Wimsy, each bot's avatar). With the group paused, from the host:

    sh dist/forge/install.sh
    service -j gitscarlet forgejo restart

`install.sh` copies them into Forgejo's custom directory in the jail, `/usr/local/etc/forgejo`, and Forgejo reads them
when it starts again. A file it no longer installs (an image taken out) stays there until it is removed by hand.

### AccordWimsy's jail

AccordWimsy's package, from the host's poudriere repository (its port `devel/accordwimsy` in noirjail.sh's ports
overlay), with the group paused:

    noirjail.sh jail pkg --name gitsync accordwimsy

Its env file, cron line and log rotation: [Install](./INSTALL.md#accordwimsy). Its cron runs with `cron_flags="-J
60"`. From the host, noirjail.sh's own cron mails the new lines of its `accordwimsy.err`
(`noirjail.sh config set JAIL_LOG_MAIL "gitsync:/var/log/accordwimsy/accordwimsy.err"`) and says when a mirror falls
behind (`JAIL_FRESH`, `"gitsync:/var/log/accordwimsy/accordwimsy.fresh:60"`, read by `noirjail.sh jail fresh`).

## The status page

`dist/status/`: [Gatus](https://github.com/TwiN/gatus) 5.38.0 in the jail `status`, its port reached only through
the jail's own Cloudflare tunnel. It checks the site, the nightly release and the Discord invite itself, and the
public bots through what the host pushes to it every minute. The forge is not checked: Cloudflare's challenge stops
any checker that is no browser.

| File | What it is |
|---|---|
| `addresses.conf` | every address the page names or probes, the one place they are written |
| `gatus.yaml.in` | the config, `@NAME@` for each address and `@ICON_<BOT>@` for each icon |
| `push.sh.in` | the bots' state, pushed every minute from the host's cron |
| `render.sh` | writes `gatus.yaml` and `push.sh` from the two; those are what runs, never edited by hand |
| `colours.py` | Wimsy's colours as Gatus's, from ScarletNX's `dist/wimsy.svg` |
| `icons/` | each bot's icon, 64 px, put into the page as data |
| `check.sh` | the page checked without Gatus ([Testing](./TESTING.md)) |
| `install.sh` | as root on the host: the push token, the config and the database directory into the jail, Gatus started again, `push.sh` and its cron line |

| Endpoint | Every | Up when |
|---|---|---|
| scarletnx.dev | 5 min | it answers 302 (to the Pages site) |
| Site (scarletnx.github.io) | 5 min | 200, and the page says "ScarletNX is a static recompiler" |
| Nightly release | 10 min | GitHub's API has a latest release named `nightly-*` (6 of the 60 calls an hour it allows) |
| Discord invite | 30 min | Discord's API knows the invite the site links to |
| ReiWimsy | pushed | `reiwimsy_enable` is YES in `discordbots` and `/usr/local/libexec/reiwimsy` has run a minute or more |
| AccordWimsy | pushed | `noirjail.sh jail fresh` passes: each mirror had a good pass within its time |

No push for 5 minutes reads as down. A running process is no proof of a live Discord connection: a bot whose gateway
stays down while it runs still reads as up. Only public bots are on the page.

    noirjail.sh jail create --name status --release 15.1-RELEASE --ip <its address>
    noirjail.sh jail pkg --name status gatus cloudflared
    sh dist/status/install.sh status
    noirjail.sh cloudflare tunnel --tunnel <the page's name>
    noirjail.sh cloudflare publish --tunnel <the page's name> --fqdn <the page's name> --public --service http://127.0.0.1:8080
    noirjail.sh cloudflared install --url status --tunnel <the page's name>

After a change to a template, an icon or an address: `sh dist/status/render.sh`, `sh dist/status/check.sh
<wimsy.svg>`, `sh dist/status/install.sh status`. After Wimsy's colours change: `python3 dist/status/colours.py
<wimsy.svg> --update` first. Gatus is FreeBSD's `www/gatus`; it is pure Go (`CGO_ENABLED=0 go build`), so the same
config runs on any device that builds it, with `GATUS_LISTEN=127.0.0.1` there.

## Another machine: what changes

- noirjail.sh's settings (`noirjail.sh config set`): `CLOUDFLARE_TOKEN_FILE`, `CLOUDFLARE_ACCOUNT_ID`,
  `CLOUDFLARE_ZONE`, `CLOUDFLARE_TUNNEL`, `CLOUDFLARE_NOTIFY_EMAIL`; `ISOLATED_NET`, `ISOLATED_DNS`;
  `GUARD_NOTIFY_EMAIL`, `GUARD_FROM`, `FORGEJO_MAIL_FROM`; `JAIL_LOG_MAIL`, `JAIL_FRESH`.
- The names: the forge's (Forgejo's `--fqdn`, the guard group, its Cloudflare route) and the status page's (its
  tunnel and route, `STATUS_URL`).
- The addresses: `gitscarlet`'s in `ACCORDWIMSY_FORGEJO`, the forge's route and pf's anchors; the status page's, all
  of them in `dist/status/addresses.conf` (`GATUS_PUSH_URL` is the `status` jail's), then `render.sh`.
- The jails' names, when they differ: `discordbots` in `dist/status/push.sh.in`, `gitscarlet` in
  `dist/forge/install.sh`.

## The secrets, and where each lives

None is in this repository; each is made or put in place on the host.

| Secret | Where |
|---|---|
| ReiWimsy's Discord token, its GitHub token | `discordbots`: `/usr/local/etc/reiwimsy.env` (root's, 600) |
| AccordWimsy's Forgejo token, its GitHub token | `gitsync`: `/usr/local/etc/accordwimsy/accordwimsy.env` (its user's, 600) |
| Forgejo's `SECRET_KEY`, `INTERNAL_TOKEN`, `JWT_SECRET` | `gitscarlet`: `app.ini`, made in place |
| Each tunnel's connector token | the file `--token-file` names, in `gitedge` and in `status` |
| The Cloudflare API token | the file `CLOUDFLARE_TOKEN_FILE` names, on the host |
| Gatus's push token | the host: `/usr/local/etc/status-scarletnx/push-header` (root's, 600), made by `install.sh`; the jail's `gatus.yaml` (root:www, 640); `@PUSH_TOKEN@` in `dist/status` |
