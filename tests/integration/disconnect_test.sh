#!/usr/bin/env bash
# Does a peer that dies ungracefully stay advertised as a live seeder?
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
TPORT=$((36000 + RANDOM % 4000)); APORT=$((TPORT+1)); BPORT=$((TPORT+2))
echo "127.0.0.1 $TPORT" > "$WORK/ti.txt"; mkdir -p "$WORK/dest"
head -c 600000 /dev/urandom > "$WORK/f.bin"

"$ROOT/build/tracker" "$WORK/ti.txt" 1 > "$WORK/t.log" 2>&1 & TPID=$!
sleep 1

mkfifo "$WORK/fa"
"$ROOT/build/client" "127.0.0.1:$APORT" "$WORK/ti.txt" < "$WORK/fa" > "$WORK/a.log" 2>&1 & APID=$!
exec 3>"$WORK/fa"; sleep 0.3
printf 'create_user alice pw\nlogin alice pw\ncreate_group g1\nupload_file g1 %s/f.bin\n' "$WORK" >&3
sleep 1.2
printf 'create_user bob pw\nlogin bob pw\njoin_group g1\nexit\n' \
  | timeout 10 "$ROOT/build/client" "127.0.0.1:$BPORT" "$WORK/ti.txt" > /dev/null 2>&1
printf 'accept_request g1 bob\n' >&3
sleep 0.8

# Kill alice's client outright -- no logout, no clean shutdown.
kill -9 $APID 2>/dev/null
exec 3>&-
sleep 1
echo "--- alice killed ungracefully; bob now tries to download ---"
OUT=$(printf 'login bob pw\ndownload_file g1 f.bin %s/dest/\nexit\n' "$WORK" \
  | timeout 25 "$ROOT/build/client" "127.0.0.1:$BPORT" "$WORK/ti.txt" 2>&1)
echo "$OUT" | grep -E "No active peers|Starting download|Failed after" | sed 's/^/  /'

kill -9 $TPID 2>/dev/null

# The tracker must no longer advertise the dead peer. Before the fix it
# reported "from 1 peers" and the downloader burned its whole retry
# budget against an address that was gone.
if echo "$OUT" | grep -q "No active peers"; then
  echo "PASS: dead peer is no longer advertised as a seeder"
else
  echo "FAIL: tracker still advertised the dead peer"
  exit 1
fi
