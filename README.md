# Discord bots

Two bots, each a service of its own in a FreeBSD jail (Bastille), `discordbots` by default: ReiWimsy in C99, built in
the jail with [Concord](https://github.com/Cogmasters/concord) v3.0.1, libcurl and [cJSON](https://github.com/DaveGamble/cJSON)
1.7.19 (`rei/third_party/cjson`); Xuji in Python 3.12 with discord.py from the jail's packages.

| Bot | What it does | What it reads | What it sends out |
|---|---|---|---|
| `rei/` (ReiWimsy) | A message that names `#160` (`issue #160`, `PR #160` too; a bare number is talk) gets one reply: each linked, with its title, issue or PR, its state and labels, in GitHub's colours. `!nightly` links the newest nightly and its downloads, `!release` the newest version (the nightly while there is none), `!release #355` a pull request's builds. With a GitHub token it edits a reply of the last week when what it links changes. The cards: a message for each open pull request, each open issue and each push to main, in channels of their own, drawn from what GitHub says now and edited as it changes (its state, its commits or changes, the latest comment a person wrote, each package of its build); its activity (a push, a person's comment, a change of state, a pull request's builds published or failed) moves it to the end of its channel, posted again before the old one is deleted; a pull request or an issue closed keeps its card a day, as it closed, then the card is deleted | the text of messages, only to find those; GitHub's API, every 30 s, for what changed | the numbers, to GitHub's API |
| `xuji/xuji.py` | Grok 4.7 (XG47, codename Xuji) for its owner alone: a reply to a message mentioning @Xuji answers about the message quoted; `@Xuji handback #channel FROM [TO] [notes]` writes a handback for Aogo as a Markdown file. In a channel or in its DMs | the quoted message, or a channel's messages of those days | to xAI: the texts, scrubbed (no names or ids, people as user1, user2..., mentions, e-mails and phones out, files by name only) |

Anyone else mentioning Xuji gets nothing and costs nothing. Neither bot keeps a message or logs what was said, and
neither pings anyone.

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

    make -C rei test && python3 xuji/test_xuji.py

No network and no Discord: the parsing, the gateway's events (read from their JSON: Concord decodes none of them, as
its decoder crashed on messages of 2 to 3 KB) and the messages waiting for an answer, the replies and their edits,
GitHub's ETags and pages, Discord's waits and answers, the cards (drawn, posted, edited, moved to the end at their activity, deleted a day after they close, reposted when deleted by hand), the state
file; Xuji's privacy scrub, transcript and prompts.

## Deploy (on the jail's host, as root)

    bastille create discordbots 15.1-RELEASE <ip> <interface>   # once, or the host's own tool for jails
    cd deploy && sh install.sh          # again after each change: builds ReiWimsy there, keeps the tokens

Concord is built at its pinned commit with the fixes in `deploy/*.patch` that it does not have yet (install.sh's
`CONCORD_PATCHES`), their tests run before it is installed; a new patch builds it again. The running bot keeps the old
build until `service reiwimsy restart`.

Each bot's tokens are in the jail's `/usr/local/etc/<bot>.env` (root's, 600; the `.env.example` beside install.sh
lists the keys). A service refuses to start while a value is empty. Then:

    bastille cmd discordbots service reiwimsy start
    bastille cmd discordbots service xuji start     # stops at once, naming xAI's models, if XUJI_MODEL is not one

Logs: `/var/log/messages` in the jail (syslog tags `reiwimsy`, `xuji`). install.sh takes linkbot over once: stopped,
`linkbot_enable=NO`, its env file copied to `reiwimsy.env` with `REIWIMSY_` keys; linkbot's files stay where they are.

## The Discord applications

One application each, in the Developer Portal (Xuji can be an existing Grok bot):

1. Bot: reset the token, put it in the env file; turn on **Message Content Intent**.
2. About me: what it does and that it keeps nothing (Xuji: that it is Grok, via xAI, for its owner alone).
3. Invite (OAuth2, scope `bot`), with only what it needs:
   - ReiWimsy: View Channels, Send Messages, Send Messages in Threads, Embed Links, Read Message History, and Manage
     Messages (its first round deletes the webhook's old cards): `permissions=274878000128`
   - Xuji: View Channels, Send Messages, Send Messages in Threads, Attach Files, Read Message History:
     `permissions=274878008320`

   `https://discord.com/oauth2/authorize?client_id=<APPLICATION_ID>&scope=bot&permissions=<number>`

`XUJI_OWNER` is the owner's Discord user id (Developer Mode, right click, Copy User ID).
