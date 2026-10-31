<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Install

## Index

* [The packages](#the-packages)
* [ReiWimsy](#reiwimsy)
  * [Its Discord application](#its-discord-application)
* [AccordWimsy](#accordwimsy)
* [Upgrade and removal](#upgrade-and-removal)

Each program runs in a FreeBSD jail of its own: ReiWimsy in one that reaches Discord and GitHub, AccordWimsy in one
that reaches GitHub and the Forgejo (how those jails are made: [Setup](./SETUP.md)). The commands below run as root on
the jails' host.

## The packages

Built on the jails' FreeBSD release ([Build](./BUILD.md#the-freebsd-packages)), then, from the checkout:

    sh scripts/install.sh discordbots packages/reiwimsy-1.0.0.pkg

`scripts/install.sh JAIL PACKAGE...` installs each package's dependencies from the jail's package repository, adds
the package from its file, and restarts its service when it was running. It turns no service on. A jail that reaches no
package repository installs from one of the host's instead (noirjail.sh: `noirjail.sh jail pkg --name JAIL
accordwimsy`, from its poudriere repository). A jail of a guard group trips on any process started in it: the group is
paused first and learns after ([Setup](./SETUP.md#the-guard)).

## ReiWimsy

1. Its Discord application (below) and its token.
2. The env file the package made, `/usr/local/etc/reiwimsy.env` (root's, 600): `REIWIMSY_TOKEN`, and the other keys
   [ReiWimsy](./REIWIMSY.md) lists. A value left empty keeps the service from starting, naming the file.
3. On, and started:

       sysrc -j discordbots reiwimsy_enable=YES
       service -j discordbots reiwimsy start

Its log is syslog's, tag `reiwimsy`, in the jail's `/var/log/messages`; what it keeps of its replies and cards is in
`/var/db/reiwimsy/state.json` (`reiwimsy_state` in rc.conf moves it).

### Its Discord application

In the Developer Portal:

1. Bot: reset the token, put it in the env file; turn on **Message Content Intent** (without it, the messages it reads
   come with no text).
2. About me: what it does and that it keeps nothing.
3. Invite (OAuth2, scope `bot`), with only what it needs: View Channels, Send Messages, Send Messages in Threads, Embed
   Links, Read Message History, and Manage Messages (its first round on a card channel deletes the webhooks' cards
   there):

   `https://discord.com/oauth2/authorize?client_id=<APPLICATION_ID>&scope=bot&permissions=274878000128`

A channel id for `REIWIMSY_CHANNELS` and the card channels: Developer Mode, right click, Copy Channel ID.

## AccordWimsy

1. On the Forgejo: a user for it (`accordwimsy`, `ACCORDWIMSY_FORGEJO_USER` when another) and its token with
   `write:repository`, `write:issue`, `write:organization` and `read:user`; the organization it fills, and, by an
   admin in that organization's settings, the number of repositories it may hold (`-1` there means the site's
   default, 0, and every repository made fails).
2. On GitHub: a fine-grained token of the organization, read-only: Contents, Issues, Pull requests, Metadata.
3. The env file the package made, `/usr/local/etc/accordwimsy/accordwimsy.env` (its user's, 600): the Forgejo's
   address, both tokens, and the other keys [AccordWimsy](./ACCORDWIMSY.md) lists.
4. A first pass by hand, then what it says of each repository (in a jail of a guard group, with the group paused:
   these start processes in it):

       jexec gitsync su -m accordwimsy -c '/usr/local/bin/accordwimsy sync'
       jexec gitsync /usr/local/bin/accordwimsy status

5. Its cron line and its log rotation, from the package's examples:

       cp /usr/local/share/examples/accordwimsy/accordwimsy.cron /usr/local/etc/cron.d/accordwimsy
       cp /usr/local/share/examples/accordwimsy/accordwimsy.newsyslog /usr/local/etc/newsyslog.conf.d/accordwimsy.conf

   (inside the jail, or under its root from the host for a jail of a guard group). A pass every 10 minutes, its output
   in `/var/log/accordwimsy/accordwimsy.log` and `.err`, with no mail from cron.

What someone has to see goes to `accordwimsy.err`; noirjail.sh's `JAIL_LOG_MAIL` mails its new lines, and its
`JAIL_FRESH` reads `ACCORDWIMSY_FRESH` to say when a mirror falls behind.

## Upgrade and removal

A new version: build the packages again and run `scripts/install.sh` again; ReiWimsy's service starts again on the
new program, AccordWimsy's next pass runs it. Removal:

    service -j discordbots reiwimsy stop
    pkg -j discordbots delete reiwimsy

The env files filled in, the state and the users stay; `pw userdel` and the files by hand take them out.
