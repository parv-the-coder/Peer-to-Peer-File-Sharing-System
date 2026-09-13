# P2P File Sharing System

A BitTorrent-style peer-to-peer file sharing system written in C++17. A central
tracker keeps the metadata (accounts, groups, and which peers currently hold
which files) while clients move file data straight to each other. File contents
never travel through the tracker.

```
                  +--------------+
                  |   TRACKER    |   accounts, groups, file metadata
                  +------+-------+   who currently has what
            control      |      control
         +---------------+---------------+
   +-----+------+                  +-----+------+
   |  CLIENT A  |<---------------->|  CLIENT B  |
   +------------+   file pieces    +------------+
                     (direct)
```

A first version of this system worked on a quiet loopback and fell apart under
load. It was rewritten to close a set of real defects: an unsynchronised
tracker, no authentication, and a crash any client could trigger remotely. The
[rewrite section](#what-the-rewrite-fixed) lists them.

---

## What it does

| Area | Capability |
|---|---|
| Accounts | Register, login, logout, salted password hashing, server-issued session tokens |
| Groups | Create, browse, request to join, owner-approved accept or reject, leave with automatic ownership handover |
| Sharing | Share a file with a group, list a group's files, stop seeding |
| Downloading | Parallel multi-peer piece fetch, per-piece SHA-1 verification, retry against a different peer, resume after interruption |
| Swarm | A client that finishes a download starts seeding it |
| Progress | `show_downloads` reports per-file piece counts and status |
| Durability | Tracker state is snapshotted atomically and reloaded on restart |
| Dashboard | Optional browser UI with live progress bars, driven by the same command layer as the CLI |
| Replication | Two trackers hold synchronised state; either one alone serves every command, and one rejoining after an outage catches up on its own |
| Failover | Clients try each tracker in turn, so a tracker being down goes unnoticed |
| Resilience | Dead peers stop being advertised; malformed input is rejected instead of crashing the process |

---

## Build

You need CMake 3.16 or newer, a C++17 compiler, and OpenSSL.

```bash
sudo apt-get install cmake g++ libssl-dev     # Debian/Ubuntu
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

A thin `Makefile` wraps the same commands if you prefer `make`, `make test`,
and `make clean`.

---

## Running it

### Start the trackers

`tracker_info.txt` holds one `<ip> <port>` line per tracker. The number on the
command line selects which line that instance binds to.

```bash
printf '127.0.0.1 9000\n127.0.0.1 9001\n' > tracker_info.txt

./build/tracker tracker_info.txt 1     # terminal 1
./build/tracker tracker_info.txt 2     # terminal 2
```

The two find each other and start replicating in either start order. A file
with a single line also works, in which case the tracker runs standalone.
Typing `quit` in a tracker console shuts it down.

### Client A shares a file

```bash
./build/client 127.0.0.1:6001 tracker_info.txt
```

```
create_user alice secret
login alice secret
create_group aos
upload_file aos /path/to/file.bin
```

### Client B fetches it

Give it a different peer port:

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

Type `commands` for the full list and `exit` to leave the client.

### Command reference

| Command | Purpose |
|---|---|
| `create_user <user> <pass>` | Register an account |
| `login <user> <pass>` / `logout` | Start or end a session |
| `create_group <gid>` | Create a group and become its owner |
| `join_group <gid>` | Request membership |
| `leave_group <gid>` | Leave, handing ownership on if you owned the group |
| `list_groups` | Every group on the network |
| `list_requests <gid>` | Pending join requests, owner only |
| `accept_request <gid> <user>` | Approve a request, owner only |
| `reject_request <gid> <user>` | Decline a request, owner only |
| `list_files <gid>` | Files shared in a group |
| `upload_file <gid> <path>` | Share a file |
| `download_file <gid> <file> <dest>` | Fetch a file |
| `show_downloads` | Download progress |
| `stop_share <gid> <file>` | Stop seeding a file |

### Optional dashboard

Pass a third argument to open a browser dashboard for that client:

```bash
./build/client 127.0.0.1:6001 tracker_info.txt 8080
# then open http://127.0.0.1:8080
```

The dashboard covers everything the CLI covers. It posts to a single endpoint
that calls the same `CommandProcessor::execute` the REPL calls, so the two
cannot drift apart and the dashboard cannot reach an action the CLI would
refuse. It binds to loopback only and stays off unless you pass a port.

---

## How a transfer works

**Upload.** The client hashes the file with SHA-1, both whole and per 512 KB
piece, registers the local path so it can serve pieces, and sends only the
metadata to the tracker.

**Download.**

1. Ask the tracker for the file metadata and the list of peers currently
   online that hold it.
2. Pre-allocate the output file at its final size.
3. Push every piece index onto a work queue.
4. Start up to 8 workers (4 for files over 1000 pieces, and never more than
   the number of peers available). Each worker starts at a different peer
   because the peer list is rotated by worker index, so workers spread across
   the swarm rather than all hitting the first seeder.
5. Each worker pulls a piece index, requests the piece, checks its SHA-1,
   writes it at its offset, and records progress.
6. A missing or corrupt piece is retried against the next peer, up to 5
   attempts.
7. Once every piece has landed, the reassembled file is checked against the
   full-file hash.
8. The client tells the tracker it now holds the file and joins the swarm.

Verification happens before a piece is written, so a misbehaving peer can waste
bandwidth but cannot corrupt the output file.

**Resume.** Piece status is written to a `<file>.downloading` sidecar after each
piece. Re-running an interrupted download reloads that file and fetches only
what is missing. The sidecar is deleted once the transfer succeeds.

---

## Design

```
src/common/    framing, parsing, hashing, credentials (shared by both binaries)
src/tracker/   state, sessions, persistence, replication, dispatch
src/client/    REPL, tracker connection, peer server, downloader, dashboard
tests/         36 unit tests and 7 integration scripts
```

### Data structures and the reasoning behind them

| Structure | Reasoning |
|---|---|
| `TrackerState`, four hash maps behind one `shared_mutex` | Most commands touch several maps together, so per-map locks would demand lock-ordering discipline for little gain. Reads such as `list_*` and `download_file` run concurrently; writes are exclusive. |
| `SessionManager`, token to session behind its own mutex | Every authenticated command validates a token, making it the hottest read in the system. Keeping it off the state lock stops it queueing behind a slow `list_files`. |
| `FileMeta` keyed by (group, filename) | Keyed by filename alone, an upload to one group silently overwrote another group's file of the same name. |
| `UploadRegistry`, a mutex-guarded filename to path map | Every peer-serving thread reads it while the CLI thread mutates it. |
| `CommandProcessor`, one `execute()` for every caller | The REPL and the dashboard share it, so there is no second copy of the login checks to keep in step. |
| Piece-status vector plus `.downloading` sidecar | Makes downloads resumable; written after each piece. |

### Thread model

The tracker runs one detached thread per connection, a snapshot thread that
wakes every 30 seconds, a console thread reading `quit`, and a replication
thread talking to the other tracker. The accept loop stays on the main thread.

The client runs the REPL on its main thread, an accept thread and a four-thread
worker pool for serving pieces, one thread per active download, and up to eight
piece-fetch workers inside each download. A queue and pool serve peer requests
so that a burst of downloaders cannot spawn unbounded threads.

Public methods on `TrackerState` take the lock and private `*_locked` helpers
assume it is already held. `std::shared_mutex` is not recursive, so a public
method calling another public method would deadlock against itself. None do,
and that rule needs to hold as commands get added.

### Protocol

Every message on both protocols is `[4-byte big-endian length][payload]`, with
the length capped at 16 MB. Length prefixing is what makes a message spanning
several TCP segments safe. The original code assumed one `read()` returned one
whole message, which quietly truncated large ones such as an `upload_file`
carrying hundreds of piece hashes.

The client-to-tracker protocol is whitespace-separated text. Every
authenticated command carries a session token as its first argument, and the
tracker resolves the username from that token rather than from the arguments.
The peer-to-peer protocol is a single `GET_PIECE <filename> <index>` request
per piece. It carries no token, because peers have nothing to validate one
against; per-piece hashing is what protects the downloader.

The two trackers exchange `SYNC_REQ` and `STATE` messages using the same record
format as the on-disk snapshot, so one serialiser and one parser cover both.
Merging is a union, which makes replication idempotent and order-independent: a
message lost while a link is down needs no acknowledgement, since the next push
carries it.

---

## Testing

```bash
ctest --test-dir build --output-on-failure
```

36 unit tests cover hashing against published known-answer vectors, parser edge
cases, password handling, session lifetime, and tracker state transitions and
authorisation rules. Six integration scripts drive the real binaries: a full
upload and download round trip with hash comparison, three simultaneous
downloads, ungraceful disconnect handling, persistence across a restart,
two-tracker synchronisation with failover and recovery, and a dashboard test.

A seventh script exercises concurrency under ThreadSanitizer:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j"$(nproc)"
./tests/integration/concurrency_test.sh build-tsan 12
```

It reports 0 data races. The same test against the tracker before the locking
work reported 27.

---

## What the rewrite fixed

Each of these was reproduced before being fixed rather than argued about on
paper.

| Defect | Impact |
|---|---|
| Tracker had no synchronisation at all | 12 concurrent clients produced 27 ThreadSanitizer races, now 0. A concurrent hashtable insert against a lookup corrupts the map, well beyond a stale read. |
| No authentication | Every command trusted a `<username>` argument, so any client could act as any user. Session tokens replaced it. |
| Remote crash | A single malformed message killed the tracker for everyone, since `std::stoi` throws and an uncaught throw calls `std::terminate`. The client had the same hole in `GET_PIECE`. |
| Cross-group corruption | File metadata keyed by filename globally, so uploading to one group overwrote another group's file. |
| Path traversal | An uploader could register `../../.bashrc`. |
| Dead peers advertised | Downloaders spent their whole retry budget on peers that had crashed. |
| Client-side races | Three races on the upload map, plus a file-descriptor reuse race at shutdown. |
| Memory leaks | `new` without `delete`, and an unbounded thread vector whose join loop was unreachable. |
| Spoofable peer IP | Login trusted a client-supplied address; the tracker now reads it from the socket. |

---

## Assumptions

- Peers and trackers are reachable at the addresses they advertise.
- A shared file is not modified or moved while it is being seeded.
- Shared filenames are plain basenames with no path separators, and this is
  enforced.
- Peers may serve wrong data. Per-piece hashing catches it, so peers are not
  assumed honest.
- Group IDs and usernames contain no whitespace, since the protocol is
  whitespace-delimited.

## Limitations

These are properties of the designs chosen here, written down rather than left
for you to find:

- **Passwords use salted SHA-256 rather than a slow KDF.** Better than
  plaintext, but bcrypt, scrypt, and Argon2 resist offline GPU cracking and
  SHA-256 does not.
- **No TLS.** Passwords cross the wire in plaintext at login. Salting protects
  storage, not transport.
- **Deletions do not replicate between trackers.** A `leave_group` or
  `stop_share` applied while the tracker link is down can come back from a
  later merge, because the merge is a union. Tombstones would fix it.
- **The tracker-to-tracker link is unauthenticated.** Anything that can reach a
  tracker's port can push state into it, so the design assumes both trackers
  sit on a trusted network.
- **The dashboard has no authentication of its own.** It inherits the client's
  session, so anything that can reach its port can act as that logged-in user.
  That is why it binds to loopback only and stays off unless a port is passed.
- Up to 30 seconds of tracker state can be lost on an unclean shutdown, which
  is the snapshot interval.
- Thread-per-connection stops scaling past tens of concurrent clients. `epoll`
  would be the answer at larger scale.
