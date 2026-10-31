# Discord bots

## Index

* [AccordWimsy](#accordwimsy)
* [ReiWimsy](#reiwimsy)
* [Tests](#tests)
* [Deploy (on the jail's host, as root)](#deploy-on-the-jails-host-as-root)
* [The Discord application](#the-discord-application)

ReiWimsy, a service in a FreeBSD jail (Bastille), `discordbots` by default, in C99, built in the jail with
[Concord](https://github.com/Cogmasters/concord) v3.0.1, libcurl and [cJSON](https://github.com/DaveGamble/cJSON)
1.7.19 (`rei/third_party/cjson`). And one that is no bot: AccordWimsy, which keeps a Forgejo the same as the GitHub
organization (below). DraVee's own bots, AogoWimsy and Xuji, moved to a private repository of his on 2026-10-30.

| Bot | What it does | What it reads | What it sends out |
|---|---|---|---|
| `rei/` (ReiWimsy) | A message that names `#160` (`issue #160`, `PR #160` too; a bare number is talk) gets one reply: each linked, with its title, issue or PR, its state and labels, in GitHub's colours. `!nightly` links the newest nightly and its downloads, `!release` the newest version (the nightly while there is none), `!release #355` a pull request's builds. With a GitHub token it edits a reply of the last week when what it links changes. The cards: a message for each open pull request, each open issue and each push to main, in channels of their own, drawn from what GitHub says now and edited as it changes (its state, its commits or changes, the latest comment a person wrote, each package of its build); its activity (a push, a person's comment, a change of state, a pull request's builds published or failed) moves it to the end of its channel, posted again before the old one is deleted; a pull request or an issue closed keeps its card a day, as it closed, then the card is deleted | the text of messages, only to find those; GitHub's API, every 30 s, for what changed | the numbers, to GitHub's API |

No bot keeps a message or logs what was said, and none pings anyone.

## AccordWimsy

`accordwimsy/`: the repositories of a GitHub organization, the same on a Forgejo, a pass at a time (cron, every 10
minutes), in C99 with libcurl and ReiWimsy's cJSON (`rei/third_party/cjson`). It needs no Discord and listens on no
port. A pass, `accordwimsy sync [ENV FILE]`, for each repository of the organization, the new ones too:

- the repository on the Forgejo, made in its organization when it is not there, with GitHub's name, description and
  visibility, its default branch, and its Releases tab off (releases stay on GitHub);
- its branches and tags, through a bare mirror in `mirrors/`: fetched from GitHub (the mirror pruned of what GitHub
  has not, GitHub's pull requests' heads with them) and pushed to the Forgejo, which loses nothing GitHub loses. A
  branch or a tag GitHub has no more stays on the Forgejo `ACCORDWIMSY_GRACE_DAYS` (7) from the pass that first sees
  it gone, said then in its log with the day it goes, and is deleted after that; back on GitHub before, it just stays.
  A branch GitHub rewrote (a force-push: its new tip does not hold the old one), or a tag moved, keeps its old tip on
  the Forgejo as the branch `accordwimsy/kept/branch/<name>/<YYYYMMDD-HHMMSS>` (`.../tag/...` for a tag), said in
  its log, for the same grace. No `--prune` push: over HTTP, with the negative refspec that kept the `accordwimsy/pull/<n>`
  branches out of it, git warned `helper reported unexpected status` for each of them on every pass (2026-10-28;
  they stayed). The tokens go to git in its
  environment (`GIT_CONFIG_*`), never on its command line, and no one's git settings are read;
- its labels, made or edited as GitHub has them;
- its issues and pull requests by their numbers, which the Forgejo then gives the same: a number GitHub has no more
  (deleted, moved to another repository) becomes a closed issue saying so, and a number the Forgejo gives otherwise
  stops the repository, said, since its issues must start empty. A pull request is a pull request into the same
  branch, from its head, which GitHub keeps for every one, a fork's too (`refs/pull/<n>/head`): pushed to the Forgejo
  as the branch `accordwimsy/pull/<n>` while it is open, moved when GitHub's moves. One merged on GitHub is marked
  merged by GitHub's merge commit (the repository allows a commit to mark it, `allow_manual_merge`), one closed is
  closed, one reopened is reopened; once it is not open its branch goes, the pull request keeps its commits. One left
  open that the Forgejo closed under the sync gets its head back and is reopened on the next pass, and one still
  open whose branch is gone gets the branch back. A merge
  commit the branch here does not hold (pushed over since) leaves it closed, said. Only one whose head or base branch
  GitHub has no more is an issue, saying why, its branches (`fork:branch` into `main`), its state and where its
  changes are. Each starts with its GitHub author, day and link; an edit on GitHub (its `updated_at`: title, text,
  state, labels) edits it;
- the comments, each with its GitHub author, day and link; one edited on GitHub is edited. One deleted there stays.

An issue, a comment or a repository is never deleted on the Forgejo. A repository GitHub lists no more (deleted, moved,
or out of the token's reach) is said once on stderr and left as it is.

Not mirrored: milestones (the organization has none, 2026-10-28), a comment's deletion, a release, a pull request's
reviews and its comments on lines of its changes.

A pass asks only for what changed since the last one (GitHub's `since`), keeps in `state.json` what it made (the next
number, each issue's and comment's `updated_at`, the Forgejo's id of each comment, the state each pull request was
left in, what waits out the grace and since when, when its last pass went through, since when GitHub lists it no
more), and writes it after each
repository: a pass that stops goes on from there. One pass at a time: a second one, while one goes, says so and does
nothing. What it did goes to stdout, its log, and so do a repository's pass that went wrong (an HTTP error, a push
refused: the next pass tries again), what git itself says, and the everyday news (a branch or a tag gone from GitHub,
a branch rewritten, a pull request closed rather than merged). stderr, which the host mails (`jail logmail`), holds
only what someone has to see: the default branch gone from GitHub or rewritten, half or more of a repository's
branches and tags gone in one pass (3 at least), a repository GitHub lists no more, the Forgejo's issue numbers no
longer GitHub's (once, until that repository passes again), a `state.json` or `ACCORDWIMSY_FRESH` it cannot write,
and what keeps a pass from starting. A repository that keeps failing shows in its last good pass, which `jail fresh`
mails about once it lags. The exit status is 1 when a repository stopped. After each pass it writes `ACCORDWIMSY_FRESH`, a line per repository GitHub lists, `<name>
<seconds since the epoch of its last good pass>` (0: none yet), for the host to tell a copy that lags (noirjail.sh's
`jail fresh`). `accordwimsy status [ENV FILE]` prints each repository's last good pass, and what waits out the grace
with the day it goes.

Its settings, a line each of its env file (`deploy/accordwimsy.env.example`):

| Key | Default | What it is |
|---|---|---|
| `ACCORDWIMSY_FORGEJO` | none: needed | the Forgejo's address, `http://host:3000`: its API under `/api/v1`, its repositories under it |
| `ACCORDWIMSY_FORGEJO_TOKEN` | none: needed | a token of the Forgejo's: `write:repository`, `write:issue`, `write:organization`, `read:user` |
| `ACCORDWIMSY_FORGEJO_USER` | `accordwimsy` | whose that token is, for git's push |
| `GITHUB_TOKEN` | none: needed | a fine-grained token, read-only, of the organization: Contents, Issues, Pull requests, Metadata |
| `ACCORDWIMSY_ORG` | `scarletnx` | the GitHub organization |
| `ACCORDWIMSY_FORGEJO_ORG` | `ACCORDWIMSY_ORG`'s | the Forgejo's organization |
| `ACCORDWIMSY_STATE` | `/var/db/accordwimsy` | `state.json`, the lock and `mirrors/` |
| `ACCORDWIMSY_GIT` | `/usr/local/bin/git` | the git it runs |
| `ACCORDWIMSY_GRACE_DAYS` | `7` | how long a branch or a tag GitHub has no more, or a rewritten one's old tip, stays on the Forgejo |
| `ACCORDWIMSY_FRESH` | `/var/log/accordwimsy/accordwimsy.fresh` | each repository's last good pass; made beforehand, owned by the user, in a directory only root writes, since root on the host reads it |
| `ACCORDWIMSY_GITHUB_API`, `ACCORDWIMSY_GITHUB` | `https://api.github.com`, `https://github.com` | where GitHub is (the tests' fakes) |

A key it does not know stops it, naming the key and its line; so does a value past the room it has, and a needed one
left empty, or days that are no number. It goes out to `github.com` and `api.github.com` on 443 and to the Forgejo,
nowhere else.

Build and test: `make` and `make test` in `accordwimsy/` (FreeBSD packages: `git-lite`, `curl`, `pkgconf`,
`ca_root_nss`). The tests run a fake GitHub and a fake Forgejo (its repositories, the issues' numbers, labels,
comments, pull requests and what it refuses of them) and git for real on bare repositories in a temporary folder: no
network. In its jail: the binary in `/usr/local/bin/accordwimsy`, the env file as above, and the crontab line
`*/10 * * * * accordwimsy /usr/local/bin/accordwimsy sync`.

## ReiWimsy

The cards were GitHub Actions jobs (a run for each event of each pull request and issue, and one following each
build); the bot asks GitHub's API instead, every 30 s, with ETags (an unchanged answer costs nothing of GitHub's 5000
an hour): the issues and pull requests updated, the builds that run and their jobs, the nightly releases, main's head.
A card that could not be drawn whole waits for the next round, as it was. The first round on a channel deletes the
cards a webhook posted there before, and messages of its own no card keeps; people's messages stay.

Its settings, a line each of its env file (an empty one stops the service):

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
without `GITHUB_TOKEN`. linkbot's `LINKBOT_TOKEN`, `LINKBOT_REPO`, `LINKBOT_NIGHTLY` and `LINKBOT_CHANNELS` are read
while the `REIWIMSY_` ones are unset, the log saying so. It keeps GitHub's answers, its replies' and its cards' ids
(no text, no author) in `/var/db/reiwimsy/state.json`.

    make -C rei test                                 # the tests: no Discord, no network, no Concord
    make -C rei CONCORD=/usr/local/reiwimsy-concord  # the bot, against Concord as its make install put it

## Tests

    make -C rei test && make -C accordwimsy test

No network and no Discord: the parsing, the gateway's events (read from their JSON: Concord decodes none of them, as its
decoder crashed on messages of 2 to 3 KB) and the messages waiting for an answer, the replies and their edits, GitHub's
ETags and pages, Discord's waits and answers, the cards (drawn, posted, edited, moved to the end at their activity,
deleted a day after they close, reposted when deleted by hand), the state file. AccordWimsy's, against a fake GitHub and
a fake Forgejo and git on bare repositories: the env file and its refusals, the first pass (repositories, branches and
tags, labels, every number, a missing one, a pull request), a pass with nothing new (GitHub asked from where the last
stopped, the Forgejo written to only for its Releases tab), edits and new ones (an issue, a comment, a branch deleted),
a Forgejo whose numbers differ, and the program's lock and usage.

## Deploy (on the jail's host, as root)

    bastille create discordbots 15.1-RELEASE <ip> <interface>   # once, or the host's own tool for jails
    cd deploy && sh install.sh          # again after each change: builds ReiWimsy there, keeps the tokens

Concord is built at its pinned commit with the fixes in `deploy/*.patch` that it does not have yet (install.sh's
`CONCORD_PATCHES`), their tests run before it is installed; a new patch builds it again. The running bot keeps the old
build until `service reiwimsy restart`.

Its tokens are in the jail's `/usr/local/etc/reiwimsy.env` (root's, 600; `reiwimsy.env.example` beside install.sh
lists the keys). The service refuses to start while a value is empty. Then:

    bastille cmd discordbots service reiwimsy start

Logs: `/var/log/messages` in the jail (syslog tag `reiwimsy`). install.sh takes linkbot over once: stopped,
`linkbot_enable=NO`, its env file copied to `reiwimsy.env` with `REIWIMSY_` keys; linkbot's files stay where they are.

## The Discord application

ReiWimsy's, in the Developer Portal:

1. Bot: reset the token, put it in the env file; turn on **Message Content Intent** (without it, the messages it reads
   come with no text).
2. About me: what it does and that it keeps nothing.
3. Invite (OAuth2, scope `bot`), with only what it needs: View Channels, Send Messages, Send Messages in Threads, Embed
   Links, Read Message History, and Manage Messages (its first round deletes the webhook's old cards):

   `https://discord.com/oauth2/authorize?client_id=<APPLICATION_ID>&scope=bot&permissions=274878000128`
