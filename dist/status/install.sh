#!/bin/sh
# Puts the status page on the host it runs on, from the folder this script is in. Run as root on the
# host, after `noirjail.sh jail pkg --name <jail> gatus cloudflared`:
#   - the push token: made once (openssl rand), kept as the header curl sends in
#     /usr/local/etc/status-scarletnx/push-header (root's, 0600), never on a command line;
#   - gatus.yaml into the jail with that token in it (root:www, 0640), and the directory its database
#     lives in (www's, 0750); gatus enabled and started again;
#   - push.sh into /usr/local/libexec/status-scarletnx and its cron line, every minute, into
#     /usr/local/etc/cron.d/status-scarletnx; one push now, its words shown.
# Then its service status and the end of its log.
# Usage: install.sh [jail]   (default: status)
set -eu
here=$(dirname "$0")
j=${1:-status}
r=/usr/local/bastille/jails/$j/root
etc=/usr/local/etc/status-scarletnx
header=$etc/push-header
if [ ! -x "$r/usr/local/bin/gatus" ]; then
	echo "no gatus in $j: noirjail.sh jail pkg --name $j gatus" >&2
	exit 69
fi

install -d -o root -g wheel -m 0700 "$etc"
if [ ! -s "$header" ]; then
	(umask 077 && printf 'Authorization: Bearer %s\n' "$(openssl rand -hex 32)" > "$header")
	echo "made $header"
fi
IFS= read -r line < "$header"
token=${line#Authorization: Bearer }

# www is uid and gid 80 on FreeBSD, in the jail as on the host.
# awk: gatus.yaml with the token, from the environment, in place of each @PUSH_TOKEN@.
(umask 027 && TOKEN=$token awk '{ gsub(/@PUSH_TOKEN@/, ENVIRON["TOKEN"]); print }' "$here/gatus.yaml" \
	> "$r/usr/local/etc/gatus.yaml.new")
chown root:80 "$r/usr/local/etc/gatus.yaml.new"
mv -f "$r/usr/local/etc/gatus.yaml.new" "$r/usr/local/etc/gatus.yaml"
install -d -o 80 -g 80 -m 0750 "$r/var/db/gatus"
sysrc -j "$j" gatus_enable=YES
# Not `restart`: the port's rc stops by waiting for gatus, not for daemon(8) to drop the pidfiles,
# and a start before they go says "already running?" and starts nothing.
if service -j "$j" gatus status > /dev/null 2>&1; then service -j "$j" gatus stop; fi
n=0
while [ -e "$r/var/run/gatus.pid" ] || [ -e "$r/var/run/gatusd.pid" ]; do
	n=$((n + 1))
	if [ "$n" -gt 30 ]; then
		echo "gatus's pidfiles still in $r/var/run after 30 s" >&2
		exit 75
	fi
	sleep 1
done
# daemon(8), which the port's rc starts without -f, keeps the stdout it was given: a pipe
# (install.sh | grep) would never end, so its own words go to the log beside gatus's.
service -j "$j" gatus start < /dev/null >> "$r/var/log/gatus-rc.log" 2>&1

install -d -o root -g wheel -m 0755 /usr/local/libexec/status-scarletnx /usr/local/etc/cron.d
install -o root -g wheel -m 0755 "$here/push.sh" /usr/local/libexec/status-scarletnx/push.sh
printf '%s\n' "# status.scarletnx.dev: each bot's state, pushed to its Gatus (status-scarletnx/install.sh)" \
	"* * * * * root /usr/local/libexec/status-scarletnx/push.sh >> /var/log/status-scarletnx-push.log 2>&1" \
	> /usr/local/etc/cron.d/status-scarletnx
sleep 5
service -j "$j" gatus status
echo "push.sh now (nothing said is every push taken):"
/usr/local/libexec/status-scarletnx/push.sh
tail -n 15 "$r/var/log/gatus.log"
