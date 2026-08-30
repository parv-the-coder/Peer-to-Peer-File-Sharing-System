# P2P File Sharing System

A BitTorrent-style peer-to-peer file sharing system in C++17. A central
**tracker** coordinates metadata — accounts, groups, and which peers hold
which files — while **clients transfer file data directly to each other**.
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

Built for IIIT-H Advanced Operating Systems (Monsoon 2025), then rewritten
to fix a set of real defects — an unsynchronised tracker, no authentication,
and a remotely triggerable crash. See [what changed](#what-the-rewrite-fixed).

---

## What it does

| Area | Capability |
|---|---|
| **Accounts** | Register, login/logout, salted-hash password storage, server-issued session tokens |
| **Groups** | Create, browse, request to join, owner-approved accept **or reject**, leave (ownership hands on automatically) |
| **Sharing** | Share a file with a group, list a group's files, stop seeding |
| **Downloading** | Parallel multi-peer piece fetch, per-piece SHA-1 verification, automatic retry against a different peer, resume after interruption |
| **Swarm** | A client that finishes a download automatically becomes a seeder |
| **Progress** | `show_downloads` reports per-file piece counts and status |
| **Durability** | Tracker state snapshotted atomically and restored on restart |
| **Replication** | Two trackers keep synchronised state; either alone serves every command, and one rejoining after an outage catches up automatically |
| **Failover** | Clients try each tracker in turn, so one tracker being down is invisible |
| **Robustness** | Dead peers stop being advertised; malformed input is rejected rather than crashing |

### How a transfer actually works

1. **Upload** — the client SHA-1 hashes the file whole *and* per 512 KB
   piece, registers the local path so it can serve pieces, and sends only
   *metadata* to the tracker.
2. **Discover** — a downloader asks the tracker for that metadata plus the
   list of peers currently online holding the file.
3. **Fetch in parallel** — up to 8 worker threads pull pieces at once. Each
   worker starts at a *different* peer (the peer list is rotated by worker
   index) so they spread across the swarm instead of all hitting the first
   seeder.
4. **Verify before writing** — every piece is checked against its expected
   SHA-1 *before* it touches the output file. A bad piece is discarded and
   retried against the next peer, up to 5 attempts.
5. **Verify the whole** — the reassembled file is checked against the
   full-file hash.
6. **Join the swarm** — the client tells the tracker it now has the file.

Because pieces are verified *before* being written, a misbehaving peer can
waste bandwidth but cannot corrupt your file.

---

## Quick start

Requires CMake ≥ 3.16, a C++17 compiler, and OpenSSL.

```bash
sudo apt-get install cmake g++ libssl-dev     # Debian/Ubuntu
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

**Start the trackers** — one `<ip> <port>` line per tracker; the number
selects which line this instance binds:

```bash
printf '127.0.0.1 9000\n127.0.0.1 9001\n' > tracker_info.txt

./build/tracker tracker_info.txt 1     # terminal 1
./build/tracker tracker_info.txt 2     # terminal 2
```

They find each other and replicate automatically, in either start order.
A single line in the file is fine too — the tracker then runs standalone.

**Client A — share a file:**

```bash
./build/client 127.0.0.1:6001 tracker_info.txt
```
```
create_user alice secret
login alice secret
create_group aos
upload_file aos /path/to/file.bin
```

**Client B — fetch it** (different peer port):

```bash
./build/client 127.0.0.1:6002 tracker_info.txt
```
```
create_user bob secret
login bob secret
join_group aos                       # alice then runs: accept_request aos bob
download_file aos file.bin /path/to/dest/
show_downloads
```

Type `commands` for the full list, `exit` to quit, `quit` in the tracker
console to shut it down.

### Command reference

| Command | Purpose |
|---|---|
| `create_user <user> <pass>` | Register an account |
| `login <user> <pass>` · `logout` | Start / end a session |
| `create_group <gid>` | Create a group (you become owner) |
| `join_group <gid>` | Request membership |
| `leave_group <gid>` | Leave (ownership passes on if you owned it) |
| `list_groups` | All groups on the network |
| `list_requests <gid>` | Pending join requests (owner only) |
| `accept_request <gid> <user>` | Approve a request (owner only) |
| `reject_request <gid> <user>` | Decline a request (owner only) |
| `list_files <gid>` | Files shared in a group |
| `upload_file <gid> <path>` | Share a file |
| `download_file <gid> <file> <dest>` | Fetch a file |
| `show_downloads` | Download progress |
| `stop_share <gid> <file>` | Stop seeding a file |

---

## Design at a glance

```
src/common/    framing · parsing · hashing · credentials   (shared)
src/tracker/   state · sessions · persistence · dispatch
src/client/    REPL · tracker connection · peer server · downloader
tests/         36 unit tests + 4 integration scripts
```

### Key data structures and why

| Structure | Why |
|---|---|
| `TrackerState` — 4 hash maps behind **one `shared_mutex`** | Most commands touch several maps at once, so per-map locks would need lock-ordering discipline for little real gain. Reads (`list_*`, `download_file`) run concurrently; writes are exclusive. |
| `SessionManager` — token → session, **its own mutex** | Every authenticated command validates a token, making it the hottest read in the system. Kept off the state lock so it never queues behind a slow `list_files`. |
| `FileMeta` keyed by **(group, filename)** | Keyed by filename alone, an upload to one group silently overwrote another group's same-named file. |
| `UploadRegistry` — mutex-guarded filename → path | Read by every peer-serving thread while the CLI thread mutates it. |
| Piece-status vector + `.downloading` sidecar | Makes downloads resumable; written after each piece. |

### Protocol in one line

Every message on both protocols is `[4-byte big-endian length][payload]`.
Length-prefixing is what makes a message spanning multiple TCP segments
safe — the original code assumed one `read()` returned one whole message,
which silently truncated large messages. Full grammar in
[docs/PROTOCOL.md](docs/PROTOCOL.md).

---

## Testing

```bash
ctest --test-dir build --output-on-failure
```

36 unit tests (hashing against published known-answer vectors, parser edge
cases, password handling, session lifetime, tracker state transitions and
authorisation rules) plus integration scripts that drive the **real
binaries**: a full upload/download round trip with hash comparison,
three simultaneous downloads, ungraceful-disconnect handling, persistence
across restart, two-tracker synchronisation with failover and recovery,
and a concurrency test for sanitizer builds.

**Under ThreadSanitizer:**

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j"$(nproc)"
./tests/integration/concurrency_test.sh build-tsan 12
```

Reports **0 data races**. The same test against the tracker before the
locking work reports **27**.

---

## What the rewrite fixed

Each was reproduced before being fixed, not just reasoned about.

| Defect | Impact |
|---|---|
| Tracker had **no synchronisation at all** | 12 concurrent clients → **27 ThreadSanitizer races**, now 0. Concurrent hashtable insert vs. lookup corrupts the map, not just stale reads. |
| **No authentication** | Every command trusted a `<username>` argument — any client could act as any user. Now session tokens. |
| **Remote crash** | One malformed message killed the tracker for everyone (`std::stoi` throws → `std::terminate`). Client had the same hole via `GET_PIECE`. |
| **Cross-group corruption** | File metadata keyed by filename globally — uploading to one group overwrote another group's file. |
| **Path traversal** | An uploader could register `../../.bashrc`. |
| **Dead peers advertised** | Downloaders burned their full retry budget on peers that had crashed. |
| **Client-side races** | 3 races on the upload map, plus an fd-reuse race at shutdown. |
| **Memory leaks** | `new` without `delete`; an unbounded thread vector whose join loop was unreachable. |
| **Spoofable peer IP** | Login trusted a client-supplied address; now read from the socket. |

---

## Assumptions

- All peers and the tracker are reachable on the addresses they advertise.
- A shared file is not modified or moved while it is being seeded.
- Shared filenames are plain basenames (no path separators) — enforced.
- Peers may serve wrong data; that is caught by per-piece hashing. They are
  not assumed honest.
- Group IDs and usernames contain no whitespace (the protocol is
  whitespace-delimited).

## Limitations

Stated plainly rather than left for you to discover:

- **Passwords use salted SHA-256, not a slow KDF.** Better than plaintext,
  but bcrypt/scrypt/Argon2 resist offline GPU cracking and SHA-256 does not.
- **No TLS** — passwords cross the wire in plaintext at login. Salting
  protects storage, not transport.
- **Deletions do not replicate between trackers.** A `leave_group` or
  `stop_share` applied while the tracker link is down can be resurrected
  by a later merge, because the merge is a union. Tombstones would fix it.
- **The tracker-to-tracker link is unauthenticated.** Anything that can
  reach a tracker's port can push state into it. It assumes the trackers
  sit on a trusted network.
- Up to 30 s of tracker state can be lost on an unclean shutdown (snapshot
  interval).
- Thread-per-connection does not scale past tens of concurrent clients;
  `epoll` would be the answer at larger scale.

### Coverage of the assignment brief

Every functional requirement is implemented: all commands in §4.1–4.3,
512 KB pieces with SHA-1 verification at piece and file level (§3.1,
§6), the three protocols including tracker-to-tracker (§3.2), concurrent
downloads and thread-safe shared state (§3.3), two synchronised trackers
with failover and recovery (§2.1, §7), and the `[C] [group_id] filename`
completion format (§8).

The limitations above are properties of the chosen designs, documented
rather than hidden. See [DECISIONS.md](docs/DECISIONS.md) for why each
was chosen and what the alternatives cost.

---

## Documentation

| | |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Components, thread model, locking strategy, transfer walkthrough |
| [PROTOCOL.md](docs/PROTOCOL.md) | Exact wire format for both protocols |
| [DECISIONS.md](docs/DECISIONS.md) | Every significant choice, the alternatives, the known downsides |
| [INTERVIEW_PREP.md](docs/INTERVIEW_PREP.md) | The defects fixed and how each was proven |
