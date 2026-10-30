#!/usr/bin/env python3
"""AogoWimsy: Aogo, the Claude Code session, on Discord as a bot of its own, every post approved by its owner first.

    aogowimsy.py [--env FILE]       FILE: ~/.config/aogowimsy/aogowimsy.env when not given; mode 600 or stricter

A local MCP server that Claude Code starts and talks to on stdin and stdout, a JSON-RPC 2.0 message a line, through
the initialize handshake (protocol 2025-11-25 back to 2024-11-05): Claude Code registers no channel on revision
2026-07-28, which cannot carry its messages. Its tools:

    discord_post         a draft in the owner's DMs with AogoWimsy, with a check mark and a cross: only the owner's
                         check mark posts it, as it is, pinging no one
    discord_post_status  the same wait, on a draft that came back still waiting
    discord_read         a channel's messages: the owner's, the listed users' and AogoWimsy's own; others only counted
    discord_wait         the first message after an id from the owner or a listed user, as the gateway hears it
    discord_attachments  a message's attachments saved to a folder of its own, for a message discord_read gives

It is also a Claude Code channel (experimental capability claude/channel): once the session is up it stays on
Discord's gateway, and pushes into the session, as notifications/claude/channel, each message addressed to AogoWimsy
(a mention, or a reply to one of its messages) from the owner or a listed user, their DMs with it, a stranger's
addressed message the owner let through with his WIMSY reaction, and only the name of a stranger who wrote in DMs.
The owner's reply that mentions AogoWimsy lets the message he replied to through too, and carries it whole.

Discord's REST API v10 (urllib) reads and posts; its gateway (a WebSocket of its own, ssl and socket) hears. It works
in the channels AOGOWIMSY_CHANNELS lists and their threads, or in every one Discord lets it see when it lists none,
and in the owner's DMs with it. The owner's check mark is the only way a text gets out: no setting turns it off.
Nothing is stored but the attachments discord_attachments is asked for, in <XDG_CACHE_HOME or ~/.cache>/aogowimsy/
<channel>/<message>/ (folders 700, files 600): a draft waiting is a message in the owner's DMs, read again at each look,
and stderr says what ran or failed, never what was said nor who said it.
"""

import argparse
import base64
import contextlib
import hashlib
import json
import os
import queue
import random
import re
import socket
import ssl
import stat
import sys
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path
from typing import NamedTuple

VERSION = '1.2.0'
SERVER_INFO = {'name': 'aogowimsy', 'title': 'AogoWimsy', 'version': VERSION}
API = 'https://discord.com/api/v10'
AGENT = f'DiscordBot (https://github.com/scarletnx/discord-bots, {VERSION})'
ENV = '~/.config/aogowimsy/aogowimsy.env'
NEEDED = ('AOGOWIMSY_TOKEN', 'AOGOWIMSY_OWNER', 'AOGOWIMSY_USERS')
KEYS = (*NEEDED, 'AOGOWIMSY_CHANNELS')
LIMIT = 2000  # a Discord message's letters
IMAGES = {'.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp'}
MOST_FILES = 4  # images a post carries; how big each may be is Discord's to say, in the owner's DMs first
CDN = ('https://cdn.discordapp.com/', 'https://media.discordapp.net/')  # where attachments are fetched from
MOST_SAVE = 64 << 20  # bytes of one attachment discord_attachments saves at most
POLL = 3.0  # seconds between two looks at a draft's reactions
LOOK = 1.0  # seconds a wait sleeps at most before looking again at what the gateway brought
MOST_WAIT = 3600  # seconds a tool waits at most
PAGE = 100  # messages one GET gives at most
TRIES = 5  # a request answered 429 this many times in a row is an error
MOST_RETRY = 60.0  # seconds a 429 may ask to wait
YES, NO = chr(0x2705), chr(0x274C)
# The owner's custom emoji (DraVee, 2026-10-21: "se alguem te citar, eu posso (somente eu) so responder com um
# emoji"): his reaction with it, and only his, lets one stranger's message addressed to AogoWimsy reach the session.
# Matched by its name; a constant, not a setting.
WIMSY = 'wimsyaogo'
DM, THREADS, REPLY = 1, (10, 11, 12), 19  # channel types (a DM; the three threads) and a reply's message type
SNOWFLAKE = re.compile(r'[0-9]{17,20}')
TOKEN_LETTER = re.compile(r'[A-Za-z0-9._-]')
WAITING = re.compile(r'(waiting|sending|not sent): to ([0-9]{17,20})(?: replying to ([0-9]{17,20}))?')
SENT = re.compile(r'sent: ([0-9]{17,20}|@me)/([0-9]{17,20})/([0-9]{17,20})')
VERSIONS = ('2025-11-25', '2025-06-18', '2025-03-26', '2024-11-05')  # the initialize handshake's
METHODS = ('initialize', 'ping', 'tools/list', 'tools/call')
CAPABILITIES = {'tools': {}, 'experimental': {'claude/channel': {}}}
CHANNEL_PUSH = 'notifications/claude/channel'
# The gateway's intents: GUILDS (1), GUILD_MESSAGES (512), GUILD_MESSAGE_REACTIONS (1024), DIRECT_MESSAGES (4096),
# DIRECT_MESSAGE_REACTIONS (8192: the owner's check mark on a draft, which is in his DMs), MESSAGE_CONTENT (32768,
# privileged: the Developer Portal's Message Content Intent)
INTENTS = 1 | 1 << 9 | 1 << 10 | 1 << 12 | 1 << 13 | 1 << 15
GATEWAY_STOPS = {  # close codes after which Discord takes no new try: AogoWimsy stops hearing and says why
    4004: "authentication failed: AOGOWIMSY_TOKEN is not the bot's token",
    4010: 'invalid shard',
    4011: 'sharding required',
    4012: 'invalid API version',
    4013: 'invalid intents',
    4014: 'disallowed intents: turn on Message Content Intent (Bot, Privileged Gateway Intents) in the Developer '
    'Portal',
}
GATEWAY_FRESH = (4007, 4009)  # close codes after which the session cannot resume: identify again
HELLO_WAIT = 30.0  # seconds the gateway has to say hello
STOP_LOOK = 0.5  # seconds the gateway reads at most before looking whether AogoWimsy stops
MOST_FRAME = 64 << 20  # bytes a gateway message may have
WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'  # RFC 6455's, for Sec-WebSocket-Accept


class Refused(Exception):
    """a value AogoWimsy does not take: the message names it and what it takes"""


class DiscordError(Exception):
    """Discord said no, or could not be reached: the message says what it answered"""

    def __init__(self, text, status=None):
        super().__init__(text)
        self.status = status


class Cancelled(Exception):
    """the client cancelled the request, or AogoWimsy is stopping: the request gets no answer"""


class Drop(Exception):
    """a gateway connection ended: AogoWimsy connects again"""


class Stopped(Exception):
    """the gateway cannot go on: Discord takes no new try"""


class Settings(NamedTuple):
    token: str
    owner: str
    users: frozenset
    channels: frozenset | None  # None: every channel Discord lets the bot see


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
    for key in NEEDED:
        if not values.get(key):
            raise Refused(f'{path}: {key} is {"empty" if key in values else "missing"}, and AogoWimsy needs it')
    if values.get('AOGOWIMSY_CHANNELS') == '':
        raise Refused(
            f'{path}: AOGOWIMSY_CHANNELS is empty: list id,id, or take the line out for every channel the bot can see'
        )
    token = values['AOGOWIMSY_TOKEN']
    for place, letter in enumerate(token, 1):
        if not TOKEN_LETTER.fullmatch(letter):
            raise Refused(
                f'{path}: AOGOWIMSY_TOKEN has {letter!a} at letter {place}: a Discord token has only letters, '
                'digits, ".", "_" and "-"'
            )
    owner = snowflake(values['AOGOWIMSY_OWNER'], f'{path}: AOGOWIMSY_OWNER')
    users = id_list(path, 'AOGOWIMSY_USERS', values['AOGOWIMSY_USERS'])
    channels = (
        id_list(path, 'AOGOWIMSY_CHANNELS', values['AOGOWIMSY_CHANNELS']) if 'AOGOWIMSY_CHANNELS' in values else None
    )
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


def scope(settings):
    """where AogoWimsy reads and posts, in words"""
    if settings.channels is None:
        return 'every channel and thread Discord lets it see (AOGOWIMSY_CHANNELS lists none)'
    return f'channels {by_id(settings.channels)} and their threads'


def instructions(settings):
    """what the server tells the session about itself"""
    return (
        f'AogoWimsy is Aogo on Discord. Every post is a draft its owner (Discord user {settings.owner}) answers in '
        f'their DMs with AogoWimsy: {YES} posts it as it is, {NO} or no answer posts nothing. It reads and posts in '
        f"{scope(settings)}, and reads the owner's DMs with it. Of what it reads, only the owner's messages, those "
        f'of users {by_id(settings.users)} and its own reach you; the others are counted, but for a message the '
        f'owner let through: one addressed to AogoWimsy (a mention, or a reply to one of its messages) that he put '
        f'his :{WIMSY}: reaction on, or any he replied to while mentioning AogoWimsy. While the session runs, '
        'Discord comes in as <channel> events: kind="message" (from the owner or a listed user: a message addressed '
        'to AogoWimsy, or their DM; when the owner replied to someone while mentioning AogoWimsy, the text ends with '
        'the message he replied to, and quoted_author and quoted_author_id name its author), kind="approved" (a '
        "stranger's addressed message the owner let through with the reaction), each with author, author_id, "
        'channel, message_id and reply_to; kind="dm" (someone else wrote to AogoWimsy in DMs: their name only), '
        'kind="draft" (the owner answered a draft in his DMs, and it was posted or refused at once: draft, status, '
        'channel, and message_id once posted), kind="error" or "notice" (the gateway). Answer in Discord with '
        f"discord_post, reply_to the message_id: it still waits for the owner's {YES}. A message's attachments (an "
        "event's attachments, or quoted_attachments of the message in reply_to) are yours to open with "
        'discord_attachments, which saves them and gives their paths.'
    )


class Clock:
    """time as the tools wait on it: a wait ends early when its event is set"""

    def now(self):
        return time.monotonic()

    def wait(self, seconds, event=None):
        """True when event was set during the wait"""
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

    def __call__(self, method, path, body=None, files=None, **query):
        """files: (name, bytes, type) sent with body as Discord's multipart form, files[n] each"""
        url = self.api + path + ('?' + urllib.parse.urlencode(query) if query else '')
        headers = {'Authorization': f'Bot {self.token}', 'User-Agent': AGENT}
        data = None
        if files:
            data, headers['Content-Type'] = multipart(body, files)
        elif body is not None:
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
                    raise DiscordError(
                        f'Discord answered {error.code} to {method} {path}: {reason(said)}', error.code
                    ) from None
                retry = said.get('retry_after') if isinstance(said, dict) else None
                retry = retry if isinstance(retry, (int, float)) else error.headers.get('Retry-After')
                try:
                    retry = max(0.0, float(retry))
                except (TypeError, ValueError):
                    raise DiscordError(f'Discord answered 429 to {method} {path} with no retry_after', 429) from None
                if retry > MOST_RETRY:
                    raise DiscordError(
                        f'Discord answered 429 to {method} {path}, asking to wait {retry} s: more than the '
                        f'{MOST_RETRY:.0f} s AogoWimsy waits for one',
                        429,
                    )
                if attempt == TRIES:
                    raise DiscordError(
                        f'Discord answered 429 to {method} {path} {TRIES} times, the last asking to wait {retry} s',
                        429,
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

    def download(self, url, most=None):
        """an attachment's bytes from Discord's CDN (or, in the tests, from where the API is), without the token;
        a DiscordError for one longer than most bytes"""
        api = urllib.parse.urlsplit(self.api)
        if not url.startswith(CDN) and not url.startswith(f'{api.scheme}://{api.netloc}/attachments/'):
            raise DiscordError(f'an attachment at {url}: AogoWimsy fetches them from {" or ".join(CDN)} alone')
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': AGENT}), timeout=60) as got:
                data = got.read() if most is None else got.read(most + 1)
                if most is not None and len(data) > most:
                    raise DiscordError(f"Discord's CDN gave more than {most} bytes for {url}")
                return data
        except urllib.error.HTTPError as error:
            error.close()
            raise DiscordError(f"Discord's CDN answered {error.code} to {url}", error.code) from None
        except OSError as error:
            raise DiscordError(
                f"Discord's CDN could not be reached for {url}: {getattr(error, 'reason', error)}"
            ) from None


def multipart(body, files):
    """a multipart/form-data body as Discord takes one: payload_json, then files[n]; and its Content-Type"""
    boundary = uuid.uuid4().hex
    parts = [
        f'--{boundary}\r\nContent-Disposition: form-data; name="payload_json"\r\n'
        'Content-Type: application/json\r\n\r\n'.encode()
        + json.dumps(body).encode()
        + b'\r\n'
    ]
    for place, (name, data, kind) in enumerate(files):
        quoted = name.replace('\\', '_').replace('"', '_').replace('\r', '_').replace('\n', '_')
        head = (
            f'--{boundary}\r\nContent-Disposition: form-data; name="files[{place}]"; filename="{quoted}"\r\n'
            f'Content-Type: {kind}\r\n\r\n'
        )
        parts.append(head.encode() + data + b'\r\n')
    parts.append(f'--{boundary}--\r\n'.encode())
    return b''.join(parts), f'multipart/form-data; boundary={boundary}'


def images(value):
    """the files discord_post was given, read: [(name, bytes, type)]; None for none; Refused naming what is wrong"""
    if value is None:
        return None
    kinds = ', '.join(kind[1:] for kind in IMAGES)
    if not isinstance(value, list):
        raise Refused(f'files is a JSON {kind_of(value)}, not a list of 1 to {MOST_FILES} absolute paths of images')
    if not value:
        raise Refused(f'files is empty: 1 to {MOST_FILES} absolute paths of images ({kinds})')
    if len(value) > MOST_FILES:
        raise Refused(f'files names {len(value)} files: a post carries 1 to {MOST_FILES} images')
    found = []
    for place, item in enumerate(value, 1):
        if not isinstance(item, str) or not os.path.isabs(item):
            raise Refused(f'files item {place} is {shown(item)}, not an absolute path')
        path = Path(item)
        kind = IMAGES.get(path.suffix.lower())
        if not kind:
            raise Refused(
                f'files item {place} is a {path.suffix or "file without a suffix"}: AogoWimsy posts images ({kinds})'
            )
        try:
            data = path.read_bytes()
        except OSError as error:
            raise Refused(f'files item {place} is {item}, which cannot be read: {error.strerror or error}') from None
        if not data:
            raise Refused(f'files item {place}, {item}, is empty')
        found.append((path.name, data, kind))
    return found


def saves_folder(environ=None):
    """where discord_attachments saves: XDG_CACHE_HOME when it is an absolute path (the XDG spec ignores any other),
    else ~/.cache; then the server's name"""
    base = (os.environ if environ is None else environ).get('XDG_CACHE_HOME') or ''
    root = Path(base) if os.path.isabs(base) else Path.home() / '.cache'
    return root / SERVER_INFO['name']


def file_name(name, fallback):
    """an attachment's name as a file of its message's folder: no slash, control letter or leading dot, at most 200
    bytes with its suffix kept; fallback when nothing is left"""
    name = re.sub(r'[\x00-\x1f\x7f/\\]', '_', name or '').lstrip('.')
    stem, suffix = os.path.splitext(name)
    if len(suffix.encode()) > 20:
        stem, suffix = name, ''
    while len((stem + suffix).encode()) > 200:
        stem = stem[:-1]
    return stem + suffix if stem else fallback


def link(guild, channel, message):
    return f'https://discord.com/channels/{guild or "@me"}/{channel}/{message}'


def view(message, approved=False):
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
        'approved': approved,
    }


def mentions_me(message, me, roles):
    """whether a message mentions AogoWimsy's user or its role: the one Discord makes for a bot, of its name, which
    @Aogo may pick as well (DraVee's of 2026-10-24 did)"""
    if any(user.get('id') == me for user in message.get('mentions') or []):
        return True
    return any(role in roles for role in message.get('mention_roles') or [])


def bot_roles(roles, me):
    """the ids of a server's roles Discord made for AogoWimsy"""
    return {role['id'] for role in roles if (role.get('tags') or {}).get('bot_id') == me}


def addressed(message, me, roles):
    """whether a message mentions AogoWimsy or replies to one of its messages"""
    if mentions_me(message, me, roles):
        return True
    answered = message.get('referenced_message') or {}
    return message.get('type') == REPLY and (answered.get('author') or {}).get('id') == me


def quoting(message, owner, me, roles):
    """the id of the message the owner replied to while mentioning AogoWimsy, which lets it through as his WIMSY
    does (DraVee, 2026-10-21: "e literalmente eu quotando outra pessoa"); None for any other message"""
    if message['author']['id'] != owner or message.get('type') != REPLY or not mentions_me(message, me, roles):
        return None
    return (message.get('message_reference') or {}).get('message_id')


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
    kinds = {bool: 'boolean', int: 'number', float: 'number', str: 'string', list: 'array', dict: 'object'}
    return kinds.get(type(value), 'value')


class Inbox:
    """the messages the gateway heard in one channel, for one wait; gap: some may have gone unheard"""

    def __init__(self, channel):
        self.channel, self.ready, self.guard = channel, threading.Event(), threading.Lock()
        self.pending, self.gap = [], True  # a new inbox reads what came before it, once

    def put(self, message):
        with self.guard:
            self.pending.append(message)
            self.ready.set()

    def lost(self):
        with self.guard:
            self.gap = True
            self.ready.set()

    def take(self):
        with self.guard:
            items, gap = self.pending, self.gap
            self.pending, self.gap = [], False
            self.ready.clear()
        return items, gap


class Hub:
    """the gateway's messages, handed to the waits listening on their channel"""

    def __init__(self):
        self.guard, self.inboxes, self.stopped = threading.Lock(), set(), None

    @contextlib.contextmanager
    def listening(self, channel):
        inbox = Inbox(channel)
        with self.guard:
            self.inboxes.add(inbox)
        try:
            yield inbox
        finally:
            with self.guard:
                self.inboxes.discard(inbox)

    def deliver(self, message):
        with self.guard:
            inboxes = [inbox for inbox in self.inboxes if inbox.channel == message.get('channel_id')]
        for inbox in inboxes:
            inbox.put(message)

    def gap(self):
        """a new gateway session: what came while none was up went unheard"""
        with self.guard:
            inboxes = list(self.inboxes)
        for inbox in inboxes:
            inbox.lost()

    def stop(self, why):
        self.stopped = why
        with self.guard:
            inboxes = list(self.inboxes)
        for inbox in inboxes:
            inbox.ready.set()


class AogoWimsy:
    """the tools: Discord with the settings' channels, people and owner"""

    def __init__(self, settings, discord, clock, hub=None, files=None):
        self.settings, self.discord, self.clock = settings, discord, clock
        self.hub = hub or Hub()
        self.files = Path(files) if files else saves_folder()  # where discord_attachments saves
        self.known = {}  # its own user id, its roles and the owner's DM channel, asked once
        self.approved = set()  # the messages the owner let through while the gateway heard: their ids, in memory only
        self.guard = threading.Lock()
        self.drafts = {}  # a lock a draft: one look at a time, so no draft is posted twice
        self.made = set()  # the drafts this AogoWimsy made: the gateway's answers act on these alone, as every
        # session started in the workspace runs one and they share no lock

    def me(self):
        if 'me' not in self.known:
            self.known['me'] = self.discord('GET', '/users/@me')['id']
        return self.known['me']

    def roles(self):
        """the roles Discord made for AogoWimsy, one a server: asked once, and again after the gateway says it joined
        a server"""
        if 'roles' not in self.known:
            me, found = self.me(), set()
            for guild in self.discord('GET', '/users/@me/guilds'):
                found |= bot_roles(self.discord('GET', f'/guilds/{guild["id"]}/roles'), me)
            self.known['roles'] = found
        return self.known['roles']

    def dm(self):
        if 'dm' not in self.known:
            self.known['dm'] = self.discord('POST', '/users/@me/channels', {'recipient_id': self.settings.owner})['id']
        return self.known['dm']

    def people(self):
        return self.settings.users | {self.settings.owner}

    def authors(self, value):
        """the ids `from` names, each the owner or a listed user; None when it names none"""
        if value is None:
            return None
        owner, users = self.settings.owner, by_id(self.settings.users)
        if not isinstance(value, list) or not value:
            raise Refused(
                f'from is {shown(value)}: a list of 1 or more ids, of the owner ({owner}) or listed users ({users})'
            )
        for place, item in enumerate(value, 1):
            snowflake(item, f'from item {place}')
            if item not in self.people():
                raise Refused(f'from item {place} is {item}, neither the owner ({owner}) nor a listed user ({users})')
        return frozenset(value)

    def where(self, channel, reading):
        """the channel AogoWimsy may post in, or read: listed, a thread of a listed one, any Discord lets it see when
        none is listed, or (reading) the owner's DMs; else Refused naming it and those it may"""
        listed = self.settings.channels
        if listed is None or channel in listed:
            return self.discord('GET', f'/channels/{channel}')  # Discord's 403 or 404 names it
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
            'discord_attachments': self.attachments,
        }
        return tools[name](args, cancelled, progress)

    def post(self, args, cancelled, progress):
        take(args, 'discord_post', ('channel', 'text'), ('reply_to', 'wait', 'files'))
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
        files = images(args.get('files'))
        where = self.where(channel, reading=False)
        if reply_to:
            self.discord('GET', f'/channels/{channel}/messages/{reply_to}')
        dm = self.dm()
        body = {'content': text, 'embeds': [waiting_card(where, reply_to)], 'allowed_mentions': {'parse': []}}
        if files:
            body['attachments'] = [{'id': place, 'filename': name} for place, (name, _, _) in enumerate(files)]
        draft = self.discord('POST', f'/channels/{dm}/messages', body, files)['id']
        self.made.add(draft)
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
                got = self.decide(dm, message, state)
                if got:
                    return got
            waited = self.clock.now() - start
            if waited >= wait:
                return {'status': 'waiting', 'draft': draft, 'link': link(None, dm, draft), 'waited': round(waited)}
            if self.clock.wait(min(POLL, wait - waited), cancelled):
                raise Cancelled
            progress(self.clock.now() - start, wait, f"waiting for the owner's answer to draft {draft}")

    def decide(self, dm, message, state):
        """a waiting draft, under its lock: refused or posted when the owner has answered, else None"""
        channel = state['channel']
        yes, no = self.answer(dm, message)
        if no:
            card = {'title': 'Not sent', 'description': f'{NO}: not sent to channel {channel}.'}
            card['footer'] = {'text': footer('not sent', channel, state['reply_to'])}
            self.edit(dm, message['id'], card)
            return {'status': 'not sent', 'channel': channel, 'draft': message['id']}
        if yes:
            return self.send(dm, message, state)
        return None

    def answered(self, draft):
        """the owner's answer the gateway brought, on a draft this AogoWimsy made and still waiting: posted or refused
        at once, whether a tool waits on it or not; None for any other message of his DMs (another session's draft
        waits for that session's look, or a discord_post_status)"""
        if draft not in self.made:
            return None
        dm, me = self.dm(), self.me()
        with self.lock(draft):
            message = self.discord('GET', f'/channels/{dm}/messages/{draft}')
            state = draft_state(message) if message['author']['id'] == me else None
            if state is None or state['kind'] != 'waiting':
                return None
            return self.decide(dm, message, state)

    def reactors(self, channel, message, emoji):
        """the ids of who reacted to a message with an emoji (a name, or name:id for a custom one)"""
        path = f'/channels/{channel}/messages/{message}/reactions/{urllib.parse.quote(emoji)}'
        return {user.get('id') for user in self.discord('GET', path, limit=PAGE)}

    def answer(self, dm, message):
        """whether the owner reacted with the check mark, and with the cross: another person's reaction, or
        AogoWimsy's own, counts for nothing"""
        said = {}
        for reaction in message.get('reactions') or []:
            emoji = reaction.get('emoji') or {}
            if emoji.get('id') is None and emoji.get('name') in (YES, NO):
                others = reaction.get('count', 0) - (1 if reaction.get('me') else 0)
                if others > 0:
                    said[emoji['name']] = self.settings.owner in self.reactors(dm, message['id'], emoji['name'])
        return said.get(YES, False), said.get(NO, False)

    def let_through(self, channel, message, me):
        """whether a stranger's message is addressed to AogoWimsy and carries the owner's WIMSY reaction"""
        if not addressed(message, me, self.roles()):
            return False
        for reaction in message.get('reactions') or []:
            emoji = reaction.get('emoji') or {}
            if emoji.get('name') == WIMSY and emoji.get('id'):
                return self.settings.owner in self.reactors(channel, message['id'], f'{WIMSY}:{emoji["id"]}')
        return False

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
        files = []
        try:
            for item in message.get('attachments') or []:  # the draft's own: what the owner saw is what goes
                kind = item.get('content_type') or 'application/octet-stream'
                files.append((item['filename'], self.discord.download(item['url']), kind))
            if files:
                body['attachments'] = [{'id': place, 'filename': name} for place, (name, _, _) in enumerate(files)]
            posted = self.discord('POST', f'/channels/{channel}/messages', body, files)['id']
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
        take(args, 'discord_read', ('channel',), ('after', 'limit', 'from'))
        channel = snowflake(args['channel'], 'channel')
        after = None if args.get('after') is None else snowflake(args['after'], 'after')
        limit = whole(args.get('limit'), 'limit', 50, 1, PAGE)
        named = self.authors(args.get('from'))
        self.where(channel, reading=True)
        query = {'limit': limit} | ({'after': after} if after else {})
        page = sorted(self.discord('GET', f'/channels/{channel}/messages', **query), key=lambda m: int(m['id']))
        me = self.me()
        heard = named if named is not None else self.people() | {me}
        roles = self.roles()
        quoted = self.approved | {quoting(m, self.settings.owner, me, roles) for m in page}
        kept = []
        for message in page:
            author = message['author']['id']
            if author in heard:
                kept.append(view(message))
            elif named is None and author != me and (message['id'] in quoted or self.let_through(channel, message, me)):
                kept.append(view(message, approved=True))
        newest = page[-1]['id'] if page else after
        return {'channel': channel, 'messages': kept, 'left_out': len(page) - len(kept), 'after': newest}

    def since(self, channel, after):
        """every message of a channel after an id, from Discord's REST, oldest first"""
        found = []
        while True:
            page = sorted(
                self.discord('GET', f'/channels/{channel}/messages', after=after, limit=PAGE),
                key=lambda m: int(m['id']),
            )
            found += page
            if len(page) < PAGE:
                return found
            after = page[-1]['id']

    def readable(self, channel, message):
        """Refused unless discord_read gives the message: the owner's, a listed user's, AogoWimsy's own, or one the owner
        let through (his WIMSY reaction, or his reply mentioning AogoWimsy among the PAGE messages after it); the
        refusal names no one"""
        me = self.me()
        if message['author']['id'] in self.people() | {me}:
            return
        if message['id'] in self.approved or self.let_through(channel, message, me):
            return
        later = self.discord('GET', f'/channels/{channel}/messages', after=message['id'], limit=PAGE)
        roles = self.roles()
        if message['id'] in {quoting(m, self.settings.owner, me, roles) for m in later}:
            return
        raise Refused(
            f"message {message['id']} is not one AogoWimsy gives: neither the owner's ({self.settings.owner}), a listed "
            f"user's ({by_id(self.settings.users)}) nor its own, and the owner has not let it through (his :{WIMSY}: "
            'reaction, or his reply to it mentioning AogoWimsy)'
        )

    def attachments(self, args, cancelled, progress):
        take(args, 'discord_attachments', ('channel', 'message'), ())
        channel = snowflake(args['channel'], 'channel')
        number = snowflake(args['message'], 'message')
        self.where(channel, reading=True)
        message = self.discord('GET', f'/channels/{channel}/messages/{number}')  # Discord's 404 names it
        self.readable(channel, message)
        found = message.get('attachments') or []
        if not found:
            raise Refused(f'message {number} in channel {channel} has no attachments')
        folder = self.files / channel / number
        self.files.parent.mkdir(parents=True, exist_ok=True)
        for level in (self.files, self.files / channel, folder):  # 700 each: other people's files
            level.mkdir(mode=0o700, exist_ok=True)
        saved, used = [], set()
        for item in found:
            if cancelled.is_set():
                raise Cancelled
            name = file_name(item.get('filename'), item['id'])
            if name in used:
                name = f'{item["id"]}-{name}'
            used.add(name)
            size = item.get('size')
            entry = {'name': item.get('filename'), 'type': item.get('content_type'), 'size': size}
            entry.update({key: item[key] for key in ('width', 'height') if item.get(key)})
            path = folder / name
            if isinstance(size, int) and size > MOST_SAVE:
                entry['error'] = f'{size} bytes, more than the {MOST_SAVE} AogoWimsy saves of one attachment'
            elif isinstance(size, int) and path.is_file() and path.stat().st_size == size:
                entry['path'] = str(path)  # saved by an earlier call: an attachment never changes
            else:
                try:
                    data = self.discord.download(item['url'], most=MOST_SAVE)
                except DiscordError as error:
                    entry['error'] = str(error)
                else:
                    part = folder / f'.{name}.part'
                    try:
                        with open(os.open(part, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), 'wb') as out:
                            out.write(data)
                        os.replace(part, path)
                    except OSError as error:
                        part.unlink(missing_ok=True)
                        entry['error'] = f'{path} could not be written: {error.strerror or error}'
                    else:
                        entry['path'], entry['size'] = str(path), len(data)
            saved.append(entry)
        return {'channel': channel, 'message': number, 'folder': str(folder), 'files': saved}

    def wait_for(self, args, cancelled, progress):
        take(args, 'discord_wait', ('channel', 'after'), ('wait', 'from'))
        channel = snowflake(args['channel'], 'channel')
        after = snowflake(args['after'], 'after')
        wait = whole(args.get('wait'), 'wait', 600, 0, MOST_WAIT)
        named = self.authors(args.get('from'))
        who = named if named is not None else self.people()
        self.where(channel, reading=True)
        me = self.me()
        start, left_out = self.clock.now(), 0
        with self.hub.listening(channel) as inbox:
            while True:
                if self.hub.stopped:
                    raise DiscordError(
                        f'the gateway stopped ({self.hub.stopped}): discord_wait hears nothing; discord_read reads'
                    )
                items, gap = inbox.take()
                if gap:
                    items = items + self.since(channel, after)
                for message in sorted({m['id']: m for m in items}.values(), key=lambda m: int(m['id'])):
                    if int(message['id']) <= int(after):
                        continue
                    after = message['id']
                    if message['author']['id'] in who:
                        return {'channel': channel, 'message': view(message), 'left_out': left_out, 'after': after}
                    if message['author']['id'] != me:
                        left_out += 1
                waited = self.clock.now() - start
                if waited >= wait:
                    return {
                        'channel': channel,
                        'message': None,
                        'waited': round(waited),
                        'left_out': left_out,
                        'after': after,
                    }
                self.clock.wait(min(LOOK, wait - waited), inbox.ready)
                if cancelled.is_set():
                    raise Cancelled
                progress(self.clock.now() - start, wait, f'waiting for a message in channel {channel}')


class WebSocket:
    """a WebSocket client (RFC 6455) on a socket of its own: text frames out, masked; text frames in; pings
    answered; no extensions. wss://, or ws:// only to 127.0.0.1"""

    def __init__(self, url, timeout=30.0):
        parts = urllib.parse.urlsplit(url)
        host = parts.hostname or ''
        if parts.scheme == 'wss':
            port, tls = parts.port or 443, True
        elif parts.scheme == 'ws' and host in ('127.0.0.1', 'localhost'):
            port, tls = parts.port or 80, False
        else:
            raise DiscordError(f'the gateway url {shown(url)} is not wss://')
        sock = socket.create_connection((host, port), timeout=timeout)
        try:
            if tls:
                sock = ssl.create_default_context().wrap_socket(sock, server_hostname=host)
            key = base64.b64encode(os.urandom(16)).decode()
            target = (parts.path or '/') + (f'?{parts.query}' if parts.query else '')
            sock.sendall(
                f'GET {target} HTTP/1.1\r\nHost: {parts.netloc}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nUser-Agent: {AGENT}\r\n\r\n'.encode()
            )
            head = bytearray()
            while b'\r\n\r\n' not in head:
                chunk = sock.recv(4096)
                if not chunk:
                    raise ConnectionError('the gateway closed the connection during the WebSocket handshake')
                head += chunk
                if len(head) > 65536:
                    raise ConnectionError('the gateway answered the WebSocket handshake with more than 64 KB of head')
            end = head.index(b'\r\n\r\n') + 4
            lines = head[:end].decode('latin-1').split('\r\n')
            status = lines[0].split(' ', 2)
            if len(status) < 2 or status[1] != '101':
                raise ConnectionError(f'the gateway answered {shown(lines[0])} to the WebSocket handshake, not 101')
            fields = {
                name.strip().lower(): value.strip() for name, _, value in (x.partition(':') for x in lines[1:] if x)
            }
            accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
            if fields.get('sec-websocket-accept') != accept:
                raise ConnectionError(
                    f'the gateway answered Sec-WebSocket-Accept {shown(fields.get("sec-websocket-accept"))}, '
                    f'not {accept}'
                )
        except BaseException:
            sock.close()
            raise
        self.sock, self.buffer, self.parts = sock, bytearray(head[end:]), None

    def frame(self, opcode, payload):
        """one frame, masked as a client's must be"""
        header = bytearray([0x80 | opcode])
        size = len(payload)
        if size < 126:
            header.append(0x80 | size)
        elif size < 1 << 16:
            header += bytes([0x80 | 126]) + size.to_bytes(2, 'big')
        else:
            header += bytes([0x80 | 127]) + size.to_bytes(8, 'big')
        mask = os.urandom(4)
        body = bytes(byte ^ mask[place % 4] for place, byte in enumerate(payload))
        self.sock.settimeout(30.0)
        self.sock.sendall(bytes(header) + mask + body)

    def send(self, text):
        self.frame(0x1, text.encode())

    def close(self, code):
        """says close with a code: 1000 ends the session; another lets it resume"""
        try:
            self.frame(0x8, code.to_bytes(2, 'big'))
        except OSError as error:
            log(f'gateway: the close frame {code} did not go out: {error}')

    def shut(self):
        self.sock.close()

    def parse(self):
        """the next whole frame in the buffer: (fin, opcode, payload), or None while it is not all there"""
        data = self.buffer
        if len(data) < 2:
            return None
        fin, opcode, size, at = bool(data[0] & 0x80), data[0] & 0x0F, data[1] & 0x7F, 2
        if data[1] & 0x80:
            raise ConnectionError('the gateway sent a masked frame, which a server never does')
        if size == 126:
            if len(data) < 4:
                return None
            size, at = int.from_bytes(data[2:4], 'big'), 4
        elif size == 127:
            if len(data) < 10:
                return None
            size, at = int.from_bytes(data[2:10], 'big'), 10
        if size > MOST_FRAME:
            raise ConnectionError(f'the gateway sent a frame of {size} bytes, more than the {MOST_FRAME} taken')
        if len(data) < at + size:
            return None
        payload = bytes(data[at : at + size])
        del data[: at + size]
        return fin, opcode, payload

    def receive(self, timeout):
        """('text', str), ('close', code, reason), or None when nothing whole came within timeout seconds"""
        deadline = time.monotonic() + timeout
        while True:
            frame = self.parse()
            if frame is None:
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.sock.settimeout(left)
                try:
                    chunk = self.sock.recv(65536)
                except TimeoutError:
                    return None
                if not chunk:
                    raise ConnectionError('the gateway closed the connection with no close frame')
                self.buffer += chunk
                continue
            fin, opcode, payload = frame
            if opcode == 0x9:
                self.frame(0xA, payload)
            elif opcode == 0x8:
                code = int.from_bytes(payload[:2], 'big') if len(payload) >= 2 else 1005
                return 'close', code, payload[2:].decode('utf-8', 'replace')
            elif opcode in (0x1, 0x0):
                if opcode == 0x1:
                    self.parts = []
                elif self.parts is None:
                    raise ConnectionError('the gateway sent a continuation frame with no message begun')
                self.parts.append(payload)
                if fin:
                    text, self.parts = b''.join(self.parts).decode('utf-8'), None
                    return 'text', text
            elif opcode != 0xA:
                raise ConnectionError(f'the gateway sent a frame of opcode {opcode}, which JSON over text never uses')


class Gateway:
    """Discord's gateway, v10, JSON, no compression: hello, identify or resume, heartbeats and their ACKs, reconnects
    with backoff; each event goes to hear(kind, data), and each change worth saying to report(kind, text)"""

    def __init__(self, token, discord, hear, report, stop, connect=WebSocket, rng=random.random, backoff=(1.0, 60.0)):
        self.token, self.discord, self.hear, self.report, self.stop = token, discord, hear, report, stop
        self.connect, self.rng, self.backoff = connect, rng, backoff
        self.session = None  # (session id, resume url), from READY
        self.seq = None
        self.ready = self.reported = False

    def identify(self):
        properties = {'os': sys.platform, 'browser': 'aogowimsy', 'device': 'aogowimsy'}
        return {'op': 2, 'd': {'token': self.token, 'intents': INTENTS, 'properties': properties}}

    def run(self):
        delay, failures = self.backoff[0], 0
        while not self.stop.is_set():
            try:
                self.connection()
                return  # AogoWimsy stops
            except Stopped as error:
                log(f'gateway stopped: {error}')
                self.report('stopped', str(error))
                return
            except Drop as error:
                why = str(error)
            except (OSError, DiscordError, ValueError) as error:  # ValueError: a message that is not JSON or UTF-8
                why = f'{type(error).__name__}: {error}'
            except Exception as error:  # noqa: BLE001 - a gateway message it did not expect: connect again, said
                log(traceback.format_exc())
                why = f'{type(error).__name__}: {error}'
            if self.ready:
                delay, failures = self.backoff[0], 0
            else:
                failures += 1
            pause = delay * (0.5 + self.rng())
            log(f'gateway: {why}; again in {pause:.1f} s')
            if failures == 3 and not self.reported:
                self.reported = True
                self.report('trouble', f'{why}; it tries again, {self.backoff[1]:.0f} s apart at most')
            delay = min(delay * 2, self.backoff[1])
            if self.stop.wait(pause):
                return

    def connection(self):
        """one connection: returns when AogoWimsy stops; Drop when it ends, Stopped when Discord takes no new try"""
        self.ready = False
        resuming = self.session is not None and self.seq is not None
        if resuming:
            url = self.session[1]
        else:
            try:
                url = self.discord('GET', '/gateway/bot')['url']
            except DiscordError as error:
                if error.status == 401:
                    raise Stopped(
                        "Discord answered 401 to GET /gateway/bot: AOGOWIMSY_TOKEN is not the bot's token"
                    ) from None
                raise
        ws = self.connect(url.rstrip('/') + '/?v=10&encoding=json')
        try:
            self.converse(ws, resuming)
        finally:
            ws.shut()

    def converse(self, ws, resuming):
        hello = ws.receive(HELLO_WAIT)
        if hello is None or hello[0] != 'text':
            raise Drop(
                f'the gateway said no hello in {HELLO_WAIT:.0f} s'
                if hello is None
                else f'the gateway closed with {hello[1]} before its hello'
            )
        hello = json.loads(hello[1])
        if hello.get('op') != 10:
            raise Drop(f'the gateway opened with op {shown(hello.get("op"))}, not 10 (hello)')
        interval = hello['d']['heartbeat_interval'] / 1000
        if resuming:
            ws.send(json.dumps({'op': 6, 'd': {'token': self.token, 'session_id': self.session[0], 'seq': self.seq}}))
        else:
            ws.send(json.dumps(self.identify()))
        beat_at, acked = time.monotonic() + interval * self.rng(), True
        while True:
            if self.stop.is_set():
                ws.close(1000)
                return
            got = ws.receive(max(0.0, min(beat_at - time.monotonic(), STOP_LOOK)))
            if got is not None:
                if got[0] == 'close':
                    self.closed(got[1], got[2])
                message = json.loads(got[1])
                op = message.get('op')
                if message.get('s') is not None:
                    self.seq = message['s']
                if op == 0:
                    self.dispatched(message.get('t'), message.get('d') or {})
                elif op == 1:
                    ws.send(json.dumps({'op': 1, 'd': self.seq}))
                elif op == 7:
                    ws.close(4000)
                    raise Drop('Discord asked to reconnect')
                elif op == 9:
                    if not message.get('d'):
                        self.session, self.seq = None, None
                    ws.close(4000)
                    raise Drop(
                        'Discord invalidated the session'
                        + ('; it resumes' if message.get('d') else '; it identifies again')
                    )
                elif op == 11:
                    acked = True
            if time.monotonic() >= beat_at:
                if not acked:
                    ws.close(4000)
                    raise Drop(f'no heartbeat ACK in {interval:.1f} s')
                ws.send(json.dumps({'op': 1, 'd': self.seq}))
                beat_at, acked = time.monotonic() + interval, False

    def closed(self, code, said):
        why = f'Discord closed the gateway with {code}' + (f' ({said})' if said else '')
        if code in GATEWAY_STOPS:
            raise Stopped(f'{why}: {GATEWAY_STOPS[code]}')
        if code in GATEWAY_FRESH:
            self.session, self.seq = None, None
        raise Drop(why)

    def dispatched(self, kind, data):
        if kind == 'READY':
            self.session = (data['session_id'], data['resume_gateway_url'])
        if kind in ('READY', 'RESUMED'):
            self.ready = True
            log(f'gateway: {kind.lower()}')
            if self.reported:
                self.reported = False
                self.report('back', 'AogoWimsy hears Discord again')
        self.hear(kind, data)


def safe_name(name):
    """a file name as one item of the attachments list"""
    return re.sub(r'[;\r\n]', '_', name or '')


class Ear:
    """what the gateway hears, sorted: every message goes to the waits on its channel; the session gets, pushed as a
    channel event, each message addressed to AogoWimsy from the owner or a listed user, their DMs, a stranger's
    addressed message the owner let through with his WIMSY reaction, and only the name of a stranger who wrote in
    DMs; the owner's reply that mentions AogoWimsy carries the message he replied to, and lets it through. Nothing
    for anyone else."""

    def __init__(self, tools, push, stop):
        self.tools, self.hub, self.push, self.stop = tools, tools.hub, push, stop
        self.queue = queue.Queue()
        self.kinds = {}  # a channel's id: its type and parent, from the gateway or asked once
        self.knocked = set()  # strangers whose DM was told

    def feed(self, kind, data):
        """from the gateway's thread: never waits on Discord's REST there"""
        self.queue.put((kind, data))

    def run(self):
        while not self.stop.is_set():
            try:
                kind, data = self.queue.get(timeout=STOP_LOOK)
            except queue.Empty:
                continue
            try:
                self.hear(kind, data)
            except Exception as error:  # noqa: BLE001 - a Discord error or a bug: said, and the ear goes on
                log(f'channel: a {kind} event failed: {type(error).__name__}: {error}')
                if not isinstance(error, DiscordError):
                    log(traceback.format_exc())
                self.push(
                    f'AogoWimsy could not sort a {kind} event: {type(error).__name__}: {error}', {'kind': 'error'}
                )

    def report(self, kind, text):
        """the gateway's news, to the session and the waits"""
        if kind == 'stopped':
            self.hub.stop(text)
            self.push(
                f'AogoWimsy stopped hearing Discord: {text}. discord_wait cannot hear; discord_read and discord_post '
                'work.',
                {'kind': 'error'},
            )
        elif kind == 'trouble':
            self.push(f"AogoWimsy cannot reach Discord's gateway: {text}", {'kind': 'error'})
        else:
            self.push(text, {'kind': 'notice'})

    def hear(self, kind, data):
        if kind == 'READY':
            self.tools.known['me'] = data['user']['id']
            self.hub.gap()
        elif kind == 'GUILD_CREATE':
            self.tools.known.pop('roles', None)  # a server joined, or seen again: its role may be new
            for channel in (data.get('channels') or []) + (data.get('threads') or []):
                self.kinds[channel['id']] = (channel.get('type'), channel.get('parent_id'))
        elif kind in ('CHANNEL_CREATE', 'CHANNEL_UPDATE', 'THREAD_CREATE', 'THREAD_UPDATE'):
            self.kinds[data['id']] = (data.get('type'), data.get('parent_id'))
        elif kind == 'MESSAGE_CREATE':
            self.hub.deliver(data)
            self.message(data)
        elif kind == 'MESSAGE_REACTION_ADD':
            self.reaction(data)

    def in_scope(self, channel):
        listed = self.tools.settings.channels
        if listed is None or channel in listed:
            return True
        if channel not in self.kinds:
            found = self.tools.discord('GET', f'/channels/{channel}')
            self.kinds[channel] = (found.get('type'), found.get('parent_id'))
        kind, parent = self.kinds[channel]
        return kind in THREADS and parent in listed

    def message(self, message):
        me, author = self.tools.me(), message['author']['id']
        if author == me:
            return
        people = self.tools.people()
        if not message.get('guild_id'):
            if author in people:
                self.tell(message, 'message')
            elif author not in self.knocked:
                self.knocked.add(author)
                name = message['author'].get('global_name') or message['author'].get('username') or ''
                text = f'{name} ({author}) wrote to AogoWimsy in DMs; nothing of what they wrote reaches the session'
                self.push(text, {'kind': 'dm', 'author': name, 'author_id': author})
                log('channel: pushed a dm event')
            return
        channel = message['channel_id']
        if author not in people:
            return
        roles = self.tools.roles()
        if addressed(message, me, roles) and self.in_scope(channel):
            quoted, number = None, quoting(message, self.tools.settings.owner, me, roles)
            if number:
                self.tools.approved.add(number)
                if 'referenced_message' in message:
                    quoted = message['referenced_message']  # None: it was deleted
                else:
                    quoted = self.tools.discord('GET', f'/channels/{channel}/messages/{number}')
            self.tell(message, 'message', quoted)

    def reaction(self, event):
        emoji = event.get('emoji') or {}
        if event.get('user_id') != self.tools.settings.owner:
            return
        if not event.get('guild_id'):
            if event['channel_id'] == self.tools.dm() and emoji.get('id') is None and emoji.get('name') in (YES, NO):
                self.drafted(event['message_id'])
            return
        if emoji.get('name') != WIMSY:
            return
        channel, number = event['channel_id'], event['message_id']
        if number in self.tools.approved:
            return
        message = self.tools.discord('GET', f'/channels/{channel}/messages/{number}')
        me, author = self.tools.me(), message['author']['id']
        if author == me or author in self.tools.people() or not addressed(message, me, self.tools.roles()):
            return
        if not self.in_scope(channel):
            return
        self.tools.approved.add(number)
        self.tell(dict(message, channel_id=channel), 'approved')

    def drafted(self, draft):
        """the owner's check mark or cross on a waiting draft: it is posted or refused now, not on a tool's next look,
        and the session is told"""
        got = self.tools.answered(draft)
        if got is None:
            return
        meta = {'kind': 'draft', 'draft': draft, 'status': got['status'], 'channel': got['channel']}
        if got['status'] == 'sent':
            meta['message_id'] = got['message']
            self.push(f'The owner approved draft {draft}: posted as {got["link"]}', meta)
        else:
            self.push(f'The owner refused draft {draft}: not posted to channel {got["channel"]}', meta)
        log(f'channel: draft {got["status"]}')

    def tell(self, message, kind, quoted=None):
        """a message pushed whole; with quoted, the message the owner replied to, whole too"""
        seen = view(message)
        meta = {
            'kind': kind,
            'author': seen['author'] or '',
            'author_id': seen['author_id'],
            'channel': message['channel_id'],
            'message_id': seen['id'],
        }
        if seen['reply_to']:
            meta['reply_to'] = seen['reply_to']
        if seen['attachments']:
            meta['attachments'] = '; '.join(safe_name(name) for name in seen['attachments'])
        text = seen['text'] or ('(no text: attachments only)' if seen['attachments'] else '(no text)')
        if quoted:
            them = view(quoted)
            meta.update(quoted_author=them['author'] or '', quoted_author_id=them['author_id'])
            if them['attachments']:
                meta['quoted_attachments'] = '; '.join(safe_name(name) for name in them['attachments'])
            said = them['text'] or ('(no text: attachments only)' if them['attachments'] else '(no text)')
            text += f'\n\nReplying to {them["author"]} ({them["author_id"]}), message {them["id"]}:\n{said}'
        self.push(text, meta)
        log(f'channel: pushed a {kind} event')


ID = {'type': 'string', 'pattern': '^[0-9]{17,20}$'}
SECONDS = {'type': 'integer', 'minimum': 0, 'maximum': MOST_WAIT}
FROM = {
    'type': 'array',
    'items': ID,
    'minItems': 1,
    'description': 'only these authors: ids of the owner or of listed users',
}
MESSAGE = (
    '{"id", "author" (their name), "author_id", "time" (ISO 8601), "text", "reply_to" (the id it answers, or null), '
    '"attachments" (file names: discord_attachments saves them), "approved" (true for a stranger\'s message the owner '
    'let through)}'
)
WHERE = (
    'Channels: those AOGOWIMSY_CHANNELS lists and their threads, or every one Discord lets the bot see when it lists '
    "none, and the owner's DMs with AogoWimsy (the channel in each draft's link)."
)
TOOLS = [
    {
        'name': 'discord_post',
        'description': (
            "Post a message on Discord once the owner approves it. The draft goes to the owner's DMs with AogoWimsy "
            f"(the text and its images, and under it where it goes) with {YES} and {NO}: only the owner's {YES} posts it, as it "
            "is, pinging no one; another person's reaction counts for nothing. It posts in the channels "
            'AOGOWIMSY_CHANNELS lists and their threads, or in any Discord lets the bot see when it lists none. '
            'Waits up to `wait` seconds for the answer, then returns JSON: {"status": "sent", "link", "message", '
            '"channel", "draft"} when approved; {"status": "not sent", "channel", "draft"} when refused; {"status": '
            '"waiting", "draft", "link", "waited"} when not answered yet: call discord_post_status with that draft '
            'to wait on. An error names the value refused and what is taken.'
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
                'files': {
                    'type': 'array',
                    'items': {'type': 'string'},
                    'minItems': 1,
                    'maxItems': MOST_FILES,
                    'description': (
                        f'absolute paths of 1 to {MOST_FILES} images ({", ".join(k[1:] for k in IMAGES)}) posted '
                        "under the text: the draft carries them, and the post sends the draft's"
                    ),
                },
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
            "owner's messages, the listed users' and AogoWimsy's own are given, or only those of the authors `from` "
            "names; and, without `from`, a stranger's message the owner let through: addressed to AogoWimsy with "
            f'his :{WIMSY}: reaction on it, or one he replied to mentioning AogoWimsy. left_out counts the others, '
            'and nothing else of them is given. "after" is the newest id read, to read on from. ' + WHERE
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
                'from': FROM,
            },
            'required': ['channel'],
            'additionalProperties': False,
        },
    },
    {
        'name': 'discord_wait',
        'description': (
            'Wait up to `wait` seconds for the first message after the id `after` from the owner or a listed user '
            '(or from the authors `from` names), as Discord\'s gateway brings it. Returns JSON {"channel", '
            '"message": ' + MESSAGE + ', "left_out", "after"}, or {"channel", "message": null, "waited", '
            '"left_out", "after"} when none came: wait again after that "after". AogoWimsy\'s own messages are '
            "passed over; left_out counts the other people's, and nothing else of them is given. " + WHERE
        ),
        'inputSchema': {
            'type': 'object',
            'properties': {
                'channel': dict(ID, description='the channel, thread or DM id to watch'),
                'after': dict(ID, description='wait for a message after this message id'),
                'wait': dict(SECONDS, default=600, description='seconds to wait'),
                'from': FROM,
            },
            'required': ['channel', 'after'],
            'additionalProperties': False,
        },
    },
    {
        'name': 'discord_attachments',
        'description': (
            "Save a message's attachments, each a file of the message's own folder, to open with the Read tool (an "
            "image, a log): a message discord_read gives (the owner's, a listed user's, AogoWimsy's own, or a "
            "stranger's the owner let through), fetched from Discord's CDN without the token. Returns JSON "
            '{"channel", "message", "folder", "files": [{"name" (as Discord has it), "type", "size", "width" and '
            '"height" (images), and "path" when saved or "error" saying why not}]}. One over '
            f'{MOST_SAVE >> 20} MiB is not saved, and says so. A file saved before is not fetched again. ' + WHERE
        ),
        'inputSchema': {
            'type': 'object',
            'properties': {
                'channel': dict(ID, description='the channel, thread or DM id the message is in'),
                'message': dict(ID, description='the message id'),
            },
            'required': ['channel', 'message'],
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


class LineWriter:
    """JSON-RPC messages on a stream, a whole line each: one lock for the answers, the progress and the channel
    pushes, so no two threads write into one line"""

    def __init__(self, stream):
        self.stream, self.guard = stream, threading.Lock()

    def __call__(self, message):
        line = json.dumps(message, separators=(',', ':')).encode() + b'\n'
        with self.guard:
            try:
                self.stream.write(line)
                self.stream.flush()
            except OSError as error:
                log(f'stdout is closed: {error}')


class Server:
    """MCP's JSON-RPC after the initialize handshake, 2025-11-25 back to 2024-11-05; once the client says it is
    initialized, listen(push) starts hearing Discord and returns its threads"""

    def __init__(self, tools, instructions, write, listen=None, stopping=None):
        self.tools, self.instructions, self.write, self.listen = tools, instructions, write, listen
        self.stopping = stopping or threading.Event()
        self.agreed = None  # the version an initialize agreed on
        self.running = {}  # a tools/call's id: the event that cancels it
        self.threads = []  # the ones listen started
        self.guard = threading.Lock()

    def begin(self, rid):
        with self.guard:
            return self.running.setdefault(rid, threading.Event())

    def end(self, rid):
        with self.guard:
            self.running.pop(rid, None)

    def stop(self):
        """every running request cancelled and the listening ended: the client closed stdin"""
        self.stopping.set()
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

    def push(self, content, meta):
        """a channel event into the session"""
        self.write({'jsonrpc': '2.0', 'method': CHANNEL_PUSH, 'params': {'content': content, 'meta': meta}})

    def notified(self, method, params):
        if method == 'notifications/cancelled':
            event = self.running.get(params.get('requestId'))
            if event:
                event.set()
        elif method == 'notifications/initialized':
            if self.agreed and self.listen and not self.threads and not self.stopping.is_set():
                self.threads = self.listen(self.push)
        else:
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
        if method == 'server/discover':
            return failure(
                rid,
                -32601,
                'Method not found: server/discover; AogoWimsy speaks the initialize handshake '
                f'({", ".join(VERSIONS)}), '
                'as Claude Code registers no channel on 2026-07-28',
            )
        if method not in METHODS:
            return failure(rid, -32601, f'Method not found: {shown(method)}; AogoWimsy answers {", ".join(METHODS)}')
        if method == 'initialize':
            return self.initialize(rid, params)
        if self.agreed is None and method != 'ping':
            return failure(
                rid, -32602, f'Invalid params: {method} came before initialize; AogoWimsy speaks {", ".join(VERSIONS)}'
            )
        if method == 'ping':
            result = {}
        elif method == 'tools/list':
            if params.get('cursor') is not None:
                return failure(rid, -32602, f'Invalid params: cursor {shown(params["cursor"])}: the tools are one page')
            result = {'tools': TOOLS}
        else:
            meta = params.get('_meta') if isinstance(params.get('_meta'), dict) else {}
            result = self.call(rid, params, meta, cancelled or threading.Event())
            if result is None or 'error' in result:
                return result
        return {'jsonrpc': '2.0', 'id': rid, 'result': result}

    def initialize(self, rid, params):
        asked = params.get('protocolVersion')
        if not isinstance(asked, str):
            return failure(rid, -32602, f'Invalid params: protocolVersion is {shown(asked)}, not a version string')
        self.agreed = asked if asked in VERSIONS else VERSIONS[0]
        result = {
            'protocolVersion': self.agreed,
            'capabilities': CAPABILITIES,
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
    end = time.monotonic() + linger  # a post under way ends; a wait and the gateway end at once
    for thread in workers + server.threads:
        thread.join(max(0.0, end - time.monotonic()))


def started(target):
    thread = threading.Thread(target=target, daemon=True)
    thread.start()
    return thread


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
    clock, stop = Clock(), threading.Event()
    discord = Discord(settings.token, clock)
    tools = AogoWimsy(settings, discord, clock)
    write = LineWriter(sys.stdout.buffer)

    def listen(push):
        ear = Ear(tools, push, stop)
        gateway = Gateway(settings.token, discord, ear.feed, ear.report, stop)
        return [started(ear.run), started(gateway.run)]

    where = 'every channel it sees' if settings.channels is None else f'{len(settings.channels)} channels'
    log(f'up: owner {settings.owner}, {len(settings.users)} users, {where}')
    serve(Server(tools, instructions(settings), write, listen, stop), sys.stdin.buffer, write)


if __name__ == '__main__':
    main()
