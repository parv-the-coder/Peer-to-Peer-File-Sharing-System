# Testing

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

That runs 7 test targets: the unit suite plus six integration scripts. The
integration scripts drive the real `tracker` and `client` binaries over real
sockets on random high ports, each in its own temp directory, so they can run
on a machine that is doing other things.

---

## Unit tests

36 tests in [`tests/unit/`](../tests/unit/), linked against the real tracker
code rather than a copy.

| File | Covers |
|---|---|
| `test_hash.cpp` | SHA-1 and SHA-256 against published known-answer vectors, plus piece splitting |
| `test_message.cpp` | Tokenising, and strict integer parsing: empty strings, trailing garbage, overflow |
| `test_auth.cpp` | Salt uniqueness, verify success and failure, empty and corrupt stored hashes |
| `test_session.cpp` | Token issue, lookup, expiry, logout, revoke-all |
| `test_tracker_state.cpp` | State transitions and the authorisation rules: who may accept a request, what a non-member may list, cross-group file isolation |

The harness is [`tests/unit/test_framework.h`](../tests/unit/test_framework.h),
under 100 lines. See
[DECISIONS.md](DECISIONS.md#11-a-small-test-harness-instead-of-catch2) for why
it is not Catch2.

---

## Integration tests

| Script | What it proves |
|---|---|
| `e2e_test.sh` | Tracker plus two clients. Upload from A, download from B, then compare SHA-1 of source and destination. |
| `concurrent_download_test.sh` | Three files download at the same time. `download_file` used to block the REPL, so a second download could not even be issued. |
| `disconnect_test.sh` | A peer killed ungracefully stops being advertised as a live seeder. |
| `persistence_test.sh` | Accounts, groups, memberships and file metadata survive a tracker restart, while connected flags and session tokens do not. |
| `tracker_sync_test.sh` | Two trackers hold the same state, the system keeps working while one is down, and a tracker that rejoins catches up. Also asserts idle trackers stay quiet, which guards the replication ping-pong bug in [DECISIONS.md](DECISIONS.md#6-tracker-replication-symmetric-union-merge-no-primary). |
| `web_ui_test.sh` | The dashboard serves its page, drives a real transfer through the same command path the REPL uses, refuses privileged commands before login, and listens on loopback only. |

Each script waits for the string `TRACKER SERVER STARTED` in the tracker log
rather than sleeping for a fixed time, because a sanitizer build starts several
times slower than a plain one.

[`tests/integration/_session.py`](../tests/integration/_session.py) is a small
helper that speaks the framed tracker protocol directly, used where a test
needs to check a raw reply rather than CLI output.

---

## ThreadSanitizer run

A seventh script is not in `ctest`, because it needs its own build:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j"$(nproc)"
./tests/integration/concurrency_test.sh build-tsan 12
```

It drives 12 concurrent clients that all mutate shared tracker state at once,
creating users, creating groups and sending join requests, which is the pattern
that exposes races on the tracker's maps.

It reports 0 data races. The same test against the tracker before the locking
work reported 27.

The second argument is the client count, so `./tests/integration/concurrency_test.sh build-tsan 30`
turns the pressure up.
