# Endgame download policy

Endgame prevents the last blocks of a download from depending on one slow peer.
It follows the request/cancel mechanism described by
[BEP 3](https://www.bittorrent.org/beps/bep_0003.html).

## Scheduling

Ordinary piece selection and reservations run first. An idle, interested,
unchoked peer can request a redundant block when:

- At most five unverified pieces remain.
- Every missing block is already requested.
- The original block request is at least 500 milliseconds old.
- Fewer than two peers currently own a request for that block.

Only one additional block is assigned to an idle worker at a time. Missing
blocks with no remaining request owner can be reassigned immediately when their
piece already has an active reservation. Received blocks are excluded from
scheduling through a locked piece-manager snapshot.

These limits are implementation policy. They avoid duplicating ordinary fast
downloads and cap redundant traffic rather than sending every request to all
connected peers.

## Completion and cancellation

When a block is accepted, the session clears its request copies and queues
matching `CANCEL` commands. The original index, offset and requested length are
preserved. Connection request maps remain owned by their worker threads; other
threads only update session state and command queues under the session mutex.

Workers poll idle sockets every 100 milliseconds to process queued commands and
schedule new work without waiting for another incoming message. They also flush
final cancellations during shutdown. Packet writes are serialized per socket.

A response can already be in flight when its cancellation is sent. A block
without a matching outstanding session request is ignored before assembly,
progress or traffic statistics are updated. SHA1 verification still determines
whether a completed piece is committed.

Disconnecting a peer releases its requests. Valid partial data is retained if
another peer still owns work for the same piece; otherwise the partial assembly
can be discarded and requested afresh.

## Verification

```sh
ctest --test-dir build -R test_endgame --output-on-failure
```

Four offline scenarios cover a silent seed, late cancelled data, the two-peer
copy limit with three seeds, and disconnecting one owner of a shared piece.
Fixtures compare exact file bytes and tracker statistics, inspect wire CANCEL
fields, and never deliver held blocks normally. The feature must therefore
finish through an alternate peer rather than waiting for the slow seed.

These checks use local TCP peers and do not establish performance gains across
all public swarm/network conditions.
