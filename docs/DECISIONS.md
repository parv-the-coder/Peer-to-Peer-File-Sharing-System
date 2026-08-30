# Design Decisions

Each entry: what was chosen, what else was on the table, and why. Where a
decision has a known downside, it is stated rather than hidden — a
trade-off you can name is a trade-off you made on purpose.

---

## 1. One `shared_mutex` for all tracker state

**Chosen:** a single `std::shared_mutex` guarding all four maps together.

**Alternatives:**

- *Per-map locks.* Most commands span several maps at once — `upload_file`
  touches groups, files and the group→file index; `download_file` reads
  files while cross-referencing connected state in users. Per-map locking
  would need a strict lock-ordering discipline to stay deadlock-free, and
  would buy very little real concurrency, because those multi-map
  operations would end up holding several locks anyway.
- *Per-entry locks / sharding.* Correct answer at thousands of concurrent
  connections. This tracker handles tens. The complexity would not be
  paying for anything.
- *Lock-free structures.* Dramatically harder to get right, and the
  contention here does not justify it.

**Known downside:** `shared_mutex` can starve writers under sustained read
load. At this scale it does not matter; at a much larger one it would,
and sharding by group ID would be the next step.

---

## 2. Session tokens rather than a username argument

**Chosen:** the tracker issues a 256-bit random token at login; every
privileged command carries it, and identity is resolved server-side.

**Alternative considered:** keep trusting the `<username>` argument. This
is what the original did, and it meant *any connected client could act as
any user by naming them* — approve join requests to groups it did not
own, deregister other people's shares. Not a theoretical weakness; it is
four lines of Python to exploit.

**Also considered:** mutual TLS, or a challenge-response handshake. Both
are stronger. Both are substantially more machinery than a course-scale
project over plain TCP justifies, and TLS would need certificate
distribution that does not exist here.

**Token lifetime:** fixed 24h TTL from issue, not refreshed on use.
Refresh-on-use would turn every command's token *read* into a *write*,
which is exactly the wrong shape for the hottest lookup in the system.
Tokens are revoked on logout and on disconnect.

---

## 3. SHA-256 + salt for passwords — and why that is not the best answer

**Chosen:** 16-byte random per-user salt, SHA-256(salt ‖ password),
constant-time comparison.

**This is a real improvement** over the original plaintext storage and
comparison, and the salt means identical passwords across accounts do not
produce identical digests.

**It is still not what production should use.** SHA-256 is a *fast* hash.
That is a virtue for file integrity and a liability for passwords: an
attacker with the snapshot can try billions of candidates per second on a
GPU. bcrypt, scrypt or Argon2 are deliberately slow and memory-hard,
which is the property that actually matters here.

**Why not used:** each means an external dependency (libsodium or
similar), where everything else in this project is OpenSSL, which is
already required for SHA-1. That is a defensible call for a project of
this scope and an indefensible one for a real system. It is recorded here
rather than glossed over.

**Also true:** the password still crosses the wire in plaintext at login,
because there is no TLS. Salting fixes storage, not transport.

---

## 4. Length-prefixed framing over delimiters

**Chosen:** 4-byte big-endian length prefix on every message.

**Alternatives:**

- *Newline-delimited.* Requires escaping any newline in the payload, and
  the reader must buffer and rescan across reads.
- *One `read()` per message.* What the original did. It works right up
  until a message spans TCP segments, at which point commands silently
  truncate. Loopback and short commands hide this; a file with hundreds
  of piece hashes exposes it.

Framing costs 4 bytes and removes the entire class of problem. The length
is capped at 16 MB so a hostile prefix cannot induce a huge allocation.

---

## 5. Line-oriented snapshot rather than JSON or SQLite

**Chosen:** one whitespace-separated record per line.

**Why it is safe rather than merely convenient:** every field stored
arrives over a protocol that is itself whitespace-split, so **no field can
contain whitespace**. That eliminates quoting and escaping — the usual
reason ad-hoc formats break.

**Alternatives:**

- *JSON via a vendored header.* ~900 KB of third-party code in a project
  whose own source is ~3,000 lines.
- *A hand-written JSON parser.* Real work, and a real source of bugs, for
  a schema that is fixed and flat.
- *SQLite.* Genuinely the right answer if the state outgrew memory or
  needed queries. It does not.

**Durability:** written to a temp file and `rename()`d into place, so a
crash mid-write leaves the previous good snapshot rather than a truncated
one. Serialisation holds the read lock for the whole walk — without that
the snapshot could capture a torn view, one group written before an edit
and another after.

**Known downside:** a 30-second interval means an unclean shutdown can
lose up to 30 seconds of registrations. Write-ahead logging would fix
that and is more machinery than this needs.

---

## 5b. Tracker replication: symmetric union merge, no primary

**Chosen:** both trackers are equal peers. Each accepts writes at any
time, and a background link pushes its full serialised state to the other
whenever its version counter moves. Convergence is by **union merge** —
users, groups, members, applicants, files and seeders present on either
side end up present on both.

**Why no primary:** a primary-backup design needs leader election and a
failover step, and during the changeover neither node can safely accept
writes. The brief requires the system keep working while one tracker is
down; with symmetric peers that is automatic, because either tracker
alone is already fully functional.

**Why full state rather than an operation log:** the state is a few KB.
Shipping it whole makes replication *idempotent and order-independent* —
re-sending the same state changes nothing — so a message lost to a
dropped link needs no acknowledgement, sequence number or retry buffer.
The next push carries it. The same property means a tracker rejoining
after an outage uses exactly the same code path as a steady-state push;
there is no separate recovery mode to get wrong.

**Alternatives:** an operation log with sequence numbers is more
bandwidth-efficient and would support deletion, at the cost of tracking
per-peer acknowledgement and handling gaps. Raft or similar would give
true linearizability and is far more machinery than two nodes at this scale
justify.

**Known downsides, both real:**

- **Deletions do not replicate.** A `leave_group` or `stop_share` applied
  while the link is down can be resurrected by a later merge, because a
  union cannot distinguish "never seen" from "deleted". Tombstones with
  timestamps would fix it.
- **Concurrent conflicting writes both survive.** If the same group is
  created on both trackers during a partition, the merge unions the
  member sets rather than picking a winner. For this data that is the
  benign outcome, but it is not last-writer-wins and should not be
  described as such.

**A bug worth recording:** the first implementation bumped the version
counter on *every* merge, including one that changed nothing. Each push
made the receiver look modified, so it pushed back, which made the sender
look modified — the two trackers ping-ponged full state at each other
forever and spun until both fell over. The merge now reports whether it
actually altered anything, and only a real change bumps the version. The
integration test asserts the trackers stay quiet while idle, so this
cannot silently come back.

---

## 6. SHA-1 for pieces, SHA-256 for passwords

Deliberately different, because the threat models are different.

SHA-1 is broken for *collision resistance* — an attacker can construct
two inputs with the same digest. That matters when a signature must not
be transferable to a different document. Here the hash detects
**corruption and wrong data from a peer**, and the expected digest comes
from the tracker over a separate channel. An attacker who could
substitute a colliding piece would already have to control the tracker's
metadata.

SHA-1 is also what BitTorrent itself uses for pieces, for the same
reason. SHA-256 is used where the adversary is offline guessing.

---

## 7. `SO_REUSEADDR` without `SO_REUSEPORT`

The original set both. `SO_REUSEPORT` lets a *second* process bind an
already-bound port, with the kernel load-balancing connections between
them. For a stateful tracker that is silent corruption: start a second
tracker by accident and clients split across two processes with separate
state, so registrations appear to randomly vanish.

`SO_REUSEADDR` alone gives the property actually wanted — rebind a port
still in `TIME_WAIT` after a restart — without permitting two live
listeners.

---

## 8. Ownership survives disconnect

The original transferred group ownership to another member whenever the
owner's connection dropped, so restarting your client meant losing your
own group. Ownership now moves only on an explicit `leave_group`.

**Known downside:** a group whose owner never returns keeps a stale owner,
and its pending join requests cannot be approved. That is the better
failure: recoverable and predictable, versus silently losing a group by
restarting a client. A real system would add an explicit ownership
transfer command.

---

## 9. A ~50-line test harness instead of Catch2

**Alternatives:** vendoring an amalgamated Catch2 header (~900 KB of
third-party code), or `FetchContent` (makes every configure, including
CI, depend on the network).

For the assertions here — known-answer hash vectors, parser edge cases,
state transitions — neither earns its keep. The harness registers tests,
runs them, reports file and line on failure, and exits non-zero for
ctest. If the suite grew to need parameterised fixtures or mocking, a
real framework would be the right move.

---

## 10. Detached connection threads

Each tracker connection gets a detached thread. The original pushed
`std::thread` objects into a vector whose join loop sat *after* an
infinite accept loop — unreachable — so the vector grew without bound for
the life of the process.

**Known downside:** thread-per-connection does not scale to thousands of
concurrent clients; each costs a stack. `epoll` with a small event loop
is the answer at that scale. At tens of clients, thread-per-connection is
simpler and easier to reason about.
