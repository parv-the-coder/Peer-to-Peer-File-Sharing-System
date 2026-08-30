#!/usr/bin/env bash
# Spec section 3.3: a client must be able to download several files at
# once. download_file used to block the REPL until the transfer finished,
# so a second download could not even be issued.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-build}
W=$(mktemp -d); trap 'kill -9 ${PIDS:-} 2>/dev/null; rm -rf "$W"' EXIT
PIDS=""
TP=$((24000 + RANDOM % 12000)); AP=$((TP+1)); BP=$((TP+2))
echo "127.0.0.1 $TP" > "$W/ti.txt"; mkdir -p "$W/dest"

wait_for_tracker() {
  for _ in $(seq 1 100); do
    grep -q "TRACKER SERVER STARTED" "$1" 2>/dev/null && return 0
    sleep 0.2
  done
  return 1
}

for i in 1 2 3; do head -c 4000000 /dev/urandom > "$W/f$i.bin"; done

( cd "$W" && exec "$ROOT/$BUILD/tracker" "$W/ti.txt" 1 ) > "$W/t.log" 2>&1 &
TPID=$!; PIDS="$TPID"
wait_for_tracker "$W/t.log" || { echo "FAIL: tracker did not start"; exit 1; }

mkfifo "$W/fa"
"$ROOT/$BUILD/client" "127.0.0.1:$AP" "$W/ti.txt" < "$W/fa" > "$W/a.log" 2>&1 &
APID=$!; PIDS="$PIDS $APID"
exec 3>"$W/fa"; sleep 0.4
printf 'create_user alice pw\nlogin alice pw\ncreate_group g\n' >&3
for i in 1 2 3; do printf 'upload_file g %s/f%d.bin\n' "$W" "$i" >&3; done
sleep 2

printf 'create_user bob pw\nlogin bob pw\njoin_group g\nexit\n' \
  | timeout 15 "$ROOT/$BUILD/client" "127.0.0.1:$BP" "$W/ti.txt" > /dev/null 2>&1
printf 'accept_request g bob\n' >&3; sleep 1

# Issue all three downloads back to back without waiting for any of them.
mkfifo "$W/fb"
"$ROOT/$BUILD/client" "127.0.0.1:$BP" "$W/ti.txt" < "$W/fb" > "$W/b.log" 2>&1 &
BPID=$!; PIDS="$PIDS $BPID"
exec 4>"$W/fb"; sleep 0.4
printf 'login bob pw\n' >&4; sleep 0.6
for i in 1 2 3; do printf 'download_file g f%d.bin %s/dest/\n' "$i" "$W" >&4; done
sleep 0.4
# The prompt must still respond while transfers are in flight.
printf 'show_downloads\n' >&4
sleep 8
printf 'exit\n' >&4; exec 3>&- ; exec 4>&-
sleep 1

echo "--- all three issued without blocking ---"
grep -c "Download started in background" "$W/b.log" | sed 's/^/  started: /'
echo "--- completions ---"
grep -E '^\[C\] \[' "$W/b.log" | sed 's/^/  /'

FAIL=0
[ "$(grep -c 'Download started in background' "$W/b.log")" = "3" ] || { echo "FAIL: not all three started"; FAIL=1; }
for i in 1 2 3; do
  A=$(sha1sum "$W/f$i.bin" | cut -d' ' -f1)
  B=$(sha1sum "$W/dest/f$i.bin" 2>/dev/null | cut -d' ' -f1 || true)
  [ "$A" = "${B:-}" ] || { echo "FAIL: f$i.bin mismatch"; FAIL=1; }
done
# spec section 8 format
grep -qE '^\[C\] \[g\] f1\.bin$' "$W/b.log" || { echo "FAIL: status format not '[C] [group] file'"; FAIL=1; }
[ "$FAIL" = 0 ] && echo "PASS: 3 concurrent downloads, all verified, correct status format"
exit $FAIL
