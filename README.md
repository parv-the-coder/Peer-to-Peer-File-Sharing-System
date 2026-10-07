# P2P File Sharing System

A BitTorrent-style peer-to-peer file sharing system in C++17.

A central tracker stores the metadata: user accounts, groups, and which peers
currently hold which files. Clients send file data straight to each other. File
contents never pass through the tracker.

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

Two trackers can run at once. They keep each other in sync, either one alone
can serve every command, and a tracker that was down catches up when it comes
back.

---

## Features

| Area | What you get |
|---|---|
| Accounts | Register, login, logout, salted password hashing, session tokens issued by the tracker |
| Groups | Create, browse, request to join, owner approves or rejects, leave with automatic ownership handover |
| Sharing | Share a file with a group, list a group's files, stop seeding |
| Downloading | Parallel fetch from several peers, SHA-1 check on every piece, retry against a different peer, resume after an interruption |
| Swarm | A client that finishes a download starts seeding it |
| Progress | `show_downloads` shows piece counts and status per file |
| Durability | Tracker state is written to disk and reloaded on restart |
| Dashboard | Optional browser UI with live progress bars, driven by the same command layer as the CLI |
| Replication | Two trackers share state, and a rejoining tracker syncs on its own |
| Failover | Clients try each tracker in turn, so one tracker being down is not noticed |
| Resilience | Dead peers stop being advertised, and bad input is rejected instead of crashing the process |

---

## Documentation

This README covers setup and usage. The deeper detail lives in
[docs/](docs/):

| Doc | Covers |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Components, the thread model in both binaries, how a transfer works, and what state deliberately does not persist |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | Message framing and all three protocols: client to tracker, tracker to tracker, peer to peer |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Each design decision, the alternatives considered, and the known downsides |
| [docs/TESTING.md](docs/TESTING.md) | What the tests cover, and how to run the ThreadSanitizer build |

---

## Build

You need CMake 3.16 or newer, a C++17 compiler, and OpenSSL.

```bash
sudo apt-get install cmake g++ libssl-dev     # Debian/Ubuntu
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

A small `Makefile` wraps the same commands if you prefer `make`, `make test`,
and `make clean`.

---

## Running it

### 1. Start the trackers

`tracker_info.txt` holds one `<ip> <port>` line per tracker. The number on the
command line picks which line that instance binds to.

```bash
printf '127.0.0.1 9000\n127.0.0.1 9001\n' > tracker_info.txt

./build/tracker tracker_info.txt 1     # terminal 1
./build/tracker tracker_info.txt 2     # terminal 2
```

The two find each other and start replicating in either start order. A file
with one line also works, and then the tracker runs on its own. Type `quit` in
a tracker console to shut it down.

### 2. Client A shares a file

The first argument is the address this client listens on for peer requests.

```bash
./build/client 127.0.0.1:6001 tracker_info.txt
```

```
create_user alice secret
login alice secret
create_group aos
upload_file aos /path/to/file.bin
```

### 3. Client B fetches it

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

The exact wire format behind each of these is in
[docs/PROTOCOL.md](docs/PROTOCOL.md).

### Optional dashboard

Pass a third argument to open a browser dashboard for that client:

```bash
./build/client 127.0.0.1:6001 tracker_info.txt 8080
# then open http://127.0.0.1:8080
```

The dashboard does everything the CLI does. It posts to one endpoint that calls
the same `CommandProcessor::execute` the REPL calls, so the two cannot drift
apart and the dashboard cannot reach an action the CLI would refuse. It binds
to loopback only and stays off unless you pass a port. See
[docs/DECISIONS.md](docs/DECISIONS.md#10-one-execute-for-the-cli-and-the-dashboard).

---

## How a transfer works

**Upload.** The client hashes the file with SHA-1, once for the whole file and
once per 512 KB piece. It records the local path so it can serve pieces later,
and sends only the metadata to the tracker.

**Download.** The client asks the tracker for the metadata and the list of
online peers holding the file, then fetches pieces in parallel with up to 8
workers. Each worker starts at a different peer, so workers spread across the
swarm instead of all hitting the first seeder. Every piece is checked against
its SHA-1 before it is written, and a bad or missing piece is retried against
the next peer. Once all pieces land, the whole file is checked against the
full-file hash and the client joins the swarm as a seeder.

Because a piece is verified before it is written, a misbehaving peer can waste
bandwidth but cannot corrupt the output file.

**Resume.** Piece status is written to a `<file>.downloading` sidecar after each
piece. Re-running an interrupted download fetches only what is missing.

The full step-by-step flow is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#how-a-transfer-works).

---

## Layout

```
src/common/    framing, parsing, hashing, credentials (shared by both binaries)
src/tracker/   state, sessions, persistence, replication, dispatch
src/client/    REPL, tracker connection, peer server, downloader, dashboard
tests/         36 unit tests and 7 integration scripts
docs/          architecture, protocol, design decisions, testing
```

### Data structures and why they look like this

| Structure | Reason |
|---|---|
| `TrackerState`, four hash maps behind one `shared_mutex` | Most commands touch several maps together, so per-map locks would need lock-ordering discipline for very little gain. Reads such as `list_*` and `download_file` run at the same time, and writes are exclusive. |
| `SessionManager`, token to session behind its own mutex | Every authenticated command validates a token, so it is the hottest read in the system. Keeping it off the state lock stops it queueing behind a slow `list_files`. |
| `FileMeta` keyed by (group, filename) | Keyed by filename alone, an upload to one group silently overwrote another group's file with the same name. |
| `UploadRegistry`, a mutex-guarded filename to path map | Every peer-serving thread reads it while the CLI thread changes it. |
| `CommandProcessor`, one `execute()` for every caller | The REPL and the dashboard share it, so there is no second copy of the login checks to keep in step. |
| Piece-status vector plus `.downloading` sidecar | Makes downloads resumable. Written after each piece. |

The alternatives considered for each of these, and the known downsides, are in
[docs/DECISIONS.md](docs/DECISIONS.md).

### Threads

The tracker runs one detached thread per connection, a snapshot thread every 30
seconds, a console thread reading `quit`, and a replication thread talking to
the other tracker. The accept loop stays on the main thread.

The client runs the REPL on its main thread, plus an accept thread and a
four-thread worker pool for serving pieces, one thread per active download, and
up to eight piece-fetch workers inside each download.

Full detail, including the locking rule that keeps `TrackerState` deadlock
free, is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#thread-model).

### Protocol

Every message on every protocol is `[4-byte big-endian length][payload]`, with
the length capped at 16 MB. Length prefixing is what makes a message that spans
several TCP segments safe to read. The original code assumed one `read()`
returned one whole message, which quietly truncated large ones such as an
`upload_file` carrying hundreds of piece hashes.

Client to tracker is whitespace-separated text, and every authenticated command
carries a session token that the tracker resolves the username from. Peer to
peer is one `GET_PIECE <filename> <index>` request per piece, with no token,
because per-piece hashing is what protects the downloader. The two trackers
exchange `SYNC_REQ` and `STATE` in the same record format as the on-disk
snapshot, merged as a union.

Message by message detail is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

---

## Testing

```bash
ctest --test-dir build --output-on-failure
```

36 unit tests cover hashing against published known-answer vectors, parser edge
cases, password handling, session lifetime, and tracker state transitions and
authorisation rules. Six integration scripts drive the real binaries: a full
upload and download round trip with a hash comparison, three simultaneous
downloads, an ungraceful disconnect, persistence across a restart, two-tracker
sync with failover and recovery, and a dashboard test.

A seventh script runs the same kind of load under ThreadSanitizer and reports 0
data races. The same test against the tracker before the locking work reported
27. Setup for that build, and what each test actually proves, is in
[docs/TESTING.md](docs/TESTING.md).

---

## What the rewrite fixed

An earlier version of this system worked on a quiet loopback and fell apart
under load. Each defect below was reproduced first and then fixed.

| Defect | Impact |
|---|---|
| Tracker had no synchronisation at all | 12 concurrent clients produced 27 ThreadSanitizer races, now 0. A concurrent hashtable insert against a lookup corrupts the map, which is far worse than a stale read. |
| No authentication | Every command trusted a `<username>` argument, so any client could act as any user. Session tokens replaced that. |
| Remote crash | One malformed message killed the tracker for everyone, because `std::stoi` throws and an uncaught throw calls `std::terminate`. The client had the same hole in `GET_PIECE`. |
| Cross-group corruption | File metadata was keyed by filename globally, so uploading to one group overwrote another group's file. |
| Path traversal | An uploader could register `../../.bashrc`. |
| Dead peers advertised | Downloaders spent their whole retry budget on peers that had crashed. |
| Client-side races | Three races on the upload map, plus a file-descriptor reuse race at shutdown. |
| Memory leaks | `new` without `delete`, and an unbounded thread vector whose join loop was unreachable. |
| Spoofable peer IP | Login trusted a client-supplied address. The tracker now reads it from the socket. |

---

## Assumptions

- Peers and trackers are reachable at the addresses they advertise.
- A shared file is not changed or moved while it is being seeded.
- Shared filenames are plain basenames with no path separators, and this is
  enforced.
- Peers may serve wrong data. Per-piece hashing catches it, so peers are not
  assumed honest.
- Group IDs and usernames contain no whitespace, because the protocol is
  whitespace-delimited.

## Limitations

These come from the designs chosen here, written down rather than left for you
to find. Each one is explained in full in
[docs/DECISIONS.md](docs/DECISIONS.md).

- **Passwords use salted SHA-256, not a slow KDF.** Better than plaintext, but
  bcrypt, scrypt, and Argon2 resist offline GPU cracking and SHA-256 does not.
- **No TLS.** Passwords cross the wire in plaintext at login. Salting protects
  storage, not transport.
- **Deletions do not replicate between trackers.** A `leave_group` or
  `stop_share` applied while the tracker link is down can come back from a
  later merge, because the merge is a union. Tombstones would fix this.
- **The tracker-to-tracker link is unauthenticated.** Anything that can reach a
  tracker's port can push state into it, so the design assumes both trackers
  sit on a trusted network.
- **The dashboard has no authentication of its own.** It inherits the client's
  session, so anything that can reach its port can act as that logged-in user.
  That is why it binds to loopback only and stays off unless a port is passed.
- Up to 30 seconds of tracker state can be lost on an unclean shutdown, which
  is the snapshot interval.
- Thread-per-connection stops scaling past tens of concurrent clients. `epoll`
  would be the answer at a larger scale.
