#!/usr/bin/env python3
"""Xuji: Grok on Discord, for its owner alone.

    XUJI_TOKEN=... XAI_API_KEY=... XUJI_OWNER=<Discord user id> [XUJI_MODEL=grok-4.7] [XUJI_MAX_TOKENS=800] xuji.py

The owner replies to a message, mentioning Xuji, with a question or none: Grok answers about the message quoted.
Anyone else mentioning Xuji gets nothing and costs nothing: no reply, no call to xAI.

    @Xuji handback #channel FROM [TO] [notes]       (dates YYYY-MM-DD, UTC, both days in; TO: today)

The owner, in a channel or in Xuji's DMs, gets a handback for Aogo as a Markdown file: the channel's messages of those
days (a forum's, by thread) summed up by Grok in English, one section per problem. Its people are user1, user2...,
numbered for that handback alone; bots' messages are left out, and a file is its name only.

What goes to xAI, and nothing else: the quoted message's text and the owner's question, scrubbed (scrub()): no
author's name, id or avatar, mentions as @someone, @role and #channel, e-mail addresses and phone numbers out, no
attachment, no other message. Nothing is kept: no message, no answer, no log of what was said; the log says only
that a question was answered or failed. The replies ping no one. The model is checked against xAI's list when Xuji
starts, and one that is not there stops it, naming the ones that are.
"""

import datetime
import io
import json
import os
import re
import sys
import urllib.error
import urllib.request

API = 'https://api.x.ai/v1'
LIMIT = 2000  # a Discord message's letters
MENTION = re.compile(r'<@!?\d+>')
ROLE = re.compile(r'<@&\d+>')
CHANNEL = re.compile(r'<#\d+>')
EMAIL = re.compile(r'[\w.+-]+@[\w-]+\.[\w.-]+')
PHONE = re.compile(r'(?<![\w.])\+?\(?\d[\d ().-]{7,}\d(?![\w.])')
PHONE_DIGITS = 10  # a phone number has this many digits at least; a date or a version has fewer
SYSTEM = (
    'You are Xuji, Grok answering one person on Discord about a message they quoted. You are not told who wrote it. '
    'Answer in the language of the question, or of the quoted message when there is no question. Be brief and plain.'
)


def scrub(text):
    """text with nothing that names or reaches a person: mentions, e-mail addresses and phone numbers"""
    text = MENTION.sub('@someone', text or '')
    text = ROLE.sub('@role', text)
    text = CHANNEL.sub('#channel', text)
    text = EMAIL.sub('[email]', text)
    return PHONE.sub(
        lambda m: '[phone]' if sum(c.isdigit() for c in m.group()) >= PHONE_DIGITS else m.group(), text
    ).strip()


def prompt(quoted, question):
    """the messages xAI gets: the system's, then the quoted text and the question, both scrubbed"""
    asked = scrub(question) or 'Explain the quoted message, or answer it.'
    said = scrub(quoted)
    user = f'Quoted message:\n"""\n{said}\n"""\n\nQuestion: {asked}' if said else asked
    return [{'role': 'system', 'content': SYSTEM}, {'role': 'user', 'content': user}]


def pieces(text, limit=LIMIT):
    """text in messages Discord takes, cut at a line or a space when it can"""
    out = []
    while len(text) > limit:
        cut = text.rfind('\n', 0, limit)
        cut = cut if cut > limit // 2 else text.rfind(' ', 0, limit)
        cut = cut if cut > 0 else limit
        out.append(text[:cut].rstrip())
        text = text[cut:].lstrip()
    return out + ([text] if text else [])


def asked_by_owner(author_id, owner, mentioned, private=False):
    """whether a message is the owner's, mentioning Xuji or in its DMs: the only ones it answers"""
    return (mentioned or private) and author_id == owner


# @Xuji handback #channel FROM [TO] [notes]: the channel by mention or name, the days in ISO
HANDBACK = re.compile(
    r'^\s*handback\s+(<#\d+>|#?[\w-]+)\s+(\d{4}-\d{2}-\d{2})(?:\s+(\d{4}-\d{2}-\d{2}))?\s*(.*)$',
    re.DOTALL | re.IGNORECASE,
)
MOST_MESSAGES = 3000  # what a handback reads at most
MOST_TRANSCRIPT = 300_000  # letters of them sent to xAI at most
HANDBACK_SYSTEM = (
    'You prepare handbacks for Aogo, an AI coding agent working on ScarletNX, a static ARM64 to x86 recompiler with a '
    "runtime for a console's games. From a support channel's messages, write in English, in Markdown: a short overview, "
    'then one section per distinct problem: what the users see, the platform and version when said, the steps, the logs '
    'or files mentioned, what was tried or answered, whether it looks solved, and which users (by the names given) '
    'reported it. Merge duplicates. Never invent a fact: say what is missing. The users are pseudonymous (user1, '
    'user2...): never guess who they are.'
)


def parse_handback(text, today):
    """(channel, first day, last day, notes) for a handback command, or None when text is not one; a ValueError for
    days that are not dates, or out of order"""
    match = HANDBACK.match(text or '')
    if not match:
        return None
    start = datetime.date.fromisoformat(match.group(2))
    end = datetime.date.fromisoformat(match.group(3)) if match.group(3) else today
    if end < start:
        raise ValueError(f'{end} comes before {start}')
    return match.group(1), start, end, match.group(4).strip()


def transcript(entries):
    """the messages as xAI reads them, and how many people wrote them: each entry a dict of thread (its title, or ''),
    author (an id), bot, when (a datetime), text and files (names). People are user1, user2... by first message; bots'
    messages are left out; a file is [file: name]"""
    names, lines, thread = {}, [], None
    for entry in entries:
        if entry['bot']:
            continue
        if entry['thread'] != thread:
            thread = entry['thread']
            if thread:
                lines += ['', f'## {scrub(thread)}']
        who = names.setdefault(entry['author'], f'user{len(names) + 1}')
        files = ''.join(f' [file: {scrub(name)}]' for name in entry['files'])
        lines.append(f'[{entry["when"]:%Y-%m-%d %H:%M}] {who}: {scrub(entry["text"])}{files}')
    text = '\n'.join(lines).strip()
    if len(text) > MOST_TRANSCRIPT:
        text = text[:MOST_TRANSCRIPT].rsplit('\n', 1)[0] + '\n[the rest left out: too long]'
    return text, len(names)


def handback_prompt(said, channel, start, end, notes):
    """the messages xAI gets for a handback"""
    head = f'Channel: #{channel}, {start} to {end} (UTC).\n'
    if notes:
        head += f"Owner's notes: {scrub(notes)}\n"
    return [{'role': 'system', 'content': HANDBACK_SYSTEM}, {'role': 'user', 'content': f'{head}Messages:\n{said}'}]


def handback_file(answer, channel, start, end, count, people):
    """the handback as the owner gets it: its name, and its Markdown"""
    head = (
        f'# Handback: #{channel}, {start} to {end}\n\n'
        f'From Xuji (Grok): {count} messages from {people} people, named user1, user2... for this handback alone.\n\n'
    )
    return f'handback-{channel}-{start}-{end}.md', head + answer.strip() + '\n'


class Grok:
    """xAI's chat API: the model checked once, then questions"""

    def __init__(self, key, model, most, post=None, get=None):
        self.key, self.model, self.most = key, model, most
        self.post, self.get = post or self.http_post, get or self.http_get

    def headers(self):
        return {'Authorization': f'Bearer {self.key}', 'Content-Type': 'application/json', 'User-Agent': 'xuji'}

    def http_get(self, url):
        with urllib.request.urlopen(urllib.request.Request(url, headers=self.headers()), timeout=30) as answer:
            return json.loads(answer.read())

    def http_post(self, url, body):
        request = urllib.request.Request(url, data=json.dumps(body).encode(), headers=self.headers(), method='POST')
        with urllib.request.urlopen(request, timeout=120) as answer:
            return json.loads(answer.read())

    def check(self):
        """the model is one xAI has, or a RuntimeError naming those it has"""
        have = sorted(m['id'] for m in self.get(f'{API}/models').get('data', []))
        if self.model not in have:
            raise RuntimeError(f'xAI has no model {self.model}; it has: {", ".join(have) or "none"}')

    def ask(self, quoted, question, messages=None, most=None):
        body = {'model': self.model, 'messages': messages or prompt(quoted, question), 'max_tokens': most or self.most}
        answer = self.post(f'{API}/chat/completions', body)
        return answer['choices'][0]['message']['content'].strip()


def main():
    import asyncio

    import discord  # only to run: the logic above is tested without it

    token, key = os.environ.get('XUJI_TOKEN', ''), os.environ.get('XAI_API_KEY', '')
    owner = int(os.environ.get('XUJI_OWNER', '0') or 0)
    if not (token and key and owner):
        sys.exit('xuji: XUJI_TOKEN, XAI_API_KEY and XUJI_OWNER are needed')
    grok = Grok(key, os.environ.get('XUJI_MODEL', 'grok-4.7'), int(os.environ.get('XUJI_MAX_TOKENS', '800')))
    handback_tokens = int(os.environ.get('XUJI_HANDBACK_TOKENS', '6000'))
    try:
        grok.check()
    except (RuntimeError, urllib.error.URLError) as error:
        sys.exit(f'xuji: {error}')
    intents = discord.Intents.none()
    intents.guilds = intents.guild_messages = intents.dm_messages = intents.message_content = True
    client = discord.Client(intents=intents, allowed_mentions=discord.AllowedMentions.none())

    @client.event
    async def on_ready():
        print(f'xuji: up as {client.user}, {grok.model}', flush=True)

    def find_channel(said, here):
        """the channel a handback names: by its mention, else by name, in the guild asked in when there is one"""
        if said.startswith('<#'):
            return client.get_channel(int(said[2:-1]))
        name = said.lstrip('#').lower()
        guilds = [here] if here else client.guilds
        found = [c for g in guilds for c in g.channels if c.name.lower() == name and hasattr(c, 'threads')]
        if len(found) > 1:
            raise ValueError(f'#{name} is a channel of {len(found)} servers: name it by its mention')
        return found[0] if found else None

    async def entries_of(channel, after, before):
        """the messages of the days, oldest first: a forum's from each of its threads that has any"""
        out = []

        async def read(source, thread):
            async for m in source.history(limit=MOST_MESSAGES, after=after, before=before, oldest_first=True):
                files = [a.filename for a in m.attachments]
                entry = {'thread': thread, 'author': m.author.id, 'bot': m.author.bot, 'when': m.created_at}
                out.append(dict(entry, text=m.content, files=files))

        if isinstance(channel, discord.ForumChannel):
            threads = list(channel.threads)
            async for t in channel.archived_threads(limit=None):
                threads.append(t)
            for t in sorted(threads, key=lambda t: t.created_at or after):
                if len(out) < MOST_MESSAGES and (t.created_at or after) < before:
                    await read(t, t.name)
        else:
            await read(channel, '')
        return out[:MOST_MESSAGES]

    async def handback(message, asked):
        said, start, end = asked[0], asked[1], asked[2]
        channel = find_channel(said, message.guild)
        if channel is None:
            await message.reply(f'Xuji: no channel {said} that I can read', mention_author=False)
            return
        after = datetime.datetime.combine(start, datetime.time.min, datetime.UTC)
        before = datetime.datetime.combine(end + datetime.timedelta(days=1), datetime.time.min, datetime.UTC)
        async with message.channel.typing():
            entries = await entries_of(channel, after, before)
            said_text, people = transcript(entries)
            if not said_text:
                await message.reply(
                    f'Xuji: #{channel.name} has no messages from {start} to {end}', mention_author=False
                )
                return
            messages = handback_prompt(said_text, channel.name, start, end, asked[3])
            answer = await asyncio.get_running_loop().run_in_executor(
                None, lambda: grok.ask('', '', messages, handback_tokens)
            )
        count = sum(1 for e in entries if not e['bot'])
        name, text = handback_file(answer, channel.name, start, end, count, people)
        await message.reply(
            f'Handback of #{channel.name}, {start} to {end}: {count} messages, {people} people.',
            file=discord.File(io.BytesIO(text.encode()), filename=name),
            mention_author=False,
        )
        print('xuji: handback written', flush=True)

    @client.event
    async def on_message(message):
        private = message.guild is None
        if not asked_by_owner(message.author.id, owner, client.user in message.mentions, private):
            return
        question = message.content.replace(f'<@{client.user.id}>', '').replace(f'<@!{client.user.id}>', '')
        try:
            asked = parse_handback(question, datetime.datetime.now(datetime.UTC).date())
        except ValueError as error:
            await message.reply(f'Xuji: {error}', mention_author=False)
            return
        try:
            if asked:
                await handback(message, asked)
                return
            quoted = ''
            if message.reference and message.reference.message_id:
                ref = message.reference.resolved
                if not isinstance(ref, discord.Message):
                    ref = await message.channel.fetch_message(message.reference.message_id)
                quoted = ref.content
            async with message.channel.typing():
                text = await asyncio.get_running_loop().run_in_executor(None, grok.ask, quoted, question)
            print('xuji: answered', flush=True)
        except Exception as error:  # noqa: BLE001 - the owner sees why; the log says what failed, not what was asked
            print(f'xuji: {type(error).__name__}', file=sys.stderr, flush=True)
            text = f'Xuji: it did not work ({type(error).__name__}: {str(error)[:200]})'
        for piece in pieces((text or '(no answer)').rstrip()):
            await message.reply(piece, mention_author=False)

    client.run(token, log_handler=None)


if __name__ == '__main__':
    main()
