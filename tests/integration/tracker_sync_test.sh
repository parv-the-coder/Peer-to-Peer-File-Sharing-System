#!/usr/bin/env bash
# Brief sections 2.1 and 7: two trackers hold synchronised state, the
# system keeps working while one is down, and a tracker that rejoins
# catches up on what it missed.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-build}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d); trap 'kill -9 ${PIDS:-} 2>/dev/null; rm -rf "$W"' EXIT
PIDS=""
T1=$((28000 + RANDOM % 8000)); T2=$((T1+1)); CP=$((T1+2))
printf '127.0.0.1 %d\n127.0.0.1 %d\n' "$T1" "$T2" > "$W/ti.txt"

up() { for _ in $(seq 1 100); do grep -q "TRACKER SERVER STARTED" "$1" 2>/dev/null && return 0; sleep 0.2; done; return 1; }

# One persistent connection per session: sessions are revoked when the
# connection drops, so a token does not survive reconnecting.
# Usage: session <port> "cmd1|cmd2|..."   ("TOKEN" is substituted)
session() { python3 "$HERE/_session.py" "$@"; }

start_t() { ( cd "$W" && exec "$ROOT/$BUILD/tracker" "$W/ti.txt" "$1" ) > "$W/t$1_$2.log" 2>&1 & PIDS="$PIDS $!"; echo $!; }

FAIL=0

echo "--- 1. both trackers start, each on its OWN port ---"
P1=$(start_t 1 a); up "$W/t1_a.log" || { echo "FAIL: tracker 1 down"; exit 1; }
P2=$(start_t 2 a); up "$W/t2_a.log" || { echo "FAIL: tracker 2 down"; exit 1; }
grep -h "Listening on IP" "$W/t1_a.log" "$W/t2_a.log" | sed 's/^/    /'
grep -q "Port: $T1" "$W/t1_a.log" || { echo "FAIL: tracker 1 bound wrong port"; FAIL=1; }
grep -q "Port: $T2" "$W/t2_a.log" || { echo "FAIL: tracker 2 bound wrong port"; FAIL=1; }
sleep 3
grep -h "\[sync\] linked" "$W/t1_a.log" "$W/t2_a.log" | head -2 | sed 's/^/    /'

echo
echo "--- 2. write on tracker 1, read it on tracker 2 ---"
session "$T1" "create_user alice pw" | sed 's/^/    T1: /'
sleep 2.5
R=$(session "$T2" "login alice pw 7001")
echo "    T2: ${R:0:34}..."
case "$R" in OK*) echo "    -> account created on T1 works on T2" ;;
             *)   echo "FAIL: user did not replicate to T2"; FAIL=1 ;; esac

echo
echo "--- 3. write on tracker 2, read it on tracker 1 ---"
session "$T2" "login alice pw 7001|create_group TOKEN proj" | tail -1 | sed 's/^/    T2: /'
sleep 2.5
G=$(session "$T1" "login alice pw 7001|list_groups TOKEN" | tail -1)
echo "    T1 list_groups: $G"
case "$G" in *proj*) echo "    -> group created on T2 is visible on T1" ;;
             *)      echo "FAIL: group did not replicate to T1"; FAIL=1 ;; esac

echo
echo "--- 4. kill tracker 1 -- tracker 2 keeps serving ---"
kill -9 "$P1" 2>/dev/null; sleep 2
R2=$(session "$T2" "create_user bob pw")
echo "    T2 (T1 down): ${R2:0:46}..."
case "$R2" in *successfully*) echo "    -> still operational on a single tracker" ;;
              *)              echo "FAIL: T2 unusable"; FAIL=1 ;; esac

echo
echo "--- 5. a client fails over to tracker 2 by itself ---"
printf 'exit\n' | timeout 15 "$ROOT/$BUILD/client" "127.0.0.1:$CP" "$W/ti.txt" 2>&1 \
  | grep -E "unreachable|Connected to tracker" | sed 's/^/    /'
printf 'exit\n' | timeout 15 "$ROOT/$BUILD/client" "127.0.0.1:$CP" "$W/ti.txt" 2>&1 \
  | grep -q "Connected to tracker 2" || { echo "FAIL: client did not fail over"; FAIL=1; }

echo
echo "--- 6. restart tracker 1 -- it catches up on what it missed ---"
P1=$(start_t 1 b); up "$W/t1_b.log" || { echo "FAIL: tracker 1 did not restart"; FAIL=1; }
sleep 6
R3=$(session "$T1" "login bob pw 7002")
echo "    T1 login bob (created while T1 was dead): ${R3:0:34}..."
case "$R3" in OK*) echo "    -> rejoining tracker recovered the missed write" ;;
              *)   echo "FAIL: T1 did not catch up"; FAIL=1 ;; esac

echo
echo "--- 7. replication is quiet when nothing changes ---"
B1=$(wc -c < "$W/t1_b.log"); sleep 4; B2=$(wc -c < "$W/t1_b.log")
echo "    tracker log grew ${B1} -> ${B2} bytes over 4 idle seconds"
[ $((B2 - B1)) -lt 2000 ] || { echo "FAIL: trackers are chattering while idle"; FAIL=1; }

echo
[ "$FAIL" = 0 ] && echo "PASS: two trackers sync, survive failure, and catch up on rejoin"
exit $FAIL
