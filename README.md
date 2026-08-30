# P2P File Sharing System

A BitTorrent-style peer-to-peer file sharing system in C++17. A central
tracker coordinates metadata — accounts, groups, and which peers hold
which files — while clients transfer file data **directly to each other**.
File contents never pass through the tracker.

```
                  ┌──────────────┐
                  │   TRACKER    │   accounts · groups · file metadata
                  └──────┬───────┘   who currently has what
            control      │      control
         ┌───────────────┴───────────────┐
   ┌─────┴──────┐                  ┌─────┴──────┐
   │  CLIENT A  │◄────────────────►│  CLIENT B  │
   └────────────┘   file pieces    └────────────┘
                    (direct)
```

---

## Features

- **Parallel piece-based transfer** — files split into 512 KB pieces,
  fetched concurrently from multiple peers, each worker starting at a
  different peer to spread load across the swarm
- **Integrity checking** — every piece verified against its SHA-1 *before*
  it is written, and the reassembled file verified as a whole
- **Resumable downloads** — piece progress persisted after each piece; an
  interrupted download resumes instead of restarting
- **Swarm growth** — a client that completes a download automatically
  becomes a seeder for it
- **Session-token authentication** — salted password hashing, server-issued
  tokens, peer addresses taken from the socket rather than trusted from
  the client
- **Group access control** — private groups with owner-approved membership
- **Crash-safe persistence** — tracker state snapshotted atomically and
  restored on restart

---

## Build

Requires CMake ≥ 3.16, a C++17 compiler, and OpenSSL.

```bash
sudo apt-get install cmake g++ libssl-dev     # Debian/Ubuntu

cmake -S . -B build
cmake --build build -j"$(nproc)"
```

## Run

**1. Start the tracker** (config file holds `<ip> <port>`):

```bash
echo "127.0.0.1 9000" > tracker_info.txt
./build/tracker tracker_info.txt 1
```

**2. Start a client**, giving it the address it should listen on for
peer connections:

```bash
./build/client 127.0.0.1:6001 tracker_info.txt
```

**3. Share a file** — in client A:

```
create_user alice secret
login alice secret
create_group project
upload_file project /path/to/file.bin
```

**4. Fetch it** — in a second client on a different port:

```
create_user bob secret
login bob secret
join_group project          # then alice runs: accept_request project bob
download_file project file.bin /path/to/destination/
show_downloads
```

### Commands

| | |
|---|---|
| `create_user <user> <pass>` | register |
| `login <user> <pass>` / `logout` | session |
| `create_group <gid>` / `join_group <gid>` / `leave_group <gid>` | groups |
| `list_requests <gid>` | pending join requests (owner only) |
| `accept_request <gid> <user>` / `reject_request <gid> <user>` | approve or decline |
| `list_groups` / `list_files <gid>` | browse |
| `upload_file <gid> <path>` | share a file |
| `download_file <gid> <file> <dest>` | fetch a file |
| `stop_share <gid> <file>` | stop seeding |
| `show_downloads` | progress |
| `commands` / `exit` | |

---

## Tests

```bash
ctest --test-dir build --output-on-failure
```

36 unit tests plus four integration tests that drive the real binaries —
a full upload/download round trip with hash comparison, ungraceful
disconnect handling, state persistence across restart, and a concurrency
test intended for sanitizer builds.

**Under ThreadSanitizer:**

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j"$(nproc)"
./tests/integration/concurrency_test.sh build-tsan 12
```

This reports **0 data races**. Against the tracker as it stood before the
locking work the same test reports **27**. To reproduce that:

```bash
git checkout "$(git log --format=%H --grep='guard all shared state' -1)~1"
```

then copy this script in — it was added alongside the fix, so it does not
exist in the older tree.

---

## Layout

```
src/common/    framing, parsing, hashing, credentials   (shared)
src/tracker/   state, sessions, persistence, dispatch
src/client/    REPL, tracker connection, peer server, downloader
tests/unit/    36 tests + a small harness
tests/integration/  four scripts driving the real binaries
```

---

## Documentation

| | |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | components, thread model, locking strategy, how a transfer works |
| [PROTOCOL.md](docs/PROTOCOL.md) | exact wire format for both protocols |
| [DECISIONS.md](docs/DECISIONS.md) | every significant choice, the alternatives, and the known downsides |
| [INTERVIEW_PREP.md](docs/INTERVIEW_PREP.md) | the defects this rewrite fixed and how each was proven |

---

## Notes on scope

This began as a university project and was rewritten to fix a set of real
defects — an unsynchronised tracker, no actual authentication, and a
crash any client could trigger remotely. `DECISIONS.md` records what was
chosen and what was knowingly left out; the significant remaining
limitations are that passwords use SHA-256 rather than a slow KDF like
Argon2, and that there is no TLS, so credentials cross the wire in
plaintext. Both are documented rather than glossed over.
