#!/bin/sh
# Puts git.scarletnx.dev's start page (templates/home.tmpl) and its icons
# (public/assets/img: logo, favicon, apple-touch-icon, wimsy.svg, and bots/: each bot's avatar) into
# Forgejo's custom directory in gitscarlet (FORGEJO_CUSTOM, its rc.d:
# /usr/local/etc/forgejo), from the folder this script is in. Run as root
# on the host (no process in the jail), with the guard group paused:
# /usr/local changes. Forgejo reads them after a restart.
set -e
here=$(dirname "$0")
G=/usr/local/bastille/jails/gitscarlet/root/usr/local/etc/forgejo
[ -d "$G/conf" ] || { echo "no $G/conf: is gitscarlet's root elsewhere?" >&2; exit 1; }
install -d -o root -g wheel -m 0755 "$G/templates" "$G/public" "$G/public/assets" "$G/public/assets/img" \
    "$G/public/assets/img/bots"
install -o root -g wheel -m 0644 "$here/templates/home.tmpl" "$G/templates/home.tmpl"
for f in "$here"/public/assets/img/*.png "$here"/public/assets/img/*.svg; do
    install -o root -g wheel -m 0644 "$f" "$G/public/assets/img/"
done
for f in "$here"/public/assets/img/bots/*.png; do
    install -o root -g wheel -m 0644 "$f" "$G/public/assets/img/bots/"
done
ls -ln "$G/templates" "$G/public/assets/img" "$G/public/assets/img/bots"
