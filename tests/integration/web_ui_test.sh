#!/usr/bin/env bash
# The dashboard must (a) serve its page, (b) drive a real transfer through
# the same command path the REPL uses, (c) refuse privileged commands
# before login, and (d) listen on loopback only.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-build}
W=$(mktemp -d); trap 'kill -9 ${PIDS:-} 2>/dev/null; rm -rf "$W"' EXIT
PIDS=""
T=$((22000 + RANDOM % 12000)); A=$((T+1)); B=$((T+2)); WA=$((T+50)); WB=$((T+51))
echo "127.0.0.1 $T" > "$W/ti.txt"; mkdir -p "$W/dest"
head -c 2000000 /dev/urandom > "$W/f.bin"
SRC=$(sha1sum "$W/f.bin" | cut -d' ' -f1)

wait_for() { for _ in $(seq 1 100); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.2; done; return 1; }
pa() { curl -s -X POST -H 'Content-Type: application/json' -d "$1" "http://127.0.0.1:$WA/api/command"; }
pb() { curl -s -X POST -H 'Content-Type: application/json' -d "$1" "http://127.0.0.1:$WB/api/command"; }

( cd "$W" && exec "$ROOT/$BUILD/tracker" "$W/ti.txt" 1 ) > "$W/t.log" 2>&1 &
PIDS="$!"
wait_for "$W/t.log" "TRACKER SERVER STARTED" || { echo "FAIL: tracker down"; exit 1; }

mkfifo "$W/fa" "$W/fb"
"$ROOT/$BUILD/client" "127.0.0.1:$A" "$W/ti.txt" "$WA" < "$W/fa" > "$W/a.log" 2>&1 & PIDS="$PIDS $!"
"$ROOT/$BUILD/client" "127.0.0.1:$B" "$W/ti.txt" "$WB" < "$W/fb" > "$W/b.log" 2>&1 & PIDS="$PIDS $!"
exec 3>"$W/fa"; exec 4>"$W/fb"
wait_for "$W/a.log" "Dashboard:" || { echo "FAIL: dashboard did not start"; cat "$W/a.log"; exit 1; }
sleep 0.5

FAIL=0

# (a) the page is served
CODE=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$WA/")
echo "  GET /            -> HTTP $CODE"
[ "$CODE" = "200" ] || { echo "FAIL: page not served"; FAIL=1; }

# (c) privileged command before login is refused
R=$(pa '{"args":["create_group","nope"]}')
echo "  create_group before login -> $(echo "$R" | head -c 52)"
case "$R" in *"must log in"*) ;; *) echo "FAIL: web bypassed the login check"; FAIL=1 ;; esac

# (d) loopback only
BOUND=$(ss -ltn 2>/dev/null | grep -c "127.0.0.1:$WA" || true)
echo "  bound to loopback only    -> $([ "$BOUND" = "1" ] && echo yes || echo NO)"
[ "$BOUND" = "1" ] || { echo "FAIL: not bound to loopback"; FAIL=1; }

# malformed bodies must not take the client down
for bad in '{}' '{"args":}' 'garbage'; do pa "$bad" > /dev/null; done
pgrep -x client > /dev/null || { echo "FAIL: client died on malformed input"; FAIL=1; }

# (b) a real transfer, driven only through the API
pa '{"args":["create_user","alice","pw"]}' > /dev/null
pa '{"args":["login","alice","pw"]}'       > /dev/null
pa '{"args":["create_group","g"]}'         > /dev/null
pa "{\"args\":[\"upload_file\",\"g\",\"$W/f.bin\"]}" > /dev/null
pb '{"args":["create_user","bob","pw"]}'   > /dev/null
pb '{"args":["login","bob","pw"]}'         > /dev/null
pb '{"args":["join_group","g"]}'           > /dev/null
pa '{"args":["accept_request","g","bob"]}' > /dev/null
pb "{\"args\":[\"download_file\",\"g\",\"f.bin\",\"$W/dest/\"]}" > /dev/null
sleep 6

DST=$(sha1sum "$W/dest/f.bin" 2>/dev/null | cut -d' ' -f1 || true)
echo "  transfer via web API      -> $([ "$SRC" = "${DST:-}" ] && echo 'hashes match' || echo MISMATCH)"
[ "$SRC" = "${DST:-}" ] || FAIL=1

# status endpoint reports the finished download
S=$(curl -s "http://127.0.0.1:$WB/api/status")
echo "  /api/status downloads     -> $(echo "$S" | grep -o '"completed_pieces":[0-9]*' | head -1)"
case "$S" in *'"filename":"f.bin"'*) ;; *) echo "FAIL: status missing the download"; FAIL=1 ;; esac

printf 'exit\n' >&3 2>/dev/null; printf 'exit\n' >&4 2>/dev/null
exec 3>&- 2>/dev/null; exec 4>&- 2>/dev/null
sleep 1
[ "$FAIL" = 0 ] && echo "PASS: dashboard serves, enforces login, stays local, and drives transfers"
exit $FAIL
