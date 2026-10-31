<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# AccordWimsy

`accordwimsy/`: the repositories of a GitHub organization, the same on a Forgejo, a pass at a time (cron, every 10
minutes), in C99 with libcurl and ReiWimsy's cJSON (`rei/third_party/cjson`). It needs no Discord and listens on no
port. A pass, `accordwimsy sync [ENV FILE]`, for each repository of the organization, the new ones too:

- the repository on the Forgejo, made in its organization when it is not there, with GitHub's name, description and
  visibility, its default branch, and its Releases tab off (releases stay on GitHub);
- its branches and tags, through a bare mirror in `mirrors/`: fetched from GitHub (the mirror pruned of what GitHub has
  not, GitHub's pull requests' heads with them) and pushed to the Forgejo, which loses nothing GitHub loses. A branch or
  a tag GitHub has no more stays on the Forgejo `ACCORDWIMSY_GRACE_DAYS` (7) from the pass that first sees it gone, said
  then in its log with the day it goes, and is deleted after that; back on GitHub before, it just stays. A branch GitHub
  rewrote (a force-push: its new tip does not hold the old one), or a tag moved, keeps its old tip on the Forgejo as the
  branch `accordwimsy/kept/branch/<name>/<YYYYMMDD-HHMMSS>` (`.../tag/...` for a tag), said in its log, for the same
  grace. No `--prune` push: over HTTP, with the negative refspec that keeps the `accordwimsy/pull/<n>` branches out of
  it, git warns `helper reported unexpected status` for each of them on every pass. The tokens go to git in its
  environment (`GIT_CONFIG_*`), never on its command line, and no one's git settings are read;
- its labels, made or edited as GitHub has them;
- its issues and pull requests by their numbers, which the Forgejo then gives the same: a number GitHub has no more
  (deleted, moved to another repository) becomes a closed issue saying so, and a number the Forgejo gives otherwise
  stops the repository, said, since its issues must start empty. A pull request is a pull request into the same branch,
  from its head, which GitHub keeps for every one, a fork's too (`refs/pull/<n>/head`): pushed to the Forgejo as the
  branch `accordwimsy/pull/<n>` while it is open, moved when GitHub's moves. One merged on GitHub is marked merged by
  GitHub's merge commit (the repository allows a commit to mark it, `allow_manual_merge`), one closed is closed, one
  reopened is reopened; once it is not open its branch goes, the pull request keeps its commits. One left open that the
  Forgejo closed under the sync gets its head back and is reopened on the next pass, and one still open whose branch is
  gone gets the branch back. A merge commit the branch here does not hold (pushed over since) leaves it closed, said.
  Only one whose head or base branch GitHub has no more is an issue, saying why, its branches (`fork:branch` into
  `main`), its state and where its changes are. Each starts with its GitHub author, day and link; an edit on GitHub (its
  `updated_at`: title, text, state, labels) edits it;
- the comments, each with its GitHub author, day and link; one edited on GitHub is edited. One deleted there stays.

An issue, a comment or a repository is never deleted on the Forgejo. A repository GitHub lists no more (deleted, moved,
or out of the token's reach) is said once on stderr and left as it is.

Not mirrored: milestones, a comment's deletion, a release, a pull request's reviews and its comments on lines of its
changes.

A pass asks only for what changed since the last one (GitHub's `since`), keeps in `state.json` what it made (the next
number, each issue's and comment's `updated_at`, the Forgejo's id of each comment, the state each pull request was left
in, what waits out the grace and since when, when its last pass went through, since when GitHub lists it no more), and
writes it after each repository: a pass that stops goes on from there. One pass at a time: a second one, while one goes,
says so and does nothing. What it did goes to stdout, its log, and so do a repository's pass that went wrong (an HTTP
error, a push refused: the next pass tries again), what git itself says, and the everyday news (a branch or a tag gone
from GitHub, a branch rewritten, a pull request closed rather than merged). stderr, which the host mails (`jail
logmail`), holds only what someone has to see: the default branch gone from GitHub or rewritten, half or more of a
repository's branches and tags gone in one pass (3 at least), a repository GitHub lists no more, the Forgejo's issue
numbers no longer GitHub's (once, until that repository passes again), a `state.json` or `ACCORDWIMSY_FRESH` it cannot
write, and what keeps a pass from starting. A repository that keeps failing shows in its last good pass, which `jail
fresh` mails about once it lags. The exit status is 1 when a repository stopped. After each pass it writes
`ACCORDWIMSY_FRESH`, a line per repository GitHub lists, `<name> <seconds since the epoch of its last good pass>` (0:
none yet), for the host to tell a copy that lags (noirjail.sh's `jail fresh`). `accordwimsy status [ENV FILE]` prints
each repository's last good pass, and what waits out the grace with the day it goes.

Its settings, a line each of its env file, `/usr/local/etc/accordwimsy/accordwimsy.env` (the package puts
`dist/freebsd/accordwimsy.env.sample` there to fill in):

| Key | Default | What it is |
|---|---|---|
| `ACCORDWIMSY_FORGEJO` | none: needed | the Forgejo's address, `http://host:3000`: its API under `/api/v1`, its repositories under it |
| `ACCORDWIMSY_FORGEJO_TOKEN` | none: needed | a token of the Forgejo's: `write:repository`, `write:issue`, `write:organization`, `read:user` |
| `ACCORDWIMSY_FORGEJO_USER` | `accordwimsy` | whose that token is, for git's push |
| `GITHUB_TOKEN` | none: needed | a fine-grained token, read-only, of the organization: Contents, Issues, Pull requests, Metadata |
| `ACCORDWIMSY_ORG` | `scarletnx` | the GitHub organization |
| `ACCORDWIMSY_FORGEJO_ORG` | `ACCORDWIMSY_ORG`'s | the Forgejo's organization |
| `ACCORDWIMSY_STATE` | `/var/db/accordwimsy` | `state.json`, the lock and `mirrors/` |
| `ACCORDWIMSY_GIT` | `/usr/local/bin/git` | the git it runs |
| `ACCORDWIMSY_GRACE_DAYS` | `7` | how long a branch or a tag GitHub has no more, or a rewritten one's old tip, stays on the Forgejo |
| `ACCORDWIMSY_FRESH` | `/var/log/accordwimsy/accordwimsy.fresh` | each repository's last good pass; made beforehand, owned by the user, in a directory only root writes, since root on the host reads it |
| `ACCORDWIMSY_GITHUB_API`, `ACCORDWIMSY_GITHUB` | `https://api.github.com`, `https://github.com` | where GitHub is (the tests' fakes) |

A key it does not know stops it, naming the key and its line; so does a value past the room it has, and a needed one
left empty, or days that are no number. It goes out to `github.com` and `api.github.com` on 443 and to the Forgejo,
nowhere else.

Its files, as its package installs them: the program in `/usr/local/bin/accordwimsy`, the env file above, its state in
`/var/db/accordwimsy`, its logs and `ACCORDWIMSY_FRESH` in `/var/log/accordwimsy`, its cron line and log rotation in
`/usr/local/share/examples/accordwimsy` to copy into place ([Install](./INSTALL.md#accordwimsy)). Building it:
[Build](./BUILD.md); its tests: [Testing](./TESTING.md).
