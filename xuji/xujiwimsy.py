#!/usr/bin/env python3
"""XujiWimsy: Xuji, the Grok Build session, on Discord as a bot of its own, every post approved by its owner first.

    xujiwimsy.py [--env FILE]       FILE: ~/.config/xujiwimsy/xujiwimsy.env when not given; mode 600 or stricter

aogo/aogowimsy.py with Xuji's identity: its bot, its env file and XUJIWIMSY_ keys, the same four tools, no
claude/channel (the session hears through discord_wait), and any client but Claude Code. A fix there is a fix here.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'aogo'))
import aogowimsy

if __name__ == '__main__':
    aogowimsy.main(identity=aogowimsy.XUJI)
