#!/usr/bin/env python3
"""AogoWimsy: Aogo, the Claude Code session, on Discord as a bot of its own, every post approved by its owner first.

    aogowimsy.py [--env FILE]       FILE: ~/.config/aogowimsy/aogowimsy.env when not given; mode 600 or stricter

A local MCP server that Claude Code starts and talks to on stdin and stdout, a JSON-RPC 2.0 message a line: protocol
revision 2026-07-28 (each request carries its version in _meta; server/discover) and, through initialize, 2025-11-25
back to 2024-11-05. Its tools:

    discord_post         a draft in the owner's DMs with AogoWimsy, with a check mark and a cross: only the owner's
                         check mark posts it, as it is, pinging no one
    discord_post_status  the same wait, on a draft that came back still waiting
    discord_read         a channel's messages: the owner's, the listed users' and AogoWimsy's own; others only counted
    discord_wait         the first message after an id from the owner or a listed user

It speaks Discord's REST API v10 with urllib, no gateway. It posts only in the channels AOGOWIMSY_CHANNELS lists and
their threads, and reads those and the owner's DMs with it. The owner's check mark is the only way a text gets out:
no setting turns it off. Nothing is stored: a draft waiting is a message in the owner's DMs, read again at each look,
and stderr says a tool ran or failed, never what was said.
"""

import argparse
import json
import os
import re
import stat
import sys
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import NamedTuple

VERSION = '1.0.0'
SERVER_INFO = {'name': 'aogowimsy', 'title': 'AogoWimsy', 'version': VERSION}
API = 'https://discord.com/api/v10'
AGENT = f'DiscordBot (https://github.com/scarletnx/discord-bots, {VERSION})'
ENV = '~/.config/aogowimsy/aogowimsy.env'
KEYS = ('AOGOWIMSY_TOKEN', 'AOGOWIMSY_OWNER', 'AOGOWIMSY_USERS', 'AOGOWIMSY_CHANNELS')
LIMIT = 2000  # a Discord message's letters
POLL = 3.0  # seconds between two looks at a draft's reactions or at a channel
MOST_WAIT = 3600  # seconds a tool waits at most
PAGE = 100  # messages one GET gives at most
TRIES = 5  # a request answered 429 this many times in a row is an error
MOST_RETRY = 60.0  # seconds a 429 may ask to wait
YES, NO = '\u2705', '\u274c'
DM, THREADS, REPLY = 1, (10, 11, 12), 19  # channel types (a DM; the three threads) and a reply's message type
SNOWFLAKE = re.compile(r'[0-9]{17,20}')
TOKEN_LETTER = re.compile(r'[A-Za-z0-9._-]')
WAITING = re.compile(r'(waiting|sending|not sent): to ([0-9]{17,20})(?: replying to ([0-9]{17,20}))?')
SENT = re.compile(r'sent: ([0-9]{17,20}|@me)/([0-9]{17,20})/([0-9]{17,20})')
MODERN = ('2026-07-28',)  # versions that carry their version and the client's capabilities in each request's _meta
LEGACY = ('2025-11-25', '2025-06-18', '2025-03-26', '2024-11-05')  # versions that open with initialize
METHODS = ('initialize', 'ping', 'server/discover', 'tools/list', 'tools/call')
VERSION_KEY = 'io.modelcontextprotocol/protocolVersion'
CAPABILITIES_KEY = 'io.modelcontextprotocol/clientCapabilities'
SERVER_INFO_KEY = 'io.modelcontextprotocol/serverInfo'
TTL_MS = 3_600_000  # the tool list and the discovery answer change only with a new AogoWimsy


class Refused(Exception):
    """a value AogoWimsy does not take: the message names it and what it takes"""


class DiscordError(Exception):
    """Discord said no, or could not be reached: the message says what it answered"""


class Cancelled(Exception):
    """the client cancelled the request, or AogoWimsy is stopping: the request gets no answer"""


class Settings(NamedTuple):
    token: str
    owner: str
    users: frozenset
    channels: frozenset


def shown(value):
    """a value as an error names it: its JSON, cut at 60 letters"""
    text = json.dumps(value, ensure_ascii=False)
    return text if len(text) <= 60 else text[:57] + '...'


def snowflake(value, what):
    """value when it is a Discord id written as a string, else Refused naming it"""
    if not isinstance(value, str):
        raise Refused(f'{what} is {shown(value)}, not a Discord id: write it as a string of 17 to 20 digits')
    if not SNOWFLAKE.fullmatch(value) or int(value) >= 1 << 64:
        raise Refused(f'{what} is {shown(value)}, not a Discord id (17 to 20 digits)')
    return value


def whole(value, what, default, low, high):
    """value when it is a whole number from low to high, default when it is None, else Refused naming it"""
    if value is None:
        return default
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise Refused(f'{what} is {shown(value)}, not a whole number from {low} to {high}')
    return value


def read_settings(path):
    """the settings in an env file, or Refused naming what is wrong: its mode, a line, a key or a value"""
    try:
        mode = stat.S_IMODE(os.stat(path).st_mode)
        text = Path(path).read_text(encoding='utf-8')
    except OSError as error:
        raise Refused(
            f'{path} cannot be read ({error.strerror or error}): AogoWimsy reads its settings there'
        ) from None
    except UnicodeDecodeError as error:
        raise Refused(f'{path} is not UTF-8 text (byte {error.start})') from None
    if mode & 0o7177:
        raise Refused(
            f"{path} has mode {mode:03o}: other users could read the bot's token; it must be 600 or stricter "
            f'(chmod 600 {path})'
        )
    values, lines = {}, {}
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, equals, value = line.partition('=')
        key = key.strip()
        if not equals:
            raise Refused(f'{path} line {number} has no "=": each line is KEY=VALUE, a # comment or empty')
        if key not in KEYS:
            raise Refused(f'{path} line {number}: {key} is not a key AogoWimsy knows; it knows {", ".join(KEYS)}')
        if key in values:
            raise Refused(f'{path} line {number}: {key} again, set on line {lines[key]} already')
        values[key], lines[key] = value.strip(), number
    for key in KEYS:
        if not values.get(key):
            raise Refused(f'{path}: {key} is {"empty" if key in values else "missing"}, and AogoWimsy needs it')
    token = values['AOGOWIMSY_TOKEN']
    for place, letter in enumerate(token, 1):
        if not TOKEN_LETTER.fullmatch(letter):
            raise Refused(
                f'{path}: AOGOWIMSY_TOKEN has {letter!a} at letter {place}: a Discord token has only letters, '
                'digits, ".", "_" and "-"'
            )
    owner = snowflake(values['AOGOWIMSY_OWNER'], f'{path}: AOGOWIMSY_OWNER')
    users, channels = (id_list(path, key, values[key]) for key in ('AOGOWIMSY_USERS', 'AOGOWIMSY_CHANNELS'))
    return Settings(token, owner, users, channels)


def id_list(path, key, value):
    """the ids of an id,id value, or Refused naming the item that is not one"""
    items = [item.strip() for item in value.split(',')]
    for place, item in enumerate(items, 1):
        if not item:
            raise Refused(f'{path}: {key} has an empty item, number {place} of {len(items)}: write id,id')
        snowflake(item, f'{path}: {key} item {place}')
    return frozenset(items)


def by_id(ids):
    return ', '.join(sorted(ids, key=int))


def instructions(settings):
    """what the server tells the session about itself"""
    return (
        f'AogoWimsy is Aogo on Discord. Every post is a draft its owner (Discord user {settings.owner}) answers in '
        f'their DMs with AogoWimsy: {YES} posts it as it is, {NO} or no answer posts nothing. It posts in channels '
        f"{by_id(settings.channels)} and their threads; it reads those and the owner's DMs with it. Of what it "
        f"reads, only the owner's messages, those of users {by_id(settings.users)} and its own reach you; others' "
        'are only counted.'
    )


class Clock:
    """time as the tools wait on it: a wait ends early when its event is set"""

    def now(self):
        return time.monotonic()

    def wait(self, seconds, event=None):
        """True when event was set: the request was cancelled"""
        if event is None:
            time.sleep(seconds)
            return False
        return event.wait(seconds)


def answer_of(raw):
    """an HTTP error's body: its JSON, else its first 300 letters"""
    try:
        return json.loads(raw)
    except ValueError:
        return raw.decode('utf-8', 'replace').strip()[:300]


def form_errors(tree, at=''):
    """the fields and messages of an Invalid Form Body's errors"""
    if isinstance(tree, dict):
        for item in tree.get('_errors', []):
            yield f'{at or "body"}: {item.get("message") if isinstance(item, dict) else item}'
        for key, value in tree.items():
            if key != '_errors':
                yield from form_errors(value, f'{at}.{key}' if at else key)


def reason(said):
    """what Discord's error answer says"""
    if not isinstance(said, dict):
        return shown(said) if said else 'no body'
    text = f'{said.get("message", "no message")} (code {said.get("code", "none")})'
    fields = list(form_errors(said.get('errors')))
    return text + (': ' + '; '.join(fields) if fields else '')


class Discord:
    """Discord's REST API as the bot: a 429 waits its retry_after and tries again; any other failure is a
    DiscordError with Discord's status and message"""

    def __init__(self, token, clock, api=API):
        self.token, self.clock, self.api = token, clock, api

    def __call__(self, method, path, body=None, **query):
        url = self.api + path + ('?' + urllib.parse.urlencode(query) if query else '')
        headers = {'Authorization': f'Bot {self.token}', 'User-Agent': AGENT}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers['Content-Type'] = 'application/json'
        for attempt in range(1, TRIES + 1):
            request = urllib.request.Request(url, data=data, headers=headers, method=method)
            try:
                with urllib.request.urlopen(request, timeout=30) as answer:
                    raw = answer.read()
            except urllib.error.HTTPError as error:
                with error:
                    said = answer_of(error.read())
                if error.code != 429:
                    raise DiscordError(f'Discord answered {error.code} to {method} {path}: {reason(said)}') from None
                retry = said.get('retry_after') if isinstance(said, dict) else None
                retry = retry if isinstance(retry, (int, float)) else error.headers.get('Retry-After')
                try:
                    retry = max(0.0, float(retry))
                except (TypeError, ValueError):
                    raise DiscordError(f'Discord answered 429 to {method} {path} with no retry_after') from None
                if retry > MOST_RETRY:
                    raise DiscordError(
                        f'Discord answered 429 to {method} {path}, asking to wait {retry} s: more than the '
                        f'{MOST_RETRY:.0f} s AogoWimsy waits for one'
                    )
                if attempt == TRIES:
                    raise DiscordError(
                        f'Discord answered 429 to {method} {path} {TRIES} times, the last asking to wait {retry} s'
                    )
                self.clock.wait(retry)
                continue
            except OSError as error:  # URLError, a timeout, a reset
                why = getattr(error, 'reason', None) or error
                raise DiscordError(f'Discord could not be reached for {method} {path}: {why}') from None
            try:
                return json.loads(raw) if raw else None
            except ValueError:
                raise DiscordError(f'Discord answered {method} {path} with something that is not JSON') from None


def link(guild, channel, message):
    return f'https://discord.com/channels/{guild or "@me"}/{channel}/{message}'


def view(message):
    """a message as the session gets it"""
    author = message['author']
    reference = message.get('message_reference') or {}
    return {
        'id': message['id'],
        'author': author.get('global_name') or author.get('username'),
        'author_id': author['id'],
        'time': message.get('timestamp'),
        'text': message.get('content', ''),
        'reply_to': reference.get('message_id') if message.get('type') == REPLY else None,
        'attachments': [item.get('filename') for item in message.get('attachments', [])],
    }


def footer(kind, channel, reply_to):
    return f'{kind}: to {channel}' + (f' replying to {reply_to}' if reply_to else '')


def waiting_card(where, reply_to, note=''):
    """the card under a draft's text: where it goes, and how the owner answers"""
    channel, name = where['id'], where.get('name') or where['id']
    lines = [f'Channel: #{name} ({channel})']
    if reply_to:
        lines.append(f'Replying to: {link(where.get("guild_id"), channel, reply_to)}')
    lines.append(f'{YES} posts the text above as it is, pinging no one; {NO} drops it.')
    if note:
        lines.append(note)
    return {
        'title': f'Draft for #{name}',
        'description': '\n'.join(lines),
        'footer': {'text': footer('waiting', channel, reply_to)},
    }


def draft_state(message):
    """what a draft's card says: waiting, sending, sent or not sent, and where to; None for a message that is no
    draft"""
    embeds = message.get('embeds') or [{}]
    text = (embeds[0].get('footer') or {}).get('text', '')
    found = SENT.fullmatch(text)
    if found:
        guild, channel, posted = found.groups()
        return {
            'kind': 'sent',
            'channel': channel,
            'reply_to': None,
            'message': posted,
            'link': link(guild, channel, posted),
        }
    found = WAITING.fullmatch(text)
    if found:
        return {'kind': found.group(1), 'channel': found.group(2), 'reply_to': found.group(3)}
    return None


def take(args, tool, required, optional):
    """Refused when args name an argument the tool does not have, or lack one it needs"""
    known = required + optional
    for name in args:
        if name not in known:
            raise Refused(f'{tool} has no argument {shown(name)}; it takes {", ".join(known)}')
    for name in required:
        if args.get(name) is None:
            raise Refused(f'{tool} needs {name}')


def kind_of(value):
    return {bool: 'boolean', int: 'number', float: 'number', list: 'array', dict: 'object'}.get(type(value), 'value')


class AogoWimsy:
    """the tools: Discord with the settings' channels, people and owner"""

    def __init__(self, settings, discord, clock):
        self.settings, self.discord, self.clock = settings, discord, clock
        self.known = {}  # its own user id and the owner's DM channel, asked once
        self.guard = threading.Lock()
        self.drafts = {}  # a lock a draft: one look at a time, so no draft is posted twice

    def me(self):
        if 'me' not in self.known:
            self.known['me'] = self.discord('GET', '/users/@me')['id']
        return self.known['me']

    def dm(self):
        if 'dm' not in self.known:
            self.known['dm'] = self.discord('POST', '/users/@me/channels', {'recipient_id': self.settings.owner})['id']
        return self.known['dm']

    def people(self):
        return self.settings.users | {self.settings.owner}

    def where(self, channel, reading):
        """the channel AogoWimsy may post in, or read: listed, a thread of a listed one, or (reading) the owner's
        DMs; else Refused naming it and those it may"""
        listed = self.settings.channels
        if channel in listed:
            return self.discord('GET', f'/channels/{channel}')
        if reading and channel == self.dm():
            return {'id': channel, 'type': DM}
        may = f'AOGOWIMSY_CHANNELS lists {by_id(listed)}, and their threads'
        if reading:
            may += f", and the owner's DMs with AogoWimsy are {self.dm()}"
        verb = 'read' if reading else 'post in'
        try:
            found = self.discord('GET', f'/channels/{channel}')
        except DiscordError as error:
            raise Refused(f'channel {channel} is not one AogoWimsy may {verb}: {may}; and {error}') from None
        if found.get('type') in THREADS and found.get('parent_id') in listed:
            return found
        raise Refused(f'channel {channel} is not one AogoWimsy may {verb}: {may}')

    def call(self, name, args, cancelled, progress):
        tools = {
            'discord_post': self.post,
            'discord_post_status': self.post_status,
            'discord_read': self.read,
            'discord_wait': self.wait_for,
        }
        return tools[name](args, cancelled, progress)

    def post(self, args, cancelled, progress):
        take(args, 'discord_post', ('channel', 'text'), ('reply_to', 'wait'))
        channel = snowflake(args['channel'], 'channel')
        text = args['text']
        if not isinstance(text, str):
            raise Refused(f'text is a JSON {kind_of(text)}, not a string')
        if not text.strip():
            raise Refused(f'text is empty: Discord posts 1 to {LIMIT} letters')
        if len(text) > LIMIT:
            raise Refused(f'text has {len(text)} letters: a Discord message holds {LIMIT} at most')
        reply_to = None if args.get('reply_to') is None else snowflake(args['reply_to'], 'reply_to')
        wait = whole(args.get('wait'), 'wait', 300, 0, MOST_WAIT)
        where = self.where(channel, reading=False)
        if reply_to:
            self.discord('GET', f'/channels/{channel}/messages/{reply_to}')
        dm = self.dm()
        body = {'content': text, 'embeds': [waiting_card(where, reply_to)], 'allowed_mentions': {'parse': []}}
        draft = self.discord('POST', f'/channels/{dm}/messages', body)['id']
        for emoji in (YES, NO):
            self.discord('PUT', f'/channels/{dm}/messages/{draft}/reactions/{urllib.parse.quote(emoji)}/@me')
        return self.verdict(draft, wait, cancelled, progress)

    def post_status(self, args, cancelled, progress):
        take(args, 'discord_post_status', ('draft',), ('wait',))
        draft = snowflake(args['draft'], 'draft')
        return self.verdict(draft, whole(args.get('wait'), 'wait', 300, 0, MOST_WAIT), cancelled, progress)

    def lock(self, draft):
        with self.guard:
            return self.drafts.setdefault(draft, threading.Lock())

    def verdict(self, draft, wait, cancelled, progress):
        """the owner's answer to a draft, within wait seconds: sent, not sent, or still waiting"""
        dm, me = self.dm(), self.me()
        start = self.clock.now()
        while True:
            with self.lock(draft):
                message = self.discord('GET', f'/channels/{dm}/messages/{draft}')
                state = draft_state(message) if message['author']['id'] == me else None
                if state is None:
                    raise Refused(f"message {draft} in the owner's DMs ({dm}) is not a draft of AogoWimsy's")
                kind, channel = state['kind'], state['channel']
                if kind == 'sent':
                    return {
                        'status': 'sent',
                        'link': state['link'],
                        'message': state['message'],
                        'channel': channel,
                        'draft': draft,
                    }
                if kind == 'not sent':
                    return {'status': 'not sent', 'channel': channel, 'draft': draft}
                if kind == 'sending':
                    raise Refused(
                        f'draft {draft} says it is being posted to channel {channel}: another AogoWimsy is posting '
                        'it, or one stopped while it did; look in that channel, AogoWimsy posts no draft twice'
                    )
                yes, no = self.answer(dm, message)
                if no:
                    card = {'title': 'Not sent', 'description': f'{NO}: not sent to channel {channel}.'}
                    card['footer'] = {'text': footer('not sent', channel, state['reply_to'])}
                    self.edit(dm, draft, card)
                    return {'status': 'not sent', 'channel': channel, 'draft': draft}
                if yes:
                    return self.send(dm, message, state)
            waited = self.clock.now() - start
            if waited >= wait:
                return {'status': 'waiting', 'draft': draft, 'link': link(None, dm, draft), 'waited': round(waited)}
            if self.clock.wait(min(POLL, wait - waited), cancelled):
                raise Cancelled
            progress(self.clock.now() - start, wait, f"waiting for the owner's answer to draft {draft}")

    def answer(self, dm, message):
        """whether the owner reacted with the check mark, and with the cross: another person's reaction, or
        AogoWimsy's own, counts for nothing"""
        said = {}
        for reaction in message.get('reactions') or []:
            emoji = reaction.get('emoji') or {}
            if emoji.get('id') is None and emoji.get('name') in (YES, NO):
                others = reaction.get('count', 0) - (1 if reaction.get('me') else 0)
                if others > 0:
                    path = f'/channels/{dm}/messages/{message["id"]}/reactions/{urllib.parse.quote(emoji["name"])}'
                    users = self.discord('GET', path, limit=PAGE)
                    said[emoji['name']] = any(user.get('id') == self.settings.owner for user in users)
        return said.get(YES, False), said.get(NO, False)

    def edit(self, dm, draft, card):
        self.discord('PATCH', f'/channels/{dm}/messages/{draft}', {'embeds': [card]})

    def send(self, dm, message, state):
        """posts an approved draft's text where it goes, then says on the draft where it went"""
        draft, channel, reply_to = message['id'], state['channel'], state['reply_to']
        where = self.where(channel, reading=False)  # today's list, not the one of the day it was drafted
        name, guild = where.get('name') or channel, where.get('guild_id')
        sending = waiting_card(where, reply_to)
        sending.update(title=f'Sending to #{name}', footer={'text': footer('sending', channel, reply_to)})
        self.edit(dm, draft, sending)
        body = {'content': message['content'], 'allowed_mentions': {'parse': [], 'replied_user': False}}
        if reply_to:
            body['message_reference'] = {'message_id': reply_to, 'channel_id': channel, 'fail_if_not_exists': True}
        try:
            posted = self.discord('POST', f'/channels/{channel}/messages', body)['id']
        except DiscordError as error:
            try:
                self.edit(dm, draft, waiting_card(where, reply_to, f'Posting failed: {error}'))
            except DiscordError as second:
                raise DiscordError(f'{error}; and draft {draft} still says sending: {second}') from None
            raise
        where_to = link(guild, channel, posted)
        card = {'title': f'Sent to #{name}', 'description': f'Sent: {where_to}'}
        card['footer'] = {'text': f'sent: {guild or "@me"}/{channel}/{posted}'}
        try:
            self.edit(dm, draft, card)
        except DiscordError as error:
            raise DiscordError(f'posted as {where_to}, but draft {draft} still says sending: {error}') from None
        return {'status': 'sent', 'link': where_to, 'message': posted, 'channel': channel, 'draft': draft}

    def read(self, args, cancelled, progress):
        take(args, 'discord_read', ('channel',), ('after', 'limit'))
        channel = snowflake(args['channel'], 'channel')
        after = None if args.get('after') is None else snowflake(args['after'], 'after')
        limit = whole(args.get('limit'), 'limit', 50, 1, PAGE)
        self.where(channel, reading=True)
        query = {'limit': limit} | ({'after': after} if after else {})
        page = sorted(self.discord('GET', f'/channels/{channel}/messages', **query), key=lambda m: int(m['id']))
        heard = self.people() | {self.me()}
        kept = [view(m) for m in page if m['author']['id'] in heard]
        newest = page[-1]['id'] if page else after
        return {'channel': channel, 'messages': kept, 'left_out': len(page) - len(kept), 'after': newest}

    def wait_for(self, args, cancelled, progress):
        take(args, 'discord_wait', ('channel', 'after'), ('wait',))
        channel = snowflake(args['channel'], 'channel')
        after = snowflake(args['after'], 'after')
        wait = whole(args.get('wait'), 'wait', 600, 0, MOST_WAIT)
        self.where(channel, reading=True)
        people, me = self.people(), self.me()
        start, left_out = self.clock.now(), 0
        while True:
            page = self.discord('GET', f'/channels/{channel}/messages', after=after, limit=PAGE)
            for message in sorted(page, key=lambda m: int(m['id'])):
                after = message['id']
                if message['author']['id'] in people:
                    return {'channel': channel, 'message': view(message), 'left_out': left_out, 'after': after}
                if message['author']['id'] != me:
                    left_out += 1
            if len(page) == PAGE:
                continue  # more came after these: read on at once
            waited = self.clock.now() - start
            if waited >= wait:
                return {
                    'channel': channel,
                    'message': None,
                    'waited': round(waited),
                    'left_out': left_out,
                    'after': after,
                }
            if self.clock.wait(min(POLL, wait - waited), cancelled):
                raise Cancelled
            progress(self.clock.now() - start, wait, f'waiting for a message in channel {channel}')


ID = {'type': 'string', 'pattern': '^[0-9]{17,20}$'}
SECONDS = {'type': 'integer', 'minimum': 0, 'maximum': MOST_WAIT}
MESSAGE = (
    '{"id", "author" (their name), "author_id", "time" (ISO 8601), "text", "reply_to" (the id it answers, or null), '
    '"attachments" (file names)}'
)
TOOLS = [
    {
        'name': 'discord_post',
        'description': (
            "Post a message on Discord once the owner approves it. The draft goes to the owner's DMs with AogoWimsy "
            f"(the text, and under it where it goes) with {YES} and {NO}: only the owner's {YES} posts it, as it "
            "is, pinging no one; another person's reaction counts for nothing. It posts in the channels "
            'AOGOWIMSY_CHANNELS lists and their threads. Waits up to `wait` seconds for the answer, then returns JSON: '
            '{"status": "sent", "link", "message", "channel", "draft"} when approved; {"status": "not sent", '
            '"channel", "draft"} when refused; {"status": "waiting", "draft", "link", "waited"} when not answered '
            'yet: call discord_post_status with that draft to wait on. An error names the value refused and what '
            'is taken.'
        ),
        'inputSchema': {
            'type': 'object',
            'properties': {
                'channel': dict(ID, description='the channel or thread id to post in'),
                'text': {
                    'type': 'string',
                    'minLength': 1,
                    'maxLength': LIMIT,
                    'description': f"what to post, 1 to {LIMIT} letters of Discord's Markdown",
                },
                'reply_to': dict(ID, description="the id of that channel's message it answers"),
                'wait': dict(SECONDS, default=300, description="seconds to wait for the owner's answer"),
            },
            'required': ['channel', 'text'],
            'additionalProperties': False,
        },
    },
    {
        'name': 'discord_post_status',
        'description': (
            f"Wait on a draft discord_post returned still waiting: up to `wait` seconds (0: one look) for the owner's "
            f'{YES} or {NO}. Returns the JSON discord_post does. A draft already sent or refused answers that again: '
            'no draft is posted twice.'
        ),
        'inputSchema': {
            'type': 'object',
            'properties': {
                'draft': dict(ID, description='the draft id discord_post returned'),
                'wait': dict(SECONDS, default=300, description="seconds to wait for the owner's answer"),
            },
            'required': ['draft'],
            'additionalProperties': False,
        },
    },
    {
        'name': 'discord_read',
        'description': (
            "Read a channel's messages: the first `limit` after the id `after`, or the newest `limit` without it, "
            'oldest first. Returns JSON {"channel", "messages": [' + MESSAGE + '], "left_out", "after"}. Only the '
            "owner's messages, the listed users' and AogoWimsy's own are given; left_out counts the others, and "
            'nothing else of them is. "after" is the newest id read, to read on from. Channels: those '
            "AOGOWIMSY_CHANNELS lists, their threads, and the owner's DMs with AogoWimsy (the channel in each "
            "draft's link)."
        ),
        'inputSchema': {
            'type': 'object',
            'properties': {
                'channel': dict(ID, description='the channel, thread or DM id to read'),
                'after': dict(ID, description='read the messages after this message id'),
                'limit': {
                    'type': 'integer',
                    'minimum': 1,
                    'maximum': PAGE,
                    'default': 50,
                    'description': 'how many messages to read',
                },
            },
            'required': ['channel'],
            'additionalProperties': False,
        },
    },
    {
        'name': 'discord_wait',
        'description': (
            'Wait up to `wait` seconds for the first message after the id `after` from the owner or a listed user, '
            'looking every few seconds. Returns JSON {"channel", "message": ' + MESSAGE + ', "left_out", "after"}, '
            'or {"channel", "message": null, "waited", "left_out", "after"} when none came: wait again after that '
            '"after". AogoWimsy\'s own messages are passed over; left_out counts the other people\'s, and nothing '
            'else of them is given. Channels: as discord_read.'
        ),
        'inputSchema': {
            'type': 'object',
            'properties': {
                'channel': dict(ID, description='the channel, thread or DM id to watch'),
                'after': dict(ID, description='wait for a message after this message id'),
                'wait': dict(SECONDS, default=600, description='seconds to wait'),
            },
            'required': ['channel', 'after'],
            'additionalProperties': False,
        },
    },
]
TOOL_NAMES = tuple(tool['name'] for tool in TOOLS)


def failure(rid, code, message, data=None):
    """a JSON-RPC error answer; with no id when the request's could not be read"""
    error = {'code': code, 'message': message}
    if data is not None:
        error['data'] = data
    answer = {'jsonrpc': '2.0', 'error': error}
    if rid is not None:
        answer['id'] = rid
    return answer


def tool_result(value, error=False):
    text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False)
    return {'content': [{'type': 'text', 'text': text}], 'isError': error}


def valid_id(rid):
    return isinstance(rid, str) or (isinstance(rid, int) and not isinstance(rid, bool))


def log(text):
    print(f'aogowimsy: {text}', file=sys.stderr, flush=True)


class Server:
    """MCP's JSON-RPC: 2026-07-28 a request at a time, and 2025-11-25 back to 2024-11-05 after initialize"""

    def __init__(self, tools, instructions, write):
        self.tools, self.instructions, self.write = tools, instructions, write
        self.agreed = None  # the version an initialize agreed on
        self.running = {}  # a tools/call's id: the event that cancels it
        self.guard = threading.Lock()

    def begin(self, rid):
        with self.guard:
            return self.running.setdefault(rid, threading.Event())

    def end(self, rid):
        with self.guard:
            self.running.pop(rid, None)

    def stop(self):
        """every running request cancelled: the client closed stdin"""
        with self.guard:
            for event in self.running.values():
                event.set()

    def work(self, message, cancelled):
        """a tools/call in a thread of its own: its answer written unless it was cancelled"""
        try:
            answer = self.handle(message, cancelled)
            if answer is not None and not cancelled.is_set():
                self.write(answer)
        finally:
            self.end(message['id'])

    def notified(self, method, params):
        if method == 'notifications/cancelled':
            event = self.running.get(params.get('requestId'))
            if event:
                event.set()
        elif method != 'notifications/initialized':
            log(f'notification {shown(method)} does nothing here')

    def handle(self, message, cancelled=None):
        """the answer to one message, or None for a notification and a cancelled request"""
        if not isinstance(message, dict) or message.get('jsonrpc') != '2.0':
            return failure(None, -32600, 'Invalid Request: a JSON-RPC 2.0 message is an object with "jsonrpc": "2.0"')
        method, rid = message.get('method'), message.get('id')
        rid_ok = rid if valid_id(rid) else None
        if method is None:
            if 'result' in message or 'error' in message:
                log(f'an answer came for request {shown(rid)}: AogoWimsy sends no requests')
                return None
            return failure(rid_ok, -32600, 'Invalid Request: no method')
        if not isinstance(method, str):
            return failure(rid_ok, -32600, f'Invalid Request: method is {shown(method)}, not a string')
        params = message.get('params')
        params = {} if params is None else params
        if 'id' not in message:
            if isinstance(params, dict):
                self.notified(method, params)
            return None
        if rid_ok is None:
            return failure(None, -32600, f'Invalid Request: id is {shown(rid)}, not a string or a whole number')
        if not isinstance(params, dict):
            return failure(rid, -32602, f'Invalid params: params is a JSON {kind_of(params)}, not an object')
        if method not in METHODS:
            return failure(rid, -32601, f'Method not found: {shown(method)}; AogoWimsy answers {", ".join(METHODS)}')
        if method == 'initialize':
            return self.initialize(rid, params)
        meta = params.get('_meta') if isinstance(params.get('_meta'), dict) else {}
        version = meta.get(VERSION_KEY)
        if version is not None:
            if version not in MODERN + LEGACY:
                data = {'supported': list(MODERN + LEGACY), 'requested': version}
                return failure(rid, -32022, 'Unsupported protocol version', data)
            if version in MODERN and not isinstance(meta.get(CAPABILITIES_KEY), dict):
                return failure(rid, -32602, f'Invalid params: _meta has no {CAPABILITIES_KEY}, which {version} needs')
        elif self.agreed is None and method != 'ping':
            return failure(
                rid,
                -32602,
                f'Invalid params: {method} came with no _meta {VERSION_KEY} and before any initialize: AogoWimsy '
                f'answers {", ".join(MODERN)} with it, and {", ".join(LEGACY)} after initialize',
            )
        modern = version in MODERN
        if (method == 'ping' and modern) or (method == 'server/discover' and not modern):
            return failure(rid, -32601, f'Method not found: {method} is no method of {version or self.agreed}')
        if method == 'ping':
            result = {}
        elif method == 'server/discover':
            result = {'supportedVersions': list(MODERN + LEGACY), 'capabilities': {'tools': {}}}
            result['instructions'] = self.instructions
        elif method == 'tools/list':
            if params.get('cursor') is not None:
                return failure(rid, -32602, f'Invalid params: cursor {shown(params["cursor"])}: the tools are one page')
            result = {'tools': TOOLS}
        else:
            result = self.call(rid, params, meta, cancelled or threading.Event())
            if result is None or 'error' in result:
                return result
        if modern:
            result = dict(result, resultType='complete', _meta={SERVER_INFO_KEY: SERVER_INFO})
            if method in ('tools/list', 'server/discover'):
                result.update(ttlMs=TTL_MS, cacheScope='private')
        return {'jsonrpc': '2.0', 'id': rid, 'result': result}

    def initialize(self, rid, params):
        asked = params.get('protocolVersion')
        if not isinstance(asked, str):
            return failure(rid, -32602, f'Invalid params: protocolVersion is {shown(asked)}, not a version string')
        self.agreed = asked if asked in LEGACY else LEGACY[0]
        result = {
            'protocolVersion': self.agreed,
            'capabilities': {'tools': {}},
            'serverInfo': SERVER_INFO,
            'instructions': self.instructions,
        }
        return {'jsonrpc': '2.0', 'id': rid, 'result': result}

    def call(self, rid, params, meta, cancelled):
        """a tools/call's result: a protocol error for an unknown tool, a tool error for what the tool refused"""
        name, args = params.get('name'), params.get('arguments')
        args = {} if args is None else args
        if not isinstance(name, str) or not isinstance(args, dict):
            return failure(rid, -32602, 'Invalid params: tools/call takes a name (a string) and arguments (an object)')
        if name not in TOOL_NAMES:
            return failure(rid, -32602, f'Unknown tool: {shown(name)}; AogoWimsy has {", ".join(TOOL_NAMES)}')
        token = meta.get('progressToken')
        progress = self.progress(token, cancelled) if valid_id(token) else (lambda *_: None)
        try:
            value = self.tools.call(name, args, cancelled, progress)
        except Cancelled:
            log(f'{name}: cancelled')
            return None
        except (Refused, DiscordError) as error:
            log(f'{name} failed: {error}')
            return tool_result(str(error), error=True)
        except Exception as error:  # noqa: BLE001 - a bug: the session sees it, stderr has where
            log(f'{name} failed: {type(error).__name__}: {error}\n{traceback.format_exc()}')
            return tool_result(f'{type(error).__name__}: {error}', error=True)
        log(f'{name}: {value.get("status", "done")}')
        return tool_result(value)

    def progress(self, token, cancelled):
        """notifications/progress for a request that asked for them, the seconds waited always growing"""
        last = [-1.0]

        def send(done, total, text):
            done = round(done, 1)
            if cancelled.is_set() or done <= last[0]:
                return
            last[0] = done
            params = {'progressToken': token, 'progress': done, 'total': total, 'message': text}
            self.write({'jsonrpc': '2.0', 'method': 'notifications/progress', 'params': params})

        return send


def serve(server, reader, write, linger=40.0):
    """reads messages a line until stdin closes; each tools/call runs in a thread of its own"""
    workers = []
    while True:
        line = reader.readline()
        if not line:
            break
        if not line.strip():
            continue
        try:
            message = json.loads(line)
        except ValueError as error:  # UnicodeDecodeError is one
            write(failure(None, -32700, f'Parse error: {error}'))
            continue
        if isinstance(message, list):
            write(failure(None, -32600, 'Invalid Request: a batch; MCP takes one message a line'))
            continue
        if isinstance(message, dict) and message.get('method') == 'tools/call' and valid_id(message.get('id')):
            worker = threading.Thread(target=server.work, args=(message, server.begin(message['id'])), daemon=True)
            worker.start()
            workers = [w for w in workers if w.is_alive()] + [worker]
            continue
        answer = server.handle(message)
        if answer is not None:
            write(answer)
    server.stop()
    end = time.monotonic() + linger  # a post under way ends; a wait ends at once
    for worker in workers:
        worker.join(max(0.0, end - time.monotonic()))


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog='aogowimsy.py', description='AogoWimsy: an MCP server (stdio) for Discord, every post approved first'
    )
    parser.add_argument('--env', default=ENV, help=f'the env file of its settings (default: {ENV}, mode 600)')
    options = parser.parse_args(argv)
    path = os.path.expanduser(options.env)
    try:
        settings = read_settings(path)
    except Refused as error:
        sys.exit(f'aogowimsy: {error}')
    clock = Clock()
    tools = AogoWimsy(settings, Discord(settings.token, clock), clock)
    out, guard = sys.stdout.buffer, threading.Lock()

    def write(message):
        line = json.dumps(message, separators=(',', ':')).encode() + b'\n'
        with guard:
            try:
                out.write(line)
                out.flush()
            except OSError as error:
                log(f'stdout is closed: {error}')

    log(f'up: owner {settings.owner}, {len(settings.users)} users, {len(settings.channels)} channels')
    serve(Server(tools, instructions(settings), write), sys.stdin.buffer, write)


if __name__ == '__main__':
    main()
