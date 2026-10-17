#!/usr/bin/env python3
"""A Discord bot that links the issues and pull requests a message names.

    LINKBOT_TOKEN=... LINKBOT_REPO=OWNER/NAME [LINKBOT_CHANNELS=id,id] [GITHUB_TOKEN=...] linkbot.py

A message that says #160, issue 160, PR 160 or pull request 160 gets one reply: each of them linked, with its
title, whether it is an issue or a pull request, its state and its labels, in GitHub's colours. A number GitHub
does not know is left out; one inside code or a link is not a reference. At most MOST of them a message.

What it reads: the text of messages in the channels it is in (LINKBOT_CHANNELS narrows them), only to find those
references. It keeps nothing: no message, no author, no log of what was said. It asks GitHub for the numbers it
found, and remembers their answers CACHE seconds. Its replies ping no one.
"""

import asyncio
import colorsys
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

MOST, CACHE = 5, 600
# #160, issue 160, issue #160, PR 160, pull request 160, pull 160; not inside a word, a path or a link
REFERENCE = re.compile(
    r'(?<![\w/#&])#(\d{1,6})\b|\b(?:issue|pr|pull request|pull)\s*#?(\d{1,6})\b',
    re.IGNORECASE,
)
# what is not prose: code, and links
CODE = re.compile(r'```.*?```|`[^`\n]*`', re.DOTALL)
LINK = re.compile(r'<?https?://\S+>?')
# GitHub's colours for a state: open, draft, merged, closed; an issue done, or not planned
COLORS = {
    'open': 0x1F883D,
    'draft': 0x6E7781,
    'merged': 0x8250DF,
    'closed': 0xCF222E,
    'done': 0x8250DF,
    'not planned': 0x6E7781,
}
HUES = {0: '\U0001f7e5', 30: '\U0001f7e7', 50: '\U0001f7e8', 120: '\U0001f7e9', 210: '\U0001f7e6', 275: '\U0001f7ea'}
BROWN, BLACK, WHITE = chr(0x1F7EB), chr(0x2B1B), chr(0x2B1C)


def references(text):
    """the numbers text names, in order, each once, at most MOST; none from code or links"""
    prose = LINK.sub(' ', CODE.sub(' ', text or ''))
    found = []
    for match in REFERENCE.finditer(prose):
        number = int(match.group(1) or match.group(2))
        if number and number not in found:
            found.append(number)
    return found[:MOST]


def square(color):
    """the coloured square nearest a colour (RRGGBB): a grey by its lightness, a dark orange brown, else by hue"""
    hue, light, saturation = colorsys.rgb_to_hls(*(int(color[i : i + 2], 16) / 255 for i in (0, 2, 4)))
    if light < 0.2 or light > 0.92 or saturation < 0.2:
        return WHITE if light >= 0.5 else BLACK
    hue *= 360
    if 10 <= hue <= 45 and light < 0.3:
        return BROWN
    return HUES[min(HUES, key=lambda h: min(abs(hue - h), 360 - abs(hue - h)))]


def chip(label):
    """a label as GitHub's pill, after the square of its colour"""
    color = label.get('color') or ''
    if not re.fullmatch(r'[0-9A-Fa-f]{6}', color):
        return f'`{label["name"]}`'
    return f'{square(color)} `{label["name"]}`'


def state(item):
    """an issue's or a pull request's state, as GitHub words it"""
    if item.get('pull_request'):
        if item['pull_request'].get('merged_at'):
            return 'merged'
        if item['state'] == 'closed':
            return 'closed'
        return 'draft' if item.get('draft') else 'open'
    if item['state'] == 'open':
        return 'open'
    return 'done' if item.get('state_reason') == 'completed' else 'not planned'


def line(item):
    """one reference as the reply says it"""
    kind = 'PR' if item.get('pull_request') else 'Issue'
    title = item['title'].replace('[', '\\[').replace(']', '\\]')
    labels = ' '.join(chip(label) for label in item.get('labels', []))
    said = f'**{kind} [#{item["number"]}]({item["html_url"]})**: {title}  ({state(item)})'
    return said + (f'\n{labels}' if labels else '')


def reply(items):
    """the embed the reply carries, as a dict (discord.Embed.from_dict takes it): one item's state colours it"""
    description = '\n'.join(line(item) for item in items)[:4096]
    color = COLORS[state(items[0])] if len(items) == 1 else 0x6E7781
    return {'description': description, 'color': color}


class GitHub:
    """issues and pull requests by number, through GitHub's REST API, remembered CACHE seconds"""

    def __init__(self, repo, token='', fetch=None, clock=time.monotonic):
        self.repo, self.token, self.clock = repo, token, clock
        self.fetch = fetch or self.http
        self.cache = {}

    def http(self, url):
        headers = {'Accept': 'application/vnd.github+json', 'User-Agent': 'scarletnx-linkbot'}
        if self.token:
            headers['Authorization'] = f'Bearer {self.token}'
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=15) as answer:
                return json.loads(answer.read())
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            raise

    def item(self, number):
        """the issue or pull request number is, or None when there is none"""
        now = self.clock()
        if number in self.cache and now - self.cache[number][0] < CACHE:
            return self.cache[number][1]
        found = self.fetch(f'https://api.github.com/repos/{self.repo}/issues/{number}')
        if found and found.get('pull_request'):  # whether it merged, and whether it is a draft, are the PR's
            pull = self.fetch(f'https://api.github.com/repos/{self.repo}/pulls/{number}') or {}
            found['pull_request']['merged_at'] = pull.get('merged_at')
            found['draft'] = pull.get('draft', False)
        self.cache[number] = (now, found)
        return found


async def answer(message_text, github, send):
    """the reply to a message: the items it names, sent through send(embed dict); whether one was sent"""
    numbers = references(message_text)
    if not numbers:
        return False
    loop = asyncio.get_running_loop()
    items = [i for i in await asyncio.gather(*(loop.run_in_executor(None, github.item, n) for n in numbers)) if i]
    if not items:
        return False
    await send(reply(items))
    return True


def main():
    import discord  # only to run: the logic above is tested without it

    token, repo = os.environ.get('LINKBOT_TOKEN', ''), os.environ.get('LINKBOT_REPO', '')
    if not token or not re.fullmatch(r'[\w.-]+/[\w.-]+', repo):
        sys.exit('linkbot: LINKBOT_TOKEN and LINKBOT_REPO (OWNER/NAME) are needed')
    channels = {int(c) for c in os.environ.get('LINKBOT_CHANNELS', '').split(',') if c.strip()}
    github = GitHub(repo, os.environ.get('GITHUB_TOKEN', ''))
    intents = discord.Intents.none()
    intents.guilds = intents.guild_messages = intents.message_content = True
    client = discord.Client(intents=intents, allowed_mentions=discord.AllowedMentions.none())

    @client.event
    async def on_ready():
        print(f'linkbot: up as {client.user} for {repo}', flush=True)

    @client.event
    async def on_message(message):
        if message.author.bot or (channels and message.channel.id not in channels):
            return

        async def send(embed):
            await message.reply(embed=discord.Embed.from_dict(embed), mention_author=False)

        try:
            await answer(message.content, github, send)
        except Exception as error:  # noqa: BLE001 - one message never stops the bot; said in the log, not the channel
            print(f'linkbot: {type(error).__name__}: {error}', file=sys.stderr, flush=True)

    client.run(token, log_handler=None)


if __name__ == '__main__':
    main()
