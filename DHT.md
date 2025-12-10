# DHT (Distributed Hash Table) Implementation

## Overview

This BitTorrent client now supports DHT (BEP 5), enabling **trackerless operation**. With DHT, peers can discover each other without relying on centralized trackers.

## Features

### Core DHT Protocol (BEP 5)

- **Kademlia-based DHT**: Industry-standard distributed hash table
- **KRPC Protocol**: UDP-based RPC protocol with 4 queries:
  - `ping` - Check if node is alive
  - `find_node` - Find closest nodes to a target ID
  - `get_peers` - Find peers for an info_hash
  - `announce_peer` - Announce ourselves as a peer
- **Routing Table**: 160 K-buckets with 8 nodes each
- **Node States**: GOOD, QUESTIONABLE, BAD (BEP 5 compliant)

### Implementation Details

#### Files Structure

```
include/
  dht_node.h            - Node representation and distance calculations
  dht_krpc.h            - KRPC message protocol
  dht_routing_table.h   - K-bucket routing table
  dht_manager.h         - Main DHT manager

src/
  dht_node.cpp
  dht_krpc.cpp
  dht_routing_table.cpp
  dht_manager.cpp
```

#### Key Components

**1. DHT Node (`dht_node.h/cpp`)**
- 160-bit node IDs
- XOR distance metric (Kademlia)
- Compact node format (26 bytes: 20 ID + 4 IP + 2 port)
- Node status tracking with timestamps

**2. KRPC Protocol (`dht_krpc.h/cpp`)**
- Bencode-based message encoding
- Query/Response/Error message types
- Transaction ID management
- Compact node/peer encoding

**3. Routing Table (`dht_routing_table.h/cpp`)**
- 160 K-buckets (one per bit)
- 8 nodes per bucket (K=8)
- LRU eviction with node status consideration
- Automatic bucket refresh every 15 minutes

**4. DHT Manager (`dht_manager.h/cpp`)**
- Bootstrap from well-known nodes
- Peer lookup (get_peers)
- Peer announcement (announce_peer)
- Token-based security for announce_peer
- Periodic maintenance (bucket refresh, bad node cleanup)
- Statistics tracking

## Usage

### Configuration

Enable DHT in `config.json`:

```json
{
  "enable_dht": true,
  "dht_port": 6881,
  ...
}
```

Or via command line:
```bash
./torrent_client example.torrent --config config.json
```

### How It Works

1. **Bootstrap Phase**
   - Client connects to well-known DHT nodes:
     - `router.bittorrent.com:6881`
     - `dht.transmissionbt.com:6881`
     - `router.utorrent.com:6881`
   - Populates routing table with initial nodes

2. **Peer Discovery**
   - Sends `get_peers` query for info_hash to closest nodes
   - Receives either:
     - Peer list (compact format: 6 bytes per peer)
     - Closest nodes to continue search
   - Iteratively searches until peers are found

3. **Peer Announcement**
   - Sends `get_peers` to obtain announce token
   - Uses token to send `announce_peer` to announce ourselves
   - Re-announces periodically (every 60 seconds)

4. **Maintenance**
   - Refreshes stale buckets every 15 minutes
   - Removes bad/unresponsive nodes
   - Rotates security tokens every hour

### Integration with DownloadManager

DHT is seamlessly integrated:
- Automatically starts with `DownloadManager`
- Supplements tracker-provided peers
- Fallback when trackers are unavailable
- No user intervention required

```cpp
// DHT is enabled by default
DownloadManager manager(torrent_file, download_dir,
                       listen_port, max_dl, max_ul,
                       enable_dht = true);
```

## Protocol Specification

### KRPC Messages

**Ping Query:**
```json
{
  "t": "aa",
  "y": "q",
  "q": "ping",
  "a": {"id": "abcdefghij0123456789"}
}
```

**Find Node Query:**
```json
{
  "t": "aa",
  "y": "q",
  "q": "find_node",
  "a": {
    "id": "abcdefghij0123456789",
    "target": "mnopqrstuvwxyz123456"
  }
}
```

**Get Peers Query:**
```json
{
  "t": "aa",
  "y": "q",
  "q": "get_peers",
  "a": {
    "id": "abcdefghij0123456789",
    "info_hash": "mnopqrstuvwxyz123456"
  }
}
```

**Announce Peer Query:**
```json
{
  "t": "aa",
  "y": "q",
  "q": "announce_peer",
  "a": {
    "id": "abcdefghij0123456789",
    "info_hash": "mnopqrstuvwxyz123456",
    "port": 6881,
    "token": "aoeusnth"
  }
}
```

### Response Format

**Ping/Announce Response:**
```json
{
  "t": "aa",
  "y": "r",
  "r": {"id": "mnopqrstuvwxyz123456"}
}
```

**Find Node Response:**
```json
{
  "t": "aa",
  "y": "r",
  "r": {
    "id": "mnopqrstuvwxyz123456",
    "nodes": "<compact node info>"
  }
}
```

**Get Peers Response (with peers):**
```json
{
  "t": "aa",
  "y": "r",
  "r": {
    "id": "mnopqrstuvwxyz123456",
    "token": "aoeusnth",
    "values": ["<compact peer 1>", "<compact peer 2>"]
  }
}
```

**Get Peers Response (with nodes):**
```json
{
  "t": "aa",
  "y": "r",
  "r": {
    "id": "mnopqrstuvwxyz123456",
    "token": "aoeusnth",
    "nodes": "<compact node info>"
  }
}
```

### Error Response

```json
{
  "t": "aa",
  "y": "e",
  "e": [201, "Generic Error"]
}
```

Error codes:
- 201 - Generic Error
- 202 - Server Error
- 203 - Protocol Error
- 204 - Method Unknown

## Compact Formats

### Compact Node Info
- 26 bytes per node
- Format: `[20 bytes Node ID][4 bytes IP][2 bytes port]`
- IP in network byte order (big-endian)
- Port in big-endian

### Compact Peer Info
- 6 bytes per peer
- Format: `[4 bytes IP][2 bytes port]`
- IP in network byte order (big-endian)
- Port in big-endian

## Statistics

View DHT statistics during operation:
```
DHT: 127 nodes in routing table (89 good)
DHT: Found 15 peers
```

Get detailed stats:
```cpp
auto stats = routing_table_->getStats();
// stats.total_nodes
// stats.good_nodes
// stats.questionable_nodes
// stats.bad_nodes
// stats.filled_buckets
```

## Performance Considerations

- **Bootstrap Time**: 5-10 seconds to populate routing table
- **Peer Discovery**: 5-30 seconds depending on network
- **Memory**: ~50-200 KB for routing table (depends on network size)
- **Network Traffic**: ~1-5 KB/s for maintenance
- **Port**: Uses `listen_port + 1` by default

## References

- [BEP 5: DHT Protocol](http://www.bittorrent.org/beps/bep_0005.html)
- [Kademlia Paper](https://pdos.csail.mit.edu/~petar/papers/maymounkov-kademlia-lncs.pdf)

## Future Enhancements

Potential improvements:
- [ ] DHT persistence (save routing table to disk)
- [ ] IPv6 support (BEP 32)
- [ ] Security extensions (BEP 42)
- [ ] DHT statistics dashboard
- [ ] Custom bootstrap nodes configuration
