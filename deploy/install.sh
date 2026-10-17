#!/bin/sh
# Installs or updates the Discord bots in their Bastille jail, on the host it runs on (as root):
#
#   sh install.sh [JAIL]          default: discordbots
#
# The jail is made once, with Bastille or the host's own tool for jails, for example:
#   bastille create discordbots 15.1-RELEASE <ip> <interface>
# The bots only go out: Discord, GitHub, xAI.
#
# Run from this folder, which holds linkbot.rc, xuji.rc and the .env examples, with ../link and ../xuji beside it.
# Again it changes only what changed: the jail and its users are made once, the code and the services copied, and an
# env file that is there is left as it is (the tokens are in it). A service starts only once its env file is filled
# in: service linkbot start, service xuji start, inside the jail (bastille cmd JAIL service NAME start).
set -eu

jail=${1:-discordbots}
here=$(cd "$(dirname "$0")" && pwd)
root=/usr/local/bastille/jails/$jail/root

if ! jls -j "$jail" jid >/dev/null 2>&1; then
	echo "$jail is not running: make it (bastille create), or start it (bastille start $jail)" >&2
	exit 1
fi
bastille pkg "$jail" install -y py312-discord.py

for bot in linkbot xuji; do
	src=$([ "$bot" = linkbot ] && echo link/linkbot.py || echo xuji/xuji.py)
	bastille cmd "$jail" pw usershow "$bot" >/dev/null 2>&1 ||
		bastille cmd "$jail" pw useradd "$bot" -c "$bot Discord bot" -d /nonexistent -s /usr/sbin/nologin
	install -d "$root/usr/local/libexec" "$root/usr/local/etc/rc.d"
	install -m 755 "$here/../$src" "$root/usr/local/libexec/$bot.py"
	install -m 555 "$here/$bot.rc" "$root/usr/local/etc/rc.d/$bot"
	if [ ! -e "$root/usr/local/etc/$bot.env" ]; then
		install -m 600 "$here/$bot.env.example" "$root/usr/local/etc/$bot.env"
		echo "$bot: fill in $root/usr/local/etc/$bot.env, then: bastille cmd $jail service $bot start"
	fi
	# on at its first install; a NO set since (the bot left off) stays
	jexec "$jail" sysrc -n "${bot}_enable" >/dev/null 2>&1 || bastille sysrc "$jail" "${bot}_enable=YES" >/dev/null
done
echo "$jail: installed"
