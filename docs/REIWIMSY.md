<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# ReiWimsy

`rei/`, in C99 with [Concord](https://github.com/Cogmasters/concord) 3.0.1, libcurl and
[cJSON](https://github.com/DaveGamble/cJSON) 1.7.19 (`rei/third_party/cjson`). A message that names `#160` (`issue
#160`, `PR #160` too; a bare number is talk) gets one reply: each linked, with its title, issue or PR, its state and
labels, in GitHub's colours. `!nightly` links the newest nightly and its downloads, `!release` the newest version (the
nightly while there is none), `!release #355` a pull request's builds. With a GitHub token it edits a reply of the last
week when what it links changes. The cards: a message for each open pull request, each open issue and each push to main,
in channels of their own, drawn from what GitHub says now and edited as it changes (its state, its commits or changes,
the latest comment a person wrote, each package of its build); its activity (a push, a person's comment, a change of
state, a pull request's builds published or failed) moves it to the end of its channel, posted again before the old one
is deleted; a pull request or an issue closed keeps its card a day, as it closed, then the card is deleted

It reads the text of messages only to find those numbers, and sends GitHub's API only the numbers. It keeps no message
and logs nothing said, and pings no one.

The bot asks GitHub's API every 30 s, with ETags (an unchanged answer costs nothing of GitHub's 5000 an hour): the
issues and pull requests updated, the builds that run and their jobs, the nightly releases, main's head. A card that
could not be drawn whole waits for the next round, as it was. The first round on a channel deletes the cards a webhook
posted there before, and messages of its own no card keeps; people's messages stay.

Its settings, a line each of its env file, `/usr/local/etc/reiwimsy.env` (the package puts
`dist/freebsd/reiwimsy.env.sample` there to fill in; an empty value stops the service):

| Key | Default | What it is |
|---|---|---|
| `REIWIMSY_TOKEN` | none: needed | the Discord bot's token |
| `REIWIMSY_REPO` | none: needed | `OWNER/NAME`, the repository whose issues and pull requests it links |
| `REIWIMSY_NIGHTLY` | `OWNER/nightly` | where the nightlies and the pull requests' builds are released |
| `REIWIMSY_CHANNELS` | every channel it sees | `id,id`: the channels its replies answer in |
| `GITHUB_TOKEN` | none: 60 questions an hour | a fine-grained token with read-only access to public repositories: 5000 an hour; the cards need it, and so does the editing of the replies |
| `REIWIMSY_CARDS_PRS` | none: no pull request cards | the channel id of the pull requests' cards |
| `REIWIMSY_CARDS_ISSUES` | none: no issue cards | the channel id of the issues' cards |
| `REIWIMSY_CARDS_NIGHTLY` | none: no nightly cards | the channel id of the nightlies' cards |

A `REIWIMSY_` key it does not know stops it, naming the key; so does a channel id that is not one, and a card channel
without `GITHUB_TOKEN`. It keeps GitHub's answers, its replies' and its cards' ids (no text, no author) in
`/var/db/reiwimsy/state.json`.

    make -C rei test                                 # the tests: no Discord, no network, no Concord
    make -C rei CONCORD=/usr/local/reiwimsy-concord  # the bot, against Concord as its make install put it

It runs as the service `reiwimsy` (`dist/freebsd/reiwimsy.rc`): daemon(8) runs `/usr/local/libexec/reiwimsy` as the user
`reiwimsy` and starts it again when it stops, its output in syslog (tag `reiwimsy`, `/var/log/messages`), its env file
exported to it by rc.subr. Installing it: [Install](./INSTALL.md#reiwimsy).
