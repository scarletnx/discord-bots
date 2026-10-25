#!/usr/bin/env python3
"""aogowimsy.py without Discord or the network: its env file, MCP over stdio in a real subprocess, its tools against a
fake Discord REST served on 127.0.0.1 with a clock that only moves when a tool waits, what the gateway hears and
pushes to the session, and the gateway client itself against a fake WebSocket gateway on 127.0.0.1"""

import base64
import email.parser
import email.policy
import hashlib
import http.server
import json
import os
import queue
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import aogowimsy as aw

os.environ['no_proxy'] = os.environ['NO_PROXY'] = '127.0.0.1'  # the fakes, never through a proxy

TOKEN = 'MTAwMDAw.fake-token_for.tests'
PNG = b'\x89PNG\r\n\x1a\n a fake image'
BOT, OWNER, FRIEND, STRANGER, OTHER_PERSON = (f'10000000000000000{n:02d}' for n in (1, 2, 3, 4, 5))
GUILD = '200000000000000001'
BOT_ROLE, OTHER_BOT_ROLE, HELPERS_ROLE = (f'70000000000000000{n:02d}' for n in (1, 2, 3))
GENERAL, HELP, OTHER, THREAD, OTHER_THREAD, NEW_THREAD = (f'3000000000000000{n:02d}' for n in (1, 2, 3, 11, 12, 13))
DM, FRIEND_DM, STRANGER_DM = '400000000000000001', '400000000000000002', '400000000000000003'
WIMSY = f'{aw.WIMSY}:600000000000000001'  # the owner's custom emoji, as a reaction's path names it
OTHER_EMOJI = 'wimsyother:600000000000000002'
YES, NO = aw.YES, aw.NO
INITIALIZE = {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'test', 'version': '1'}}


class FakeDiscord:
    """Discord's REST routes AogoWimsy uses, over an in-memory guild: answers as Discord does, newest first"""

    ROUTES = (
        ('GET', r'/users/@me', 'me'),
        ('GET', r'/users/@me/guilds', 'guilds'),
        ('GET', r'/guilds/(\d+)/roles', 'guild_roles'),
        ('POST', r'/users/@me/channels', 'open_dm'),
        ('GET', r'/gateway/bot', 'gateway'),
        ('GET', r'/channels/(\d+)', 'channel'),
        ('GET', r'/channels/(\d+)/messages', 'history'),
        ('POST', r'/channels/(\d+)/messages', 'create'),
        ('GET', r'/channels/(\d+)/messages/(\d+)', 'message'),
        ('PATCH', r'/channels/(\d+)/messages/(\d+)', 'edit'),
        ('PUT', r'/channels/(\d+)/messages/(\d+)/reactions/([^/]+)/@me', 'react_me'),
        ('GET', r'/channels/(\d+)/messages/(\d+)/reactions/([^/]+)', 'reactors'),
    )

    def __init__(self):
        self.users = {
            BOT: {'id': BOT, 'username': 'aogowimsy', 'global_name': 'AogoWimsy', 'bot': True},
            OWNER: {'id': OWNER, 'username': 'dravee', 'global_name': 'DraVee'},
            FRIEND: {'id': FRIEND, 'username': 'friend', 'global_name': None},
            STRANGER: {'id': STRANGER, 'username': 'stranger', 'global_name': 'Stranger'},
            OTHER_PERSON: {'id': OTHER_PERSON, 'username': 'other', 'global_name': 'Other'},
        }
        guild = {'type': 0, 'guild_id': GUILD}
        self.channels = {
            GENERAL: dict(guild, id=GENERAL, name='general'),
            HELP: dict(guild, id=HELP, name='help'),
            OTHER: dict(guild, id=OTHER, name='other'),
            THREAD: dict(guild, id=THREAD, name='a-thread', type=11, parent_id=GENERAL),
            OTHER_THREAD: dict(guild, id=OTHER_THREAD, name='elsewhere', type=11, parent_id=OTHER),
            NEW_THREAD: dict(guild, id=NEW_THREAD, name='new-thread', type=11, parent_id=HELP),
            DM: {'id': DM, 'type': 1, 'recipients': [self.users[OWNER]]},
            FRIEND_DM: {'id': FRIEND_DM, 'type': 1, 'recipients': [self.users[FRIEND]]},
            STRANGER_DM: {'id': STRANGER_DM, 'type': 1, 'recipients': [self.users[STRANGER]]},
        }
        # the guild's roles: the one Discord made for AogoWimsy and another bot's, tagged with their bot, and a plain one
        self.server_roles = [
            {'id': GUILD, 'name': '@everyone', 'managed': False},
            {'id': BOT_ROLE, 'name': 'Aogo', 'managed': True, 'tags': {'bot_id': BOT}},
            {'id': OTHER_BOT_ROLE, 'name': 'ReiWimsy', 'managed': True, 'tags': {'bot_id': OTHER_PERSON}},
            {'id': HELPERS_ROLE, 'name': 'helpers', 'managed': False},
        ]
        self.messages = {channel: [] for channel in self.channels}  # oldest first
        self.reactions = {}  # a message id: {emoji: [user ids]}
        self.requests = []  # (method, path, query, body)
        self.limits = []  # (method, path) answered 429 once each
        self.fail = {}  # (method, path): (status, body), answered until removed
        self.gateway_url = 'ws://127.0.0.1:9/'
        self.origin = 'http://127.0.0.1:9'  # the fake's own address, where its attachments are served
        self.blobs = {}  # an attachment's path: its bytes, as the CDN serves them
        self.serial = 1_500_000_000_000_000_000
        self.lock = threading.Lock()

    def say(self, channel, author, content='', mentions=(), reply_to=None, **extra):
        """a message from author; its id"""
        self.serial += 1
        message = {'id': str(self.serial), 'channel_id': channel, 'author': author, 'content': content, 'type': 0}
        message.update(timestamp=f'2026-10-21T12:{len(self.messages[channel]) % 60:02d}:00.000000+00:00')
        message.update(attachments=[], embeds=[], mentions=list(mentions))
        if reply_to:
            message.update(type=19, message_reference={'message_id': reply_to, 'channel_id': channel})
        message.update(extra)
        self.messages[channel].append(message)
        return message['id']

    def find(self, channel, message):
        return next((m for m in self.messages.get(channel, []) if m['id'] == message), None)

    def react(self, message, emoji, user):
        self.reactions.setdefault(message, {}).setdefault(emoji, []).append(user)

    def last(self, channel):
        return self.messages[channel][-1]

    def footer(self, message):
        return message['embeds'][0]['footer']['text']

    def posts(self, method, path):
        return [r for r in self.requests if r[0] == method and r[1] == path]

    def user(self, who):
        return self.users.get(who, {'id': who, 'username': 'webhook'})

    def render(self, message, deep=True):
        shown = {k: v for k, v in message.items() if k not in ('author', 'mentions')}
        shown['author'] = self.user(message['author'])
        shown['mentions'] = [self.user(who) for who in message.get('mentions', [])]
        reference = message.get('message_reference')
        if deep and reference and message.get('type') == 19:
            answered = self.find(message['channel_id'], reference['message_id'])
            shown['referenced_message'] = self.render(answered, deep=False) if answered else None
        reactions = self.reactions.get(message['id'], {})
        if reactions:
            shown['reactions'] = []
            for emoji, who in reactions.items():
                name, _, number = emoji.partition(':')
                shown['reactions'].append(
                    {'count': len(who), 'me': BOT in who, 'emoji': {'id': number or None, 'name': name}}
                )
        return shown

    def event(self, channel, message):
        """a message as the gateway's MESSAGE_CREATE carries it"""
        shown = self.render(self.find(channel, message))
        if self.channels[channel].get('guild_id'):
            shown['guild_id'] = self.channels[channel]['guild_id']
        return shown

    def handle(self, method, path, query, body, headers):
        with self.lock:
            return self.route(method, path, query, body, headers)

    def route(self, method, path, query, body, headers):
        if headers.get('Authorization') != f'Bot {TOKEN}':
            return 401, {'message': '401: Unauthorized', 'code': 0}, {}
        if not (headers.get('User-Agent') or '').startswith('DiscordBot ('):
            return 400, {'message': 'no DiscordBot User-Agent', 'code': 0}, {}
        path = path.removeprefix('/api/v10')
        self.requests.append((method, path, query, body))
        if (method, path) in self.limits:
            self.limits.remove((method, path))
            return 429, {'message': 'You are being rate limited.', 'retry_after': 1.5, 'global': False}, {}
        if (method, path) in self.fail:
            status, said = self.fail[(method, path)]
            return status, said, {}
        for verb, pattern, name in self.ROUTES:
            found = re.fullmatch(pattern, path)
            if verb == method and found:
                return getattr(self, name)(query, body, *found.groups())
        return 404, {'message': '404: Not Found', 'code': 0}, {}

    def me(self, query, body):
        return 200, self.users[BOT], {}

    def guilds(self, query, body):
        return 200, [{'id': GUILD, 'name': 'ScarletNX'}], {}

    def guild_roles(self, query, body, guild):
        if guild != GUILD:
            return 404, {'message': 'Unknown Guild', 'code': 10004}, {}
        return 200, self.server_roles, {}

    def open_dm(self, query, body):
        if body != {'recipient_id': OWNER}:
            return 400, {'message': f'fake: DMs only with the owner, not {body}', 'code': 0}, {}
        return 200, self.channels[DM], {}

    def gateway(self, query, body):
        return 200, {'url': self.gateway_url, 'shards': 1}, {}

    def channel(self, query, body, channel):
        if channel not in self.channels:
            return 404, {'message': 'Unknown Channel', 'code': 10003}, {}
        return 200, self.channels[channel], {}

    def history(self, query, body, channel):
        if channel not in self.channels:
            return 404, {'message': 'Unknown Channel', 'code': 10003}, {}
        limit, kept = int(query.get('limit', 50)), self.messages[channel]
        if 'after' in query:
            chosen = [m for m in kept if int(m['id']) > int(query['after'])][:limit]
        else:
            chosen = kept[-limit:]
        return 200, [self.render(m) for m in reversed(chosen)], {}

    def create(self, query, body, channel):
        files = body.pop('_files', [])
        listed = body.get('attachments')
        if files and [item.get('id') for item in listed or []] != list(range(len(files))):
            return 400, {'code': 50035, 'message': 'Invalid Form Body', 'errors': {'attachments': {}}}, {}
        content = body.get('content', '')
        if len(content) > 2000:
            message = 'Must be 2000 or fewer in length.'
            errors = {'content': {'_errors': [{'code': 'BASE_TYPE_MAX_LENGTH', 'message': message}]}}
            return 400, {'code': 50035, 'errors': errors, 'message': 'Invalid Form Body'}, {}
        reference = body.get('message_reference')
        if reference and not self.find(channel, reference['message_id']):
            return 400, {'code': 50035, 'message': 'Invalid Form Body', 'errors': {}}, {}
        number = self.say(
            channel, BOT, content, reply_to=reference and reference['message_id'], embeds=body.get('embeds', [])
        )
        kept = []
        for place, (name, data, kind) in enumerate(files):
            path = f'/attachments/{channel}/{number}{place}/{urllib.parse.quote(name)}'
            self.blobs[path] = (data, kind)
            kept.append({'id': f'{number}{place}', 'filename': name, 'size': len(data), 'url': self.origin + path})
            kept[-1]['content_type'] = kind
        self.find(channel, number)['attachments'] = kept
        if reference:
            self.find(channel, number)['message_reference'] = reference
        return 200, self.render(self.find(channel, number)), {}

    def message(self, query, body, channel, message):
        found = self.find(channel, message)
        if not found:
            return 404, {'message': 'Unknown Message', 'code': 10008}, {}
        return 200, self.render(found), {}

    def edit(self, query, body, channel, message):
        found = self.find(channel, message)
        if not found or found['author'] != BOT:
            return 403, {'message': 'Cannot edit a message authored by another user', 'code': 50005}, {}
        found.update(body)
        return 200, self.render(found), {}

    def react_me(self, query, body, channel, message, emoji):
        self.react(message, emoji, BOT)
        return 204, None, {}

    def reactors(self, query, body, channel, message, emoji):
        return 200, [self.users[u] for u in self.reactions.get(message, {}).get(emoji, [])], {}


def form_body(kind, raw):
    """Discord's multipart form: payload_json's JSON, with its files[n] as _files [(name, bytes, type)]"""
    message = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(
        f'Content-Type: {kind}\r\n\r\n'.encode() + raw
    )
    body, files = None, []
    for part in message.iter_parts():
        name = part.get_param('name', header='content-disposition')
        if name == 'payload_json':
            body = json.loads(part.get_content())
        else:
            assert name == f'files[{len(files)}]', name
            files.append((part.get_filename(), part.get_payload(decode=True), part.get_content_type()))
    return dict(body, _files=files)


class Handler(http.server.BaseHTTPRequestHandler):
    def answer(self):
        parts = urllib.parse.urlsplit(self.path)
        if parts.path in self.server.fake.blobs:  # the CDN: no token
            data, kind = self.server.fake.blobs[parts.path]
            self.server.fake.requests.append(('CDN', parts.path, {}, self.headers.get('Authorization')))
            self.send_response(200)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        query = dict(urllib.parse.parse_qsl(parts.query))
        length = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(length) if length else b''
        kind = self.headers.get('Content-Type') or ''
        body = form_body(kind, raw) if kind.startswith('multipart/form-data') else json.loads(raw) if raw else None
        status, said, extra = self.server.fake.handle(
            self.command, urllib.parse.unquote(parts.path), query, body, self.headers
        )
        data = b'' if said is None else json.dumps(said).encode()
        self.send_response(status)
        for key, value in extra.items():
            self.send_header(key, value)
        if data:
            self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = answer

    def log_message(self, *args):
        pass


class FakeClock:
    """time that moves only when a tool waits; hooks run once their time comes"""

    def __init__(self):
        self.time, self.waits, self.hooks = 0.0, [], []

    def now(self):
        return self.time

    def at(self, when, hook):
        self.hooks.append((when, hook))

    def wait(self, seconds, event=None):
        self.waits.append(seconds)
        self.time += seconds
        for due in [h for h in self.hooks if h[0] <= self.time]:
            self.hooks.remove(due)
            due[1]()
        return bool(event and event.is_set())


def write_env(folder, text, mode=0o600, name='aogowimsy.env'):
    path = Path(folder) / name
    path.write_text(text)
    path.chmod(mode)
    return str(path)


GOOD_ENV = f"""# AogoWimsy
AOGOWIMSY_TOKEN={TOKEN}
AOGOWIMSY_OWNER={OWNER}

AOGOWIMSY_USERS = {FRIEND}
AOGOWIMSY_CHANNELS={GENERAL}, {HELP}
"""


class EnvFile(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)

    def env(self, text, mode=0o600):
        return write_env(self.folder.name, text, mode)

    def refused(self, text, mode=0o600):
        with self.assertRaises(aw.Refused) as caught:
            aw.read_settings(self.env(text, mode))
        self.assertNotIn(TOKEN, str(caught.exception))
        return str(caught.exception)

    def test_a_good_file_with_comments_blanks_and_spaces(self):
        settings = aw.read_settings(self.env(GOOD_ENV))
        self.assertEqual(settings, aw.Settings(TOKEN, OWNER, frozenset({FRIEND}), frozenset({GENERAL, HELP})))

    def test_without_channels_it_works_where_discord_lets_it_and_says_so(self):
        settings = aw.read_settings(self.env(GOOD_ENV.replace(f'AOGOWIMSY_CHANNELS={GENERAL}, {HELP}\n', '')))
        self.assertIsNone(settings.channels)
        self.assertIn(
            'every channel and thread Discord lets it see (AOGOWIMSY_CHANNELS lists none)', aw.instructions(settings)
        )
        listed = aw.instructions(settings._replace(channels=frozenset({GENERAL})))
        self.assertIn(f'channels {GENERAL} and their threads', listed)
        said = self.refused(GOOD_ENV.replace(f'{GENERAL}, {HELP}', ''))
        self.assertIn('AOGOWIMSY_CHANNELS is empty: list id,id, or take the line out', said)

    def test_mode_600_and_stricter_are_taken(self):
        for mode in (0o600, 0o400):
            self.assertEqual(aw.read_settings(self.env(GOOD_ENV, mode)).owner, OWNER)

    def test_a_mode_others_can_read_is_refused_naming_it(self):
        self.assertIn('has mode 644', self.refused(GOOD_ENV, 0o644))
        self.assertIn('has mode 640', self.refused(GOOD_ENV, 0o640))
        self.assertIn('has mode 700', self.refused(GOOD_ENV, 0o700))

    def test_an_unknown_key_stops_it_naming_the_key(self):
        said = self.refused(GOOD_ENV + 'AOGOWIMSY_CHANNEL=1\n')
        self.assertIn('line 7: AOGOWIMSY_CHANNEL is not a key AogoWimsy knows', said)
        self.assertIn('AOGOWIMSY_CHANNELS', said)

    def test_an_id_that_is_not_one_is_named(self):
        said = self.refused(GOOD_ENV.replace(f'= {FRIEND}', f'= {FRIEND},abc'))
        self.assertIn('AOGOWIMSY_USERS item 2 is "abc", not a Discord id (17 to 20 digits)', said)
        said = self.refused(GOOD_ENV.replace(f'_OWNER={OWNER}', '_OWNER=12345'))
        self.assertIn('AOGOWIMSY_OWNER is "12345", not a Discord id', said)
        said = self.refused(GOOD_ENV.replace(f'{GENERAL}, {HELP}', f'{GENERAL},,{HELP}'))
        self.assertIn('AOGOWIMSY_CHANNELS has an empty item, number 2 of 3', said)

    def test_an_empty_or_missing_needed_value_is_named(self):
        self.assertIn('AOGOWIMSY_TOKEN is empty', self.refused(GOOD_ENV.replace(TOKEN, '')))
        self.assertIn('AOGOWIMSY_USERS is missing', self.refused(GOOD_ENV.replace(f'AOGOWIMSY_USERS = {FRIEND}', '')))

    def test_a_key_twice_a_line_with_no_equals_and_a_bad_token_letter_never_show_the_token(self):
        said = self.refused(GOOD_ENV + f'AOGOWIMSY_OWNER={OWNER}\n')
        self.assertIn('line 7: AOGOWIMSY_OWNER again, set on line 3', said)
        self.assertIn('line 7 has no "="', self.refused(GOOD_ENV + TOKEN + '\n'))
        said = self.refused(GOOD_ENV.replace(TOKEN, f'"{TOKEN}"'))
        self.assertIn("AOGOWIMSY_TOKEN has '\"' at letter 1", said)

    def test_the_example_lists_the_keys_it_knows_and_stops_at_the_first_empty_one(self):
        example = self.env((HERE / 'aogowimsy.env.example').read_text())
        with self.assertRaisesRegex(aw.Refused, 'AOGOWIMSY_TOKEN is empty, and AogoWimsy needs it'):
            aw.read_settings(example)

    def test_a_missing_file_says_so(self):
        with self.assertRaisesRegex(aw.Refused, 'nowhere.env cannot be read'):
            aw.read_settings(str(Path(self.folder.name) / 'nowhere.env'))


class WithDiscord(unittest.TestCase):
    """the tools through the server's tools/call, against the fake Discord over HTTP on 127.0.0.1"""

    @classmethod
    def setUpClass(cls):
        cls.http = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=cls.http.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.http.shutdown()
        cls.http.server_close()

    def setUp(self):
        self.fake = self.http.fake = FakeDiscord()
        self.fake.origin = f'http://127.0.0.1:{self.http.server_port}'
        self.clock = FakeClock()
        self.settings = aw.Settings(TOKEN, OWNER, frozenset({FRIEND}), frozenset({GENERAL, HELP}))
        self.api = f'http://127.0.0.1:{self.http.server_port}/api/v10'
        self.bot = aw.AogoWimsy(self.settings, aw.Discord(TOKEN, self.clock, self.api), self.clock)
        self.sent = []
        self.server = self.started_server(self.bot)
        self.logs = []
        self.addCleanup(setattr, aw, 'log', aw.log)
        aw.log = self.logs.append

    def started_server(self, bot, **extra):
        server = aw.Server(bot, 'test', self.sent.append, **extra)
        server.handle({'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': INITIALIZE})
        return server

    def answer(self, name, meta=None, cancelled=None, **args):
        params = {'name': name, 'arguments': args}
        if meta:
            params['_meta'] = meta
        return self.server.handle({'jsonrpc': '2.0', 'id': 7, 'method': 'tools/call', 'params': params}, cancelled)

    def tool(self, name, **args):
        """the tool's JSON when it worked; fails the test with its error when not"""
        result = self.answer(name, **args)['result']
        self.assertFalse(result['isError'], result['content'][0]['text'])
        return json.loads(result['content'][0]['text'])

    def error(self, name, **args):
        """the tool's error text; fails the test when it worked"""
        result = self.answer(name, **args)['result']
        self.assertTrue(result['isError'], result['content'][0]['text'])
        return result['content'][0]['text']

    def owner_answers(self, when, emoji, who=OWNER):
        self.clock.at(when, lambda: self.fake.react(self.fake.last(DM)['id'], emoji, who))

    def arrive(self, channel, author, content='', **extra):
        """a message Discord keeps and the gateway brings to the waits"""
        number = self.fake.say(channel, author, content, **extra)
        self.bot.hub.deliver(self.fake.event(channel, number))
        return number


class Post(WithDiscord):
    def test_approved_posts_the_text_as_it_is_pinging_no_one_and_says_so_on_the_draft(self):
        text = 'Built: <@1000000000000000004> @everyone **done**\nsecond line'
        self.owner_answers(6, YES)
        got = self.tool('discord_post', channel=GENERAL, text=text)
        posted = self.fake.last(GENERAL)
        self.assertEqual(got['status'], 'sent')
        self.assertEqual((got['message'], got['channel']), (posted['id'], GENERAL))
        self.assertEqual(got['link'], f'https://discord.com/channels/{GUILD}/{GENERAL}/{posted["id"]}')
        self.assertEqual(posted['content'], text)
        body = self.fake.posts('POST', f'/channels/{GENERAL}/messages')[0][3]
        self.assertEqual(body['allowed_mentions'], {'parse': [], 'replied_user': False})
        self.assertNotIn('message_reference', body)
        draft = self.fake.last(DM)
        self.assertEqual(got['draft'], draft['id'])
        self.assertEqual(draft['content'], text)
        self.assertEqual(self.fake.reactions[draft['id']][YES][0], BOT)
        self.assertEqual(self.fake.reactions[draft['id']][NO], [BOT])
        self.assertEqual(self.fake.footer(draft), f'sent: {GUILD}/{GENERAL}/{posted["id"]}')
        self.assertIn(got['link'], draft['embeds'][0]['description'])
        self.assertEqual(self.clock.waits, [3.0, 3.0])
        self.assertEqual(self.logs, ['discord_post: sent'])

    def test_the_draft_says_where_it_goes_and_the_message_it_answers_as_a_link(self):
        asked = self.fake.say(GENERAL, FRIEND, 'does it build?')
        self.owner_answers(3, YES)
        got = self.tool('discord_post', channel=GENERAL, text='it does', reply_to=asked)
        card = self.fake.posts('POST', f'/channels/{DM}/messages')[0][3]['embeds'][0]
        self.assertIn(f'Channel: #general ({GENERAL})', card['description'])
        self.assertIn(f'Replying to: https://discord.com/channels/{GUILD}/{GENERAL}/{asked}', card['description'])
        self.assertEqual(card['footer']['text'], f'waiting: to {GENERAL} replying to {asked}')
        self.assertEqual(self.fake.posts('POST', f'/channels/{DM}/messages')[0][3]['allowed_mentions'], {'parse': []})
        body = self.fake.posts('POST', f'/channels/{GENERAL}/messages')[0][3]
        reference = {'message_id': asked, 'channel_id': GENERAL, 'fail_if_not_exists': True}
        self.assertEqual(body['message_reference'], reference)
        self.assertEqual(got['status'], 'sent')

    def test_refused_posts_nothing_and_says_so_on_the_draft(self):
        self.owner_answers(3, NO)
        got = self.tool('discord_post', channel=GENERAL, text='hello')
        self.assertEqual(got, {'status': 'not sent', 'channel': GENERAL, 'draft': self.fake.last(DM)['id']})
        self.assertEqual(self.fake.messages[GENERAL], [])
        self.assertEqual(self.fake.footer(self.fake.last(DM)), f'not sent: to {GENERAL}')

    def test_both_answers_post_nothing(self):
        self.owner_answers(3, YES)
        self.owner_answers(3, NO)
        self.assertEqual(self.tool('discord_post', channel=GENERAL, text='hello')['status'], 'not sent')
        self.assertEqual(self.fake.messages[GENERAL], [])

    def test_timed_out_then_approved_through_post_status_and_never_posted_twice(self):
        got = self.tool('discord_post', channel=GENERAL, text='hello', wait=7)
        draft = self.fake.last(DM)['id']
        link = f'https://discord.com/channels/@me/{DM}/{draft}'
        self.assertEqual(got, {'status': 'waiting', 'draft': draft, 'link': link, 'waited': 7})
        self.assertEqual(self.clock.waits, [3.0, 3.0, 1.0])
        self.assertEqual(self.fake.messages[GENERAL], [])
        self.assertEqual(self.tool('discord_post_status', draft=draft, wait=0)['status'], 'waiting')
        self.fake.react(draft, YES, OWNER)
        sent = self.tool('discord_post_status', draft=draft)
        self.assertEqual((sent['status'], sent['draft']), ('sent', draft))
        again = self.tool('discord_post_status', draft=draft)
        self.assertEqual(again, sent)
        self.assertEqual(len(self.fake.messages[GENERAL]), 1)

    def image(self, name, data=PNG):
        folder = tempfile.TemporaryDirectory(prefix='aogowimsy-')
        self.addCleanup(folder.cleanup)
        path = Path(folder.name) / name
        path.write_bytes(data)
        return str(path)

    def test_images_go_with_the_draft_and_with_the_post(self):
        shots = [self.image('menu before-after.png'), self.image('b.JPG', b'\xff\xd8\xff jpeg')]
        self.owner_answers(3, YES)
        got = self.tool('discord_post', channel=GENERAL, text='the menu', files=shots)
        draft, posted = self.fake.last(DM), self.fake.last(GENERAL)
        self.assertEqual(got['status'], 'sent')
        for message in (draft, posted):
            names = [(a['filename'], a['content_type']) for a in message['attachments']]
            self.assertEqual(names, [('menu before-after.png', 'image/png'), ('b.JPG', 'image/jpeg')])
        self.assertEqual(self.fake.blobs[urllib.parse.urlsplit(posted['attachments'][0]['url']).path][0], PNG)
        self.assertEqual(posted['content'], 'the menu')
        # the post's images come from the draft, fetched from the CDN without the token
        fetched = [r for r in self.fake.requests if r[0] == 'CDN']
        self.assertEqual(
            [(r[1].rsplit('/', 1)[1], r[3]) for r in fetched], [('menu%20before-after.png', None), ('b.JPG', None)]
        )

    def test_a_restart_keeps_the_drafts_images(self):
        draft = self.tool('discord_post', channel=GENERAL, text='later', files=[self.image('a.webp', b'RIFF webp')])
        fresh = aw.AogoWimsy(self.settings, aw.Discord(TOKEN, self.clock, self.api), self.clock)
        self.server = self.started_server(fresh)
        self.fake.react(draft['draft'], YES, OWNER)
        self.assertEqual(self.tool('discord_post_status', draft=draft['draft'])['status'], 'sent')
        self.assertEqual([a['filename'] for a in self.fake.last(GENERAL)['attachments']], ['a.webp'])

    def test_files_are_checked_before_discord_hears_of_them(self):
        png = self.image('a.png')
        cases = (
            ('a.png', 'files is a JSON string, not a list of 1 to 4 absolute paths of images'),
            ([], 'files is empty: 1 to 4 absolute paths of images (png, jpg, jpeg, gif, webp)'),
            ([png] * 5, 'files names 5 files: a post carries 1 to 4 images'),
            (['a.png'], 'files item 1 is "a.png", not an absolute path'),
            ([png + '.gone.png'], f'files item 1 is {png}.gone.png, which cannot be read: '),
            ([self.image('log.txt')], 'files item 1 is a .txt: AogoWimsy posts images (png, jpg, jpeg, gif, webp)'),
            ([self.image('empty.png', b'')], 'files item 1, '),
        )
        for files, said in cases:
            with self.subTest(files=files):
                self.assertIn(said, self.error('discord_post', channel=GENERAL, text='x', files=files))
        self.assertIn(
            'is empty', self.error('discord_post', channel=GENERAL, text='x', files=[self.image('e.gif', b'')])
        )
        self.assertEqual(self.fake.messages[DM], [])

    def test_a_download_from_anywhere_but_discord_is_refused(self):
        discord = aw.Discord(TOKEN, self.clock, self.api)
        for url in ('https://example.com/a.png', 'http://cdn.discordapp.com/a.png', 'file:///etc/passwd'):
            with self.subTest(url=url), self.assertRaises(aw.DiscordError) as caught:
                discord.download(url)
            self.assertIn(url, str(caught.exception))

    def test_a_restart_loses_no_draft_the_draft_is_the_state(self):
        draft = self.tool('discord_post', channel=GENERAL, text='after a restart', wait=0)['draft']
        fresh = aw.AogoWimsy(self.settings, aw.Discord(TOKEN, self.clock, self.api), self.clock)
        self.server = self.started_server(fresh)
        self.fake.react(draft, YES, OWNER)
        self.assertEqual(self.tool('discord_post_status', draft=draft)['status'], 'sent')
        self.assertEqual(self.fake.last(GENERAL)['content'], 'after a restart')

    def test_a_strangers_check_mark_and_its_own_count_for_nothing(self):
        self.owner_answers(3, YES, who=STRANGER)
        got = self.tool('discord_post', channel=GENERAL, text='hello', wait=9)
        self.assertEqual(got['status'], 'waiting')
        self.assertEqual(self.fake.messages[GENERAL], [])
        self.assertTrue(self.fake.posts('GET', f'/channels/{DM}/messages/{got["draft"]}/reactions/{YES}'))

    def test_a_channel_not_listed_is_refused_naming_it_and_the_list(self):
        said = self.error('discord_post', channel=OTHER, text='hello')
        lists = f'channel {OTHER} is not one AogoWimsy may post in: AOGOWIMSY_CHANNELS lists {GENERAL}, {HELP}'
        self.assertIn(lists, said)
        said = self.error('discord_post', channel=OTHER_THREAD, text='hello')
        self.assertIn(f'channel {OTHER_THREAD} is not one AogoWimsy may post in', said)
        said = self.error('discord_post', channel='3000000000000000099', text='hello')
        self.assertIn('Discord answered 404 to GET /channels/3000000000000000099: Unknown Channel (code 10003)', said)
        self.assertEqual(self.fake.messages[DM], [])

    def test_with_no_channels_listed_any_channel_discord_shows_it_and_its_404_names_the_rest(self):
        self.bot.settings = self.settings._replace(channels=None)
        self.owner_answers(3, YES)
        self.assertEqual(self.tool('discord_post', channel=OTHER, text='elsewhere')['status'], 'sent')
        self.assertEqual(self.fake.last(OTHER)['content'], 'elsewhere')
        said = self.error('discord_post', channel='3000000000000000099', text='hello')
        self.assertEqual(
            said, 'Discord answered 404 to GET /channels/3000000000000000099: Unknown Channel (code 10003)'
        )

    def test_a_thread_of_a_listed_channel_is_one(self):
        self.owner_answers(3, YES)
        got = self.tool('discord_post', channel=THREAD, text='in the thread')
        self.assertEqual((got['status'], self.fake.last(THREAD)['content']), ('sent', 'in the thread'))
        card = self.fake.posts('POST', f'/channels/{DM}/messages')[0][3]['embeds'][0]
        self.assertIn(f'#a-thread ({THREAD})', card['description'])

    def test_a_text_over_2000_letters_is_refused_naming_its_length(self):
        said = self.error('discord_post', channel=GENERAL, text='x' * 2001)
        self.assertIn('text has 2001 letters: a Discord message holds 2000 at most', said)
        self.assertEqual(self.fake.messages[DM], [])
        self.assertEqual(self.tool('discord_post', channel=GENERAL, text='x' * 2000, wait=0)['status'], 'waiting')
        self.assertIn('text is empty', self.error('discord_post', channel=GENERAL, text=' \n'))

    def test_arguments_are_checked_before_discord_hears_of_them(self):
        said = self.error('discord_post', channel=int(GENERAL), text='hi')
        self.assertIn('channel is 300000000000000001, not a Discord id: write it as a string', said)
        said = self.error('discord_post', channel=GENERAL, text='hi', wait=3601)
        self.assertIn('wait is 3601, not a whole number from 0 to 3600', said)
        said = self.error('discord_post', channel=GENERAL, text='hi', wait=True)
        self.assertIn('wait is true, not a whole number', said)
        said = self.error('discord_post', channel=GENERAL, text='hi', mention=True)
        self.assertIn('discord_post has no argument "mention"; it takes channel, text, reply_to, wait', said)
        self.assertIn('discord_post needs text', self.error('discord_post', channel=GENERAL))
        self.assertIn('text is a JSON array, not a string', self.error('discord_post', channel=GENERAL, text=['hi']))
        self.assertEqual(self.fake.requests, [])

    def test_post_status_takes_only_its_own_drafts(self):
        mine = self.fake.say(DM, OWNER, 'a message of the owner')
        said = self.error('discord_post_status', draft=mine)
        self.assertIn(f"message {mine} in the owner's DMs ({DM}) is not a draft of AogoWimsy's", said)
        plain = self.fake.say(DM, BOT, 'a plain message of its own')
        self.assertIn('is not a draft', self.error('discord_post_status', draft=plain))

    def test_a_post_discord_refuses_shows_its_error_and_the_draft_waits_again(self):
        self.fake.fail[('POST', f'/channels/{GENERAL}/messages')] = (
            403,
            {'message': 'Missing Permissions', 'code': 50013},
        )
        self.owner_answers(3, YES)
        said = self.error('discord_post', channel=GENERAL, text='hello')
        refused = f'Discord answered 403 to POST /channels/{GENERAL}/messages: Missing Permissions (code 50013)'
        self.assertIn(refused, said)
        draft = self.fake.last(DM)
        self.assertEqual(self.fake.footer(draft), f'waiting: to {GENERAL}')
        self.assertIn('Posting failed: Discord answered 403', draft['embeds'][0]['description'])
        del self.fake.fail[('POST', f'/channels/{GENERAL}/messages')]
        self.assertEqual(self.tool('discord_post_status', draft=draft['id'])['status'], 'sent')
        self.assertEqual(len(self.fake.messages[GENERAL]), 1)

    def test_a_draft_left_sending_is_never_posted_again(self):
        self.owner_answers(3, YES)
        self.tool('discord_post', channel=GENERAL, text='hello')
        draft = self.fake.last(DM)
        draft['embeds'][0]['footer']['text'] = f'sending: to {GENERAL}'
        self.assertIn('says it is being posted to channel', self.error('discord_post_status', draft=draft['id']))
        self.assertEqual(len(self.fake.messages[GENERAL]), 1)

    def test_a_channel_no_longer_listed_when_approved_is_refused(self):
        got = self.tool('discord_post', channel=HELP, text='hello', wait=0)
        self.bot.settings = self.settings._replace(channels=frozenset({GENERAL}))
        self.fake.react(got['draft'], YES, OWNER)
        said = self.error('discord_post_status', draft=got['draft'])
        self.assertIn(f'channel {HELP} is not one AogoWimsy may post in', said)
        self.assertEqual(self.fake.messages[HELP], [])


class Read(WithDiscord):
    def conversation(self, channel):
        ids = [self.fake.say(channel, OWNER, 'first', attachments=[{'filename': 'build.log', 'url': 'x'}])]
        ids.append(self.fake.say(channel, STRANGER, 'a secret of the stranger'))
        ids.append(self.fake.say(channel, FRIEND, 'a reply', reply_to=ids[0]))
        ids.append(self.fake.say(channel, BOT, 'its own'))
        ids.append(self.fake.say(channel, '1000000000000000099', 'a webhook', webhook_id='1000000000000000099'))
        return ids

    def test_only_the_owner_the_listed_users_and_its_own_reach_the_session_the_others_counted(self):
        ids = self.conversation(GENERAL)
        got = self.tool('discord_read', channel=GENERAL)
        self.assertEqual([m['id'] for m in got['messages']], [ids[0], ids[2], ids[3]])
        self.assertEqual((got['left_out'], got['after'], got['channel']), (2, ids[4], GENERAL))
        first, reply = got['messages'][0], got['messages'][1]
        self.assertEqual((first['author'], first['author_id'], first['text']), ('DraVee', OWNER, 'first'))
        self.assertEqual((first['attachments'], first['reply_to'], first['approved']), (['build.log'], None, False))
        self.assertEqual((reply['author'], reply['reply_to']), ('friend', ids[0]))
        self.assertTrue(first['time'].startswith('2026-10-21T12:'))
        self.assertNotIn('stranger', json.dumps(got).lower())
        self.assertNotIn('webhook', json.dumps(got))

    def test_after_and_limit(self):
        ids = self.conversation(GENERAL)
        got = self.tool('discord_read', channel=GENERAL, after=ids[1], limit=2)
        self.assertEqual(
            ([m['id'] for m in got['messages']], got['left_out'], got['after']), ([ids[2], ids[3]], 0, ids[3])
        )
        self.assertEqual(
            self.fake.posts('GET', f'/channels/{GENERAL}/messages')[-1][2], {'limit': '2', 'after': ids[1]}
        )
        newest = self.tool('discord_read', channel=GENERAL, limit=1)
        self.assertEqual((newest['messages'], newest['left_out'], newest['after']), ([], 1, ids[4]))
        empty = self.tool('discord_read', channel=GENERAL, after=ids[4])
        self.assertEqual((empty['messages'], empty['after']), ([], ids[4]))
        said = self.error('discord_read', channel=GENERAL, limit=101)
        self.assertIn('limit is 101, not a whole number from 1 to 100', said)

    def test_from_gives_only_those_authors_and_refuses_anyone_else(self):
        ids = self.conversation(GENERAL)
        got = self.tool('discord_read', channel=GENERAL, **{'from': [FRIEND]})
        self.assertEqual(([m['id'] for m in got['messages']], got['left_out']), ([ids[2]], 4))
        both = self.tool('discord_read', channel=GENERAL, **{'from': [OWNER, FRIEND]})
        self.assertEqual([m['id'] for m in both['messages']], [ids[0], ids[2]])
        said = self.error('discord_read', channel=GENERAL, **{'from': [FRIEND, STRANGER]})
        self.assertEqual(said, f'from item 2 is {STRANGER}, neither the owner ({OWNER}) nor a listed user ({FRIEND})')
        self.assertIn(
            'from is []: a list of 1 or more ids', self.error('discord_read', channel=GENERAL, **{'from': []})
        )
        self.assertIn(
            'from item 1 is "abc", not a Discord id', self.error('discord_read', channel=GENERAL, **{'from': ['abc']})
        )

    def test_a_strangers_message_to_aogo_the_owner_let_through_is_read_and_marked(self):
        mine = self.fake.say(GENERAL, BOT, 'a post of mine')
        let = self.fake.say(GENERAL, STRANGER, 'quoting you', reply_to=mine)
        other = self.fake.say(GENERAL, STRANGER, 'me too', mentions=[BOT])
        plain = self.fake.say(GENERAL, STRANGER, 'not to aogo')
        self.fake.react(let, WIMSY, OWNER)
        self.fake.react(other, WIMSY, OTHER_PERSON)
        self.fake.react(plain, WIMSY, OWNER)
        got = self.tool('discord_read', channel=GENERAL)
        self.assertEqual([(m['id'], m['approved']) for m in got['messages']], [(mine, False), (let, True)])
        self.assertEqual(got['left_out'], 2)
        self.assertNotIn('me too', json.dumps(got))
        self.assertTrue(self.fake.posts('GET', f'/channels/{GENERAL}/messages/{let}/reactions/{WIMSY}'))
        named = self.tool('discord_read', channel=GENERAL, **{'from': [OWNER]})
        self.assertEqual((named['messages'], named['left_out']), ([], 4))

    def test_a_message_the_owner_replied_to_mentioning_aogo_is_read_with_his_reply_in_the_page(self):
        start = self.fake.say(GENERAL, FRIEND, 'before')
        theirs = self.fake.say(GENERAL, STRANGER, 'a question for the room')
        reply = self.fake.say(GENERAL, OWNER, 'asking <@1000000000000000001>', reply_to=theirs, mentions=[BOT])
        got = self.tool('discord_read', channel=GENERAL, after=start)
        self.assertEqual([(m['id'], m['approved']) for m in got['messages']], [(theirs, True), (reply, False)])
        alone = self.tool('discord_read', channel=GENERAL, after=start, limit=1)  # no reply in the page, none heard
        self.assertEqual((alone['messages'], alone['left_out']), ([], 1))

    def test_a_message_the_owner_replied_to_mentioning_aogos_role_is_read_too(self):
        start = self.fake.say(GENERAL, FRIEND, 'before')
        theirs = self.fake.say(GENERAL, STRANGER, 'a question for the room')
        reply = self.fake.say(GENERAL, OWNER, f'<@&{BOT_ROLE}> also this', reply_to=theirs, mention_roles=[BOT_ROLE])
        got = self.tool('discord_read', channel=GENERAL, after=start)
        self.assertEqual([(m['id'], m['approved']) for m in got['messages']], [(theirs, True), (reply, False)])

    def test_a_thread_of_a_listed_channel_and_the_owners_dm_read_without_being_listed(self):
        ids = self.conversation(THREAD)
        got = self.tool('discord_read', channel=THREAD)
        self.assertEqual(([m['id'] for m in got['messages']], got['left_out']), ([ids[0], ids[2], ids[3]], 2))
        self.fake.say(DM, OWNER, 'in the DMs')
        self.assertEqual([m['text'] for m in self.tool('discord_read', channel=DM)['messages']], ['in the DMs'])

    def test_other_channels_are_refused_naming_what_it_reads(self):
        said = self.error('discord_read', channel=OTHER)
        lists = f'channel {OTHER} is not one AogoWimsy may read: AOGOWIMSY_CHANNELS lists {GENERAL}, {HELP}'
        self.assertIn(lists, said)
        self.assertIn(f"the owner's DMs with AogoWimsy are {DM}", said)
        self.assertIn('may read', self.error('discord_read', channel=OTHER_THREAD))
        self.assertFalse(self.fake.posts('GET', f'/channels/{OTHER}/messages'))
        self.bot.settings = self.settings._replace(channels=None)
        self.fake.say(OTHER, OWNER, 'anywhere')
        self.assertEqual([m['text'] for m in self.tool('discord_read', channel=OTHER)['messages']], ['anywhere'])


class Attachments(WithDiscord):
    def setUp(self):
        super().setUp()
        folder = tempfile.TemporaryDirectory(prefix='aogowimsy-files-')
        self.addCleanup(folder.cleanup)
        self.bot.files = Path(folder.name) / 'cache' / 'aogowimsy'

    def carrying(self, channel, author, files, content='', **extra):
        """a message with files [(name, bytes, type)], each served by the fake CDN; its id"""
        number = self.fake.say(channel, author, content, **extra)
        kept = []
        for place, (name, data, kind) in enumerate(files):
            path = f'/attachments/{channel}/{number}{place}/{urllib.parse.quote(name)}'
            self.fake.blobs[path] = (data, kind)
            item = {'id': f'{number}{place}', 'filename': name, 'size': len(data), 'url': self.fake.origin + path}
            kept.append(dict(item, content_type=kind))
        self.fake.find(channel, number)['attachments'] = kept
        return number

    def cdn(self):
        return [r for r in self.fake.requests if r[0] == 'CDN']

    def test_the_owners_files_land_in_the_messages_folder_private_and_fetched_without_the_token(self):
        log = b'line 1\nline 2\n'
        number = self.carrying(GENERAL, OWNER, [('shot.png', PNG, 'image/png'), ('game.log', log, 'text/plain')])
        self.fake.find(GENERAL, number)['attachments'][0].update(width=1920, height=1080)
        got = self.tool('discord_attachments', channel=GENERAL, message=number)
        folder = self.bot.files / GENERAL / number
        self.assertEqual((got['channel'], got['message'], got['folder']), (GENERAL, number, str(folder)))
        self.assertEqual(
            got['files'],
            [
                {
                    'name': 'shot.png',
                    'type': 'image/png',
                    'size': len(PNG),
                    'width': 1920,
                    'height': 1080,
                    'path': str(folder / 'shot.png'),
                },
                {'name': 'game.log', 'type': 'text/plain', 'size': len(log), 'path': str(folder / 'game.log')},
            ],
        )
        self.assertEqual(((folder / 'shot.png').read_bytes(), (folder / 'game.log').read_bytes()), (PNG, log))
        for level in (self.bot.files, self.bot.files / GENERAL, folder):
            self.assertEqual(level.stat().st_mode & 0o777, 0o700, level)
        self.assertEqual((folder / 'shot.png').stat().st_mode & 0o777, 0o600)
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ['game.log', 'shot.png'])  # no .part left
        self.assertEqual([r[3] for r in self.cdn()], [None, None])

    def test_a_file_saved_before_is_not_fetched_again(self):
        number = self.carrying(GENERAL, FRIEND, [('a.png', PNG, 'image/png')])
        first = self.tool('discord_attachments', channel=GENERAL, message=number)
        again = self.tool('discord_attachments', channel=GENERAL, message=number)
        self.assertEqual(first, again)
        self.assertEqual(len(self.cdn()), 1)

    def test_a_strangers_message_the_owner_did_not_let_through_is_refused_naming_no_one(self):
        number = self.carrying(GENERAL, STRANGER, [('a.png', PNG, 'image/png')], 'look', mentions=[BOT])
        self.fake.react(number, WIMSY, OTHER_PERSON)
        said = self.error('discord_attachments', channel=GENERAL, message=number)
        self.assertEqual(
            said,
            f"message {number} is not one AogoWimsy gives: neither the owner's ({OWNER}), a listed user's ({FRIEND}) "
            f'nor its own, and the owner has not let it through (his :{aw.WIMSY}: reaction, or his reply to it '
            'mentioning AogoWimsy)',
        )
        self.assertNotIn('tranger', said)
        self.assertEqual(self.cdn(), [])
        self.assertFalse(self.bot.files.exists())

    def test_a_strangers_message_the_owner_replied_to_mentioning_aogo_or_let_through_is_saved(self):
        quoted = self.carrying(GENERAL, STRANGER, [('q.jpg', b'\xff\xd8\xff jpeg', 'image/jpeg')])
        self.fake.say(GENERAL, FRIEND, 'in between')
        self.fake.say(GENERAL, OWNER, '<@1000000000000000001> any idea?', reply_to=quoted, mentions=[BOT])
        mine = self.fake.say(GENERAL, BOT, 'a post of mine')
        let = self.carrying(GENERAL, STRANGER, [('l.png', PNG, 'image/png')], 'for you', reply_to=mine)
        self.fake.react(let, WIMSY, OWNER)
        for number in (quoted, let):
            with self.subTest(message=number):
                got = self.tool('discord_attachments', channel=GENERAL, message=number)
                self.assertEqual([('path' in f, 'error' in f) for f in got['files']], [(True, False)])
        self.bot.approved.add(heard := self.carrying(GENERAL, STRANGER, [('h.png', PNG, 'image/png')]))
        self.assertIn('path', self.tool('discord_attachments', channel=GENERAL, message=heard)['files'][0])

    def test_one_too_big_or_failing_says_why_and_the_others_are_saved(self):
        self.addCleanup(setattr, aw, 'MOST_SAVE', aw.MOST_SAVE)
        aw.MOST_SAVE = 20
        number = self.carrying(
            GENERAL,
            OWNER,
            [
                ('big.png', b'x' * 21, 'image/png'),
                ('gone.png', b'gone', 'image/png'),
                ('ok.txt', b'fits', 'text/plain'),
            ],
        )
        attached = self.fake.find(GENERAL, number)['attachments']
        del self.fake.blobs[urllib.parse.urlsplit(attached[1]['url']).path]
        got = self.tool('discord_attachments', channel=GENERAL, message=number)
        big, gone, ok = got['files']
        self.assertEqual(big['error'], '21 bytes, more than the 20 AogoWimsy saves of one attachment')
        self.assertTrue(gone['error'].startswith(f"Discord's CDN answered 401 to {attached[1]['url']}"), gone)
        self.assertNotIn('path', big)
        self.assertEqual(Path(ok['path']).read_bytes(), b'fits')
        self.assertEqual([r[1].rsplit('/', 1)[1] for r in self.cdn()], ['ok.txt'])  # the big one is never fetched

    def test_a_size_discord_did_not_give_is_capped_while_reading(self):
        self.addCleanup(setattr, aw, 'MOST_SAVE', aw.MOST_SAVE)
        aw.MOST_SAVE = 4
        number = self.carrying(GENERAL, OWNER, [('a.png', PNG, 'image/png')])
        del self.fake.find(GENERAL, number)['attachments'][0]['size']
        got = self.tool('discord_attachments', channel=GENERAL, message=number)
        self.assertIn("Discord's CDN gave more than 4 bytes for", got['files'][0]['error'])
        self.assertEqual(list((self.bot.files / GENERAL / number).iterdir()), [])

    def test_names_stay_in_the_folder_and_a_name_twice_takes_its_id(self):
        number = self.carrying(
            GENERAL,
            OWNER,
            [
                ('../../up.txt', b'1', 'text/plain'),
                ('a.png', PNG, 'image/png'),
                ('a.png', b'2', 'image/png'),
                ('...', b'3', 'text/plain'),
                ('tab\there\n.log', b'4', 'text/plain'),
            ],
        )
        ids = [a['id'] for a in self.fake.find(GENERAL, number)['attachments']]
        got = self.tool('discord_attachments', channel=GENERAL, message=number)
        folder = self.bot.files / GENERAL / number
        names = [Path(f['path']).name for f in got['files']]
        self.assertEqual(names, ['_.._up.txt', 'a.png', f'{ids[2]}-a.png', ids[3], 'tab_here_.log'])
        self.assertTrue(all(Path(f['path']).parent == folder for f in got['files']))
        self.assertEqual((folder / f'{ids[2]}-a.png').read_bytes(), b'2')

    def test_what_is_refused_before_any_file_is_fetched(self):
        plain = self.fake.say(GENERAL, OWNER, 'no files')
        cases = (
            ({'channel': GENERAL, 'message': plain}, f'message {plain} in channel {GENERAL} has no attachments'),
            ({'channel': GENERAL, 'message': '1999999999999999999'}, 'Discord answered 404 to GET'),
            ({'channel': OTHER, 'message': plain}, f'channel {OTHER} is not one AogoWimsy may read'),
            ({'channel': GENERAL}, 'discord_attachments needs message'),
            ({'channel': GENERAL, 'message': 'abc'}, 'message is "abc", not a Discord id'),
            ({'channel': GENERAL, 'message': plain, 'into': '/tmp'}, 'discord_attachments has no argument "into"'),
        )
        for args, said in cases:
            with self.subTest(args=args):
                self.assertIn(said, self.error('discord_attachments', **args))
        self.assertEqual(self.cdn(), [])

    def test_a_write_that_fails_says_where_and_leaves_no_part(self):
        number = self.carrying(GENERAL, OWNER, [('a.png', PNG, 'image/png')])
        folder = self.bot.files / GENERAL / number
        folder.mkdir(parents=True)
        (folder / 'a.png').mkdir()  # a folder where the file goes: os.replace fails
        got = self.tool('discord_attachments', channel=GENERAL, message=number)
        self.assertTrue(got['files'][0]['error'].startswith(f'{folder / "a.png"} could not be written: '))
        self.assertEqual([p.name for p in folder.iterdir()], ['a.png'])


class SavesFolder(unittest.TestCase):
    def test_xdg_cache_home_when_absolute_else_the_home_cache(self):
        home = Path.home() / '.cache' / 'aogowimsy'
        self.assertEqual(aw.saves_folder({'XDG_CACHE_HOME': '/var/tmp/c'}), Path('/var/tmp/c/aogowimsy'))
        self.assertEqual(aw.saves_folder({'XDG_CACHE_HOME': 'relative/c'}), home)
        self.assertEqual(aw.saves_folder({'XDG_CACHE_HOME': ''}), home)
        self.assertEqual(aw.saves_folder({}), home)

    def test_a_long_name_keeps_its_suffix_within_200_bytes(self):
        name = aw.file_name('é' * 300 + '.png', 'fallback')
        self.assertTrue(name.endswith('.png'))
        self.assertLessEqual(len(name.encode()), 200)
        self.assertEqual(aw.file_name('', '123'), '123')


class Wait(WithDiscord):
    def test_the_first_message_from_the_owner_or_a_listed_user_passing_over_the_others(self):
        start = self.fake.say(GENERAL, OWNER, 'go')
        self.fake.say(GENERAL, STRANGER, 'noise')
        self.fake.say(GENERAL, BOT, 'its own')
        self.clock.at(2, lambda: self.arrive(GENERAL, STRANGER, 'more noise'))
        self.clock.at(3, lambda: self.arrive(GENERAL, FRIEND, 'here'))
        got = self.tool('discord_wait', channel=GENERAL, after=start)
        self.assertEqual((got['message']['text'], got['message']['author_id'], got['left_out']), ('here', FRIEND, 2))
        self.assertEqual(got['after'], got['message']['id'])
        self.assertEqual(self.clock.waits, [1.0, 1.0, 1.0])
        self.assertNotIn('noise', json.dumps(got))
        self.assertEqual(len(self.fake.posts('GET', f'/channels/{GENERAL}/messages')), 1)  # the gateway, not polling

    def test_from_waits_only_for_those_authors(self):
        start = self.fake.say(GENERAL, OWNER, 'go')
        self.clock.at(1, lambda: self.arrive(GENERAL, OWNER, 'not this one'))
        self.clock.at(2, lambda: self.arrive(GENERAL, FRIEND, 'this one'))
        got = self.tool('discord_wait', channel=GENERAL, after=start, **{'from': [FRIEND]})
        self.assertEqual((got['message']['text'], got['left_out']), ('this one', 1))
        said = self.error('discord_wait', channel=GENERAL, after=start, **{'from': [STRANGER]})
        self.assertIn(f'from item 1 is {STRANGER}, neither the owner', said)

    def test_none_in_time_says_so_with_the_id_to_wait_after_next(self):
        start = self.fake.say(GENERAL, OWNER, 'go')
        noise = self.fake.say(GENERAL, STRANGER, 'noise')
        got = self.tool('discord_wait', channel=GENERAL, after=start, wait=5)
        self.assertEqual(got, {'channel': GENERAL, 'message': None, 'waited': 5, 'left_out': 1, 'after': noise})

    def test_what_came_before_it_listened_is_read_from_rest_page_after_page(self):
        start = self.fake.say(GENERAL, OWNER, 'go')
        for n in range(150):
            self.fake.say(GENERAL, STRANGER, f'noise {n}')
        self.fake.say(GENERAL, OWNER, 'mine')
        got = self.tool('discord_wait', channel=GENERAL, after=start)
        self.assertEqual((got['message']['text'], got['left_out'], self.clock.waits), ('mine', 150, []))

    def test_a_new_gateway_session_reads_what_went_unheard(self):
        start = self.fake.say(GENERAL, OWNER, 'go')

        def unheard():
            self.fake.say(GENERAL, FRIEND, 'said while the gateway was down')
            self.bot.hub.gap()

        self.clock.at(2, unheard)
        got = self.tool('discord_wait', channel=GENERAL, after=start)
        self.assertEqual(got['message']['text'], 'said while the gateway was down')

    def test_a_thread_of_a_listed_channel_and_the_owners_dm(self):
        start = self.fake.say(THREAD, FRIEND, 'start')
        self.clock.at(3, lambda: self.arrive(THREAD, OWNER, 'in the thread'))
        self.assertEqual(self.tool('discord_wait', channel=THREAD, after=start)['message']['text'], 'in the thread')
        start = self.fake.say(DM, BOT, 'a draft')
        self.clock.at(6, lambda: self.arrive(DM, OWNER, 'answer in the DMs'))
        self.assertEqual(self.tool('discord_wait', channel=DM, after=start)['message']['text'], 'answer in the DMs')
        self.assertIn('may read', self.error('discord_wait', channel=OTHER, after=start))

    def test_a_stopped_gateway_is_an_error_naming_why(self):
        start = self.fake.say(GENERAL, OWNER, 'go')
        self.clock.at(2, lambda: self.bot.hub.stop('Discord closed the gateway with 4014: disallowed intents'))
        said = self.error('discord_wait', channel=GENERAL, after=start)
        self.assertEqual(
            said,
            'the gateway stopped (Discord closed the gateway with 4014: disallowed intents): discord_wait hears '
            'nothing; discord_read reads',
        )


class RateLimits(WithDiscord):
    def test_a_429_waits_its_retry_after_and_tries_again(self):
        self.fake.say(GENERAL, OWNER, 'hello')
        self.fake.limits.append(('GET', f'/channels/{GENERAL}/messages'))
        got = self.tool('discord_read', channel=GENERAL)
        self.assertEqual([m['text'] for m in got['messages']], ['hello'])
        self.assertEqual(self.clock.waits, [1.5])
        self.assertEqual(len(self.fake.posts('GET', f'/channels/{GENERAL}/messages')), 2)

    def test_429s_without_end_and_other_failures_are_errors_with_discords_status_and_message(self):
        self.fake.limits.extend([('GET', f'/channels/{GENERAL}/messages')] * aw.TRIES)
        said = self.error('discord_read', channel=GENERAL)
        self.assertIn(f'Discord answered 429 to GET /channels/{GENERAL}/messages 5 times', said)
        self.fake.fail[('GET', f'/channels/{HELP}/messages')] = (403, {'message': 'Missing Access', 'code': 50001})
        said = self.error('discord_read', channel=HELP)
        self.assertEqual(said, f'Discord answered 403 to GET /channels/{HELP}/messages: Missing Access (code 50001)')

    def test_an_invalid_form_body_names_its_fields(self):
        errors = {'content': {'_errors': [{'message': 'Must be 2000 or fewer in length.'}]}}
        said = aw.reason({'code': 50035, 'message': 'Invalid Form Body', 'errors': errors})
        self.assertEqual(said, 'Invalid Form Body (code 50035): content: Must be 2000 or fewer in length.')


class Protocol(WithDiscord):
    """the JSON-RPC answers, in process"""

    def ask(self, method, params=None, rid=1, server=None):
        message = {'jsonrpc': '2.0', 'id': rid, 'method': method}
        if params is not None:
            message['params'] = params
        return (server or self.server).handle(message)

    def test_initialize_answers_the_clients_version_when_it_has_it_else_its_newest_and_says_it_is_a_channel(self):
        for asked, agreed in (('2025-06-18', '2025-06-18'), ('2024-11-05', '2024-11-05'), ('2026-07-28', '2025-11-25')):
            server = aw.Server(self.bot, 'test', self.sent.append)
            params = dict(INITIALIZE, protocolVersion=asked)
            result = self.ask('initialize', params, server=server)['result']
            self.assertEqual(result['protocolVersion'], agreed)
            self.assertEqual(result['capabilities'], {'tools': {}, 'experimental': {'claude/channel': {}}})
            self.assertEqual(result['serverInfo']['name'], 'aogowimsy')
        self.assertEqual(self.ask('initialize', {})['error']['code'], -32602)

    def test_2026_07_28_is_not_spoken_so_a_client_falls_back_to_initialize(self):
        server = aw.Server(self.bot, 'test', self.sent.append)
        meta = {
            'io.modelcontextprotocol/protocolVersion': '2026-07-28',
            'io.modelcontextprotocol/clientCapabilities': {},
        }
        error = self.ask('server/discover', {'_meta': meta}, server=server)['error']
        self.assertEqual(error['code'], -32601)
        self.assertIn(
            'speaks the initialize handshake (2025-11-25, 2025-06-18, 2025-03-26, 2024-11-05)', error['message']
        )
        error = self.ask('tools/list', {'_meta': meta}, server=server)['error']
        self.assertEqual(error['code'], -32602)
        self.assertIn('tools/list came before initialize; AogoWimsy speaks 2025-11-25', error['message'])
        self.assertEqual(self.ask('ping', server=server), {'jsonrpc': '2.0', 'id': 1, 'result': {}})
        self.assertEqual(self.ask('tools/list')['result'], {'tools': aw.TOOLS})

    def test_it_starts_hearing_discord_once_the_client_is_initialized_and_only_then(self):
        calls = []

        def listen(push):
            calls.append(push)
            return [aw.started(lambda: None)]

        server = aw.Server(self.bot, 'test', self.sent.append, listen=listen)
        server.handle({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        self.assertEqual(calls, [])  # no initialize yet
        self.ask('initialize', INITIALIZE, server=server)
        self.assertEqual(calls, [])
        server.handle({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        server.handle({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        self.assertEqual(calls, [server.push])
        calls[0]('a text', {'kind': 'message'})
        self.assertEqual(
            self.sent[-1],
            {
                'jsonrpc': '2.0',
                'method': 'notifications/claude/channel',
                'params': {'content': 'a text', 'meta': {'kind': 'message'}},
            },
        )
        server.stop()
        self.assertTrue(server.stopping.is_set())

    def test_methods_it_has_not_and_unknown_tools(self):
        error = self.ask('resources/list')['error']
        self.assertEqual(error['code'], -32601)
        self.assertIn('"resources/list"; AogoWimsy answers initialize, ping, tools/list, tools/call', error['message'])
        error = self.ask('tools/call', {'name': 'discord_delete', 'arguments': {}})['error']
        self.assertEqual(error['code'], -32602)
        self.assertIn('Unknown tool: "discord_delete"; AogoWimsy has discord_post', error['message'])
        self.assertEqual(self.ask('tools/list', {'cursor': 'x'})['error']['code'], -32602)

    def test_notifications_and_bad_messages(self):
        self.assertIsNone(self.server.handle({'jsonrpc': '2.0', 'method': 'notifications/initialized'}))
        cancel = {'jsonrpc': '2.0', 'method': 'notifications/cancelled', 'params': {'requestId': 9}}
        self.assertIsNone(self.server.handle(cancel))
        self.assertEqual(self.server.handle({'id': 1, 'method': 'ping'})['error']['code'], -32600)
        self.assertEqual(self.server.handle({'jsonrpc': '2.0', 'id': None, 'method': 'ping'})['error']['code'], -32600)
        params = {'jsonrpc': '2.0', 'id': 2, 'method': 'ping', 'params': [1]}
        self.assertEqual(self.server.handle(params)['error']['code'], -32602)

    def test_a_cancelled_wait_stops_and_gets_no_answer(self):
        cancelled = threading.Event()
        self.clock.at(2, cancelled.set)
        start = self.fake.say(GENERAL, OWNER, 'go')
        self.assertIsNone(self.answer('discord_wait', cancelled=cancelled, channel=GENERAL, after=start))
        self.assertEqual(self.clock.waits, [1.0, 1.0])

    def test_progress_when_asked_for_it_always_growing(self):
        start = self.fake.say(GENERAL, OWNER, 'go')
        self.clock.at(3, lambda: self.arrive(GENERAL, FRIEND, 'here'))
        answer = self.answer('discord_wait', meta={'progressToken': 'p1'}, channel=GENERAL, after=start)
        here = json.loads(answer['result']['content'][0]['text'])['message']
        self.assertEqual(here['text'], 'here')
        sent = [(n['params']['progressToken'], n['params']['progress'], n['params']['total']) for n in self.sent]
        self.assertEqual(sent, [('p1', 1.0, 600), ('p1', 2.0, 600), ('p1', 3.0, 600)])
        self.assertTrue(all(n['method'] == 'notifications/progress' for n in self.sent))
        self.assertIsNone(self.tool('discord_wait', channel=GENERAL, after=here['id'], wait=2)['message'])
        self.assertEqual(len(self.sent), 3)  # asked for none: none sent

    def test_progress_never_repeats_nor_goes_back_and_stops_once_cancelled(self):
        cancelled = threading.Event()
        send = self.server.progress(5, cancelled)
        for done in (1.0, 1.02, 0.5, 2.0):
            send(done, 10, 'waiting')
        cancelled.set()
        send(3.0, 10, 'waiting')
        self.assertEqual([n['params']['progress'] for n in self.sent], [1.0, 2.0])


class Channels(WithDiscord):
    """what the gateway hears and what reaches the session as a channel event"""

    def setUp(self):
        super().setUp()
        self.pushes = []
        self.ear = aw.Ear(self.bot, lambda text, meta: self.pushes.append((text, meta)), threading.Event())
        self.ear.hear('READY', {'user': {'id': BOT}, 'session_id': 's', 'resume_gateway_url': 'ws://127.0.0.1:9'})
        guild = {'id': GUILD, 'channels': [self.fake.channels[c] for c in (GENERAL, HELP, OTHER)]}
        guild['threads'] = [self.fake.channels[THREAD], self.fake.channels[OTHER_THREAD]]
        self.ear.hear('GUILD_CREATE', guild)

    def heard(self, channel, author, content='', **extra):
        number = self.fake.say(channel, author, content, **extra)
        self.ear.hear('MESSAGE_CREATE', self.fake.event(channel, number))
        return number

    def reacted(self, channel, message, emoji, who):
        self.fake.react(message, emoji, who)
        name, _, number = emoji.partition(':')
        event = {'user_id': who, 'channel_id': channel, 'message_id': message}
        if self.fake.channels[channel].get('guild_id'):
            event['guild_id'] = self.fake.channels[channel]['guild_id']
        self.ear.hear('MESSAGE_REACTION_ADD', dict(event, emoji={'id': number or None, 'name': name}))

    def test_the_owners_answer_to_a_draft_posts_it_at_once_with_no_tool_waiting(self):
        draft = self.tool('discord_post', channel=GENERAL, text='approved later', wait=0)['draft']
        refused = self.tool('discord_post', channel=HELP, text='refused later', wait=0)['draft']
        self.reacted(DM, draft, YES, STRANGER)
        self.reacted(DM, draft, YES, BOT)
        self.assertEqual((self.fake.messages[GENERAL], self.pushes), ([], []))
        self.reacted(DM, draft, YES, OWNER)
        posted = self.fake.last(GENERAL)
        self.assertEqual((posted['content'], len(self.fake.messages[GENERAL])), ('approved later', 1))
        sent = f'https://discord.com/channels/{GUILD}/{GENERAL}/{posted["id"]}'
        meta = {'kind': 'draft', 'draft': draft, 'status': 'sent', 'channel': GENERAL, 'message_id': posted['id']}
        self.assertEqual(self.pushes, [(f'The owner approved draft {draft}: posted as {sent}', meta)])
        self.reacted(DM, draft, YES, OWNER)  # the event again, or a tool that saw it first: never twice
        self.assertEqual((len(self.fake.messages[GENERAL]), len(self.pushes)), (1, 1))
        self.assertEqual(self.tool('discord_post_status', draft=draft, wait=0)['message'], posted['id'])
        self.reacted(DM, refused, NO, OWNER)
        self.assertEqual(self.fake.messages[HELP], [])
        self.assertEqual(self.fake.footer(self.fake.find(DM, refused)), f'not sent: to {HELP}')
        meta = {'kind': 'draft', 'draft': refused, 'status': 'not sent', 'channel': HELP}
        self.assertEqual(self.pushes[1:], [(f'The owner refused draft {refused}: not posted to channel {HELP}', meta)])
        mine = self.fake.say(DM, OWNER, 'not a draft')
        self.reacted(DM, mine, YES, OWNER)
        self.assertEqual(len(self.pushes), 2)

    def test_a_draft_another_session_made_waits_for_that_sessions_look(self):
        other = aw.AogoWimsy(self.settings, aw.Discord(TOKEN, self.clock, self.api), self.clock)
        self.server = self.started_server(other)
        draft = self.tool('discord_post', channel=GENERAL, text='from another session', wait=0)['draft']
        self.reacted(DM, draft, YES, OWNER)  # this session's ear: not its draft
        self.assertEqual((self.fake.messages[GENERAL], self.pushes), ([], []))
        self.assertEqual(self.tool('discord_post_status', draft=draft, wait=0)['status'], 'sent')
        self.assertEqual(len(self.fake.messages[GENERAL]), 1)

    def test_a_reply_to_aogo_from_the_owner_is_pushed_once_with_its_fields(self):
        mine = self.fake.say(GENERAL, BOT, 'a post of mine')
        files = [{'filename': 'log;1\n.txt'}, {'filename': 'shot.png'}]
        number = self.heard(GENERAL, OWNER, 'is this it?', reply_to=mine, attachments=files)
        meta = {'kind': 'message', 'author': 'DraVee', 'author_id': OWNER, 'channel': GENERAL, 'message_id': number}
        meta.update(reply_to=mine, attachments='log_1_.txt; shot.png')
        self.assertEqual(self.pushes, [('is this it?', meta)])
        self.assertEqual(self.logs, ['channel: pushed a message event'])

    def test_a_mention_from_a_listed_user_is_pushed(self):
        number = self.heard(HELP, FRIEND, '<@1000000000000000001> look', mentions=[BOT])
        meta = {'kind': 'message', 'author': 'friend', 'author_id': FRIEND, 'channel': HELP, 'message_id': number}
        self.assertEqual(self.pushes, [('<@1000000000000000001> look', meta)])

    def test_a_mention_of_aogos_role_is_a_mention_of_aogo(self):
        number = self.heard(HELP, FRIEND, f'<@&{BOT_ROLE}> look', mention_roles=[BOT_ROLE])
        meta = {'kind': 'message', 'author': 'friend', 'author_id': FRIEND, 'channel': HELP, 'message_id': number}
        self.assertEqual(self.pushes, [(f'<@&{BOT_ROLE}> look', meta)])
        self.heard(HELP, FRIEND, 'another bot, a plain role', mention_roles=[OTHER_BOT_ROLE, HELPERS_ROLE, GUILD])
        self.assertEqual(len(self.pushes), 1)

    def test_the_owners_reply_mentioning_aogos_role_carries_the_message_he_replied_to(self):
        theirs = self.fake.say(GENERAL, STRANGER, 'the percent is the current game only')
        asked = f'<@&{BOT_ROLE}> also fix this'
        number = self.heard(GENERAL, OWNER, asked, reply_to=theirs, mention_roles=[BOT_ROLE])
        quote = f'\n\nReplying to Stranger ({STRANGER}), message {theirs}:\nthe percent is the current game only'
        meta = {'kind': 'message', 'author': 'DraVee', 'author_id': OWNER, 'channel': GENERAL, 'message_id': number}
        meta.update(reply_to=theirs, quoted_author='Stranger', quoted_author_id=STRANGER)
        self.assertEqual(self.pushes, [(asked + quote, meta)])
        self.assertEqual(self.bot.approved, {theirs})

    def test_aogos_roles_are_asked_once_and_again_after_a_server_is_joined(self):
        for _ in range(2):
            self.heard(GENERAL, OWNER, f'<@&{BOT_ROLE}> ping', mention_roles=[BOT_ROLE])
        self.assertEqual(len(self.fake.posts('GET', f'/guilds/{GUILD}/roles')), 1)
        self.fake.server_roles[1] = dict(self.fake.server_roles[1], id=HELPERS_ROLE)  # invited again: a new role
        self.fake.server_roles.pop()
        self.ear.hear('GUILD_CREATE', {'id': GUILD, 'channels': [], 'threads': []})
        self.heard(GENERAL, OWNER, f'<@&{BOT_ROLE}> the old role', mention_roles=[BOT_ROLE])
        self.heard(GENERAL, OWNER, f'<@&{HELPERS_ROLE}> the new one', mention_roles=[HELPERS_ROLE])
        self.assertEqual([text for text, _ in self.pushes][2:], [f'<@&{HELPERS_ROLE}> the new one'])
        self.assertEqual(len(self.fake.posts('GET', f'/guilds/{GUILD}/roles')), 2)

    def test_a_message_not_addressed_to_aogo_is_not_pushed(self):
        theirs = self.fake.say(GENERAL, FRIEND, 'a post of a friend')
        self.heard(GENERAL, OWNER, 'talking to the room')
        self.heard(GENERAL, OWNER, 'answering the friend', reply_to=theirs)
        self.heard(GENERAL, OWNER, 'naming someone', mentions=[FRIEND])
        self.heard(GENERAL, BOT, 'its own, naming itself', mentions=[BOT])
        self.assertEqual(self.pushes, [])

    def test_a_strangers_message_to_aogo_reaches_the_session_only_on_the_owners_wimsy_once(self):
        mine = self.fake.say(GENERAL, BOT, 'a post of mine')
        quoted = self.heard(GENERAL, STRANGER, 'quoting you', reply_to=mine)
        self.assertEqual(self.pushes, [])
        self.reacted(GENERAL, quoted, WIMSY, OTHER_PERSON)
        self.reacted(GENERAL, quoted, OTHER_EMOJI, OWNER)
        self.reacted(GENERAL, quoted, YES, OWNER)
        self.assertEqual(self.pushes, [])
        self.reacted(GENERAL, quoted, WIMSY, OWNER)
        meta = {'kind': 'approved', 'author': 'Stranger', 'author_id': STRANGER, 'channel': GENERAL}
        meta.update(message_id=quoted, reply_to=mine)
        self.assertEqual(self.pushes, [('quoting you', meta)])
        self.reacted(GENERAL, quoted, WIMSY, OWNER)
        self.assertEqual(len(self.pushes), 1)

    def test_the_owners_reply_mentioning_aogo_carries_the_message_he_replied_to_and_lets_it_through(self):
        before = self.fake.say(GENERAL, FRIEND, 'before')
        theirs = self.fake.say(GENERAL, STRANGER, 'a question for the room', attachments=[{'filename': 'crash.log'}])
        asked = 'Gonna ask <@1000000000000000001>, lets see if this works'
        number = self.heard(GENERAL, OWNER, asked, reply_to=theirs, mentions=[STRANGER, BOT])
        quote = f'\n\nReplying to Stranger ({STRANGER}), message {theirs}:\na question for the room'
        meta = {'kind': 'message', 'author': 'DraVee', 'author_id': OWNER, 'channel': GENERAL, 'message_id': number}
        meta.update(
            reply_to=theirs, quoted_author='Stranger', quoted_author_id=STRANGER, quoted_attachments='crash.log'
        )
        self.assertEqual(self.pushes, [(asked + quote, meta)])
        self.assertEqual(self.bot.approved, {theirs})
        self.reacted(GENERAL, theirs, WIMSY, OWNER)  # through already: once
        self.assertEqual(len(self.pushes), 1)
        got = self.tool('discord_read', channel=GENERAL, after=before)
        self.assertEqual([(m['id'], m['approved']) for m in got['messages']], [(theirs, True), (number, False)])
        self.assertEqual(got['left_out'], 0)
        alone = self.tool('discord_read', channel=GENERAL, after=before, limit=1)  # his reply is not in this page
        self.assertEqual([(m['id'], m['approved']) for m in alone['messages']], [(theirs, True)])

    def test_a_reply_whose_answered_message_the_event_lacks_reads_it_from_discord(self):
        theirs = self.fake.say(GENERAL, STRANGER, 'asked before')
        number = self.fake.say(GENERAL, OWNER, 'see this <@1000000000000000001>', reply_to=theirs, mentions=[BOT])
        event = self.fake.event(GENERAL, number)
        del event['referenced_message']
        self.ear.hear('MESSAGE_CREATE', event)
        self.assertTrue(self.pushes[0][0].endswith(f'message {theirs}:\nasked before'))
        self.assertEqual(len(self.fake.posts('GET', f'/channels/{GENERAL}/messages/{theirs}')), 1)

    def test_only_the_owners_reply_with_a_mention_lets_a_message_through(self):
        theirs = self.fake.say(GENERAL, STRANGER, 'a question for the room')
        self.heard(GENERAL, FRIEND, 'asking <@1000000000000000001>', reply_to=theirs, mentions=[BOT])
        self.heard(GENERAL, OWNER, 'answering them, no mention', reply_to=theirs)
        self.assertEqual(
            [(text, 'quoted_author_id' in meta) for text, meta in self.pushes],
            [('asking <@1000000000000000001>', False)],
        )
        self.assertEqual(self.bot.approved, set())
        self.assertNotIn(theirs, [m['id'] for m in self.tool('discord_read', channel=GENERAL)['messages']])

    def test_the_owners_wimsy_elsewhere_pushes_nothing(self):
        plain = self.heard(GENERAL, STRANGER, 'not to aogo')
        own = self.heard(GENERAL, OWNER, 'his own, to aogo', mentions=[BOT])
        outside = self.heard(OTHER, STRANGER, 'to aogo, in a channel not listed', mentions=[BOT])
        self.pushes.clear()
        for channel, message in ((GENERAL, plain), (GENERAL, own), (OTHER, outside)):
            self.reacted(channel, message, WIMSY, OWNER)
        self.assertEqual(self.pushes, [])

    def test_the_owners_and_a_listed_users_dms_are_pushed_a_strangers_only_says_who_once(self):
        self.heard(DM, BOT, 'a draft of its own, as the gateway brings it back')
        self.heard(DM, OWNER, 'in my DMs')
        self.heard(FRIEND_DM, FRIEND, 'in the friend DMs')
        self.heard(STRANGER_DM, STRANGER, 'a secret to aogo')
        self.heard(STRANGER_DM, STRANGER, 'and another')
        self.assertEqual(
            [(text, meta['kind']) for text, meta in self.pushes[:2]],
            [('in my DMs', 'message'), ('in the friend DMs', 'message')],
        )
        knock = f'Stranger ({STRANGER}) wrote to AogoWimsy in DMs; nothing of what they wrote reaches the session'
        self.assertEqual(self.pushes[2:], [(knock, {'kind': 'dm', 'author': 'Stranger', 'author_id': STRANGER})])
        self.assertNotIn('secret', json.dumps(self.pushes))

    def test_with_channels_listed_their_threads_count_and_others_do_not(self):
        self.heard(OTHER, OWNER, 'to aogo elsewhere', mentions=[BOT])
        self.heard(OTHER_THREAD, OWNER, 'to aogo in a thread elsewhere', mentions=[BOT])
        self.assertEqual(self.pushes, [])
        self.heard(THREAD, OWNER, 'to aogo in a thread of general', mentions=[BOT])
        self.assertEqual([text for text, _ in self.pushes], ['to aogo in a thread of general'])
        self.assertFalse(self.fake.posts('GET', f'/channels/{THREAD}'))  # known from GUILD_CREATE
        self.heard(NEW_THREAD, FRIEND, 'to aogo in a new thread of help', mentions=[BOT])
        self.assertEqual(self.pushes[-1][0], 'to aogo in a new thread of help')
        self.assertEqual(len(self.fake.posts('GET', f'/channels/{NEW_THREAD}')), 1)  # asked once
        self.heard(NEW_THREAD, FRIEND, 'again', mentions=[BOT])
        self.assertEqual(len(self.fake.posts('GET', f'/channels/{NEW_THREAD}')), 1)

    def test_with_no_channels_listed_every_channel_it_sees_counts(self):
        self.bot.settings = self.settings._replace(channels=None)
        self.heard(OTHER, OWNER, 'to aogo anywhere', mentions=[BOT])
        self.assertEqual([text for text, _ in self.pushes], ['to aogo anywhere'])

    def test_ready_and_the_guilds_push_nothing_and_ready_makes_the_waits_read_again(self):
        self.assertEqual(self.pushes, [])
        with self.bot.hub.listening(GENERAL) as inbox:
            inbox.take()
            self.ear.hear('READY', {'user': {'id': BOT}})
            self.assertEqual(inbox.take(), ([], True))

    def test_the_gateway_stopping_tells_the_session_and_the_waits(self):
        self.ear.report('stopped', 'Discord closed the gateway with 4004')
        self.assertEqual(self.bot.hub.stopped, 'Discord closed the gateway with 4004')
        self.assertEqual(self.pushes[0][1], {'kind': 'error'})
        self.assertIn('AogoWimsy stopped hearing Discord: Discord closed the gateway with 4004', self.pushes[0][0])

    def test_the_ear_runs_apart_from_the_gateway_and_stops(self):
        stop = threading.Event()
        pushes = queue.Queue()
        ear = aw.Ear(self.bot, lambda text, meta: pushes.put(text), stop)
        thread = aw.started(ear.run)
        number = self.fake.say(GENERAL, OWNER, 'to aogo', mentions=[BOT])
        ear.feed('MESSAGE_CREATE', self.fake.event(GENERAL, number))
        self.assertEqual(pushes.get(timeout=5), 'to aogo')
        ear.feed('MESSAGE_CREATE', {'channel_id': GENERAL, 'guild_id': GUILD})
        self.assertEqual(pushes.get(timeout=5), "AogoWimsy could not sort a MESSAGE_CREATE event: KeyError: 'author'")
        number = self.fake.say(GENERAL, FRIEND, 'still hearing', mentions=[BOT])
        ear.feed('MESSAGE_CREATE', self.fake.event(GENERAL, number))
        self.assertEqual(pushes.get(timeout=5), 'still hearing')
        stop.set()
        thread.join(5)
        self.assertFalse(thread.is_alive())


def ws_frame(opcode, payload, fin=True):
    """a frame as a server sends it: never masked"""
    size = len(payload)
    head = bytes([(0x80 if fin else 0) | opcode])
    if size < 126:
        head += bytes([size])
    elif size < 1 << 16:
        head += bytes([126]) + size.to_bytes(2, 'big')
    else:
        head += bytes([127]) + size.to_bytes(8, 'big')
    return head + payload


class FakeGateway:
    """Discord's gateway on 127.0.0.1: hello, READY on identify, RESUMED on resume, heartbeat ACKs unless told not to;
    each frame the client sends is kept, and checked masked"""

    def __init__(self, interval=0.05, accept_right=True):
        self.listener = socket.create_server(('127.0.0.1', 0))
        self.url = f'ws://127.0.0.1:{self.listener.getsockname()[1]}'
        self.interval, self.accept_right, self.ack = interval, accept_right, True
        self.got = queue.Queue()  # (connection, what the client sent)
        self.connections, self.paths, self.seq, self.unmasked = [], [], 0, 0
        threading.Thread(target=self.accept, daemon=True).start()

    def shut(self):
        self.listener.close()
        for conn in self.connections:
            conn.close()

    def accept(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            self.connections.append(conn)
            threading.Thread(target=self.serve, args=(conn, len(self.connections) - 1), daemon=True).start()

    def send(self, payload, conn=None, opcode=1):
        (conn or self.connections[-1]).sendall(
            ws_frame(opcode, payload if isinstance(payload, bytes) else json.dumps(payload).encode())
        )

    def dispatch(self, kind, data, conn=None):
        self.seq += 1
        self.send({'op': 0, 't': kind, 's': self.seq, 'd': data}, conn)

    def close_with(self, code):
        self.send(code.to_bytes(2, 'big') + b'bye', opcode=8)

    def serve(self, conn, number):
        try:
            head = b''
            while b'\r\n\r\n' not in head:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                head += chunk
            lines = head.decode().split('\r\n')
            self.paths.append(lines[0].split(' ')[1])
            key = next(x.split(':', 1)[1].strip() for x in lines if x.lower().startswith('sec-websocket-key:'))
            accept = base64.b64encode(hashlib.sha1((key + aw.WS_GUID).encode()).digest()).decode()
            if not self.accept_right:
                accept = 'wrong'
            conn.sendall(
                f'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                f'Sec-WebSocket-Accept: {accept}\r\n\r\n'.encode()
            )
            self.send({'op': 10, 'd': {'heartbeat_interval': self.interval * 1000}}, conn)
            buffer = b''
            while True:
                while True:
                    frame = self.parse(buffer)
                    if frame:
                        break
                    chunk = conn.recv(65536)
                    if not chunk:
                        return
                    buffer += chunk
                opcode, payload, masked, buffer = frame
                if not masked:
                    self.unmasked += 1
                elif opcode == 8:
                    self.got.put((number, {'close': int.from_bytes(payload[:2], 'big')}))
                    conn.close()
                    return
                elif opcode == 10:
                    self.got.put((number, {'pong': payload}))
                else:
                    message = json.loads(payload)
                    self.got.put((number, message))
                    if message['op'] == 1 and self.ack:
                        self.send({'op': 11}, conn)
                    elif message['op'] == 2:
                        ready = {'session_id': 'fake-session', 'resume_gateway_url': self.url, 'user': {'id': BOT}}
                        self.dispatch('READY', ready, conn)
                    elif message['op'] == 6:
                        self.dispatch('RESUMED', {}, conn)
        except OSError:
            return

    @staticmethod
    def parse(data):
        if len(data) < 2:
            return None
        opcode, masked, size, at = data[0] & 0x0F, bool(data[1] & 0x80), data[1] & 0x7F, 2
        if size == 126:
            size, at = int.from_bytes(data[2:4], 'big'), 4
        elif size == 127:
            size, at = int.from_bytes(data[2:10], 'big'), 10
        mask = data[at : at + 4] if masked else b''
        at += len(mask)
        if len(data) < at + size:
            return None
        payload = data[at : at + size]
        if masked:
            payload = bytes(byte ^ mask[place % 4] for place, byte in enumerate(payload))
        return opcode, payload, masked, data[at + size :]

    def next(self, want, timeout=5.0):
        """the next thing a connection got that want(message) takes: (connection, message)"""
        deadline = time.monotonic() + timeout
        while True:
            number, message = self.got.get(timeout=max(0.01, deadline - time.monotonic()))
            if want(message):
                return number, message


def op(code):
    return lambda message: isinstance(message, dict) and message.get('op') == code


class GatewayClient(WithDiscord):
    """aogowimsy's Gateway and WebSocket against FakeGateway; the REST /gateway/bot gives its url"""

    def setUp(self):
        super().setUp()
        self.begin()

    def begin(self, interval=0.05):
        """a fake gateway beating every interval seconds, and a client on it in a thread of its own"""
        if getattr(self, 'thread', None):
            self.stop.set()
            self.thread.join(5)
        self.gateway = FakeGateway(interval)
        self.addCleanup(self.gateway.shut)
        self.fake.gateway_url = self.gateway.url
        self.heard, self.reports, self.stop = queue.Queue(), queue.Queue(), threading.Event()
        self.client = aw.Gateway(
            TOKEN,
            self.bot.discord,
            lambda kind, data: self.heard.put((kind, data)),
            lambda kind, text: self.reports.put((kind, text)),
            self.stop,
            rng=lambda: 0.5,
            backoff=(0.01, 0.05),
        )
        self.thread = aw.started(self.client.run)
        self.addCleanup(self.thread.join, 5)
        self.addCleanup(self.stop.set)

    def kind(self, want, timeout=5.0):
        deadline = time.monotonic() + timeout
        while True:
            kind, data = self.heard.get(timeout=max(0.01, deadline - time.monotonic()))
            if kind == want:
                return data

    def test_identify_with_its_intents_then_heartbeats_carrying_the_last_seq(self):
        _, identify = self.gateway.next(op(2))
        self.assertEqual(identify['d']['token'], TOKEN)
        self.assertEqual(identify['d']['intents'], 46593)
        # 8192, DIRECT_MESSAGE_REACTIONS: without it Discord sends no reaction in the owner's DMs, and a draft he
        # answered waited for a tool's next look
        self.assertEqual(aw.INTENTS, 1 + 512 + 1024 + 4096 + 8192 + 32768)
        self.assertEqual(identify['d']['properties']['browser'], 'aogowimsy')
        self.assertEqual(self.kind('READY')['session_id'], 'fake-session')
        self.gateway.dispatch('MESSAGE_CREATE', {'id': '1', 'content': 'x'})
        self.gateway.seq = 41
        self.gateway.dispatch('MESSAGE_CREATE', {'id': '2', 'content': 'y'})
        self.assertEqual(self.kind('MESSAGE_CREATE')['id'], '1')
        self.gateway.next(lambda m: op(1)(m) and m['d'] == 42)
        self.assertEqual(self.gateway.paths[0], '/?v=10&encoding=json')
        self.assertEqual(self.client.seq, 42)
        self.assertEqual(self.gateway.unmasked, 0)

    def test_a_heartbeat_asked_for_goes_at_once(self):
        self.begin(interval=30)  # its own first beat would come in 15 s
        self.gateway.next(op(2))
        self.kind('READY')
        self.gateway.send({'op': 1, 'd': None})
        self.assertEqual(self.gateway.next(op(1), timeout=2)[1], {'op': 1, 'd': 1})

    def test_a_missed_ack_reconnects_and_resumes_the_session(self):
        self.gateway.next(op(2))
        self.kind('READY')
        self.gateway.ack = False
        number, closed = self.gateway.next(lambda m: isinstance(m, dict) and 'close' in m)
        self.assertEqual((number, closed['close']), (0, 4000))
        number, resume = self.gateway.next(op(6))
        self.assertEqual(number, 1)
        self.assertEqual((resume['d']['session_id'], resume['d']['token']), ('fake-session', TOKEN))
        self.assertEqual(resume['d']['seq'], 1)
        self.kind('RESUMED')

    def test_reconnect_resumes_an_invalid_session_identifies_again(self):
        self.gateway.next(op(2))
        self.kind('READY')
        self.gateway.send({'op': 7, 'd': None})
        self.assertEqual(self.gateway.next(op(6))[0], 1)
        self.kind('RESUMED')
        self.gateway.send({'op': 9, 'd': False})
        self.assertEqual(self.gateway.next(op(2))[0], 2)
        self.kind('READY')
        self.gateway.close_with(4009)
        self.assertEqual(self.gateway.next(op(2))[0], 3)

    def test_a_close_discord_takes_no_new_try_after_stops_it_saying_why(self):
        self.gateway.next(op(2))
        self.kind('READY')
        self.gateway.close_with(4014)
        kind, text = self.reports.get(timeout=5)
        self.assertEqual(kind, 'stopped')
        self.assertIn(
            'Discord closed the gateway with 4014 (bye): disallowed intents: turn on Message Content Intent', text
        )
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(len(self.gateway.connections), 1)

    def test_a_bad_token_stops_it(self):
        self.gateway.next(op(2))
        self.gateway.close_with(4004)
        self.assertIn(
            "4004 (bye): authentication failed: AOGOWIMSY_TOKEN is not the bot's token", self.reports.get(timeout=5)[1]
        )

    def test_a_ping_gets_its_pong_and_a_message_in_two_frames_comes_whole(self):
        self.gateway.next(op(2))
        self.kind('READY')
        self.gateway.send(b'are you there', opcode=9)
        self.assertEqual(
            self.gateway.next(lambda m: isinstance(m, dict) and 'pong' in m)[1], {'pong': b'are you there'}
        )
        whole = json.dumps({'op': 0, 't': 'MESSAGE_CREATE', 's': 9, 'd': {'id': '3', 'content': 'two halves'}}).encode()
        conn = self.gateway.connections[-1]
        conn.sendall(ws_frame(1, whole[:20], fin=False) + ws_frame(0, whole[20:]))
        self.assertEqual(self.kind('MESSAGE_CREATE')['content'], 'two halves')

    def test_a_stop_closes_with_1000(self):
        self.gateway.next(op(2))
        self.kind('READY')
        self.stop.set()
        self.assertEqual(self.gateway.next(lambda m: isinstance(m, dict) and 'close' in m)[1], {'close': 1000})


class GatewayRefusals(WithDiscord):
    def test_a_gateway_url_that_is_not_wss_is_refused_naming_it(self):
        with self.assertRaisesRegex(aw.DiscordError, 'the gateway url "ws://example.com/" is not wss://'):
            aw.WebSocket('ws://example.com/')
        with self.assertRaisesRegex(aw.DiscordError, 'is not wss://'):
            aw.WebSocket('http://127.0.0.1:9/')

    def test_a_wrong_handshake_answer_is_refused(self):
        gateway = FakeGateway(accept_right=False)
        self.addCleanup(gateway.shut)
        with self.assertRaisesRegex(ConnectionError, 'Sec-WebSocket-Accept "wrong", not '):
            aw.WebSocket(gateway.url + '/?v=10&encoding=json')

    def test_a_401_for_the_gateway_url_stops_it(self):
        self.fake.fail[('GET', '/gateway/bot')] = (401, {'message': '401: Unauthorized', 'code': 0})
        reports, stop = [], threading.Event()
        self.addCleanup(stop.set)
        report = lambda kind, text: reports.append((kind, text))
        client = aw.Gateway(TOKEN, self.bot.discord, None, report, stop, rng=lambda: 0.5, backoff=(0.01, 0.05))
        thread = aw.started(client.run)
        thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(
            reports, [('stopped', "Discord answered 401 to GET /gateway/bot: AOGOWIMSY_TOKEN is not the bot's token")]
        )

    def test_from_the_gateway_to_the_session_end_to_end(self):
        gateway = FakeGateway()
        self.addCleanup(gateway.shut)
        self.fake.gateway_url = gateway.url
        stop, pushes = threading.Event(), queue.Queue()
        self.addCleanup(stop.set)
        ear = aw.Ear(self.bot, lambda text, meta: pushes.put((text, meta)), stop)
        client = aw.Gateway(TOKEN, self.bot.discord, ear.feed, ear.report, stop, rng=lambda: 0.5, backoff=(0.01, 0.05))
        threads = [aw.started(ear.run), aw.started(client.run)]
        gateway.next(op(2))
        number = self.fake.say(GENERAL, OWNER, 'to aogo, live', mentions=[BOT])
        gateway.dispatch('MESSAGE_CREATE', self.fake.event(GENERAL, number))
        text, meta = pushes.get(timeout=5)
        self.assertEqual((text, meta['message_id'], meta['kind']), ('to aogo, live', number, 'message'))
        stop.set()
        for thread in threads:
            thread.join(5)
            self.assertFalse(thread.is_alive())


class Writer(unittest.TestCase):
    def test_lines_from_many_threads_never_mix(self):
        class Slow:
            """a stream that writes a line in two halves, with time for another thread between them"""

            def __init__(self):
                self.data = bytearray()

            def write(self, line):
                half = len(line) // 2
                self.data += line[:half]
                time.sleep(0.002)
                self.data += line[half:]

            def flush(self):
                pass

        stream = Slow()
        write = aw.LineWriter(stream)

        def many(who):
            for n in range(25):
                write({'jsonrpc': '2.0', 'method': 'notifications/claude/channel', 'params': {'content': f'{who} {n}'}})

        threads = [aw.started(lambda who=who: many(who)) for who in 'abcd']
        for thread in threads:
            thread.join(10)
        lines = bytes(stream.data).split(b'\n')
        self.assertEqual(lines[-1], b'')
        self.assertEqual(len([json.loads(line) for line in lines[:-1]]), 100)


class Stdio(unittest.TestCase):
    """the real program, as Claude Code starts it: a line a message on stdin and stdout, logs on stderr. It is never
    told initialized here, so it never reaches for Discord's gateway"""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.env = write_env(self.folder.name, GOOD_ENV)

    def start(self, *args):
        command = [sys.executable, str(HERE / 'aogowimsy.py'), *args]
        proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        watchdog = threading.Timer(20, proc.kill)
        watchdog.start()
        self.addCleanup(watchdog.cancel)
        return proc

    def exchange(self, proc, message):
        line = message if isinstance(message, bytes) else json.dumps(message).encode()
        proc.stdin.write(line + b'\n')
        proc.stdin.flush()
        answer = proc.stdout.readline()
        self.assertTrue(answer.endswith(b'\n') and answer.isascii(), answer)
        return json.loads(answer)

    def finish(self, proc):
        proc.stdin.close()
        self.assertEqual(proc.wait(), 0)
        rest, errors = proc.stdout.read(), proc.stderr.read().decode()
        proc.stdout.close()
        proc.stderr.close()
        self.assertEqual(rest, b'')
        self.assertNotIn(TOKEN, errors)
        self.assertNotIn('Traceback', errors)
        return errors

    def test_the_handshake_tools_list_and_a_tool_call_through_a_subprocess(self):
        proc = self.start('--env', self.env)
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': INITIALIZE})
        self.assertEqual((answer['id'], answer['result']['protocolVersion']), (1, '2025-11-25'))
        self.assertEqual(answer['result']['capabilities'], {'tools': {}, 'experimental': {'claude/channel': {}}})
        self.assertIn(f'channels {GENERAL}, {HELP} and their threads', answer['result']['instructions'])
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 'list', 'method': 'tools/list'})
        self.assertEqual(answer, {'jsonrpc': '2.0', 'id': 'list', 'result': {'tools': aw.TOOLS}})
        self.assertEqual(self.exchange(proc, {'jsonrpc': '2.0', 'id': 3, 'method': 'ping'})['result'], {})
        answer = self.exchange(proc, b'{"jsonrpc": "2.0", "id": 4, "method": ')
        self.assertEqual(answer['error']['code'], -32700)
        self.assertNotIn('id', answer)
        call = {'name': 'discord_read', 'arguments': {'channel': 'general'}}
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 5, 'method': 'tools/call', 'params': call})
        self.assertTrue(answer['result']['isError'])
        self.assertEqual(
            answer['result']['content'][0]['text'], 'channel is "general", not a Discord id (17 to 20 digits)'
        )
        errors = self.finish(proc)
        self.assertIn(f'aogowimsy: up: owner {OWNER}, 1 users, 2 channels', errors)
        self.assertIn('aogowimsy: discord_read failed: channel is "general"', errors)

    def test_a_2026_07_28_client_gets_method_not_found_for_discovery(self):
        proc = self.start('--env', self.env)
        meta = {
            'io.modelcontextprotocol/protocolVersion': '2026-07-28',
            'io.modelcontextprotocol/clientCapabilities': {},
        }
        answer = self.exchange(
            proc, {'jsonrpc': '2.0', 'id': 1, 'method': 'server/discover', 'params': {'_meta': meta}}
        )
        self.assertEqual(answer['error']['code'], -32601)
        self.finish(proc)

    def test_a_bad_env_file_or_option_stops_it_naming_what(self):
        os.chmod(self.env, 0o644)
        proc = self.start('--env', self.env)
        self.assertEqual(proc.wait(), 1)
        self.assertIn(f'aogowimsy: {self.env} has mode 644', proc.stderr.read().decode())
        self.assertEqual(proc.stdout.read(), b'')
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            stream.close()
        proc = self.start('--bogus')
        self.assertEqual(proc.wait(), 2)
        self.assertIn('unrecognized arguments: --bogus', proc.stderr.read().decode())
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            stream.close()


if __name__ == '__main__':
    unittest.main()
