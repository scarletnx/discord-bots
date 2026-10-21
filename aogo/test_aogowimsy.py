#!/usr/bin/env python3
"""aogowimsy.py without Discord or the network: its env file, MCP over stdio in a real subprocess, and its tools
against a fake Discord served on 127.0.0.1, with a clock that only moves when a tool waits"""

import http.server
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.parse
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import aogowimsy as aw

os.environ['no_proxy'] = os.environ['NO_PROXY'] = '127.0.0.1'  # the fake Discord, never through a proxy

TOKEN = 'MTAwMDAw.fake-token_for.tests'
BOT, OWNER, FRIEND, STRANGER = (f'10000000000000000{n:02d}' for n in (1, 2, 3, 4))
GUILD = '200000000000000001'
GENERAL, HELP, OTHER, THREAD, OTHER_THREAD = (f'3000000000000000{n:02d}' for n in (1, 2, 3, 11, 12))
DM = '400000000000000001'
YES, NO = aw.YES, aw.NO
META = {aw.VERSION_KEY: '2026-07-28', aw.CAPABILITIES_KEY: {}}


class FakeDiscord:
    """Discord's REST routes AogoWimsy uses, over an in-memory guild: answers as Discord does, newest first"""

    ROUTES = (
        ('GET', r'/users/@me', 'me'),
        ('POST', r'/users/@me/channels', 'open_dm'),
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
        }
        guild = {'type': 0, 'guild_id': GUILD}
        self.channels = {
            GENERAL: dict(guild, id=GENERAL, name='general'),
            HELP: dict(guild, id=HELP, name='help'),
            OTHER: dict(guild, id=OTHER, name='other'),
            THREAD: dict(guild, id=THREAD, name='a-thread', type=11, parent_id=GENERAL),
            OTHER_THREAD: dict(guild, id=OTHER_THREAD, name='elsewhere', type=11, parent_id=OTHER),
            DM: {'id': DM, 'type': 1, 'recipients': [self.users[OWNER]]},
        }
        self.messages = {channel: [] for channel in self.channels}  # oldest first
        self.reactions = {}  # a message id: {emoji: [user ids]}
        self.requests = []  # (method, path, query, body)
        self.limits = []  # (method, path) answered 429 once each
        self.fail = {}  # (method, path): (status, body), answered until removed
        self.serial = 1_500_000_000_000_000_000
        self.lock = threading.Lock()

    def say(self, channel, author, content='', **extra):
        """a message from author; its id"""
        self.serial += 1
        message = {'id': str(self.serial), 'channel_id': channel, 'author': author, 'content': content, 'type': 0}
        message.update(timestamp=f'2026-10-21T12:{len(self.messages[channel]) % 60:02d}:00.000000+00:00')
        message.update(attachments=[], embeds=[])
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

    def render(self, message):
        shown = {k: v for k, v in message.items() if k != 'author'}
        shown['author'] = self.users.get(message['author'], {'id': message['author'], 'username': 'webhook'})
        reactions = self.reactions.get(message['id'], {})
        if reactions:
            shown['reactions'] = [
                {'count': len(who), 'me': BOT in who, 'emoji': {'id': None, 'name': emoji}}
                for emoji, who in reactions.items()
            ]
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

    def open_dm(self, query, body):
        if body != {'recipient_id': OWNER}:
            return 400, {'message': f'fake: DMs only with the owner, not {body}', 'code': 0}, {}
        return 200, self.channels[DM], {}

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
        content = body.get('content', '')
        if len(content) > 2000:
            errors = {
                'content': {
                    '_errors': [{'code': 'BASE_TYPE_MAX_LENGTH', 'message': 'Must be 2000 or fewer in length.'}]
                }
            }
            return 400, {'code': 50035, 'errors': errors, 'message': 'Invalid Form Body'}, {}
        reference = body.get('message_reference')
        extra = {'embeds': body.get('embeds', [])}
        if reference:
            if not self.find(channel, reference['message_id']):
                return 400, {'code': 50035, 'message': 'Invalid Form Body', 'errors': {}}, {}
            extra.update(type=19, message_reference=reference)
        message = self.find(channel, self.say(channel, BOT, content, **extra))
        return 200, self.render(message), {}

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


class Handler(http.server.BaseHTTPRequestHandler):
    def answer(self):
        parts = urllib.parse.urlsplit(self.path)
        query = dict(urllib.parse.parse_qsl(parts.query))
        length = int(self.headers.get('Content-Length') or 0)
        body = json.loads(self.rfile.read(length)) if length else None
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
        self.assertIn(
            'line 7: AOGOWIMSY_OWNER again, set on line 3', self.refused(GOOD_ENV + f'AOGOWIMSY_OWNER={OWNER}\n')
        )
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
        self.clock = FakeClock()
        self.settings = aw.Settings(TOKEN, OWNER, frozenset({FRIEND}), frozenset({GENERAL, HELP}))
        api = f'http://127.0.0.1:{self.http.server_port}/api/v10'
        self.bot = aw.AogoWimsy(self.settings, aw.Discord(TOKEN, self.clock, api), self.clock)
        self.sent = []
        self.server = aw.Server(self.bot, 'test', self.sent.append)
        self.logs = []
        self.addCleanup(setattr, aw, 'log', aw.log)
        aw.log = self.logs.append

    def answer(self, name, meta=META, cancelled=None, **args):
        params = {'name': name, 'arguments': args, '_meta': meta}
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
        self.assertEqual(
            body['message_reference'], {'message_id': asked, 'channel_id': GENERAL, 'fail_if_not_exists': True}
        )
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
        self.assertEqual(
            got,
            {
                'status': 'waiting',
                'draft': draft,
                'link': f'https://discord.com/channels/@me/{DM}/{draft}',
                'waited': 7,
            },
        )
        self.assertEqual(self.clock.waits, [3.0, 3.0, 1.0])
        self.assertEqual(self.fake.messages[GENERAL], [])
        self.assertEqual(self.tool('discord_post_status', draft=draft, wait=0)['status'], 'waiting')
        self.fake.react(draft, YES, OWNER)
        sent = self.tool('discord_post_status', draft=draft)
        self.assertEqual((sent['status'], sent['draft']), ('sent', draft))
        again = self.tool('discord_post_status', draft=draft)
        self.assertEqual(again, sent)
        self.assertEqual(len(self.fake.messages[GENERAL]), 1)

    def test_a_restart_loses_no_draft_the_draft_is_the_state(self):
        draft = self.tool('discord_post', channel=GENERAL, text='after a restart', wait=0)['draft']
        api = f'http://127.0.0.1:{self.http.server_port}/api/v10'
        fresh = aw.AogoWimsy(self.settings, aw.Discord(TOKEN, self.clock, api), self.clock)
        self.server = aw.Server(fresh, 'test', self.sent.append)
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
        self.assertIn(
            f'channel {OTHER} is not one AogoWimsy may post in: AOGOWIMSY_CHANNELS lists {GENERAL}, {HELP}', said
        )
        said = self.error('discord_post', channel=OTHER_THREAD, text='hello')
        self.assertIn(f'channel {OTHER_THREAD} is not one AogoWimsy may post in', said)
        said = self.error('discord_post', channel='3000000000000000099', text='hello')
        self.assertIn('Discord answered 404 to GET /channels/3000000000000000099: Unknown Channel (code 10003)', said)
        self.assertEqual(self.fake.messages[DM], [])

    def test_a_thread_of_a_listed_channel_is_one(self):
        self.owner_answers(3, YES)
        got = self.tool('discord_post', channel=THREAD, text='in the thread')
        self.assertEqual((got['status'], self.fake.last(THREAD)['content']), ('sent', 'in the thread'))
        card = self.fake.posts('POST', f'/channels/{DM}/messages')[0][3]['embeds'][0]
        self.assertIn(f'#a-thread ({THREAD})', card['description'])

    def test_a_text_over_2000_letters_is_refused_naming_its_length(self):
        self.assertIn(
            'text has 2001 letters: a Discord message holds 2000 at most',
            self.error('discord_post', channel=GENERAL, text='x' * 2001),
        )
        self.assertEqual(self.fake.messages[DM], [])
        self.assertEqual(self.tool('discord_post', channel=GENERAL, text='x' * 2000, wait=0)['status'], 'waiting')
        self.assertIn('text is empty', self.error('discord_post', channel=GENERAL, text=' \n'))

    def test_arguments_are_checked_before_discord_hears_of_them(self):
        said = self.error('discord_post', channel=int(GENERAL), text='hi')
        self.assertIn('channel is 300000000000000001, not a Discord id: write it as a string', said)
        self.assertIn(
            'wait is 3601, not a whole number from 0 to 3600',
            self.error('discord_post', channel=GENERAL, text='hi', wait=3601),
        )
        self.assertIn(
            'wait is true, not a whole number', self.error('discord_post', channel=GENERAL, text='hi', wait=True)
        )
        self.assertIn(
            'discord_post has no argument "mention"; it takes channel, text, reply_to, wait',
            self.error('discord_post', channel=GENERAL, text='hi', mention=True),
        )
        self.assertIn('discord_post needs text', self.error('discord_post', channel=GENERAL))
        self.assertIn('text is a JSON array, not a string', self.error('discord_post', channel=GENERAL, text=['hi']))
        self.assertEqual(self.fake.requests, [])

    def test_post_status_takes_only_its_own_drafts(self):
        mine = self.fake.say(DM, OWNER, 'a message of the owner')
        self.assertIn(
            f"message {mine} in the owner's DMs ({DM}) is not a draft of AogoWimsy's",
            self.error('discord_post_status', draft=mine),
        )
        plain = self.fake.say(DM, BOT, 'a plain message of its own')
        self.assertIn('is not a draft', self.error('discord_post_status', draft=plain))

    def test_a_post_discord_refuses_shows_its_error_and_the_draft_waits_again(self):
        self.fake.fail[('POST', f'/channels/{GENERAL}/messages')] = (
            403,
            {'message': 'Missing Permissions', 'code': 50013},
        )
        self.owner_answers(3, YES)
        said = self.error('discord_post', channel=GENERAL, text='hello')
        self.assertIn(
            f'Discord answered 403 to POST /channels/{GENERAL}/messages: Missing Permissions (code 50013)', said
        )
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
        self.assertIn(
            f'channel {HELP} is not one AogoWimsy may post in', self.error('discord_post_status', draft=got['draft'])
        )
        self.assertEqual(self.fake.messages[HELP], [])


class Read(WithDiscord):
    def conversation(self, channel):
        ids = [self.fake.say(channel, OWNER, 'first', attachments=[{'filename': 'build.log', 'url': 'x'}])]
        ids.append(self.fake.say(channel, STRANGER, 'a secret of the stranger'))
        ids.append(self.fake.say(channel, FRIEND, 'a reply', type=19, message_reference={'message_id': ids[0]}))
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
        self.assertEqual((first['attachments'], first['reply_to']), (['build.log'], None))
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
        self.assertIn(
            'limit is 101, not a whole number from 1 to 100', self.error('discord_read', channel=GENERAL, limit=101)
        )

    def test_a_thread_of_a_listed_channel_and_the_owners_dm_read_without_being_listed(self):
        ids = self.conversation(THREAD)
        got = self.tool('discord_read', channel=THREAD)
        self.assertEqual(([m['id'] for m in got['messages']], got['left_out']), ([ids[0], ids[2], ids[3]], 2))
        self.fake.say(DM, OWNER, 'in the DMs')
        self.assertEqual([m['text'] for m in self.tool('discord_read', channel=DM)['messages']], ['in the DMs'])

    def test_other_channels_are_refused_naming_what_it_reads(self):
        said = self.error('discord_read', channel=OTHER)
        self.assertIn(
            f'channel {OTHER} is not one AogoWimsy may read: AOGOWIMSY_CHANNELS lists {GENERAL}, {HELP}', said
        )
        self.assertIn(f"the owner's DMs with AogoWimsy are {DM}", said)
        self.assertIn('may read', self.error('discord_read', channel=OTHER_THREAD))
        self.assertFalse(self.fake.posts('GET', f'/channels/{OTHER}/messages'))


class Wait(WithDiscord):
    def test_the_first_message_from_the_owner_or_a_listed_user_passing_over_the_others(self):
        start = self.fake.say(GENERAL, OWNER, 'go')
        self.fake.say(GENERAL, STRANGER, 'noise')
        self.fake.say(GENERAL, BOT, 'its own')
        self.clock.at(6, lambda: self.fake.say(GENERAL, STRANGER, 'more noise'))
        self.clock.at(9, lambda: self.fake.say(GENERAL, FRIEND, 'here'))
        got = self.tool('discord_wait', channel=GENERAL, after=start)
        self.assertEqual((got['message']['text'], got['message']['author_id'], got['left_out']), ('here', FRIEND, 2))
        self.assertEqual(got['after'], got['message']['id'])
        self.assertEqual(self.clock.waits, [3.0, 3.0, 3.0])
        self.assertNotIn('noise', json.dumps(got))

    def test_none_in_time_says_so_with_the_id_to_wait_after_next(self):
        start = self.fake.say(GENERAL, OWNER, 'go')
        noise = self.fake.say(GENERAL, STRANGER, 'noise')
        got = self.tool('discord_wait', channel=GENERAL, after=start, wait=5)
        self.assertEqual(got, {'channel': GENERAL, 'message': None, 'waited': 5, 'left_out': 1, 'after': noise})

    def test_a_busy_channel_is_read_on_at_once_page_after_page(self):
        start = self.fake.say(GENERAL, OWNER, 'go')
        for n in range(150):
            self.fake.say(GENERAL, STRANGER, f'noise {n}')
        self.fake.say(GENERAL, OWNER, 'mine')
        got = self.tool('discord_wait', channel=GENERAL, after=start)
        self.assertEqual((got['message']['text'], got['left_out'], self.clock.waits), ('mine', 150, []))

    def test_a_thread_of_a_listed_channel_and_the_owners_dm(self):
        start = self.fake.say(THREAD, FRIEND, 'start')
        self.clock.at(3, lambda: self.fake.say(THREAD, OWNER, 'in the thread'))
        self.assertEqual(self.tool('discord_wait', channel=THREAD, after=start)['message']['text'], 'in the thread')
        start = self.fake.say(DM, BOT, 'a draft')
        self.clock.at(6, lambda: self.fake.say(DM, OWNER, 'answer in the DMs'))
        self.assertEqual(self.tool('discord_wait', channel=DM, after=start)['message']['text'], 'answer in the DMs')
        self.assertIn('may read', self.error('discord_wait', channel=OTHER, after=start))


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
        said = aw.reason(
            {
                'code': 50035,
                'message': 'Invalid Form Body',
                'errors': {'content': {'_errors': [{'message': 'Must be 2000 or fewer in length.'}]}},
            }
        )
        self.assertEqual(said, 'Invalid Form Body (code 50035): content: Must be 2000 or fewer in length.')


class Protocol(WithDiscord):
    """the JSON-RPC answers, in process"""

    def ask(self, method, params=None, rid=1):
        message = {'jsonrpc': '2.0', 'id': rid, 'method': method}
        if params is not None:
            message['params'] = params
        return self.server.handle(message)

    def test_initialize_answers_the_clients_version_when_it_has_it_else_its_newest(self):
        for asked, agreed in (('2025-06-18', '2025-06-18'), ('2024-11-05', '2024-11-05'), ('2099-01-01', '2025-11-25')):
            server = aw.Server(self.bot, 'test', self.sent.append)
            params = {'protocolVersion': asked, 'capabilities': {}, 'clientInfo': {'name': 't', 'version': '1'}}
            result = server.handle({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': params})['result']
            self.assertEqual(result['protocolVersion'], agreed)
            self.assertEqual(result['capabilities'], {'tools': {}})
            self.assertEqual(result['serverInfo']['name'], 'aogowimsy')
        self.assertEqual(self.ask('initialize', {})['error']['code'], -32602)

    def test_before_initialize_and_with_no_version_a_request_is_refused_saying_what_it_takes(self):
        error = self.ask('tools/list')['error']
        self.assertEqual(error['code'], -32602)
        self.assertIn(
            '2026-07-28 with it, and 2025-11-25, 2025-06-18, 2025-03-26, 2024-11-05 after initialize', error['message']
        )
        self.assertEqual(self.ask('ping'), {'jsonrpc': '2.0', 'id': 1, 'result': {}})
        self.ask('initialize', {'protocolVersion': '2025-11-25'})
        self.assertEqual(self.ask('tools/list')['result']['tools'], aw.TOOLS)
        self.assertNotIn('resultType', self.ask('tools/list')['result'])

    def test_2026_07_28_requests_carry_their_version_and_get_results_with_resultType(self):
        result = self.ask('tools/list', {'_meta': META})['result']
        self.assertEqual(
            [t['name'] for t in result['tools']],
            ['discord_post', 'discord_post_status', 'discord_read', 'discord_wait'],
        )
        self.assertEqual(
            (result['resultType'], result['ttlMs'], result['cacheScope']), ('complete', 3600000, 'private')
        )
        self.assertEqual(result['_meta'], {aw.SERVER_INFO_KEY: aw.SERVER_INFO})
        found = self.ask('server/discover', {'_meta': META})['result']
        self.assertEqual(
            found['supportedVersions'], ['2026-07-28', '2025-11-25', '2025-06-18', '2025-03-26', '2024-11-05']
        )
        self.assertEqual((found['capabilities'], found['instructions']), ({'tools': {}}, 'test'))

    def test_a_version_it_lacks_is_unsupported_naming_those_it_has(self):
        error = self.ask('tools/list', {'_meta': {aw.VERSION_KEY: '1900-01-01', aw.CAPABILITIES_KEY: {}}})['error']
        self.assertEqual(error['code'], -32022)
        self.assertEqual(
            error['data'],
            {
                'supported': ['2026-07-28', '2025-11-25', '2025-06-18', '2025-03-26', '2024-11-05'],
                'requested': '1900-01-01',
            },
        )
        error = self.ask('tools/list', {'_meta': {aw.VERSION_KEY: '2026-07-28'}})['error']
        self.assertEqual(error['code'], -32602)
        self.assertIn(aw.CAPABILITIES_KEY, error['message'])

    def test_methods_it_has_not_and_unknown_tools(self):
        error = self.ask('resources/list', {'_meta': META})['error']
        self.assertEqual(error['code'], -32601)
        self.assertIn(
            '"resources/list"; AogoWimsy answers initialize, ping, server/discover, tools/list, tools/call',
            error['message'],
        )
        self.assertEqual(self.ask('ping', {'_meta': META})['error']['code'], -32601)
        error = self.ask('tools/call', {'name': 'discord_delete', 'arguments': {}, '_meta': META})['error']
        self.assertEqual(error['code'], -32602)
        self.assertIn('Unknown tool: "discord_delete"; AogoWimsy has discord_post', error['message'])
        self.assertEqual(self.ask('tools/list', {'_meta': META, 'cursor': 'x'})['error']['code'], -32602)

    def test_notifications_and_bad_messages(self):
        self.assertIsNone(self.server.handle({'jsonrpc': '2.0', 'method': 'notifications/initialized'}))
        self.assertIsNone(
            self.server.handle({'jsonrpc': '2.0', 'method': 'notifications/cancelled', 'params': {'requestId': 9}})
        )
        self.assertEqual(self.server.handle({'id': 1, 'method': 'ping'})['error']['code'], -32600)
        self.assertEqual(self.server.handle({'jsonrpc': '2.0', 'id': None, 'method': 'ping'})['error']['code'], -32600)
        self.assertEqual(
            self.server.handle({'jsonrpc': '2.0', 'id': 2, 'method': 'ping', 'params': [1]})['error']['code'], -32602
        )

    def test_a_cancelled_wait_stops_and_gets_no_answer(self):
        cancelled = threading.Event()
        self.clock.at(6, cancelled.set)
        start = self.fake.say(GENERAL, OWNER, 'go')
        self.assertIsNone(self.answer('discord_wait', cancelled=cancelled, channel=GENERAL, after=start))
        self.assertEqual(self.clock.waits, [3.0, 3.0])

    def test_progress_when_asked_for_it_always_growing(self):
        start = self.fake.say(GENERAL, OWNER, 'go')
        self.clock.at(9, lambda: self.fake.say(GENERAL, FRIEND, 'here'))
        answer = self.answer('discord_wait', meta=dict(META, progressToken='p1'), channel=GENERAL, after=start)
        here = json.loads(answer['result']['content'][0]['text'])['message']
        self.assertEqual(here['text'], 'here')
        sent = [(n['params']['progressToken'], n['params']['progress'], n['params']['total']) for n in self.sent]
        self.assertEqual(sent, [('p1', 3.0, 600), ('p1', 6.0, 600), ('p1', 9.0, 600)])
        self.assertTrue(all(n['method'] == 'notifications/progress' for n in self.sent))
        self.assertIsNone(self.tool('discord_wait', channel=GENERAL, after=here['id'], wait=6)['message'])
        self.assertEqual(len(self.sent), 3)  # asked for none: none sent

    def test_progress_never_repeats_nor_goes_back_and_stops_once_cancelled(self):
        cancelled = threading.Event()
        send = self.server.progress(5, cancelled)
        for done in (1.0, 1.02, 0.5, 2.0):
            send(done, 10, 'waiting')
        cancelled.set()
        send(3.0, 10, 'waiting')
        self.assertEqual([n['params']['progress'] for n in self.sent], [1.0, 2.0])


class Stdio(unittest.TestCase):
    """the real program, as Claude Code starts it: a line a message on stdin and stdout, logs on stderr"""

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
        params = {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'test', 'version': '1'}}
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': params})
        self.assertEqual((answer['id'], answer['result']['protocolVersion']), (1, '2025-11-25'))
        self.assertIn(f'channels {GENERAL}, {HELP} and their threads', answer['result']['instructions'])
        proc.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
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

    def test_2026_07_28_discovery_then_tools_list(self):
        proc = self.start('--env', self.env)
        answer = self.exchange(
            proc, {'jsonrpc': '2.0', 'id': 1, 'method': 'server/discover', 'params': {'_meta': META}}
        )
        self.assertEqual(
            (answer['result']['resultType'], answer['result']['supportedVersions'][0]), ('complete', '2026-07-28')
        )
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list', 'params': {'_meta': META}})
        self.assertEqual(len(answer['result']['tools']), 4)
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
