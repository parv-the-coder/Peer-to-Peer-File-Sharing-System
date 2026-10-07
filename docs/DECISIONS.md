# Design Decisions

Each entry says what was chosen, what else was on the table, and why. Where a
decision has a known downside, it is stated rather than hidden. A trade-off you
can name is a trade-off you made on purpose.

Related docs: [ARCHITECTURE.md](ARCHITECTURE.md) for the components and threads,
[PROTOCOL.md](PROTOCOL.md) for the message formats.

---

## 1. One `shared_mutex` for all tracker state

**Chosen:** a single `std::shared_mutex` guarding all four maps together.

**Alternatives:**

- *Per-map locks.* Most commands span several maps at once. `upload_file`
  touches groups, files and the group to file index, and `download_file` reads
  files while cross-referencing connected state in users. Per-map locking would
  need a strict lock-ordering discipline to stay deadlock-free, and would buy
  very little real concurrency, because those multi-map operations would end up
  holding several locks anyway.
- *Per-entry locks or sharding.* The right answer at thousands of concurrent
  connections. This tracker handles tens, so the complexity would not be paying
  for anything.
- *Lock-free structures.* Much harder to get right, and the contention here
  does not justify it.

**Known downside:** `shared_mutex` can starve writers under sustained read
load. At this scale it does not matter. At a much larger one it would, and
sharding by group ID would be the next step.

Code: [`src/tracker/tracker_state.h`](../src/tracker/tracker_state.h).

---

## 2. Session tokens rather than a username argument

**Chosen:** the tracker issues a 256-bit random token at login. Every
privileged command carries it, and identity is resolved on the server side.

**Alternative considered:** keep trusting the `<username>` argument. This is
what the original did, and it meant *any connected client could act as any user
by naming them*: approve join requests to groups it did not own, or deregister
other people's shares. That is not a theoretical weakness. It is a few lines of
Python to exploit.

**Also considered:** mutual TLS, or a challenge-response handshake. Both are
stronger, and both are a lot more machinery than a project of this scope over
plain TCP justifies. TLS would also need certificate distribution that does not
exist here.

**Token lifetime:** fixed 24 hour TTL from issue, not refreshed on use.
Refresh-on-use would turn every command's token *read* into a *write*, which is
exactly the wrong shape for the hottest lookup in the system. Tokens are
revoked on logout and on disconnect.

Code: [`src/tracker/session_manager.h`](../src/tracker/session_manager.h).

---

## 3. Salted SHA-256 for passwords, and why that is not the best answer

**Chosen:** a 16-byte random per-user salt, `SHA-256(salt + password)`, and a
constant-time comparison on verify.

**This is a real improvement** over the original plaintext storage and
comparison, and the salt means identical passwords across accounts do not
produce identical digests.

**It is still not what production should use.** SHA-256 is a *fast* hash. That
is a virtue for file integrity and a liability for passwords, because an
attacker holding the snapshot can try billions of candidates per second on a
GPU. bcrypt, scrypt and Argon2 are deliberately slow and memory-hard, which is
the property that actually matters here.

**Why not used:** each one means an external dependency such as libsodium,
where everything else in this project uses OpenSSL, which is already required
for SHA-1. That is a defensible call for a project of this scope and an
indefensible one for a real system, so it is recorded here rather than glossed
over.

**Also true:** the password still crosses the wire in plaintext at login,
because there is no TLS. Salting fixes storage, not transport.

Code: [`src/common/auth.cpp`](../src/common/auth.cpp).

---

## 4. Length-prefixed framing over delimiters

**Chosen:** a 4-byte big-endian length prefix on every message.

**Alternatives:**

- *Newline-delimited.* Requires escaping any newline in the payload, and the
  reader must buffer and rescan across reads.
- *One `read()` per message.* What the original did. It works right up until a
  message spans TCP segments, at which point commands silently truncate.
  Loopback and short commands hide this, and a file with hundreds of piece
  hashes exposes it.

Framing costs 4 bytes and removes the whole class of problem. The length is
capped at 16 MB so a hostile prefix cannot cause a huge allocation.

Code: [`src/common/socket_io.cpp`](../src/common/socket_io.cpp).

---

## 5. Line-oriented snapshot rather than JSON or SQLite

**Chosen:** one whitespace-separated record per line.

**Why it is safe rather than just convenient:** every field stored arrives over
a protocol that is itself whitespace-split, so **no field can contain
whitespace**. That removes quoting and escaping, which is the usual reason
ad-hoc formats break.

**Alternatives:**

- *JSON via a vendored header.* Roughly 900 KB of third-party code in a project
  whose own source is about 4,400 lines.
- *A hand-written JSON parser.* Real work, and a real source of bugs, for a
  schema that is fixed and flat.
- *SQLite.* Genuinely the right answer if the state outgrew memory or needed
  queries. It does not.

**Durability:** written to a temp file and renamed into place, so a crash
mid-write leaves the previous good snapshot rather than a truncated one.
Serialisation holds the read lock for the whole walk. Without that the snapshot
could capture a torn view, with one group written before an edit and another
after.

**Known downside:** a 30 second interval means an unclean shutdown can lose up
to 30 seconds of registrations. Write-ahead logging would fix that and is more
machinery than this needs.

Code: [`src/tracker/persistence.cpp`](../src/tracker/persistence.cpp).

---

## 6. Tracker replication: symmetric union merge, no primary

**Chosen:** both trackers are equal peers. Each accepts writes at any time, and
a background link pushes its full serialised state to the other whenever its
version counter moves. Convergence is by **union merge**, so users, groups,
members, applicants, files and seeders present on either side end up present on
both.

**Why no primary:** a primary-backup design needs leader election and a
failover step, and during the changeover neither node can safely accept writes.
The system has to keep working while one tracker is down, and with symmetric
peers that is automatic, because either tracker alone is already fully
functional.

**Why full state rather than an operation log:** the state is a few KB.
Shipping it whole makes replication *idempotent and independent of order*, since
re-sending the same state changes nothing. A message lost to a dropped link
therefore needs no acknowledgement, sequence number or retry buffer, because
the next push carries it. The same property means a tracker rejoining after an
outage uses exactly the same code path as a steady-state push, so there is no
separate recovery mode to get wrong.

**Alternatives:** an operation log with sequence numbers is more
bandwidth-efficient and would support deletion, at the cost of tracking
per-peer acknowledgement and handling gaps. Raft or similar would give true
linearizability and is far more machinery than two nodes at this scale justify.

**Known downsides, both real:**

- **Deletions do not replicate.** A `leave_group` or `stop_share` applied while
  the link is down can be brought back by a later merge, because a union cannot
  tell "never seen" apart from "deleted". Tombstones with timestamps would fix
  it.
- **Concurrent conflicting writes both survive.** If the same group is created
  on both trackers during a partition, the merge unions the member sets rather
  than picking a winner. For this data that is the harmless outcome, but it is
  not last-writer-wins and should not be described as such.

**The link is unauthenticated.** `SYNC_REQ` and `STATE` carry no token, because
the peer is another tracker rather than a logged-in user. Anything that can
reach a tracker's port can push state into it, so the design assumes both
trackers sit on a trusted network. A shared secret, or binding the listener to
a private interface, would be the fix.

**A bug worth recording:** the first implementation bumped the version counter
on *every* merge, including one that changed nothing. Each push made the
receiver look modified, so it pushed back, which made the sender look modified,
and the two trackers pushed full state at each other forever until both fell
over. The merge now reports whether it actually altered anything, and only a
real change bumps the version. The integration test asserts the trackers stay
quiet while idle, so this cannot silently come back.

Code: [`src/tracker/peer_link.h`](../src/tracker/peer_link.h).

---

## 7. SHA-1 for pieces, SHA-256 for passwords

Deliberately different, because the threat models are different.

SHA-1 is broken for *collision resistance*, meaning an attacker can construct
two inputs with the same digest. That matters when a signature must not be
transferable to a different document. Here the hash detects **corruption and
wrong data from a peer**, and the expected digest comes from the tracker over a
separate channel. An attacker who could substitute a colliding piece would
already have to control the tracker's metadata.

SHA-1 is also what BitTorrent itself uses for pieces, for the same reason.
SHA-256 is used where the attacker is guessing offline.

---

## 8. `SO_REUSEADDR` without `SO_REUSEPORT`

The original set both. `SO_REUSEPORT` lets a *second* process bind an already
bound port, with the kernel load-balancing connections between them. For a
stateful tracker that is silent corruption: start a second tracker by accident
and clients split across two processes with separate state, so registrations
appear to randomly vanish.

`SO_REUSEADDR` alone gives the property actually wanted, which is rebinding a
port still in `TIME_WAIT` after a restart, without permitting two live
listeners.

---

## 9. Ownership survives disconnect

The original transferred group ownership to another member whenever the owner's
connection dropped, so restarting your client meant losing your own group.
Ownership now moves only on an explicit `leave_group`.

**Known downside:** a group whose owner never returns keeps a stale owner, and
its pending join requests cannot be approved. That is the better failure, since
it is recoverable and predictable, versus silently losing a group by restarting
a client. A real system would add an explicit ownership transfer command.

---

## 10. One `execute()` for the CLI and the dashboard

**Chosen:** `CommandProcessor::execute` is the only place a command is
interpreted. The REPL calls it, and so does the dashboard's
`POST /api/command`.

**Alternative:** a separate handler per route, which is the usual shape for a
web layer. It also means a second copy of the login and argument checks, and
two copies drift. The dashboard cannot reach an action the CLI would refuse,
because there is nothing for it to reach it through.

**The dashboard has no authentication of its own.** It inherits the client's
session, so anything that can reach its port can act as that logged-in user.
That is why it binds 127.0.0.1 only and stays off unless a port is passed.

Code: [`src/client/commands.h`](../src/client/commands.h).

---

## 11. A small test harness instead of Catch2

**Alternatives:** vendoring an amalgamated Catch2 header, roughly 900 KB of
third-party code, or `FetchContent`, which makes every configure including CI
depend on the network.

For the assertions here, which are known-answer hash vectors, parser edge cases
and state transitions, neither earns its keep. The harness is under 100 lines:
it registers tests, runs them, reports file and line on failure, and exits
non-zero for ctest. If the suite grew to need parameterised fixtures or
mocking, a real framework would be the right move.

Code: [`tests/unit/test_framework.h`](../tests/unit/test_framework.h).

---

## 12. Detached connection threads

Each tracker connection gets a detached thread. The original pushed
`std::thread` objects into a vector whose join loop sat *after* an infinite
accept loop, so it was unreachable and the vector grew without bound for the
life of the process.

Download threads on the client are the opposite case: they are kept and joined
at shutdown, because a detached download still touching the registry or the
tracker socket while `main()` tears them down is a use-after-free.

**Known downside:** thread-per-connection does not scale to thousands of
concurrent clients, since each costs a stack. `epoll` with a small event loop
is the answer at that scale. At tens of clients, thread-per-connection is
simpler and easier to reason about.
