<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Testing

    make -C rei test && make -C accordwimsy test && sh scripts/test-check-docs.sh && sh scripts/check-docs.sh

What CI runs on each pull request and each push to main. No network and no Discord:

- ReiWimsy (`rei/tests/test_rei.c`, no Concord): the parsing, the gateway's events (read from their JSON: Concord
  decodes none of them, as its decoder crashes on messages of 2 to 3 KB) and the messages waiting for an answer, the
  replies and their edits, GitHub's ETags and pages, Discord's waits and answers, the cards (drawn, posted, edited,
  moved to the end at their activity, deleted a day after they close, posted again when deleted by hand), the state
  file.
- AccordWimsy (`accordwimsy/tests/test_aw.c`), against a fake GitHub and a fake Forgejo and git on bare repositories:
  the env file and its refusals, the first pass (repositories, branches and tags, labels, every number, a missing one,
  a pull request), a pass with nothing new (GitHub asked from where the last stopped, the Forgejo written to only for
  its Releases tab), edits and new ones (an issue, a comment, a branch deleted), a Forgejo whose numbers differ, and
  the program's lock and usage.
- The docs (`scripts/check-docs.sh`): no date in `README.md` and `docs/*.md` outside code, as a doc says what is and
  the commit message says when; `scripts/test-check-docs.sh` holds the check to dates in text, in code and none.

Beside them:

- `dist/freebsd/build.sh` runs both programs' tests, and Concord's `unit-gateway-lifecycle`, before it packages.
- `dist/status/check.sh <ScarletNX's dist/wimsy.svg>` checks the status page without Gatus and ends with `ok`: Wimsy's
  colours, `gatus.yaml` and `push.sh` rendered from their templates, the YAML Gatus reads, and each endpoint's answer
  now (this one goes to the network).
