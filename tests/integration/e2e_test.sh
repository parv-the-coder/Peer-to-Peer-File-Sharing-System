#!/usr/bin/env bash
# End-to-end: tracker + 2 clients, upload from A, download from B, compare hashes.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

TPORT=$((20000 + RANDOM % 20000))
APORT=$((TPORT + 1))
BPORT=$((TPORT + 2))
echo "127.0.0.1 $TPORT" > "$WORK/tracker_info.txt"
mkdir -p "$WORK/dest"

head -c 2000000 /dev/urandom > "$WORK/testfile.bin"
SRC=$(sha1sum "$WORK/testfile.bin" | cut -d' ' -f1)
echo "source sha1: $SRC  (tracker port $TPORT)"

"$ROOT/build/tracker" "$WORK/tracker_info.txt" 1 > "$WORK/tracker.log" 2>&1 &
TPID=$!
sleep 1
if ! grep -q "TRACKER SERVER STARTED" "$WORK/tracker.log"; then
  echo "FAIL: tracker did not start"; cat "$WORK/tracker.log"; kill -9 $TPID 2>/dev/null; exit 1
fi

# Client A stays alive (it is the seeder) and is driven through a FIFO.
mkfifo "$WORK/fifoA"
"$ROOT/build/client" "127.0.0.1:$APORT" "$WORK/tracker_info.txt" < "$WORK/fifoA" > "$WORK/clientA.log" 2>&1 &
APID=$!
exec 3>"$WORK/fifoA"
sleep 0.3
printf 'create_user alice pw\nlogin alice pw\ncreate_group g1\nupload_file g1 %s/testfile.bin\n' "$WORK" >&3
sleep 1.2

# Client B requests to join, then exits.
printf 'create_user bob pw\nlogin bob pw\njoin_group g1\nexit\n' \
  | timeout 10 "$ROOT/build/client" "127.0.0.1:$BPORT" "$WORK/tracker_info.txt" > "$WORK/clientB_join.log" 2>&1

printf 'accept_request g1 bob\n' >&3
sleep 0.8

# Client B logs back in and downloads.
printf 'login bob pw\ndownload_file g1 testfile.bin %s/dest/\nshow_downloads\nexit\n' "$WORK" \
  | timeout 30 "$ROOT/build/client" "127.0.0.1:$BPORT" "$WORK/tracker_info.txt" > "$WORK/clientB_dl.log" 2>&1

printf 'exit\n' >&3
exec 3>&-
sleep 0.5
kill -9 $APID $TPID 2>/dev/null
wait 2>/dev/null

echo "=== client A ==="; grep '>>>' "$WORK/clientA.log" | sed 's/^/  /'
echo "=== client B download ==="; grep -E '\[C\]|Progress|Status|mismatch|No active peers|Access denied' "$WORK/clientB_dl.log" | sed 's/^/  /'

if [ ! -f "$WORK/dest/testfile.bin" ]; then
  echo "FAIL: no downloaded file"; exit 1
fi
DST=$(sha1sum "$WORK/dest/testfile.bin" | cut -d' ' -f1)
echo "dest sha1:   $DST"
[ "$SRC" = "$DST" ] && echo "PASS: hashes match" || { echo "FAIL: hash mismatch"; exit 1; }
