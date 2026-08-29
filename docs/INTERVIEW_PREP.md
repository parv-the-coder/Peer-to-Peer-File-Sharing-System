# Interview Preparation

Questions you are likely to get about this project, and answers grounded
in what the code actually does. Read `DECISIONS.md` alongside this — the
trade-offs live there.

**One rule that matters more than any answer here: do not claim more than
you did.** Every fix below is real and reproducible. Say "SHA-256 salted,
though bcrypt would be better and here is why I didn't" and you sound
like an engineer. Overstate it and one follow-up question undoes you.

---

## "Walk me through the project."

> It's a BitTorrent-style file sharing system in C++. A central tracker
> coordinates metadata — accounts, groups, which peers hold which files —
> and clients transfer file data directly between each other, so file
> content never passes through the tracker. Files are split into 512 KB
> pieces, each SHA-1 hashed, and a download pulls pieces in parallel from
> multiple peers, verifying each one before writing it. Downloads resume
> after an interruption, and a client that finishes a download becomes a
> seeder itself.
>
> The version on GitHub is a rewrite. The original worked but had a set
> of real defects — an unsynchronised tracker, no actual authentication,
> and a crash any client could trigger. Most of what I'd want to talk
> about is what was wrong and how I proved it was fixed.

---

## "What was actually broken, and how did you find it?"

Four worth naming.

**1. The tracker had no thread safety at all.** One thread per connection,
four shared `unordered_map`s, zero synchronisation. Not stale reads —
concurrent `operator[]` insert against `find()` on a hashtable means a
reader can follow a bucket pointer while it's being rewritten during a
rehash. Corruption or a crash.

*How I proved it:* built under ThreadSanitizer and drove 12 concurrent
clients. **27 reported races.** After moving all state behind a
`shared_mutex`: **zero**, same test. That test is committed and runs in
CI, so it's a regression net, not a one-off.

**2. Any client could impersonate any user.** Every privileged command
took a bare `<username>` argument and the tracker believed it. Roughly
four lines of Python to approve your own join request to someone else's
group. Fixed with server-issued session tokens; identity is resolved from
the token, never from an argument.

**3. One malformed message killed the tracker for everyone.**
`std::stoi` throws on bad input, and an uncaught throw on a connection
thread calls `std::terminate` — the whole process, not the one
connection. Sending `upload_file g1 f.bin alice notanumber deadbeef
NOTANUMBER` was an unauthenticated remote DoS. The client had the same
hole via `GET_PIECE`'s piece index, so a malicious peer could crash any
client that talked to it. Replaced with strict non-throwing parsers.

**4. A peer that died was still advertised as alive.** Killed clients
stayed marked connected forever, so the tracker handed out dead
addresses and downloaders burned their entire retry budget against them.
Fixed by clearing the flag on disconnect; there's an integration test
that kills a client with `kill -9` and asserts the tracker stops offering
it.

**The honest meta-answer:** most of these came from *tooling*, not from
staring at the code. ThreadSanitizer found the races. `-Wall -Wextra`
found the ignored bind address. Writing unit tests found a parser
inconsistency. That's a better story than "I read it carefully."

---

## "Why one big lock? Isn't that a bottleneck?"

Because most commands touch several maps at once — `upload_file` writes
groups, files and the group→file index together. Per-map locks would need
a lock-ordering discipline to avoid deadlock, and those operations would
end up holding several locks anyway, so the concurrency win is smaller
than it looks.

It's a `shared_mutex`, so reads run concurrently — and reads are the
common case. Writers can starve under sustained read load; at tens of
connections that doesn't bite. If it needed to scale, I'd shard by group
ID before I'd go finer-grained on one map.

Two details I'd volunteer, because they're where this goes wrong:
public methods take the lock and private `_locked` helpers assume it's
held — `shared_mutex` isn't recursive, so a public method calling another
would deadlock against itself. And the read paths use `find()` rather
than `operator[]`, because `operator[]` inserts, which would mutate the
map under a shared lock.

---

## "Why not bcrypt?"

Straight answer: bcrypt, scrypt or Argon2 would be better and I'd use one
in production. SHA-256 is fast, which is a virtue for file integrity and
a liability for passwords — an attacker with the snapshot can try
billions of candidates a second on a GPU. The KDFs are deliberately slow
and memory-hard, which is the property that matters.

I didn't use one because it means an external dependency where everything
else here is OpenSSL, which was already required. That's a defensible
call at this scope and not one at real scale. It's written down in
`DECISIONS.md` rather than left implicit.

I'd also add that salting fixes *storage*, not *transport* — the password
still crosses the wire in plaintext, because there's no TLS.

---

## "How would you add TLS?"

Wrap the tracker connection in an OpenSSL `SSL_CTX`. The framing helpers
in `common/socket_io` are the only place that touches the socket, so
`send_all`/`recv_all` become `SSL_write`/`SSL_read` and nothing above
them changes — that isolation is most of the work already done.

The hard part isn't the code, it's trust: certificates need distributing
and validating, and self-signed certs everyone accepts buy nothing. For
peer-to-peer connections I'd argue TLS matters less — pieces are already
hash-verified, so the payload is tamper-evident — but it would still hide
*which* files you're transferring.

---

## "Why SHA-1 for pieces if it's broken?"

Because "broken" is specific: SHA-1 is broken for *collision resistance*,
meaning an attacker can construct two inputs with the same digest. That
matters for signatures. Here the hash detects corruption and wrong data
from a peer, and the expected digest comes from the tracker over a
separate channel — an attacker who could substitute a colliding piece
would already need to control the tracker's metadata, at which point
they don't need a collision.

It's also what BitTorrent uses for pieces, for the same reason. I used
SHA-256 where the adversary is offline guessing, which is passwords.

---

## "What happens if two clients join the same group simultaneously?"

Both `join_group` calls take the exclusive lock, so they serialise —
`unordered_set::insert` is never running concurrently. Both land as
pending applicants; the set deduplicates.

The more interesting version is two uploads of the same *filename*, and
this one is worth telling as a story because I got it wrong first.

While writing the docs I claimed files were keyed per group. I checked
before publishing it, and they weren't — `files_` was keyed by filename
alone. Uploading `data.bin` to one group silently overwrote the metadata
of a completely unrelated `data.bin` in another group, so members of the
first group were handed the second group's size and piece hashes. Their
downloads would fail verification against a file they never uploaded.

The same root cause was also a path-traversal vector: a downloader builds
its output path as `<destination>/<filename>`, so an uploader registering
`../../.bashrc` would have a downloader who copied that name out of
`list_files` write outside their chosen directory.

Both are fixed — files are keyed by `(group, filename)`, and filenames
containing path separators or the dot entries are rejected at upload.
There are unit tests pinning both.

The honest lesson: I nearly shipped a confident, wrong answer in my own
interview notes. Verifying a claim about your own code before making it
is the whole point.

---

## "How does resumable download work?"

Piece status is written to a `<file>.downloading` sidecar after each
piece completes. On restart the downloader reads it and enqueues only the
missing pieces. The output file is pre-allocated to full size up front,
so pieces can be written at their real offsets in any order with
`pwrite`. The sidecar is deleted on success.

Pieces are verified *before* being written, which is what makes resume
safe — anything already on disk is known-good, so there's no
half-verified state to reason about.

---

## "What would you do differently with more time?"

- **Key files by content hash, not filename** — fixes the collision above.
- **A real KDF for passwords**, and TLS for transport.
- **`epoll` instead of thread-per-connection** if it needed thousands of
  concurrent clients; each thread costs a stack.
- **Rarest-piece-first scheduling.** Right now workers start at different
  peers and pull pieces in order. BitTorrent prioritises the rarest piece
  in the swarm, which keeps availability up when seeders leave.
- **Write-ahead logging** instead of a 30-second snapshot, which can lose
  up to 30 seconds of state on an unclean shutdown.
- **Bandwidth limits and fairness.** Nothing stops one downloader
  saturating a seeder.

---

## Questions to ask them back

- How do they handle schema/protocol migration across independently
  deployed clients? (This project sidesteps it by deploying both halves
  together — a real system can't.)
- Do they run sanitizers or fuzzing in CI, or rely on review?
- Where do they draw the line between a coarse lock and sharding?

---

## Numbers worth remembering

| | |
|---|---|
| Piece size | 512 KB |
| Piece hash / file hash | SHA-1 |
| Password hash | SHA-256 + 16-byte random salt |
| Session token | 256-bit, `RAND_bytes`, 24h TTL |
| Frame header | 4-byte big-endian length, 16 MB cap |
| Download workers | up to 8, 5 retries per piece |
| Peer-server pool | 4 threads |
| Snapshot interval | 30s, plus on clean shutdown |
| TSan races, before → after | **27 → 0** |
| Tests | 34 unit + 4 integration |
