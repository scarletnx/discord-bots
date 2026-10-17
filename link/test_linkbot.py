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
            linkbot.references('see #160, issue 12, PR 305, pull request #7 and pull 8'), [160, 12, 305, 7, 8]
        )

    def test_each_once_and_at_most_five(self):
        self.assertEqual(linkbot.references('#1 #1 #2 #3 #4 #5 #6'), [1, 2, 3, 4, 5])

    def test_not_code_links_words_or_zero(self):
        text = 'run `make #12`, ```\n#13\n``` see https://x/issues/14#15 a#16 &#17 #0 color #fff'
        self.assertEqual(linkbot.references(text), [])

    def test_case(self):
        self.assertEqual(linkbot.references('Issue 9 and pr 10'), [9, 10])


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

        def fetch(url):
            self.asked.append(url)
            if url.endswith('/issues/160'):
                return dict(ISSUE)
            if url.endswith('/issues/305'):
                return dict(PULL, pull_request={'url': 'x'})
            if url.endswith('/pulls/305'):
                return {'merged_at': '2026-10-17', 'draft': False}
            return None

        self.github = linkbot.GitHub('o/r', fetch=fetch, clock=lambda: self.now[0])

    def test_pull_request_asked_whether_it_merged(self):
        self.assertEqual(linkbot.state(self.github.item(305)), 'merged')

    def test_remembered_then_asked_again(self):
        self.github.item(160)
        self.github.item(160)
        self.now[0] += linkbot.CACHE + 1
        self.github.item(160)
        self.assertEqual(self.asked.count('https://api.github.com/repos/o/r/issues/160'), 2)

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


if __name__ == '__main__':
    unittest.main()
