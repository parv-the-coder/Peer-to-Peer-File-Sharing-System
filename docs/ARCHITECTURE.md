# Architecture

A BitTorrent-style file sharing system. A central **tracker** coordinates
metadata, and **clients** transfer file data directly to each other. The
tracker never sees file contents.

```
        +--------------+   replication   +--------------+
        |  TRACKER 1   |<--------------->|  TRACKER 2   |
        +------+-------+  (union merge)  +-------+------+
               |                                 |
      control  |      +--------------------------+ failover
               |      |
        +------+------+-+                 +------------+
        |    CLIENT A   |<--------------->|  CLIENT B  |
        +---------------+   file pieces   +------------+
                             (direct)
```

Either tracker alone serves every command. Clients try each in turn, so one
being down is invisible to them.

Related docs: [PROTOCOL.md](PROTOCOL.md) for the message formats,
[DECISIONS.md](DECISIONS.md) for why each design was picked.

---

## Components

### `src/common/`, shared by both binaries

| Module | Responsibility |
|---|---|
| `socket_io` | `send_all` and `recv_all`, plus the length-prefixed framing both protocols use |
| `message` | Whitespace tokenising, and strict non-throwing integer parsing for anything read off the wire |
| `hash` | SHA-1 for piece and file integrity, SHA-256 for passwords |
| `auth` | Random hex from the OS CSPRNG, salted password hashing, constant-time verification |
| `config` | Reads the tracker info file |

### `src/tracker/`

| Module | Responsibility |
|---|---|
| `main` | Socket setup, accept loop, command dispatch, signal handling |
| `tracker_state` | All mutable state (users, groups, files, group to file index) behind one lock |
| `session_manager` | Token issue, lookup and revoke, with its own independent lock |
| `persistence` | Serialisation, snapshot save and load, and the union merge |
| `peer_link` | Background replication to the other tracker |

### `src/client/`

| Module | Responsibility |
|---|---|
| `main` | Startup, tracker failover, and the REPL loop |
| `commands` | `CommandProcessor::execute`, the single place a command is interpreted |
| `tracker_client` | The tracker connection and the session token |
| `peer_server` | Listening socket and worker pool serving `GET_PIECE` |
| `downloader` | Piece scheduling, retry, verification, resume |
| `upload_registry` | Mutex-guarded filename to local path map |
| `http_server`, `web_ui` | The optional loopback dashboard |

---

## Thread model

This is where the interesting bugs were, so it is worth being precise.

### Tracker

- **One thread per connection**, detached. Each runs a blocking
  read, dispatch, reply loop.
- **One snapshot thread** waking every 30 seconds to persist state.
- **One console thread** reading `quit`.
- **One replication thread** talking to the other tracker.
- The main thread runs the accept loop.

All of those touch shared state, so all tracker state lives in
`TrackerState`, which owns a single `std::shared_mutex`:

- **shared (read) lock** for `list_groups`, `list_requests`, `list_files`,
  `download_file`, and snapshot serialisation
- **exclusive (write) lock** for everything else

The locking rule is: public methods take the lock, and private `*_locked`
helpers assume it is already held. `std::shared_mutex` is not recursive, so a
public method calling another public method would deadlock against itself. None
do, and that rule has to be kept when adding commands.

One subtlety worth knowing: the read paths use `find()` rather than
`operator[]`, because `operator[]` **inserts** when the key is missing. A read
path using it would be changing the map under a shared lock, where several
readers can run at the same time.

`SessionManager` has its **own** mutex rather than living under the same one.
Every authenticated command validates a token, so that lookup is the hottest
read in the system. Putting it behind the state lock would make it queue behind
slow operations like `list_files`.

### Replication

The `PeerLink` thread keeps the other tracker in step. It dials the peer, pulls
its state and merges it, then pushes this tracker's state whenever the version
counter moves. Both trackers do this symmetrically, and neither is a primary.

Because the merge is a union, replication is idempotent and does not depend on
order. Re-sending identical state does nothing, so a lost message needs no
acknowledgement or retry buffer, and a tracker rejoining after an outage takes
the same code path as a normal push.

`TrackerState::version()` gates the push so idle trackers stay silent. Without
that gate the two push state at each other forever. See
[DECISIONS.md](DECISIONS.md#6-tracker-replication-symmetric-union-merge-no-primary).

### Client

- **CLI thread**, the REPL, which stays responsive during transfers
- **Peer-server accept thread**, which hands accepted sockets to the pool
- **Peer-server worker pool**, 4 threads, serving `GET_PIECE`
- **Download threads**, one per `download_file`, so several files transfer at
  once
- **Download worker threads**, up to 8 per download, fetching pieces in
  parallel
- **Dashboard thread**, only when a dashboard port is passed

`UploadRegistry` is shared between the CLI thread, which registers and removes
files, and every peer-server worker, which reads it. It was an unsynchronised
`unordered_map`, and ThreadSanitizer reports races on any run where one file is
served while another is registered.

Peer serving uses a queue and a fixed pool rather than a thread per request, so
a burst of downloaders cannot spawn unbounded threads.

---

## How a transfer works

**Upload.** The client hashes the file, SHA-1 for the whole file plus SHA-1 per
512 KB piece, registers the path locally so it can serve pieces, and sends the
metadata to the tracker. No file data goes to the tracker.

**Download**

1. Ask the tracker for metadata and the current seeder list.
2. Pre-allocate the output file to its final size, by seeking to the last byte
   and writing there.
3. Push every piece index onto a work queue.
4. Start up to 8 workers, or 4 for files over 1000 pieces, and never more than
   the number of peers available. Each worker begins at a *different* peer,
   because the peer list is rotated by worker index, so workers spread across
   the swarm instead of all hitting the first one.
5. Each worker pulls a piece index, requests it with `GET_PIECE`, **verifies
   its SHA-1**, then opens its own file handle and writes the piece at its
   offset with `fseeko` and `fwrite`.
6. A failed or corrupt piece is retried against the next peer, up to 5
   attempts.
7. After all pieces land, the reassembled file is verified against the
   full-file hash.
8. Tell the tracker, which adds this client as a seeder, so the swarm grows as
   downloads complete.

**Resume.** Piece status is written to a `<file>.downloading` sidecar after
each piece. Re-running an interrupted download reloads it and fetches only what
is missing. The sidecar is removed on success.

Pieces are verified *before* being written, so a bad peer can waste bandwidth
but cannot corrupt the output file.

---

## One command path for CLI and dashboard

`CommandProcessor::execute` is the only place a command is interpreted. The
REPL calls it, and so does the dashboard's `POST /api/command`. There is no
second copy of the login checks to keep in step, and the dashboard cannot
reach an action the CLI would refuse.

The dashboard binds 127.0.0.1 only and stays off unless a port is passed, since
it inherits the client's session and has no authentication of its own.

---

## State that deliberately does not persist

The tracker snapshots accounts, groups and file metadata. It deliberately does
**not** persist:

- **connected flags**, because they describe a live socket. Restoring them
  would advertise peers that are not there.
- **session tokens**, because restoring them would honour credentials whose
  owners are long gone.

Both are rebuilt as clients reconnect.
