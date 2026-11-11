# DHT discovery

The client uses IPv4 UDP KRPC from [BEP 5](https://www.bittorrent.org/beps/bep_0005.html)
to discover TCP peers for v1 torrents. Both trackerless magnets and `.torrent`
files without an announce URL use this path. Metadata is still transferred over
TCP using BEP 9 and verified before payload files are opened.

## Configuration

```json
{
  "enable_dht": true,
  "dht_port": 6881,
  "dht_bootstrap_nodes": []
}
```

An empty or omitted bootstrap list uses `router.bittorrent.com:6881`,
`dht.transmissionbt.com:6881` and `router.utorrent.com:6881`. A nonempty list
replaces those routers, for example for a local DHT:

```json
{
  "enable_dht": true,
  "dht_port": 0,
  "dht_bootstrap_nodes": [{"host": "127.0.0.1", "port": 49001}]
}
```

Port `0` asks the OS to select a free UDP port. The desktop queue assigns distinct
DHT/TCP port pairs to jobs. An occupied UDP port reports an error. The listening
TCP port is what the client announces as its peer port; the DHT UDP port is a
separate endpoint. The application does not configure router or firewall rules.

## Lookup and lifetime

Bootstrap sends `find_node` queries asynchronously. A validated response records
the responding node's actual address, including a router that returns no other
nodes. Lookup waits for bootstrap responses rather than a fixed warm-up delay.
If an early router fails, lookup also considers nodes from bootstrap replies
that arrive later, instead of treating the first failure as an exhausted search.

`dht_lookup.cpp` walks returned nodes in XOR-distance order with three concurrent
queries. It visits at most 64 endpoints, retains at most 256 candidates and
returns at most 200 distinct peer addresses. Invalid compact peers and port zero
are ignored. The first useful peer response, exhausted candidates, cancellation
or the caller's deadline ends the lookup. Individual UDP transactions expire
after two seconds, allowing another node to answer without waiting on an idle one.

Responses and errors must match a pending transaction and its UDP source address
and port. Transaction IDs are unique across threads. Completion callbacks run
after the transaction-map mutex has been released, so an announce callback can
submit another query. Shared lookup state and removal of unfinished transactions
make late replies harmless after a timeout or cancellation.

Announcing first obtains a token with `get_peers`, then returns it in
`announce_peer`. Served tokens use a random secret bound to the requester's IP
and info hash. Secrets rotate every five minutes; the previous secret remains
valid during the next interval. The node supports `implied_port` for incoming
announcements.

The magnet worker passes cancellation into discovery. The download session takes
ownership of the same running DHT instance and immediately starts peer lookup;
it does not bootstrap a second public network. DHT-only sessions do not create a
tracker client or send tracker completion events. Session discovery, UDP polling
and maintenance observe shutdown. Hostname resolution uses the OS resolver and
its timing; cancellation checks happen between resolutions.

## Verification

```sh
ctest --test-dir build -R "test_dht|test_magnet_dht" --output-on-failure
```

The ordinary suite checks node chains, unavailable routers, KRPC errors, wrong
transaction IDs, wrong response sources, duplicate/invalid peers, late replies,
announce tokens, occupied ports and cancellation during metadata discovery.
End-to-end cases acquire two metadata blocks, verify SHA1 and compare downloaded
bytes without any tracker request. Another case falls back from an unavailable
tracker; a disabled-DHT case creates no payload files or UDP traffic.

Optional tests use the independent `bittorrent-dht` 11.0.12 implementation with
public bootstrap disabled and two loopback nodes. One verifies lookup and announce
in both implementations. With the qBittorrent fixture enabled, another discovers
qBittorrent through this DHT, obtains metadata from a magnet without `tr`, and
verifies the complete multi-file payload. See [test setup](../../tests/README.md#dht-interoperability).

The automated suite verifies a local IPv4 DHT and TCP transfer path. A separate
[manual public-swarm run](../PUBLIC_SWARMS.md) also resolved Debian metadata from
a trackerless magnet through public IPv4 DHT. IPv6 DHT, node-ID enforcement
extensions and persistent routing tables remain separate work. Routing and peer
storage are in memory for the lifetime of a job.
