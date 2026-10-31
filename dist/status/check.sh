#!/bin/sh
# What can be checked without running Gatus: the colours are Wimsy's (colours.py), gatus.yaml and
# push.sh are their templates rendered (render.sh) and leave no @NAME@ but @PUSH_TOKEN@ (install.sh's),
# gatus.yaml is YAML with the keys Gatus reads, a bot with an icon for each external endpoint and the
# reverse, push.sh pushes to each of them, the logo and icons it links to are there, and every
# endpoint answers now as its conditions expect (curl; the Discord one by its code).
# Usage: check.sh WIMSY_SVG
set -eu
here=$(dirname "$0")
[ $# -eq 1 ] || { echo "usage: $0 WIMSY_SVG (ScarletNX's dist/wimsy.svg)" >&2; exit 64; }
python3 -I "$here/colours.py" "$1" --check
cp "$here/gatus.yaml" "$here/gatus.yaml.kept"
cp "$here/push.sh" "$here/push.sh.kept"
"$here/render.sh"
for f in gatus.yaml push.sh; do
	if ! cmp -s "$here/$f" "$here/$f.kept"; then
		diff -u "$here/$f.kept" "$here/$f" >&2 || true
		rm -f "$here/gatus.yaml.kept" "$here/push.sh.kept"
		echo "$f was not its template rendered: it is now, look at the diff" >&2
		exit 1
	fi
done
rm -f "$here/gatus.yaml.kept" "$here/push.sh.kept"
if grep -n '@[A-Z_]*@' "$here/gatus.yaml" "$here/push.sh" | grep -v '@PUSH_TOKEN@' >&2; then
	echo "a @NAME@ that neither addresses.conf nor icons/ gives" >&2
	exit 1
fi
sh -n "$here/push.sh"
if command -v shellcheck > /dev/null; then shellcheck "$here/push.sh"; fi
python3 -I - "$here/gatus.yaml" "$here/push.sh" "$here/icons" <<'EOF'
import os, re, sys, yaml
c = yaml.safe_load(open(sys.argv[1]))
assert set(c) == {"metrics", "storage", "web", "ui", "endpoints", "external-endpoints"}, sorted(c)
bots = [e["name"] for e in c["external-endpoints"]]
icons = sorted(f[:-4] for f in os.listdir(sys.argv[3]) if f.endswith(".png"))
assert sorted(b.lower() for b in bots) == icons, (bots, icons)
css = c["ui"]["custom-css"]
push = open(sys.argv[2]).read()
for e in c["external-endpoints"]:
    assert e["group"] == "Bots" and e["token"] == "@PUSH_TOKEN@" and e["heartbeat"]["interval"] == "5m", e
    assert f'span[title="{e["name"]}"]::before{{background-image:url(data:image/png;base64,' in css, e["name"]
    key = "bots_" + re.sub(r"[ /_,.#+&]", "-", e["name"]).lower()
    assert re.search(rf"^\s*(bot|push) {key} ", push, re.M), ("push.sh never pushes", key)
print(len(bots), "bots:", ", ".join(bots))
assert set(c["ui"]["favicon"]) == {"default", "size16x16", "size32x32"}, c["ui"]["favicon"]
for e in c["endpoints"]:
    missing = {"name", "group", "url", "interval", "conditions"} - set(e)
    assert not missing, (e.get("name"), missing)
print(len(c["endpoints"]), "endpoints:", ", ".join(e["name"] for e in c["endpoints"]))
EOF
# Each endpoint as its conditions see it now: the status, and the one thing of the body each checks.
eval "$(grep -v '^#' "$here/addresses.conf" | grep '=')"
got() { curl -sS -o "$2" -w '%{http_code}' -H 'Accept: application/vnd.github+json' "$1"; }
body=$(mktemp)
trap 'rm -f "$body"' EXIT
for u in "$WIMSY_URL" "$WIMSY_ICONS/wimsy.ico" "$WIMSY_ICONS/hicolor/16x16/apps/wimsy.png" "$WIMSY_ICONS/hicolor/32x32/apps/wimsy.png"; do
	s=$(curl -sS -o /dev/null -w '%{http_code}' "$u"); echo "$u: $s (200)"; [ "$s" = 200 ]
done
s=$(curl -sS -o /dev/null -w '%{http_code}' "$SITE_URL/")
echo "scarletnx.dev: $s (302)"; [ "$s" = 302 ]
s=$(got "$PAGES_URL/" "$body"); echo "Site: $s (200)"; [ "$s" = 200 ]
grep -q 'ScarletNX is a static recompiler' "$body"
s=$(got "$NIGHTLY_API" "$body"); echo "Nightly release: $s (200), $(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["tag_name"])' "$body")"
[ "$s" = 200 ]
s=$(got "https://discord.com/api/v10/invites/$DISCORD_INVITE" "$body"); echo "Discord invite: $s (200)"
[ "$s" = 200 ]
python3 -I -c 'import json,sys; assert json.load(open(sys.argv[1]))["code"] == sys.argv[2]' "$body" "$DISCORD_INVITE"
echo "ok"
