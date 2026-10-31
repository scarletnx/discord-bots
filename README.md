<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# wimsy-core

The services around [ScarletNX](https://github.com/scarletnx/ScarletNX), as FreeBSD packages for jails:

| Program | What it does |
|---|---|
| [ReiWimsy](docs/REIWIMSY.md) (`rei/`) | ScarletNX's Discord bot: a message that names an issue or a pull request gets a link to each, and a card is kept for each open pull request, each open issue and each nightly, edited as GitHub changes |
| [AccordWimsy](docs/ACCORDWIMSY.md) (`accordwimsy/`) | the GitHub organization kept the same on a Forgejo, every 10 minutes |

Beside them, the pages they show on: the status page (`dist/status`, Gatus) and the forge's start page
(`dist/forge`, Forgejo).

## Quick start

On FreeBSD 15.1, from a checkout:

    pkg install git gmake pkgconf curl ca_root_nss
    sh dist/freebsd/build.sh                 # packages/reiwimsy-1.0.0.pkg, packages/accordwimsy-1.0.0.pkg
    sh scripts/install.sh discordbots packages/reiwimsy-1.0.0.pkg

then fill in `/usr/local/etc/reiwimsy.env` in the jail and start the service: [Install](docs/INSTALL.md).

## Documentation

[docs/README.md](docs/README.md): install, set the whole thing up again, build, test, licenses.

## License

[GPL-3.0-or-later](LICENSE); what it uses and their licenses: [Licenses and credits](docs/CREDITS.md).
