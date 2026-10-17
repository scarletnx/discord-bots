#!/usr/bin/env python3
"""A Discord bot that links the issues and pull requests a message names, and the builds it is asked for.

    LINKBOT_TOKEN=... LINKBOT_REPO=OWNER/NAME [LINKBOT_NIGHTLY=OWNER/NAME] [LINKBOT_CHANNELS=id,id] [GITHUB_TOKEN=...]
    linkbot.py

A message that names #160 (issue #160 and PR #160 too; a number without its # is not a reference) gets one reply:
each of them linked, with its title, whether it is an issue or a pull request, its state and its labels, in GitHub's
colours. A number GitHub does not know is left out; one inside code or a link is not a reference. At most MOST of
them a message.

Commands, each a message of its own: !nightly links the newest nightly and its downloads; !release the newest
version, or the newest nightly while there is none; !release #355 pull request 355's builds. The nightlies and a pull
request's builds are releases of LINKBOT_NIGHTLY (OWNER/nightly by default), as ScarletNX's publish workflow makes them.

What it reads: the text of messages in the channels it is in (LINKBOT_CHANNELS narrows them), only to find those
references. It keeps no message text, no author and no log of what was said. It asks GitHub for the numbers it found
and remembers each answer, an open one CACHE seconds and a closed or merged one SETTLED. With LINKBOT_STATE, a file,
those answers outlive a restart, and so do its replies of the last AGE days, as their channel and message ids and the
numbers in them: with GITHUB_TOKEN it asks again every REFRESH seconds and edits a reply whose items changed (without
a token GitHub allows 60 questions an hour, so its replies stay as sent). Its replies ping no one.
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

MOST, CACHE, FILES = 5, 600, 12
SETTLED, AGE, TRACK, REFRESH = 86400, 7 * 86400, 200, 600
# #160, as in issue #160 or PR #160; not inside a word, a path or a link. A bare 160 is not one: "pr 2 broke" is talk
REFERENCE = re.compile(r'(?<![\w/#&])#(\d{1,6})\b')
# the whole message: !nightly, !release, or !release #N
COMMAND = re.compile(r'\s*!(?:(nightly)|release(?:\s+#(\d{1,6}))?)\s*', re.IGNORECASE)
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
        number = int(match.group(1))
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


def size(count):
    return f'{count / 1048576:.0f} MiB' if count >= 1048576 else f'{max(count, 1) / 1024:.0f} KiB'


def build(release, said=''):
    """a release as the reply says it: its name linked, then each download and its size; a version green"""
    files = sorted(release.get('assets', []), key=lambda asset: asset['name'])
    lines = [f'[{a["name"]}]({a["browser_download_url"]}) ({size(a["size"])})' for a in files[:FILES]]
    if len(files) > FILES:
        lines.append(f'and {len(files) - FILES} more on its page')
    description = '\n'.join(([said] if said else []) + (lines or ['No downloads on it yet.']))
    return {
        'title': (release.get('name') or release['tag_name'])[:256],
        'url': release['html_url'],
        'description': description[:4096],
        'color': 0x6E7781 if release.get('prerelease') else COLORS['open'],
    }


def note(text):
    return {'description': text, 'color': 0x6E7781}


class GitHub:
    """issues and pull requests by number, and releases, through GitHub's REST API, remembered CACHE seconds"""

    def __init__(self, repo, token='', fetch=None, clock=time.time, nightly='', items=None):
        self.repo, self.token, self.clock = repo, token, clock
        self.nightly = nightly or repo.split('/')[0] + '/nightly'
        self.fetch = fetch or self.http
        self.cache = {int(number): tuple(kept) for number, kept in (items or {}).items()}

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
        if number in self.cache:
            then, found = self.cache[number]
            if now - then < (
                SETTLED if found and state(found) in ('merged', 'closed', 'done', 'not planned') else CACHE
            ):
                return found
        found = self.fetch(f'https://api.github.com/repos/{self.repo}/issues/{number}')
        if found and found.get('pull_request'):  # whether it merged, and whether it is a draft, are the PR's
            pull = self.fetch(f'https://api.github.com/repos/{self.repo}/pulls/{number}') or {}
            found['pull_request']['merged_at'] = pull.get('merged_at')
            found['draft'] = pull.get('draft', False)
        self.cache[number] = (now, found)
        return found

    def release(self, repo, which):
        """a release of repo, 'latest' or 'tags/<tag>', or None when there is none"""
        url = f'https://api.github.com/repos/{repo}/releases/{which}'
        now = self.clock()
        if url in self.cache and now - self.cache[url][0] < CACHE:
            return self.cache[url][1]
        found = self.fetch(url)
        self.cache[url] = (now, found)
        return found

    def items(self):
        """what GitHub said of each number, to keep in the state file"""
        return {str(n): list(kept) for n, kept in self.cache.items() if isinstance(n, int) and kept[1]}


class Replies:
    """its replies that name items, kept AGE days at most TRACK of them: [when, channel id, message id, numbers, embed]"""

    def __init__(self, kept=None, clock=time.time):
        self.kept, self.clock = [list(entry) for entry in (kept or [])], clock

    def keep(self, where, numbers, embed):
        self.kept.append([self.clock(), where[0], where[1], list(numbers), embed])
        del self.kept[:-TRACK]

    def drop(self, channel, message):
        self.kept = [entry for entry in self.kept if (entry[1], entry[2]) != (channel, message)]

    async def changed(self, github):
        """(channel id, message id, embed) for each kept reply whose items read differently now; older ones dropped"""
        now, loop, edits = self.clock(), asyncio.get_running_loop(), []
        self.kept = [entry for entry in self.kept if now - entry[0] < AGE]
        for entry in self.kept:
            items = [await loop.run_in_executor(None, github.item, number) for number in entry[3]]
            items = [item for item in items if item]
            if items and reply(items) != entry[4]:
                entry[4] = reply(items)
                edits.append((entry[1], entry[2], entry[4]))
        return edits


class State:
    """the state file: GitHub's answers and the kept replies, as JSON; none when it is unset, missing or unreadable"""

    def __init__(self, path):
        self.path = path

    def load(self):
        try:
            with open(self.path, encoding='utf-8') as file:
                state = json.load(file)
            return state if isinstance(state, dict) else {}
        except (OSError, ValueError, TypeError):
            return {}

    def save(self, github, replies):
        if not self.path:
            return
        temporary = f'{self.path}.new'
        with open(temporary, 'w', encoding='utf-8') as file:
            json.dump({'items': github.items(), 'replies': replies.kept}, file)
        os.replace(temporary, self.path)


async def command(message_text, github, send):
    """a command's reply, sent through send(embed dict); whether the message was a command"""
    match = COMMAND.fullmatch(message_text or '')
    if not match:
        return False
    loop = asyncio.get_running_loop()

    async def ask(repo, which):
        return await loop.run_in_executor(None, github.release, repo, which)

    if match.group(1):
        found = await ask(github.nightly, 'latest')
        await send(build(found) if found else note('No nightly is published yet.'))
    elif match.group(2):
        number = int(match.group(2))
        found = await ask(github.nightly, f'tags/pr-{number}')
        missing = f'No builds for #{number}: a pull request has them while it is open and its last push built.'
        await send(build(found) if found else note(missing))
    else:
        found = await ask(github.repo, 'latest')
        newest = None if found else await ask(github.nightly, 'latest')
        if found:
            await send(build(found))
        elif newest:
            await send(build(newest, 'No version is released yet. The newest nightly:'))
        else:
            await send(note('No version and no nightly are published yet.'))
    return True


async def answer(message_text, github, send, replies=None):
    """the reply to a message: a command's, else the items it names, sent through send(embed dict), which gives back
    where it went, (channel id, message id), for replies to keep; whether one was sent"""
    if await command(message_text, github, send):
        return True
    numbers = references(message_text)
    if not numbers:
        return False
    loop = asyncio.get_running_loop()
    items = [i for i in await asyncio.gather(*(loop.run_in_executor(None, github.item, n) for n in numbers)) if i]
    if not items:
        return False
    embed = reply(items)
    where = await send(embed)
    if replies is not None and where:
        replies.keep(where, [item['number'] for item in items], embed)
    return True


def main():
    import discord  # only to run: the logic above is tested without it

    token, repo = os.environ.get('LINKBOT_TOKEN', ''), os.environ.get('LINKBOT_REPO', '')
    if not token or not re.fullmatch(r'[\w.-]+/[\w.-]+', repo):
        sys.exit('linkbot: LINKBOT_TOKEN and LINKBOT_REPO (OWNER/NAME) are needed')
    channels = {int(c) for c in os.environ.get('LINKBOT_CHANNELS', '').split(',') if c.strip()}
    nightly = os.environ.get('LINKBOT_NIGHTLY', '')
    if nightly and not re.fullmatch(r'[\w.-]+/[\w.-]+', nightly):
        sys.exit('linkbot: LINKBOT_NIGHTLY is OWNER/NAME')
    state = State(os.environ.get('LINKBOT_STATE', ''))
    saved = state.load()
    github = GitHub(repo, os.environ.get('GITHUB_TOKEN', ''), nightly=nightly, items=saved.get('items'))
    replies = Replies(saved.get('replies'))
    intents = discord.Intents.none()
    intents.guilds = intents.guild_messages = intents.message_content = True
    client = discord.Client(intents=intents, allowed_mentions=discord.AllowedMentions.none())

    def saved_now():
        try:
            state.save(github, replies)
        except OSError as error:
            print(f'linkbot: {state.path}: {error}', file=sys.stderr, flush=True)

    async def refresh():
        while True:
            await asyncio.sleep(REFRESH)
            for channel, message, embed in await replies.changed(github):
                try:
                    where = client.get_channel(channel) or await client.fetch_channel(channel)
                    await where.get_partial_message(message).edit(embed=discord.Embed.from_dict(embed))
                except discord.NotFound:  # the reply, or its channel, is gone
                    replies.drop(channel, message)
                except Exception as error:  # noqa: BLE001 - one edit never stops the others
                    print(f'linkbot: edit: {type(error).__name__}: {error}', file=sys.stderr, flush=True)
            saved_now()

    @client.event
    async def on_ready():
        print(f'linkbot: up as {client.user} for {repo}', flush=True)
        if github.token and not getattr(client, 'refreshing', False):
            client.refreshing = True
            asyncio.get_running_loop().create_task(refresh())
        elif not github.token:
            print('linkbot: no GITHUB_TOKEN: its replies stay as sent', flush=True)

    @client.event
    async def on_message(message):
        if message.author.bot or (channels and message.channel.id not in channels):
            return

        async def send(embed):
            sent = await message.reply(embed=discord.Embed.from_dict(embed), mention_author=False)
            return sent.channel.id, sent.id

        try:
            if await answer(message.content, github, send, replies):
                saved_now()
        except Exception as error:  # noqa: BLE001 - one message never stops the bot; said in the log, not the channel
            print(f'linkbot: {type(error).__name__}: {error}', file=sys.stderr, flush=True)

    client.run(token, log_handler=None)


if __name__ == '__main__':
    main()
