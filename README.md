# Discord bots

Two bots, each a service of its own in a FreeBSD jail (Bastille), `discordbots` by default: ReiWimsy in C99, built in
the jail with [Concord](https://github.com/Cogmasters/concord) v3.0.1, libcurl and [cJSON](https://github.com/DaveGamble/cJSON)
1.7.19 (`rei/third_party/cjson`); Xuji in Python 3.12 with discord.py from the jail's packages. A third, AogoWimsy,
runs beside a Claude Code session instead: the MCP server the session starts, Python 3.12 with its standard library.

| Bot | What it does | What it reads | What it sends out |
|---|---|---|---|
| `rei/` (ReiWimsy) | A message that names `#160` (`issue #160`, `PR #160` too; a bare number is talk) gets one reply: each linked, with its title, issue or PR, its state and labels, in GitHub's colours. `!nightly` links the newest nightly and its downloads, `!release` the newest version (the nightly while there is none), `!release #355` a pull request's builds. With a GitHub token it edits a reply of the last week when what it links changes. The cards: a message for each open pull request, each open issue and each push to main, in channels of their own, drawn from what GitHub says now and edited as it changes (its state, its commits or changes, the latest comment a person wrote, each package of its build); its activity (a push, a person's comment, a change of state, a pull request's builds published or failed) moves it to the end of its channel, posted again before the old one is deleted; a pull request or an issue closed keeps its card a day, as it closed, then the card is deleted | the text of messages, only to find those; GitHub's API, every 30 s, for what changed | the numbers, to GitHub's API |
| `xuji/xuji.py` | Grok 4.7 (XG47, codename Xuji) for its owner alone: a reply to a message mentioning @Xuji answers about the message quoted; `@Xuji handback #channel FROM [TO] [notes]` writes a handback for Aogo as a Markdown file. In a channel or in its DMs | the quoted message, or a channel's messages of those days | to xAI: the texts, scrubbed (no names or ids, people as user1, user2..., mentions, e-mails and phones out, files by name only) |
| `aogo/aogowimsy.py` (AogoWimsy) | Aogo, the Claude Code session, on Discord as a bot user of its own, through four MCP tools: post (only once its owner approves the draft), the same wait on a draft still waiting, read a channel, wait for a reply. A Claude Code channel too: while the session runs it stays on Discord's gateway and wakes the session with what is addressed to it | the messages of the channels it lists and their threads, or of every one it sees, and its owner's DMs with it: the owner's, the listed users' and its own reach the session, and a stranger's only once its owner lets it through; the others only counted, and a stranger's DM only by name; its owner's reactions to a draft or to a stranger's message | to Discord, the drafts (to its owner's DMs) and the posts approved; to the session, the messages it may read and the ones addressed to it |

Anyone else mentioning Xuji gets nothing and costs nothing. No bot keeps a message or logs what was said, and none
pings anyone.

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

## AogoWimsy

A local MCP server: Claude Code starts `aogo/aogowimsy.py` and talks to it on stdin and stdout through the
`initialize` handshake, protocol 2025-11-25 back to 2024-11-05. It does not speak revision 2026-07-28: Claude Code
registers no [channel](https://code.claude.com/docs/en/channels-reference) on it, as that revision cannot carry channel
messages ([MCP, Push messages with channels](https://code.claude.com/docs/en/mcp#push-messages-with-channels)), so it
answers a 2026-07-28 client's `server/discover` with method not found, and the client falls back to `initialize`.
Discord's REST API v10 reads and posts; once the client says it is initialized, it also stays on Discord's gateway (a
WebSocket of its own: heartbeats, resume after a drop, a fresh session when Discord ends the old one, backoff up to
60 s), which wakes the session and feeds `discord_wait`. Its tools, as the session sees them:

| Tool | What it does |
|---|---|
| `discord_post(channel, text, reply_to, wait=300, files)` | sends the text as a draft to its owner's DMs with AogoWimsy (the text and its images, `files`: 1 to 4 png, jpg, gif or webp by absolute path, how big each may be Discord's to say; under it a card with the channel's name and id and, for a reply, the message it answers as a link), with a check mark and a cross, then waits up to `wait` s for its owner's reaction, looking every 3 s: the check mark posts the text as it is, with the draft's images fetched from Discord's CDN (without the token), pinging no one, and edits the card to link the post; the cross edits it to say it was not sent; no answer returns the draft's id, still waiting. Another person's reaction counts for nothing |
| `discord_post_status(draft, wait=300)` | the same wait, on a draft that came back still waiting; a draft sent or refused answers that again, and is never posted twice |
| `discord_read(channel, after, limit=50, from)` | the first `limit` messages after an id, or the newest `limit` without one, oldest first: id, author, time, text, the message it answers, attachments by name, and whether it is a stranger's its owner let through |
| `discord_wait(channel, after, wait=600, from)` | the first message after an id from the owner or a listed user, as the gateway brings it; what came before it began listening, from REST |

`from` (a list of ids, each the owner or a listed user; anyone else is an error naming the id) narrows read and wait to
those authors: the others join the count of the left out, and wait returns only on one of theirs.

Its owner's check mark is the only way a text gets out, and no setting turns it off. It reads and posts in the listed
channels and their threads, or, with none listed, in every channel and thread Discord lets the bot see (a channel it
cannot see is Discord's 403 or 404, naming it); it reads its owner's DMs with it too. Of what it reads, only the
owner's messages, the listed users' and its own reach the session; a stranger's message reaches it once the owner
lets it through, the others are counted and nothing else of them given. The owner lets a stranger's message through,
that one message:

- with his `:wimsytetoutau:` reaction, on a message addressed to AogoWimsy (a mention, or a reply to one of its
  messages); only his reaction counts;
- by replying to it with a message that mentions AogoWimsy, whoever wrote it; a listed user's reply lets nothing
  through.

Nothing is stored: a draft waiting is its message in the owner's DMs, read again at each look, so a restart loses none;
what the owner let through is kept in memory while it runs; stderr says what ran or failed, never what was said nor
who said it. A 429 waits its `retry_after` and tries again; any other failure comes back to the session with Discord's
status and message, as does a value a tool refuses, named with what it takes.

### As a channel

Started with the development flag (custom channels are not on the research preview's allowlist, so `--channels`
does not take it), from the folder whose `.mcp.json` names it:

    claude --dangerously-load-development-channels server:aogowimsy

Each start shows a warning naming the development channel: answer **I am using this for local development**. The flag
works in an interactive session only (with `-p` Claude Code drops it), and a claude.ai or Console login is needed.
Then, while the session runs, each of these wakes it as a `<channel source="aogowimsy" ...>` event, the text as its
body:

| `kind` | What wakes the session | Its attributes |
|---|---|---|
| `message` | a message addressed to AogoWimsy (a mention, or a reply to one of its messages) from the owner or a listed user, in a channel it may read; any DM the owner or a listed user sends it. When the owner replies to someone while mentioning AogoWimsy, the text ends with the message he replied to, whole | `author`, `author_id`, `channel`, `message_id`, `reply_to`, `attachments`; `quoted_author`, `quoted_author_id`, `quoted_attachments` for the owner's reply |
| `approved` | a stranger's message addressed to AogoWimsy, once the owner puts his `:wimsytetoutau:` reaction on it, once | as `message` |
| `dm` | someone neither the owner nor listed wrote to AogoWimsy in DMs: their name and id, never what they wrote; once a person while it runs | `author`, `author_id` |
| `draft` | the owner's check mark or cross on a draft this session made, still waiting: it is posted or refused then, not on a tool's next look (another session's draft waits for that session) | `draft`, `status` (sent, not sent), `channel`; `message_id` once posted |
| `error`, `notice` | the gateway stopped (a close code Discord takes no new try after: a token that is not the bot's, intents not turned on), cannot be reached for three tries in a row, or is back | |

Nothing else wakes it: a message not addressed to AogoWimsy, anything a stranger says before the owner lets it
through, its own messages. Without the flag, the gateway still runs and feeds `discord_wait`, and Claude Code drops
the events. The session answers in Discord with `discord_post` (reply_to the `message_id`), which still waits for the
owner's check mark.

Its settings, a line each of its env file, `~/.config/aogowimsy/aogowimsy.env` unless `--env FILE` names another:

| Key | Default | What it is |
|---|---|---|
| `AOGOWIMSY_TOKEN` | none: needed | the bot's token |
| `AOGOWIMSY_OWNER` | none: needed | the owner's user id: who approves every post and lets a stranger's message through |
| `AOGOWIMSY_USERS` | none: needed | `id,id`: whose messages reach the session (the owner's always do) |
| `AOGOWIMSY_CHANNELS` | every channel and thread the bot can see | `id,id`: where it reads and posts; a thread of one of them counts as it |

A key it does not know stops it, naming the key; so does an id that is not one, an empty or missing needed value, an
empty `AOGOWIMSY_CHANNELS`, a key set twice, and an env file other users can read (its mode must be 600 or stricter: it
says the mode it found). The server's instructions to the session say which channels are in effect. Setting it up,
after its application (below):

1. Its owner shares a server with it and lets that server's members send them DMs (the server's Privacy Settings,
   Direct Messages), or no draft reaches them.
2. The env file, then a look at it: the line `up` names the owner, counts the users and the channels (or says every
   channel it sees), or it says what is wrong and stops. Discord hears nothing until a session is initialized.

       install -d -m 700 ~/.config/aogowimsy
       install -m 600 aogo/aogowimsy.env.example ~/.config/aogowimsy/aogowimsy.env   # then fill it in
       python3 aogo/aogowimsy.py </dev/null

3. Claude Code, from the folder whose sessions may use it (scope `project` writes `.mcp.json` there; `user` would give
   it to every session of the account, wherever it starts). A session started there asks once to use it:

       claude mcp add --scope project aogowimsy -- python3 /path/to/discord-bots/aogo/aogowimsy.py

## Tests

    make -C rei test && python3 xuji/test_xuji.py && python3 aogo/test_aogowimsy.py

No network and no Discord: the parsing, the gateway's events (read from their JSON: Concord decodes none of them, as
its decoder crashed on messages of 2 to 3 KB) and the messages waiting for an answer, the replies and their edits,
GitHub's ETags and pages, Discord's waits and answers, the cards (drawn, posted, edited, moved to the end at their activity, deleted a day after they close, reposted when deleted by hand), the state
file; Xuji's privacy scrub, transcript and prompts; AogoWimsy's env file, its MCP answers (and the program itself on
stdin and stdout), its tools against a fake Discord on 127.0.0.1 (drafts approved, refused, still waiting, another
person's reaction, the channels and the people it may, `from`, Discord's 429s and errors), what the gateway's events
push to the session (who and what wakes it, the owner's reaction and reply letting a stranger through), and its gateway
client against a fake gateway on 127.0.0.1 (identify, heartbeats and their ACKs, resume, the close codes that stop it,
pings, messages in frames), and that lines from several threads never mix on stdout.

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

1. Bot: reset the token, put it in the env file; turn on **Message Content Intent** (without it, the messages
   AogoWimsy reads come with no text, and its gateway closes with 4014). AogoWimsy: its name and avatar are set there
   too, and **Public Bot** is off, so only its owner adds it to a server. Its gateway asks for the intents GUILDS,
   GUILD_MESSAGES, GUILD_MESSAGE_REACTIONS, DIRECT_MESSAGES, DIRECT_MESSAGE_REACTIONS (the owner's answer to a draft,
   in his DMs) and MESSAGE_CONTENT (46593); only the last is privileged.
2. About me: what it does and that it keeps nothing (Xuji: that it is Grok, via xAI, for its owner alone; AogoWimsy:
   that it is Aogo, a Claude Code session, and posts only what its owner approved).
3. Invite (OAuth2, scope `bot`), with only what it needs:
   - ReiWimsy: View Channels, Send Messages, Send Messages in Threads, Embed Links, Read Message History, and Manage
     Messages (its first round deletes the webhook's old cards): `permissions=274878000128`
   - Xuji: View Channels, Send Messages, Send Messages in Threads, Attach Files, Read Message History:
     `permissions=274878008320`
   - AogoWimsy: View Channels, Send Messages, Send Messages in Threads, Read Message History (its drafts, their card
     and their reactions are in DMs, which need none): `permissions=274877975552`

   `https://discord.com/oauth2/authorize?client_id=<APPLICATION_ID>&scope=bot&permissions=<number>`

`XUJI_OWNER` and `AOGOWIMSY_OWNER` are the owner's Discord user id (Developer Mode, right click, Copy User ID).
