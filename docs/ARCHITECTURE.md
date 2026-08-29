# Architecture

A BitTorrent-style file sharing system: a central **tracker** coordinates
metadata, and **clients** transfer file data directly to each other. The
tracker never sees file contents.

```
                  ┌──────────────┐
                  │   TRACKER    │   accounts, groups, file metadata,
                  │              │   who currently has what
                  └──────┬───────┘
            control      │      control
         ┌───────────────┴───────────────┐
         │                               │
   ┌─────┴──────┐                  ┌─────┴──────┐
   │  CLIENT A  │◄────────────────►│  CLIENT B  │
   └────────────┘   file pieces    └────────────┘
                    (direct)
```

---

## Components

### `src/common/` — shared by both binaries

| Module | Responsibility |
|---|---|
| `socket_io` | `send_all`/`recv_all` and the length-prefixed framing both protocols use |
| `message` | Whitespace tokenising, and strict non-throwing integer parsing for anything off the wire |
| `hash` | SHA-1 for piece and file integrity; SHA-256 for passwords |
| `auth` | CSPRNG random hex, salted password hashing, constant-time verification |

### `src/tracker/`

| Module | Responsibility |
|---|---|
| `main` | Socket setup, accept loop, command dispatch, signal handling |
| `tracker_state` | All mutable state (users, groups, files, group→file index) behind one lock |
| `session_manager` | Token issue/lookup/revoke, with its own independent lock |
| `persistence` | Snapshot save/load (implements two `TrackerState` methods) |

### `src/client/`

| Module | Responsibility |
|---|---|
| `main` | REPL and command table only |
| `tracker_client` | The tracker connection and the session token |
| `peer_server` | Listening socket + worker pool serving `GET_PIECE` |
| `downloader` | Piece scheduling, retry, verification, resume |
| `upload_registry` | Mutex-guarded filename → local path map |

---

## Thread model

This is where the interesting bugs were, so it is worth being precise.

### Tracker

- **One thread per connection**, detached. Each runs a blocking
  read-dispatch-reply loop.
- **One snapshot thread** waking every 30s to persist state.
- **One console thread** reading `quit`.
- The main thread runs the accept loop.

Every one of those touches shared state, so **all tracker state is behind
`TrackerState`**, which owns a single `std::shared_mutex`:

- **shared (read) lock** — `list_groups`, `list_requests`, `list_files`,
  `download_file`, and snapshot serialisation
- **exclusive (write) lock** — everything else

The locking discipline is: *public methods take the lock, private
`*_locked` helpers assume it is already held*. `std::shared_mutex` is not
recursive, so a public method calling another public method would
deadlock against itself. None do, and that is a rule to preserve when
adding commands.

One subtlety worth knowing: the read paths use `find()` rather than
`operator[]`, because `operator[]` **inserts** when the key is missing.
A read path using it would be mutating the map under a shared lock, where
several readers can run concurrently.

`SessionManager` deliberately has its **own** mutex rather than living
under the same one. Every authenticated command validates a token, so
that lookup is the hottest read in the system; putting it behind the
state lock would make it queue behind slow operations like `list_files`.

### Client

- **CLI thread** — the REPL; also runs downloads synchronously
- **Peer-server accept thread** — hands accepted sockets to the pool
- **Peer-server worker pool** (4 threads) — serve `GET_PIECE`
- **Download worker threads** (up to 8, transient) — fetch pieces in parallel

`UploadRegistry` is shared between the CLI thread (which registers and
removes files) and every peer-server worker (which reads it). It was an
unsynchronised `unordered_map`; ThreadSanitizer reports races on any run
where one file is served while another is registered.

A queue-and-pool design is used for peer serving rather than
thread-per-request so that a burst of downloaders cannot spawn unbounded
threads.

---

## How a transfer works

**Upload** — the client hashes the file (SHA-1 whole, plus SHA-1 per
512 KB piece), registers the path locally so it can serve pieces, and
sends the metadata to the tracker. No file data goes to the tracker.

**Download**

1. Ask the tracker for metadata and the current seeder list.
2. Pre-allocate the output file to its final size.
3. Push every piece index onto a work queue.
4. Start up to 8 workers. Each begins at a *different* peer — the peer
   list is rotated by worker index — so concurrent workers spread across
   the swarm instead of all hammering the first one.
5. Each worker: pull a piece index, `GET_PIECE` it, **verify its SHA-1**,
   write it at its offset with `pwrite`, record progress.
6. A failed or corrupt piece is retried against the next peer, up to 5
   attempts.
7. After all pieces land, verify the reassembled file against the
   full-file hash.
8. Tell the tracker, which adds this client as a seeder — the swarm grows
   as downloads complete.

**Resume** — piece status is written to a `<file>.downloading` sidecar
after each piece. Re-running an interrupted download reloads it and
fetches only what is missing. The sidecar is removed on success.

Pieces are verified *before* being written, so a bad peer can waste
bandwidth but cannot corrupt the output file.

---

## State that deliberately does not persist

The tracker snapshots accounts, groups and file metadata. It deliberately
does **not** persist:

- **connected flags** — they describe a live socket. Restoring them would
  advertise peers that are not there.
- **session tokens** — restoring them would honour credentials whose
  owners are long gone.

Both are reconstructed as clients reconnect.
