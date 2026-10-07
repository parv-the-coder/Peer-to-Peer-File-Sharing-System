# Wire Protocol

Three protocols run in this system: **client to tracker** for coordination,
**tracker to tracker** for replication, and **peer to peer** for bulk data.
All three use TCP and share the same framing.

Related docs: [ARCHITECTURE.md](ARCHITECTURE.md) for the components,
[DECISIONS.md](DECISIONS.md) for the reasoning behind these choices.

---

## Framing

Every message, on all three protocols, is:

```
[4-byte length, big-endian][payload bytes]
```

The length counts payload bytes only and is capped at 16 MB
(`kMaxFrameSize`). A larger prefix is treated as a protocol violation and the
connection is dropped, rather than allocating what it asked for.

Framing exists because TCP is a byte stream with no message boundaries. The
original implementation issued one `read()` per command and assumed whatever
came back was exactly one complete message. That holds for short commands on an
idle loopback and breaks as soon as a message spans segments, for example an
`upload_file` carrying hundreds of piece hashes, or a `download_file` reply
listing many peers.

Implementation: [`src/common/socket_io.h`](../src/common/socket_io.h).

---

## Client to tracker

The client opens one long-lived TCP connection to the tracker and sends
commands over it. Every command gets exactly one reply.

Payloads are whitespace-separated text. No field may contain whitespace, which
is why the snapshot format can also be whitespace-separated with no quoting.
See [DECISIONS.md](DECISIONS.md#5-line-oriented-snapshot-rather-than-json-or-sqlite).

### Unauthenticated

| Command | Reply |
|---|---|
| `create_user <username> <password>` | success or error text |
| `login <username> <password> <listen_port>` | `OK <token>` on success, error text otherwise |

`login` takes only the port the client listens on. **The IP is not accepted
from the client.** The tracker reads it from the socket with `getpeername()`.
The client cannot be asked for its own IP, because that address is what other
peers will be told to connect to, so a client that supplied it could direct
traffic at a third party.

On success the tracker returns a 256-bit random session token.

### Authenticated

Every command below carries the token as its first argument. The tracker
resolves the username from the token and never reads an identity from the
arguments.

| Command |
|---|
| `logout <token>` |
| `create_group <token> <groupid>` |
| `join_group <token> <groupid>` |
| `leave_group <token> <groupid>` |
| `list_requests <token> <groupid>` |
| `accept_request <token> <groupid> <username>` |
| `reject_request <token> <groupid> <username>` |
| `list_groups <token>` |
| `list_files <token> <groupid>` |
| `upload_file <token> <groupid> <filename> <size> <fullhash> <num_pieces> <hash_1> ... <hash_n>` |
| `download_file <token> <groupid> <filename>` |
| `file_downloaded <token> <groupid> <filename>` |
| `stop_share <token> <groupid> <filename>` |

An invalid or expired token gets:

```
ERROR: AUTH_REQUIRED - session invalid or expired, please log in again
```

### `upload_file` validation

The tracker rejects the command unless `num_pieces` equals
`ceil(size / 512KB)` **and** exactly `num_pieces` hashes follow. Without the
first check a client could declare any piece count it liked and the tracker
would size a vector to match. Without the second, a short command read past the
end of its own argument list.

Filenames are also checked: a shared filename must be a plain basename with no
path separators, so an uploader cannot register something like `../../.bashrc`.

### `download_file` reply

```
FILE <filename> SIZE <size> HASH <fullhash> PIECES <n> PIECE_HASHES <h_1> ... <h_n>
PEERS
<username> <ip> <port>
<username> <ip> <port>
...
```

Only peers currently **connected** are listed. A peer that uploaded a file and
then dropped stays in the file's seeder set, because it still holds the data,
but it is left out here until it reconnects. Downloaders are therefore never
handed a dead address.

---

## Tracker to tracker

The replication link between the two trackers, over the same framing.

| Message | Reply |
|---|---|
| `SYNC_REQ` | the sender's full serialised state |
| `STATE <serialised state>` | `OK`, or `ERR <reason>` if the payload is malformed |

The payload is the same record format the on-disk snapshot uses (`USER`,
`GROUP`, `FILE`, `GFILE` lines), so there is one serialiser and one parser
rather than two of each that can drift apart.

On connecting, a tracker sends `SYNC_REQ` and merges the reply, which is how a
tracker that was offline catches up. After that it sends `STATE` whenever its
own version counter moves. Merging is a **union**, which makes this idempotent
and independent of order. Re-sending identical state changes nothing, so a
message lost when a link drops needs no acknowledgement or retransmission,
because the next push carries it.

Only a merge that actually changed something bumps the receiver's version.
Without that, every push would make the receiver look modified and it would
push straight back, and the two trackers would exchange state forever.

**These two messages are not authenticated.** They carry no session token,
because the peer is another tracker rather than a logged-in user. Anything that
can reach a tracker's listening port can therefore push state into it, so the
design assumes both trackers sit on a trusted network. A shared secret between
trackers, or binding the replication listener to a private interface, would be
the fix. Neither is implemented. This is recorded in
[DECISIONS.md](DECISIONS.md#6-tracker-replication-symmetric-union-merge-no-primary)
rather than left implicit.

---

## Peer to peer

One short-lived TCP connection per piece.

- **Request:** `GET_PIECE <filename> <piece_index>`
- **Reply:** the raw piece bytes, framed like any other message

This protocol deliberately carries **no session token**. Tokens authenticate to
the tracker, and peers have no way to validate one and no shared secret to
check it against. What actually protects the downloader is that every piece is
verified against the SHA-1 the tracker supplied before it is written to disk, so
a peer serving wrong bytes is detected and the piece is retried against a
different peer. Authenticating the request would not add to that.

An unparseable piece index closes the connection. It used to reach
`std::stoi`, which throws, and an uncaught throw on a worker thread calls
`std::terminate`, so any peer could crash any client that talked to it.

---

## Dashboard HTTP endpoints

Only present when a dashboard port is passed, and bound to 127.0.0.1 only.

| Route | Purpose |
|---|---|
| `GET /` | the page |
| `GET /api/status` | session, groups and download progress as JSON |
| `POST /api/command` | `{"args":[...]}` in, `{"output":"..."}` out |

`POST /api/command` goes through `CommandProcessor::execute`, the same function
the REPL calls, so the dashboard cannot perform anything the REPL would refuse,
including anything at all before login.

---

## Protocol versioning

There is none. Both binaries are built and deployed together from this
repository, so no old client ever meets a new tracker. A system with
independently deployed clients would need a version handshake or dual support
during rollout. That is deliberately out of scope here rather than overlooked.
