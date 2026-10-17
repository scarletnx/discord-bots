#!/usr/bin/env python3
"""xuji.py without Discord or the network: who it answers, what reaches xAI, how answers are cut, handbacks"""

import datetime
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xuji


class Privacy(unittest.TestCase):
    def test_scrub_takes_out_what_names_or_reaches_a_person(self):
        text = 'ask <@123> or <@!456> in <#789> with <@&42>: me@mail.com, +55 (11) 91234-5678'
        self.assertEqual(xuji.scrub(text), 'ask @someone or @someone in #channel with @role: [email], [phone]')

    def test_numbers_that_are_not_phones_stay(self):
        kept = 'version 1.2.3, PR #305, 2026-10-17, 3.14159, at 12:30'
        self.assertEqual(xuji.scrub(kept), kept)
        self.assertEqual(xuji.scrub('call 11 98765-4321 now'), 'call [phone] now')

    def test_prompt_carries_only_the_scrubbed_texts(self):
        messages = xuji.prompt('hi <@123>, mail me@x.io', '  what is this? ')
        self.assertEqual(messages[0]['role'], 'system')
        self.assertEqual(
            messages[1]['content'], 'Quoted message:\n"""\nhi @someone, mail [email]\n"""\n\nQuestion: what is this?'
        )
        self.assertEqual(len(messages), 2)

    def test_no_quote_no_question(self):
        self.assertEqual(xuji.prompt('', '')[1]['content'], 'Explain the quoted message, or answer it.')


class Owner(unittest.TestCase):
    def test_only_the_owner_mentioning_it_or_in_its_dms(self):
        self.assertTrue(xuji.asked_by_owner(7, 7, True))
        self.assertTrue(xuji.asked_by_owner(7, 7, False, private=True))
        self.assertFalse(xuji.asked_by_owner(8, 7, True))
        self.assertFalse(xuji.asked_by_owner(8, 7, False, private=True))
        self.assertFalse(xuji.asked_by_owner(7, 7, False))


TODAY = datetime.date(2026, 10, 17)


class Handback(unittest.TestCase):
    def test_the_command(self):
        self.assertEqual(
            xuji.parse_handback(' handback #help-n-issues 2026-10-01 2026-10-16 only Android', TODAY),
            ('#help-n-issues', datetime.date(2026, 10, 1), datetime.date(2026, 10, 16), 'only Android'),
        )
        self.assertEqual(
            xuji.parse_handback('Handback <#123> 2026-10-10', TODAY),
            ('<#123>', datetime.date(2026, 10, 10), TODAY, ''),
        )
        self.assertIsNone(xuji.parse_handback('what does handback mean?', TODAY))

    def test_days_out_of_order_or_not_dates(self):
        with self.assertRaisesRegex(ValueError, 'comes before'):
            xuji.parse_handback('handback #x 2026-10-16 2026-10-01', TODAY)
        with self.assertRaises(ValueError):
            xuji.parse_handback('handback #x 2026-13-40', TODAY)

    def test_transcript_names_people_by_number_and_leaves_bots_out(self):
        when = datetime.datetime(2026, 10, 3, 12, 30)
        entries = [
            {
                'thread': 'Crash <@9>',
                'author': 55,
                'bot': False,
                'when': when,
                'text': 'it crashes, mail a@b.io',
                'files': ['crash.log'],
            },
            {'thread': 'Crash <@9>', 'author': 66, 'bot': True, 'when': when, 'text': 'card', 'files': []},
            {'thread': 'Crash <@9>', 'author': 77, 'bot': False, 'when': when, 'text': 'same, <@55>', 'files': []},
            {'thread': 'Audio', 'author': 55, 'bot': False, 'when': when, 'text': 'no sound', 'files': []},
        ]
        text, people = xuji.transcript(entries)
        self.assertEqual(people, 2)
        self.assertEqual(
            text,
            '## Crash @someone\n'
            '[2026-10-03 12:30] user1: it crashes, mail [email] [file: crash.log]\n'
            '[2026-10-03 12:30] user2: same, @someone\n\n'
            '## Audio\n'
            '[2026-10-03 12:30] user1: no sound',
        )
        self.assertNotIn('55', text)
        self.assertNotIn('card', text)

    def test_a_long_transcript_is_cut_and_says_so(self):
        when = datetime.datetime(2026, 10, 3)
        many = [{'thread': '', 'author': 1, 'bot': False, 'when': when, 'text': 'x' * 1000, 'files': []}] * 400
        text, _ = xuji.transcript(many)
        self.assertLessEqual(len(text), xuji.MOST_TRANSCRIPT + 40)
        self.assertTrue(text.endswith('[the rest left out: too long]'))

    def test_prompt_and_file(self):
        messages = xuji.handback_prompt('[..] user1: hi', 'help-n-issues', TODAY, TODAY, 'mail <@1>')
        self.assertIn('Aogo', messages[0]['content'])
        self.assertEqual(
            messages[1]['content'],
            "Channel: #help-n-issues, 2026-10-17 to 2026-10-17 (UTC).\nOwner's notes: mail @someone\n"
            'Messages:\n[..] user1: hi',
        )
        name, text = xuji.handback_file(' summary ', 'help', TODAY, TODAY, 12, 3)
        self.assertEqual(name, 'handback-help-2026-10-17-2026-10-17.md')
        self.assertTrue(text.startswith('# Handback: #help, 2026-10-17 to 2026-10-17\n'))
        self.assertIn('12 messages from 3 people', text)
        self.assertTrue(text.endswith('summary\n'))

    def test_a_handback_asks_with_its_own_messages_and_limit(self):
        sent = {}

        def post(url, body):
            sent.update(body)
            return {'choices': [{'message': {'content': 'ok'}}]}

        grok = xuji.Grok('k', 'grok-4.7', 300, post=post, get=lambda url: {'data': []})
        grok.ask('', '', [{'role': 'user', 'content': 'x'}], 6000)
        self.assertEqual((sent['messages'], sent['max_tokens']), ([{'role': 'user', 'content': 'x'}], 6000))


class Answers(unittest.TestCase):
    def test_pieces_fit_discord_and_keep_everything(self):
        text = '\n'.join(f'line {n} ' + 'x' * 90 for n in range(60))
        parts = xuji.pieces(text)
        self.assertTrue(all(len(p) <= xuji.LIMIT for p in parts) and len(parts) > 1)
        self.assertEqual(''.join(parts).replace('\n', '').replace(' ', ''), text.replace('\n', '').replace(' ', ''))


class Model(unittest.TestCase):
    def test_a_model_xai_lacks_stops_it_naming_those_it_has(self):
        grok = xuji.Grok('k', 'grok-4.7', 100, get=lambda url: {'data': [{'id': 'grok-4'}, {'id': 'grok-3'}]})
        with self.assertRaisesRegex(RuntimeError, 'no model grok-4.7; it has: grok-3, grok-4'):
            grok.check()

    def test_ask_sends_model_limit_and_scrubbed_prompt(self):
        sent = {}

        def post(url, body):
            sent.update(url=url, body=body)
            return {'choices': [{'message': {'content': ' answer '}}]}

        grok = xuji.Grok('k', 'grok-4.7', 300, post=post, get=lambda url: {'data': [{'id': 'grok-4.7'}]})
        grok.check()
        self.assertEqual(grok.ask('hi <@1>', 'why?'), 'answer')
        self.assertEqual(sent['url'], 'https://api.x.ai/v1/chat/completions')
        self.assertEqual((sent['body']['model'], sent['body']['max_tokens']), ('grok-4.7', 300))
        self.assertNotIn('<@1>', str(sent['body']))


if __name__ == '__main__':
    unittest.main()
