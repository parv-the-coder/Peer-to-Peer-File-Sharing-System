#!/usr/bin/env bash
# Hammer the tracker with N concurrent clients all mutating shared state
# (users, groups, join requests) at once. Intended to be run against a
# ThreadSanitizer build to surface data races on the tracker's maps.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-build-tsan}
N=${2:-12}
CLIENTS=""
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

TPORT=$((40000 + RANDOM % 20000))
echo "127.0.0.1 $TPORT" > "$WORK/tracker_info.txt"

TSAN_OPTIONS="halt_on_error=0 log_path=$WORK/tsan" \
  setarch -R "$ROOT/$BUILD/tracker" "$WORK/tracker_info.txt" 1 > "$WORK/tracker.log" 2>&1 &
TPID=$!
sleep 2
grep -q "TRACKER SERVER STARTED" "$WORK/tracker.log" || { echo "tracker failed to start"; cat "$WORK/tracker.log"; exit 1; }

# All clients start at once and contend on the same groups.
for i in $(seq 1 "$N"); do
  {
    printf 'create_user u%d pw\nlogin u%d pw\ncreate_group shared%d\njoin_group shared1\njoin_group shared2\njoin_group shared3\nlist_groups\nlist_requests shared1\nlist_files shared1\nlogout\nexit\n' "$i" "$i" $((i % 3 + 1))
  } | timeout 25 setarch -R "$ROOT/$BUILD/client" "127.0.0.1:$((TPORT + i))" "$WORK/tracker_info.txt" > "$WORK/c$i.log" 2>&1 &
  CLIENTS="$CLIENTS $!"
done
# Wait only on the clients: a bare `wait` would also wait on the tracker,
# which runs until killed.
for p in $CLIENTS; do wait "$p" 2>/dev/null; done

sleep 1
kill -9 $TPID 2>/dev/null
wait 2>/dev/null

echo "clients that completed: $(grep -l 'Exiting Client' "$WORK"/c*.log 2>/dev/null | wc -l)/$N"
echo "users registered:       $(grep -h 'registered successfully' "$WORK"/c*.log 2>/dev/null | wc -l)/$N"

REPORTS=$(cat "$WORK"/tsan.* 2>/dev/null | grep -c 'WARNING: ThreadSanitizer' || true)
echo "TSan warnings:          ${REPORTS:-0}"
if [ "${REPORTS:-0}" != "0" ]; then
  echo "--- first report ---"
  cat "$WORK"/tsan.* 2>/dev/null | sed -n '/WARNING: ThreadSanitizer/,/^$/p' | head -40
  exit 1
fi
echo "PASS: no data races reported"
