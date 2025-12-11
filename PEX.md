# Peer Exchange (PEX) - BEP 11

## Overview

Peer Exchange (PEX) is an extension to the BitTorrent protocol that allows peers to exchange information about other peers they know. This reduces dependency on centralized trackers and DHT, enabling faster peer discovery through direct peer-to-peer communication.

## Implementation

Our implementation follows [BEP 11 specification](http://www.bittorrent.org/beps/bep_0011.html).

## Architecture

```
┌─────────────────────┐
│  DownloadManager    │  - Enables PEX for connections
│                     │  - Runs pexLoop() for updates
└──────────┬──────────┘
           │
           │ manages
           ▼
┌─────────────────────┐
│  PeerConnection     │  - enablePex() activates PEX
│                     │  - sendPexMessage() exchanges peers
└──────────┬──────────┘
           │
           │ uses
           ▼
┌─────────────────────┐
│    PexManager       │  - Tracks known peers
│                     │  - Builds/parses PEX messages
│                     │  - Manages added/dropped peers
└─────────────────────┘
```

## Key Components

### 1. PexManager Class
Located in `include/pex_manager.h` and `src/pex_manager.cpp`

**Responsibilities:**
- Track known peers for a torrent
- Maintain lists of added/dropped peers since last update
- Build bencoded PEX messages
- Parse incoming PEX messages
- Enforce message limits and intervals

**Key Methods:**
```cpp
bool addPeer(const std::string& ip, uint16_t port, uint8_t flags = 0);
bool removePeer(const std::string& ip, uint16_t port);
bencode::BencodeValue buildPexMessage();
int parsePexMessage(const bencode::BencodeValue& data, std::vector<PexPeer>& new_peers_out);
bool shouldSendUpdate() const;
```

### 2. PEX Integration in PeerConnection
Located in `src/peer_connection.cpp`

**Workflow:**
1. `enablePex()` - Initializes PEX manager and registers extension handler
2. `sendExtendedHandshake()` - Announces PEX support to peer
3. `sendPexMessage()` - Sends PEX updates when needed
4. Extension handler - Automatically processes incoming PEX messages

### 3. PEX Coordination in DownloadManager
Located in `src/download_manager.cpp`

**Workflow:**
1. `pexLoop()` - Background thread that runs PEX updates every 60 seconds
2. `updatePEX()` - Exchanges peers with all active connections
3. Discovered peers are added to `available_peers_` for connection

## PEX Message Format

PEX messages are sent via the Extension Protocol (BEP 10) with the name `ut_pex`.

### Message Structure (Bencoded Dictionary)
```
{
    "added": <compact peer list>,      # IPv4 peers added (6 bytes each)
    "added.f": <peer flags>,           # Flags for added peers (1 byte each)
    "dropped": <compact peer list>,    # IPv4 peers dropped (6 bytes each)
    "added6": <compact IPv6 list>,     # IPv6 peers (18 bytes each) [future]
    "added6.f": <IPv6 peer flags>,     # Flags for IPv6 peers [future]
    "dropped6": <compact IPv6 list>    # IPv6 peers dropped [future]
}
```

### Compact Peer Format (IPv4)
```
[4 bytes: IP address] [2 bytes: port (big-endian)]
```

### Peer Flags
```cpp
0x01 - PEX_PREFER_ENCRYPTION  // Peer prefers encrypted connections
0x02 - PEX_SEED               // Peer is a seed (upload only)
0x04 - PEX_SUPPORTS_UTP       // Peer supports uTP protocol
0x08 - PEX_HOLEPUNCH          // Peer supports NAT hole punching
0x10 - PEX_OUTGOING           // Peer was connected outgoing
```

## Configuration

PEX can be enabled/disabled in `config.json`:

```json
{
    "enable_pex": true
}
```

Or via the Config class:
```cpp
torrent::Config config;
config.enable_pex = true;  // Enable PEX
```

## PEX Protocol Flow

### 1. Connection Establishment
```
Peer A                          Peer B
   |                               |
   |--- BitTorrent Handshake ---->|
   |<-- BitTorrent Handshake -----|
   |                               |
   |--- Extended Handshake ------>|
   |    (announces ut_pex)         |
   |<-- Extended Handshake --------|
   |    (announces ut_pex)         |
```

### 2. Peer Exchange
```
Peer A                          Peer B
   |                               |
   |--- PEX Message -------------->|
   |    {                          |
   |      "added": [peers...],     |
   |      "added.f": [flags...],   |
   |      "dropped": [...]         |
   |    }                          |
   |                               |
   |<-- PEX Message --------------|
   |    (responds with own peers)  |
```

### 3. Periodic Updates
- PEX messages are sent every 60 seconds (configurable)
- Maximum 50 peers per message
- Only changes since last message are sent

## Usage Example

### Enabling PEX for a Download
```cpp
#include "download_manager.h"
#include "config.h"

torrent::Config config;
config.enable_pex = true;  // Enable PEX

torrent::DownloadManager manager(
    "example.torrent",
    "./downloads",
    6881,                    // listen_port
    0,                       // max_download_speed (unlimited)
    0,                       // max_upload_speed (unlimited)
    true,                    // enable_dht
    true                     // enable_pex
);

manager.start();
```

### Manual PEX Control
```cpp
// Get a peer connection
PeerConnection* peer = /* ... */;

// Enable PEX
peer->enablePex();

// Send extended handshake
peer->sendExtendedHandshake();

// Add peers to share
auto* pex_mgr = peer->pexManager();
pex_mgr->addPeer("192.168.1.100", 6881, PEX_SEED);
pex_mgr->addPeer("10.0.0.50", 51413, 0);

// Send PEX message
if (pex_mgr->shouldSendUpdate()) {
    peer->sendPexMessage();
}

// Get discovered peers
const auto& known_peers = pex_mgr->getKnownPeers();
for (const auto& p : known_peers) {
    std::cout << "Known peer: " << p.ip << ":" << p.port << "\n";
}
```

## Implementation Details

### PEX Manager Internals

**State Tracking:**
- `known_peers_` - All peers currently known
- `added_peers_` - Peers added since last PEX message
- `dropped_peers_` - Peers dropped since last PEX message
- `last_pex_sent_` - Timestamp of last PEX message sent

**Constraints:**
- Maximum 50 peers per message (`MAX_PEERS_PER_MESSAGE`)
- Minimum 60 seconds between messages (`PEX_INTERVAL_SECONDS`)

### Integration with Extension Protocol

PEX is registered as an extension:
```cpp
extension_protocol_->registerExtension(
    "ut_pex",
    [this](uint8_t ext_id, const std::vector<uint8_t>& payload) {
        // Handle incoming PEX message
        bencode::BencodeValue data = bencode::decode(payload);
        std::vector<PexPeer> new_peers;
        int count = pex_manager_->parsePexMessage(data, new_peers);
    }
);
```

## Performance Considerations

### Bandwidth
- PEX messages are small (typically <1 KB)
- Sent only every 60 seconds
- Minimal bandwidth overhead

### CPU
- Message building/parsing uses efficient bencode operations
- Peer list management uses std::set for O(log n) lookups
- No significant CPU overhead

### Memory
- Each PexPeer is ~30 bytes (IP, port, flags, timestamp)
- Typical torrents track 50-200 peers
- Total memory usage: ~1.5-6 KB per torrent

## Advantages of PEX

1. **Faster Peer Discovery**
   - No need to wait for tracker announcements
   - Direct peer-to-peer communication

2. **Reduced Tracker Load**
   - Less frequent tracker requests needed
   - More resilient to tracker downtime

3. **Better Swarm Health**
   - Peers discover each other faster
   - Improved connectivity in the swarm

4. **Privacy Benefits**
   - Less reliance on centralized trackers
   - Harder to monitor swarm participants

## Limitations

1. **Bootstrap Problem**
   - Still needs initial peers from tracker or DHT
   - Cannot work in complete isolation

2. **Peer Quality**
   - No guarantee about peer quality
   - May receive peers that are offline or slow

3. **Network Effects**
   - Only exchanges peers with connected peers
   - Limited to the "neighborhood" of known peers

## Security Considerations

### Privacy
- IP addresses are shared with other peers
- Consider using VPN for sensitive torrents
- PEX can be disabled if privacy is a concern

### Peer Poisoning
- Malicious peers could send fake peer lists
- Mitigation: Validate peers before connecting
- Implementation: Timeout and disconnect bad peers

### DoS Protection
- Rate limiting: 60 second minimum interval
- Size limiting: 50 peers maximum per message
- Connection limits prevent PEX amplification

## Testing PEX

### Enable Debug Logging
```cpp
torrent::Logger::setLevel("debug");
```

### Check PEX Activity
Look for log messages:
```
PEX enabled for peer 192.168.1.100:6881
PEX: Added peer 10.0.0.50:51413 (flags: 0x02)
PEX: Sent update to 192.168.1.100:6881
PEX: Discovered 5 new peers from 192.168.1.100:6881
```

### Verify Configuration
```cpp
config.print();  // Should show "PEX enabled: yes"
```

## Troubleshooting

### PEX Not Working
1. Check configuration: `config.enable_pex == true`
2. Verify extended handshake sent: `sendExtendedHandshake()`
3. Confirm peer supports PEX: Check logs for extension handshake
4. Ensure peers are being added to PexManager

### No New Peers Discovered
1. Check if connected peers have different peer lists
2. Verify PEX messages are being sent (check logs)
3. Ensure pexLoop() thread is running
4. Check if peers support PEX extension

### Memory Issues
1. Monitor peer count: `pex_manager->getKnownPeerCount()`
2. Implement peer cleanup if count grows too large
3. Consider limiting `MAX_PEERS_PER_MESSAGE`

## Future Enhancements

### IPv6 Support
- Add `added6`, `added6.f`, `dropped6` fields
- Support compact IPv6 format (18 bytes per peer)
- Update PexPeer to handle both IPv4 and IPv6

### Advanced Features
- Peer quality metrics (upload/download speed)
- Prefer sharing high-quality peers
- Geolocation-aware peer selection
- PEX statistics and analytics

### Optimizations
- Bloom filters for efficient peer deduplication
- Differential updates (only send changes)
- Compressed peer lists for large swarms
- Adaptive update intervals based on swarm size

## References

- [BEP 11: Peer Exchange (PEX)](http://www.bittorrent.org/beps/bep_0011.html)
- [BEP 10: Extension Protocol](http://www.bittorrent.org/beps/bep_0010.html)
- [BitTorrent Protocol Specification](http://www.bittorrent.org/beps/bep_0003.html)

## Conclusion

PEX is a powerful addition to the BitTorrent protocol that enhances peer discovery and reduces reliance on centralized infrastructure. Our implementation provides a robust, configurable PEX system that integrates seamlessly with the existing extension protocol and download manager.
