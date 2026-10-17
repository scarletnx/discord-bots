# Discord bots

Two bots, each a service of its own in a FreeBSD jail (Bastille), `discordbots` by default. Python 3.12 and
discord.py from the jail's packages, no pip.

| Bot | What it does | What it reads | What it sends out |
|---|---|---|---|
| `link/linkbot.py` | A message that names `#160` (`issue #160`, `PR #160` too; a bare number is talk) gets one reply: each linked, with its title, issue or PR, its state and labels, in GitHub's colours. `!nightly` links the newest nightly and its downloads, `!release` the newest version (the nightly while there is none), `!release #355` a pull request's builds. With a GitHub token it edits a reply of the last week when what it links changes | the text of messages, only to find those; nothing kept of them | the numbers, to GitHub's API |
| `xuji/xuji.py` | Grok 4.7 (XG47, codename Xuji) for its owner alone: a reply to a message mentioning @Xuji answers about the message quoted; `@Xuji handback #channel FROM [TO] [notes]` writes a handback for Aogo as a Markdown file. In a channel or in its DMs | the quoted message, or a channel's messages of those days | to xAI: the texts, scrubbed (no names or ids, people as user1, user2..., mentions, e-mails and phones out, files by name only) |

Anyone else mentioning Xuji gets nothing and costs nothing. Neither bot keeps a message or logs what was said, and
neither pings anyone.

## Tests

    python3 link/test_linkbot.py && python3 xuji/test_xuji.py

No network and no Discord: the parsing, the privacy scrub, the transcript, the prompts and the replies.

## Deploy (on the jail's host, as root)

    bastille create discordbots 15.1-RELEASE <ip> <interface>   # once, or the host's own tool for jails
    cd deploy && sh install.sh          # again after each change: copies the code, keeps the tokens

Each bot's tokens are in the jail's `/usr/local/etc/<bot>.env` (root's, 600; the `.env.example` beside install.sh
lists the keys). A service refuses to start while a value is empty. Then:

    bastille cmd discordbots service linkbot start
    bastille cmd discordbots service xuji start     # stops at once, naming xAI's models, if XUJI_MODEL is not one

Logs: `/var/log/messages` in the jail (syslog tags `linkbot`, `xuji`).

linkbot's optional settings, each a line of its env file (none left empty: the service refuses an empty value):
`LINKBOT_CHANNELS=id,id` to answer only there; `LINKBOT_NIGHTLY=OWNER/NAME` when the nightlies are not
`OWNER/nightly`; `GITHUB_TOKEN=...`, a fine-grained token with read-only access to public repositories, which takes
GitHub's limit from 60 questions an hour to 5000 and lets it edit its replies when what they link changes. It keeps
GitHub's answers and its replies' ids (no text, no author) in `/var/db/linkbot/state.json`.

## The Discord applications

One application each, in the Developer Portal (Xuji can be an existing Grok bot):

1. Bot: reset the token, put it in the env file; turn on **Message Content Intent**.
2. About me: what it does and that it keeps nothing (Xuji: that it is Grok, via xAI, for its owner alone).
3. Invite (OAuth2, scope `bot`), with only what it needs:
   - linkbot: View Channels, Send Messages, Send Messages in Threads, Embed Links, Read Message History:
     `permissions=274877991936`
   - Xuji: View Channels, Send Messages, Send Messages in Threads, Attach Files, Read Message History:
     `permissions=274878008320`

   `https://discord.com/oauth2/authorize?client_id=<APPLICATION_ID>&scope=bot&permissions=<number>`

`XUJI_OWNER` is the owner's Discord user id (Developer Mode, right click, Copy User ID).
