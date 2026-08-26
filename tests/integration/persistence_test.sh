#!/usr/bin/env bash
# Tracker state must survive a restart: accounts, groups, memberships and
# file metadata are reloaded, while connected flags and session tokens
# (both properties of a live socket) are not.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
TPORT=$((38000 + RANDOM % 2000)); APORT=$((TPORT+1))
echo "127.0.0.1 $TPORT" > "$WORK/ti.txt"
head -c 300000 /dev/urandom > "$WORK/f.bin"
DB="$WORK/state.db"

# exec so the subshell is replaced by the tracker itself and $! is the
# tracker's own pid, not the subshell's.
run_tracker() {
  ( cd "$WORK" && exec "$ROOT/build/tracker" "$WORK/ti.txt" 1 ) > "$WORK/t$1.log" 2>&1 &
  echo $! > "$WORK/tpid"
  sleep 1
}

# --- first run: create state ---
run_tracker 1
TPID=$(cat "$WORK/tpid")
printf 'create_user alice pw\nlogin alice pw\ncreate_group g1\nupload_file g1 %s/f.bin\nexit\n' "$WORK" \
  | timeout 15 "$ROOT/build/client" "127.0.0.1:$APORT" "$WORK/ti.txt" > "$WORK/c1.log" 2>&1
kill -TERM $TPID 2>/dev/null
sleep 1.5

if [ ! -f "$WORK/tracker_state.db" ]; then
  echo "FAIL: no snapshot written"; ls -la "$WORK"; exit 1
fi
echo "snapshot contents:"; sed 's/^/  /' "$WORK/tracker_state.db" | cut -c1-90

# --- second run: state must come back ---
run_tracker 2
TPID=$(cat "$WORK/tpid")
grep -q "Restored state" "$WORK/t2.log" || { echo "FAIL: did not restore"; cat "$WORK/t2.log"; exit 1; }

OUT=$(printf 'login alice pw\nlist_groups\nlist_files g1\nexit\n' \
  | timeout 15 "$ROOT/build/client" "127.0.0.1:$APORT" "$WORK/ti.txt" 2>&1)
kill -9 $TPID 2>/dev/null

echo "after restart:"; echo "$OUT" | grep -E "logged in|g1|f.bin|SIZE" | sed 's/^/  /'

FAIL=0
echo "$OUT" | grep -q "You are now logged in" || { echo "FAIL: password did not survive"; FAIL=1; }
echo "$OUT" | grep -q "f.bin SIZE:300000"     || { echo "FAIL: file metadata did not survive"; FAIL=1; }
[ "$FAIL" = 0 ] && echo "PASS: accounts, groups and file metadata survived restart"
exit $FAIL
