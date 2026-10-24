#!/usr/bin/env python3
"""xujiwimsy.py without Discord or the network: what makes XujiWimsy Xuji's (its env file and keys, its server's
name, no claude/channel, its instructions, its tools in its names), the client each Wimsy serves, the news a client
with no channel gets with its next tool result, a post as it reaches Discord, and the program on stdin and stdout.
AogoWimsy's suite tests the code they share; its fake Discord serves here too."""

import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'aogo'))
import aogowimsy as aw
import test_aogowimsy as base  # its fake Discord, clock and constants

TOKEN, OWNER, FRIEND, STRANGER, BOT = base.TOKEN, base.OWNER, base.FRIEND, base.STRANGER, base.BOT
GENERAL, HELP, OTHER, DM, GUILD = base.GENERAL, base.HELP, base.OTHER, base.DM, base.GUILD
YES = aw.YES
# what Grok Build 1.0.60 sends (grok mcp doctor against a server of that name logged it)
GROK = {
    'protocolVersion': '2025-11-25',
    'capabilities': {},
    'clientInfo': {'name': 'grok-shell-xujiwimsy', 'version': '1'},
}
CLAUDE = base.INITIALIZE
GOOD_ENV = base.GOOD_ENV.replace('AOGOWIMSY_', 'XUJIWIMSY_').replace('# AogoWimsy', '# XujiWimsy')
KNOWS = 'XUJIWIMSY_TOKEN, XUJIWIMSY_OWNER, XUJIWIMSY_USERS, XUJIWIMSY_CHANNELS'


def initialize(server, params):
    return server.handle({'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': params})


class EnvFile(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)

    def refused(self, text, identity=aw.XUJI, mode=0o600):
        with self.assertRaises(aw.Refused) as caught:
            aw.read_settings(base.write_env(self.folder.name, text, mode, 'bot.env'), identity)
        self.assertNotIn(TOKEN, str(caught.exception))
        return str(caught.exception)

    def test_its_file_takes_the_xujiwimsy_keys_with_aogowimsys_meanings(self):
        path = base.write_env(self.folder.name, GOOD_ENV, name='xujiwimsy.env')
        settings = aw.read_settings(path, aw.XUJI)
        self.assertEqual(settings, aw.Settings(TOKEN, OWNER, frozenset({FRIEND}), frozenset({GENERAL, HELP}), aw.XUJI))
        every = aw.read_settings(
            base.write_env(self.folder.name, GOOD_ENV.replace(f'XUJIWIMSY_CHANNELS={GENERAL}, {HELP}\n', '')), aw.XUJI
        )
        self.assertIsNone(every.channels)
        self.assertIn(
            'every channel and thread Discord lets it see (XUJIWIMSY_CHANNELS lists none)', aw.instructions(every)
        )
        self.assertEqual(aw.XUJI.env, '~/.config/xujiwimsy/xujiwimsy.env')

    def test_an_aogowimsy_key_in_its_file_stops_it_naming_the_key_and_the_reverse(self):
        said = self.refused(GOOD_ENV + f'AOGOWIMSY_TOKEN={TOKEN}\n')
        self.assertIn(f'line 7: AOGOWIMSY_TOKEN is not a key XujiWimsy knows; it knows {KNOWS}', said)
        said = self.refused(base.GOOD_ENV)  # AogoWimsy's whole file
        self.assertIn('line 2: AOGOWIMSY_TOKEN is not a key XujiWimsy knows', said)
        said = self.refused(base.GOOD_ENV + f'XUJIWIMSY_OWNER={OWNER}\n', aw.AOGO)
        self.assertIn('line 7: XUJIWIMSY_OWNER is not a key AogoWimsy knows; it knows AOGOWIMSY_TOKEN', said)
        said = self.refused(GOOD_ENV, aw.AOGO)
        self.assertIn('line 2: XUJIWIMSY_TOKEN is not a key AogoWimsy knows', said)

    def test_its_values_are_checked_as_aogowimsys_naming_its_keys(self):
        self.assertIn('XUJIWIMSY_TOKEN is empty, and XujiWimsy needs it', self.refused(GOOD_ENV.replace(TOKEN, '')))
        said = self.refused(GOOD_ENV.replace(f'XUJIWIMSY_USERS = {FRIEND}', ''))
        self.assertIn('XUJIWIMSY_USERS is missing, and XujiWimsy needs it', said)
        said = self.refused(GOOD_ENV.replace(f'{GENERAL}, {HELP}', ''))
        self.assertIn('XUJIWIMSY_CHANNELS is empty: list id,id, or take the line out', said)
        said = self.refused(GOOD_ENV.replace(f'_OWNER={OWNER}', '_OWNER=12345'))
        self.assertIn('XUJIWIMSY_OWNER is "12345", not a Discord id', said)
        self.assertIn("XUJIWIMSY_TOKEN has '\"' at letter 1", self.refused(GOOD_ENV.replace(TOKEN, f'"{TOKEN}"')))
        self.assertIn('has mode 644', self.refused(GOOD_ENV, mode=0o644))
        with self.assertRaisesRegex(aw.Refused, 'nowhere.env cannot be read .*: XujiWimsy reads its settings there'):
            aw.read_settings(str(Path(self.folder.name) / 'nowhere.env'), aw.XUJI)

    def test_the_example_lists_its_keys_and_stops_at_the_first_empty_one(self):
        example = base.write_env(self.folder.name, (HERE / 'xujiwimsy.env.example').read_text(), name='example.env')
        with self.assertRaisesRegex(aw.Refused, 'XUJIWIMSY_TOKEN is empty, and XujiWimsy needs it'):
            aw.read_settings(example, aw.XUJI)


class WithXuji(base.WithDiscord):
    """XujiWimsy's tools through its server, initialized by Grok Build, against the fake Discord"""

    def setUp(self):
        super().setUp()
        self.settings = self.settings._replace(identity=aw.XUJI)
        self.bot = aw.Wimsy(self.settings, aw.Discord(TOKEN, self.clock, self.api, identity=aw.XUJI), self.clock)
        self.server = self.started_server(self.bot)

    def started_server(self, bot, **extra):
        """bot's server, initialized by its client: Grok Build for XujiWimsy (AogoWimsy's in the base's setUp)"""
        server = aw.Server(bot, aw.instructions(bot.settings), self.sent.append, **extra)
        self.assertIn('result', initialize(server, CLAUDE if bot.settings.identity.claude else GROK))
        return server

    def aogo_server(self, **extra):
        aogo = aw.Wimsy(self.settings._replace(identity=aw.AOGO), aw.Discord(TOKEN, self.clock, self.api), self.clock)
        return aw.Server(aogo, 'test', self.sent.append, **extra)


class Handshake(WithXuji):
    def test_its_server_info_and_no_claude_channel_while_aogowimsy_keeps_it(self):
        server = aw.Server(self.bot, 'test', self.sent.append)
        result = initialize(server, GROK)['result']
        self.assertEqual(result['serverInfo'], {'name': 'xujiwimsy', 'title': 'XujiWimsy', 'version': aw.VERSION})
        self.assertEqual(result['capabilities'], {'tools': {}})
        result = initialize(self.aogo_server(), CLAUDE)['result']
        self.assertEqual(result['serverInfo']['name'], 'aogowimsy')
        self.assertEqual(result['capabilities'], {'tools': {}, 'experimental': {'claude/channel': {}}})

    def test_its_instructions_say_who_it_is_whose_and_to_hear_through_discord_wait(self):
        said = aw.instructions(self.settings)
        self.assertTrue(
            said.startswith(f'XujiWimsy is Xuji on Discord. Every post is a draft its owner (Discord user {OWNER})')
        )
        self.assertIn(f"only the owner's messages, those of users {FRIEND} and its own reach you", said)
        self.assertIn('to hear Discord, call discord_wait on a channel after the id of the newest message', said)
        self.assertIn('the next tool result says it', said)
        self.assertNotIn('<channel>', said)
        self.assertNotIn('Aogo', said)
        self.assertIn('<channel> events', aw.instructions(self.settings._replace(identity=aw.AOGO)))

    def test_its_tools_are_aogowimsys_in_its_own_names(self):
        xuji, aogo = aw.tool_list(aw.XUJI), aw.tool_list(aw.AOGO)
        text = json.dumps(xuji, ensure_ascii=False)
        self.assertNotIn('Aogo', text)
        self.assertNotIn('AOGO', text)
        self.assertIn('XUJIWIMSY_CHANNELS lists', text)
        as_aogo = text.replace('XujiWimsy', 'AogoWimsy').replace('XUJIWIMSY', 'AOGOWIMSY')
        self.assertEqual(json.loads(as_aogo), aogo)
        self.assertEqual(
            self.server.handle({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/list'})['result'], {'tools': xuji}
        )

    def test_its_errors_name_it_and_its_keys(self):
        lists = f'channel {OTHER} is not one XujiWimsy may post in: XUJIWIMSY_CHANNELS lists {GENERAL}, {HELP}'
        self.assertIn(lists, self.error('discord_post', channel=OTHER, text='hi'))
        said = self.error('discord_post', channel=GENERAL, text='hi', files=['/x/a.txt'])
        self.assertIn('XujiWimsy posts images', said)
        answer = self.server.handle({'jsonrpc': '2.0', 'id': 2, 'method': 'server/discover', 'params': {}})
        self.assertEqual(
            answer['error']['message'],
            'Method not found: server/discover; XujiWimsy speaks the initialize handshake '
            '(2025-11-25, 2025-06-18, 2025-03-26, 2024-11-05)',
        )


class Clients(WithXuji):
    """each Wimsy serves its own client: Claude Code AogoWimsy alone, any other XujiWimsy, so a session started
    where the other one's server is listed cannot post as the other bot"""

    def listening(self, server_of):
        calls = []
        server = server_of(lambda push: calls.append(push) or [])
        return server, calls

    def test_aogowimsy_refuses_a_client_that_is_not_claude_code_naming_both(self):
        server, calls = self.listening(lambda listen: self.aogo_server(listen=listen))
        grok = dict(GROK, clientInfo={'name': 'grok-shell-aogowimsy', 'version': '1.0.60'})
        error = initialize(server, grok)['error']
        self.assertEqual(error['code'], -32602)
        self.assertEqual(
            error['message'],
            'Invalid params: clientInfo.name is "grok-shell-aogowimsy", not "claude-code": AogoWimsy is Aogo\'s, for '
            'Claude Code alone; another client runs XujiWimsy (xujiwimsy) and keeps aogowimsy off (Grok Build: grok '
            'mcp disable aogowimsy)',
        )
        self.assertIn(f'initialize refused: {error["message"].removeprefix("Invalid params: ")}', self.logs)
        no_info = initialize(server, {'protocolVersion': '2025-11-25', 'capabilities': {}})['error']['message']
        self.assertIn('clientInfo.name is null, not "claude-code"', no_info)
        server.handle({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        self.assertEqual(calls, [])  # refused: it never hears Discord with Aogo's token
        error = server.handle({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/list'})['error']
        self.assertIn('tools/list came before initialize', error['message'])
        self.assertIn('result', initialize(server, CLAUDE))

    def test_xujiwimsy_refuses_claude_code_naming_both_and_serves_any_other(self):
        server, calls = self.listening(lambda listen: aw.Server(self.bot, 'test', self.sent.append, listen=listen))
        error = initialize(server, CLAUDE)['error']
        self.assertEqual(
            error['message'],
            'Invalid params: clientInfo.name is "claude-code": XujiWimsy is Xuji\'s, for any client but Claude Code, '
            'which runs AogoWimsy (aogowimsy); take xujiwimsy out of its MCP servers',
        )
        server.handle({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        self.assertEqual(calls, [])
        for params in (GROK, {'protocolVersion': '2025-11-25', 'capabilities': {}}):
            self.assertIn('result', initialize(server, params))
        server.handle({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        self.assertEqual(calls, [server.push])


class Posting(WithXuji):
    def test_a_post_reaches_discord_as_its_text_with_no_signature(self):
        text = 'Built: **done**\nsecond line'
        self.owner_answers(6, YES)
        got = self.tool('discord_post', channel=GENERAL, text=text)
        self.assertEqual(got['status'], 'sent')
        self.assertEqual(self.fake.find(DM, got['draft'])['content'], text)
        posted = self.fake.posts('POST', f'/channels/{GENERAL}/messages')
        self.assertEqual(len(posted), 1)
        self.assertEqual(posted[0][3]['content'], text)
        self.assertEqual(self.fake.last(GENERAL)['content'], text)


class News(WithXuji):
    """with no channel, the gateway's news and the owner's answers to drafts come with the next tool result"""

    def setUp(self):
        super().setUp()
        self.ear = aw.Ear(self.bot, self.server.push, threading.Event())
        self.ear.hear('READY', {'user': {'id': BOT}, 'session_id': 's', 'resume_gateway_url': 'ws://127.0.0.1:9'})

    def contents(self, **args):
        return self.answer('discord_read', channel=GENERAL, **args)['result']['content']

    def test_nothing_is_pushed_and_a_drafts_answer_comes_with_the_next_tool_result(self):
        draft = self.tool('discord_post', channel=GENERAL, text='approved later', wait=0)['draft']
        number = self.fake.say(GENERAL, OWNER, 'to xuji', mentions=[BOT])
        self.ear.hear('MESSAGE_CREATE', self.fake.event(GENERAL, number))
        self.fake.react(draft, YES, OWNER)
        self.ear.hear(
            'MESSAGE_REACTION_ADD',
            {'user_id': OWNER, 'channel_id': DM, 'message_id': draft, 'emoji': {'id': None, 'name': YES}},
        )
        posted = self.fake.last(GENERAL)
        self.assertEqual(posted['content'], 'approved later')  # posted at once, no tool waiting
        self.assertNotIn(aw.CHANNEL_PUSH, [message.get('method') for message in self.sent])
        self.assertIn('channel: a message event, not pushed: XujiWimsy offers no channel', self.logs)
        self.assertNotIn('channel: pushed a message event', self.logs)
        content = self.contents()
        self.assertEqual(json.loads(content[0]['text'])['channel'], GENERAL)
        link = f'https://discord.com/channels/{GUILD}/{GENERAL}/{posted["id"]}'
        news = f'XujiWimsy news: The owner approved draft {draft}: posted as {link}'
        self.assertEqual(content[1:], [{'type': 'text', 'text': news}])
        self.assertEqual(len(self.contents()), 1)  # said once

    def test_the_gateways_news_come_with_a_tool_error_too(self):
        self.ear.report('trouble', 'OSError: down; it tries again, 60 s apart at most')
        self.ear.report('back', 'XujiWimsy hears Discord again')
        result = self.answer('discord_read', channel='general')['result']
        self.assertTrue(result['isError'])
        self.assertEqual(
            [item['text'] for item in result['content'][1:]],
            [
                "XujiWimsy news: XujiWimsy cannot reach Discord's gateway: OSError: down; it tries again, 60 s apart at most",
                'XujiWimsy news: XujiWimsy hears Discord again',
            ],
        )

    def test_a_stopped_gateway_is_said_and_discord_wait_says_it_cannot_hear(self):
        self.ear.report('stopped', 'Discord closed the gateway with 4004')
        start = self.fake.say(GENERAL, OWNER, 'go')
        result = self.answer('discord_wait', channel=GENERAL, after=start)['result']
        self.assertTrue(result['isError'])
        self.assertIn('the gateway stopped (Discord closed the gateway with 4004)', result['content'][0]['text'])
        self.assertIn(
            'XujiWimsy stopped hearing Discord: Discord closed the gateway with 4004', result['content'][1]['text']
        )

    def test_past_twenty_the_oldest_news_go_and_are_counted(self):
        for number in range(1, 26):
            self.ear.report('trouble', f'try {number}')
        texts = [item['text'] for item in self.contents()[1:]]
        self.assertEqual(texts[0], 'XujiWimsy news: 5 older news were left out: XujiWimsy keeps the newest 20')
        self.assertEqual(len(texts), 21)
        self.assertEqual(texts[1], "XujiWimsy news: XujiWimsy cannot reach Discord's gateway: try 6")
        self.assertEqual(texts[-1], "XujiWimsy news: XujiWimsy cannot reach Discord's gateway: try 25")


class GatewayNames(WithXuji):
    def test_the_gateway_identifies_as_xujiwimsy_and_names_its_token(self):
        stop, reports = threading.Event(), []
        client = aw.Gateway(TOKEN, self.bot.discord, None, lambda kind, text: reports.append((kind, text)), stop)
        self.assertEqual(client.identify()['d']['properties']['browser'], 'xujiwimsy')
        with self.assertRaisesRegex(aw.Stopped, "4004: authentication failed: XUJIWIMSY_TOKEN is not the bot's token"):
            client.closed(4004, '')
        self.fake.fail[('GET', '/gateway/bot')] = (401, {'message': '401: Unauthorized', 'code': 0})
        client.run()
        self.assertEqual(
            reports, [('stopped', "Discord answered 401 to GET /gateway/bot: XUJIWIMSY_TOKEN is not the bot's token")]
        )


class Stdio(unittest.TestCase):
    """the real xujiwimsy.py, as Grok Build starts it; never told initialized, so it never reaches for Discord"""

    exchange, finish = base.Stdio.exchange, base.Stdio.finish

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.env = base.write_env(self.folder.name, GOOD_ENV, name='xujiwimsy.env')

    def start(self, *args, environ=None, program=HERE / 'xujiwimsy.py'):
        command = [sys.executable, str(program), *args]
        proc = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environ
        )
        watchdog = threading.Timer(20, proc.kill)
        watchdog.start()
        self.addCleanup(self.ended, proc, watchdog)
        return proc

    def ended(self, proc, watchdog):
        """a program a test left running or open, when it failed halfway: killed and closed"""
        watchdog.cancel()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            stream.close()

    def test_the_handshake_tools_list_and_a_tool_call_through_a_subprocess(self):
        proc = self.start('--env', self.env)
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': GROK})
        result = answer['result']
        self.assertEqual(result['serverInfo']['name'], 'xujiwimsy')
        self.assertEqual(result['capabilities'], {'tools': {}})
        self.assertIn('call discord_wait', result['instructions'])
        self.assertIn(f'channels {GENERAL}, {HELP} and their threads', result['instructions'])
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 'list', 'method': 'tools/list'})
        self.assertEqual(answer['result'], {'tools': aw.tool_list(aw.XUJI)})
        call = {'name': 'discord_read', 'arguments': {'channel': 'general'}}
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 5, 'method': 'tools/call', 'params': call})
        self.assertEqual(
            answer['result']['content'],
            [{'type': 'text', 'text': 'channel is "general", not a Discord id (17 to 20 digits)'}],
        )
        errors = self.finish(proc)
        self.assertIn(f'xujiwimsy: up: owner {OWNER}, 1 users, 2 channels', errors)
        self.assertIn('xujiwimsy: discord_read failed: channel is "general"', errors)
        self.assertNotIn('aogowimsy', errors)

    def test_claude_code_is_refused_and_stderr_says_so(self):
        proc = self.start('--env', self.env)
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': CLAUDE})
        self.assertIn('clientInfo.name is "claude-code": XujiWimsy is Xuji\'s', answer['error']['message'])
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'})
        self.assertIn('tools/list came before initialize; XujiWimsy speaks', answer['error']['message'])
        self.assertIn('xujiwimsy: initialize refused: clientInfo.name is "claude-code"', self.finish(proc))

    def test_aogowimsy_refuses_grok_through_its_subprocess(self):
        aogo_env = base.write_env(self.folder.name, base.GOOD_ENV)
        proc = self.start('--env', aogo_env, program=HERE.parent / 'aogo' / 'aogowimsy.py')
        grok = dict(GROK, clientInfo={'name': 'grok-shell-aogowimsy', 'version': '1.0.60'})
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': grok})
        self.assertIn('"grok-shell-aogowimsy", not "claude-code": AogoWimsy is Aogo\'s', answer['error']['message'])
        self.assertIn('aogowimsy: initialize refused: clientInfo.name is "grok-shell-aogowimsy"', self.finish(proc))

    def test_its_env_file_is_found_where_it_says_and_an_aogowimsy_one_stops_it(self):
        home = Path(self.folder.name) / 'home'
        (home / '.config' / 'xujiwimsy').mkdir(parents=True)
        base.write_env(home / '.config' / 'xujiwimsy', GOOD_ENV, name='xujiwimsy.env')
        proc = self.start(environ=dict(os.environ, HOME=str(home)))
        answer = self.exchange(proc, {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': GROK})
        self.assertEqual(answer['result']['serverInfo']['title'], 'XujiWimsy')
        self.assertIn(f'xujiwimsy: up: owner {OWNER}', self.finish(proc))
        aogo_env = base.write_env(self.folder.name, base.GOOD_ENV)
        proc = self.start('--env', aogo_env)
        self.assertEqual(proc.wait(), 1)
        said = proc.stderr.read().decode()
        self.assertIn(f'xujiwimsy: {aogo_env} line 2: AOGOWIMSY_TOKEN is not a key XujiWimsy knows', said)
        self.assertNotIn(TOKEN, said)
        proc = self.start('--help')
        self.assertEqual(proc.wait(), 0)
        said = proc.stdout.read().decode()
        self.assertIn('usage: xujiwimsy.py', said)
        self.assertIn('~/.config/xujiwimsy/xujiwimsy.env', said)


if __name__ == '__main__':
    unittest.main()
