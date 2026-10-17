#!/usr/bin/env python3
"""linkbot.py without Discord or the network: what a message names, how GitHub's answer reads, the cache"""

import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import linkbot

ISSUE = {
    'number': 160,
    'title': 'todo: aarch64 [builds]',
    'html_url': 'https://x/issues/160',
    'state': 'closed',
    'state_reason': 'completed',
    'labels': [{'name': 'priority: low', 'color': '0e8a16'}],
}
PULL = {
    'number': 305,
    'title': 'Players',
    'html_url': 'https://x/pull/305',
    'state': 'closed',
    'labels': [],
    'pull_request': {'url': 'x'},
}


class References(unittest.TestCase):
    def test_every_way_to_say_one(self):
        self.assertEqual(
            linkbot.references('see #160, issue #12, PR #305, pull request #7 and pull #8'), [160, 12, 305, 7, 8]
        )

    def test_each_once_and_at_most_five(self):
        self.assertEqual(linkbot.references('#1 #1 #2 #3 #4 #5 #6'), [1, 2, 3, 4, 5])

    def test_not_code_links_words_or_zero(self):
        text = 'run `make #12`, ```\n#13\n``` see https://x/issues/14#15 a#16 &#17 #0 color #fff'
        self.assertEqual(linkbot.references(text), [])

    def test_case(self):
        self.assertEqual(linkbot.references('Issue #9 and pr #10'), [9, 10])

    def test_a_number_without_its_hash_is_talk(self):
        self.assertEqual(linkbot.references('issue 12, PR 305, pull request 7, pull 8 and pr 2 broke'), [])


class Reply(unittest.TestCase):
    def test_issue_line(self):
        self.assertEqual(
            linkbot.line(ISSUE),
            '**Issue [#160](https://x/issues/160)**: todo: aarch64 \\[builds\\]  (done)\n\U0001f7e9 `priority: low`',
        )

    def test_states_as_github_words_them(self):
        merged = dict(PULL, pull_request={'merged_at': '2026'})
        self.assertEqual(linkbot.state(merged), 'merged')
        self.assertEqual(linkbot.state(dict(PULL, pull_request={'merged_at': None})), 'closed')
        self.assertEqual(linkbot.state(dict(PULL, state='open', draft=True)), 'draft')
        self.assertEqual(linkbot.state(dict(ISSUE, state='closed', state_reason='not_planned')), 'not planned')

    def test_one_colours_the_reply_several_do_not(self):
        self.assertEqual(linkbot.reply([ISSUE])['color'], 0x8250DF)
        self.assertEqual(linkbot.reply([ISSUE, dict(PULL, state='open')])['color'], 0x6E7781)

    def test_squares(self):
        self.assertEqual(
            [linkbot.square(c) for c in ('b60205', 'd93f0b', 'fbca04', '0075ca', 'd4c5f9', 'ededed')],
            ['\U0001f7e5', '\U0001f7e7', '\U0001f7e8', '\U0001f7e6', '\U0001f7ea', chr(0x2B1C)],
        )


class Lookup(unittest.TestCase):
    def setUp(self):
        self.asked = []
        self.now = [0.0]
        self.issue = dict(ISSUE)

        def fetch(url):
            self.asked.append(url)
            if url.endswith('/issues/160'):
                return dict(self.issue)
            if url.endswith('/issues/305'):
                return dict(PULL, pull_request={'url': 'x'})
            if url.endswith('/pulls/305'):
                return {'merged_at': '2026-10-17', 'draft': False}
            return None

        self.github = linkbot.GitHub('o/r', fetch=fetch, clock=lambda: self.now[0])

    def test_pull_request_asked_whether_it_merged(self):
        self.assertEqual(linkbot.state(self.github.item(305)), 'merged')

    def asked_160(self):
        return self.asked.count('https://api.github.com/repos/o/r/issues/160')

    def test_an_open_one_is_asked_again_after_cache(self):
        self.issue['state'] = 'open'
        self.github.item(160)
        self.github.item(160)
        self.now[0] += linkbot.CACHE + 1
        self.github.item(160)
        self.assertEqual(self.asked_160(), 2)

    def test_a_closed_one_is_asked_again_only_after_settled(self):
        self.github.item(160)
        self.now[0] += linkbot.CACHE + 1
        self.github.item(160)
        self.assertEqual(self.asked_160(), 1)
        self.now[0] += linkbot.SETTLED
        self.github.item(160)
        self.assertEqual(self.asked_160(), 2)

    def test_a_reply_is_kept_and_edited_when_its_item_changes(self):
        replies = linkbot.Replies(clock=lambda: self.now[0])

        async def send(embed):
            return (7, 70)

        self.issue['state'] = 'open'
        asyncio.run(linkbot.answer('#160 and #999', self.github, send, replies))
        self.assertEqual(replies.kept[0][1:4], [7, 70, [160]])
        self.assertEqual(asyncio.run(replies.changed(self.github)), [])  # nothing changed
        self.issue['state'], self.issue['state_reason'] = 'closed', 'completed'
        self.now[0] += linkbot.CACHE
        edits = asyncio.run(replies.changed(self.github))
        self.assertEqual([(c, m) for c, m, _ in edits], [(7, 70)])
        self.assertIn('(done)', edits[0][2]['description'])
        self.now[0] += linkbot.AGE
        self.assertEqual(asyncio.run(replies.changed(self.github)), [])
        self.assertEqual(replies.kept, [])  # too old to edit: dropped

    def test_a_command_reply_is_not_kept(self):
        replies = linkbot.Replies()

        async def send(embed):
            return (7, 71)

        asyncio.run(linkbot.answer('!nightly', self.github, send, replies))
        self.assertEqual(replies.kept, [])

    def test_at_most_track_replies_are_kept(self):
        replies = linkbot.Replies()
        for i in range(linkbot.TRACK + 3):
            replies.keep((1, i), [160], {})
        self.assertEqual((len(replies.kept), replies.kept[0][2]), (linkbot.TRACK, 3))

    def test_the_state_file_keeps_answers_and_replies_and_nothing_else(self):
        import os
        import tempfile

        replies = linkbot.Replies(clock=lambda: self.now[0])
        self.github.item(160)
        self.github.item(999)  # GitHub knows no 999: nothing to keep
        replies.keep((7, 70), [160], {'description': 'x'})
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, 'state.json')
            linkbot.State(path).save(self.github, replies)
            saved = linkbot.State(path).load()
            self.assertEqual(sorted(saved), ['items', 'replies'])
            self.assertEqual(list(saved['items']), ['160'])
            again = linkbot.GitHub(
                'o/r', fetch=lambda url: self.fail(url), clock=lambda: self.now[0], items=saved['items']
            )
            self.assertEqual(again.item(160)['number'], 160)  # from the file, not asked
            self.assertEqual(linkbot.Replies(saved['replies']).kept, replies.kept)
            with open(path, 'w') as file:
                file.write('{not json')
            self.assertEqual(linkbot.State(path).load(), {})
        self.assertEqual(linkbot.State('').load(), {})

    def test_answer_leaves_out_numbers_github_does_not_know(self):
        sent = []

        async def send(embed):
            sent.append(embed)

        self.assertTrue(asyncio.run(linkbot.answer('#160 and #999', self.github, send)))
        self.assertIn('#160', sent[0]['description'])
        self.assertNotIn('999', sent[0]['description'])
        self.assertFalse(asyncio.run(linkbot.answer('#999 only', self.github, send)))
        self.assertFalse(asyncio.run(linkbot.answer('nothing here', self.github, send)))
        self.assertEqual(len(sent), 1)


NIGHTLY = {
    'name': 'Nightly 2026-10-17 (44aa300)',
    'tag_name': 'nightly-2026-10-17-44aa300',
    'html_url': 'https://n/nightly-2026-10-17-44aa300',
    'prerelease': False,
    'assets': [
        {'name': 'ScarletNX-windows-x86_64.zip', 'size': 80 * 1048576, 'browser_download_url': 'https://n/win'},
        {'name': 'ScarletNX-linux-x86_64.AppImage', 'size': 50 * 1048576, 'browser_download_url': 'https://n/lin'},
    ],
}
PR_BUILDS = dict(
    NIGHTLY, name='PR #355: Android menu files', tag_name='pr-355', html_url='https://n/pr-355', prerelease=True
)
VERSION = dict(NIGHTLY, name='ScarletNX 0.1.0', tag_name='v0.1.0', html_url='https://x/v0.1.0')


class Commands(unittest.TestCase):
    def setUp(self):
        self.asked, self.version = [], None

        def fetch(url):
            self.asked.append(url)
            if url == 'https://api.github.com/repos/o/nightly/releases/latest':
                return NIGHTLY
            if url == 'https://api.github.com/repos/o/nightly/releases/tags/pr-355':
                return PR_BUILDS
            if url == 'https://api.github.com/repos/o/r/releases/latest':
                return self.version
            if url.endswith('/issues/355'):
                return dict(PULL, number=355, html_url='https://x/pull/355')
            return None

        self.github = linkbot.GitHub('o/r', fetch=fetch, clock=lambda: 0.0)
        self.sent = []

    def say(self, text):
        async def send(embed):
            self.sent.append(embed)

        return asyncio.run(linkbot.answer(text, self.github, send))

    def test_nightly_links_the_newest_and_its_downloads(self):
        self.assertTrue(self.say('!nightly'))
        embed = self.sent[0]
        self.assertEqual((embed['title'], embed['url']), (NIGHTLY['name'], NIGHTLY['html_url']))
        self.assertIn('[ScarletNX-linux-x86_64.AppImage](https://n/lin) (50 MiB)', embed['description'])
        self.assertLess(embed['description'].index('linux'), embed['description'].index('windows'))

    def test_release_without_a_version_says_so_and_gives_the_nightly(self):
        self.assertTrue(self.say('!release'))
        self.assertTrue(self.sent[0]['description'].startswith('No version is released yet. The newest nightly:'))
        self.assertEqual(self.sent[0]['url'], NIGHTLY['html_url'])

    def test_release_gives_the_newest_version(self):
        self.version = VERSION
        self.assertTrue(self.say('  !Release '))
        self.assertEqual(self.sent[0]['url'], VERSION['html_url'])
        self.assertEqual(self.sent[0]['color'], linkbot.COLORS['open'])

    def test_release_of_a_pull_request_gives_its_builds_and_no_link_besides(self):
        self.assertTrue(self.say('!release #355'))
        self.assertEqual(len(self.sent), 1)
        self.assertEqual((self.sent[0]['url'], self.sent[0]['color']), (PR_BUILDS['html_url'], 0x6E7781))

    def test_release_of_a_pull_request_without_builds_says_so(self):
        self.assertTrue(self.say('!release #354'))
        self.assertTrue(self.sent[0]['description'].startswith('No builds for #354'))

    def test_only_a_message_of_its_own_is_a_command(self):
        self.assertFalse(self.say('try !nightly later'))
        self.assertFalse(self.say('!nightlies'))
        self.assertEqual(self.sent, [])

    def test_asked_once_while_remembered(self):
        self.say('!nightly')
        self.say('!nightly')
        self.assertEqual(self.asked.count('https://api.github.com/repos/o/nightly/releases/latest'), 1)

    def test_nightly_repository_can_be_named(self):
        self.assertEqual(linkbot.GitHub('o/r', nightly='p/builds').nightly, 'p/builds')


if __name__ == '__main__':
    unittest.main()
