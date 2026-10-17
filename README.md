# Discord bots

Two bots, each a service of its own in a FreeBSD jail (Bastille), `discordbots` by default. Python 3.12 and
discord.py from the jail's packages, no pip.

| Bot | What it does | What it reads | What it sends out |
|---|---|---|---|
| `link/linkbot.py` | A message that says `#160`, `issue 160`, `PR 160` or `pull request 160` gets one reply: each linked, with its title, issue or PR, its state and labels, in GitHub's colours | the text of messages, only to find those numbers; nothing kept | the numbers, to GitHub's API |
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
