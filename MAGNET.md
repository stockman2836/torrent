# Magnet Link Support

## Overview

This BitTorrent client has **partial support** for magnet links (BEP 9). The infrastructure for parsing magnet URIs, extension protocol, and metadata exchange is implemented, but full integration with the peer connection system requires additional work.

## Status: Partially Implemented

### ✅ Completed Components

1. **Magnet URI Parser** (`magnet_uri.h/cpp`)
   - Parse magnet:// URIs
   - Extract info hash (hex and base32 formats)
   - Extract display name, trackers, web seeds
   - Validate magnet links
   - Convert back to URI format

2. **Extension Protocol** (`extension_protocol.h/cpp`) - BEP 10
   - Extended handshake mechanism
   - Extension registration and discovery
   - Extension message building and parsing
   - Peer extension capability detection

3. **Metadata Exchange** (`metadata_exchange.h/cpp`) - BEP 9
   - ut_metadata extension implementation
   - Metadata piece request/response handling
   - Progress tracking
   - Metadata assembly and validation

4. **Magnet Download Manager** (`magnet_download_manager.h/cpp`)
   - High-level coordination
   - DHT and tracker peer discovery
   - Metadata download workflow structure

### ⚠️ Requires Further Work

- **PeerConnection Integration**: Extension protocol needs to be integrated with existing `PeerConnection` class
- **Metadata Download Loop**: Complete the peer connection and metadata piece request/response cycle
- **TorrentFile Creation**: Create TorrentFile from downloaded metadata
- **Transition to Regular Download**: Seamlessly switch from metadata download to piece download

## Magnet URI Format

### Basic Format
```
magnet:?xt=urn:btih:<INFO_HASH>&dn=<NAME>&tr=<TRACKER_URL>
```

### Parameters

- **xt** (eXact Topic) - Required
  - Format: `urn:btih:<hash>`
  - Hash can be:
    - 40-character hex (e.g., `a1b2c3d4...`)
    - 32-character base32 (e.g., `ABCDEFGH...`)

- **dn** (Display Name) - Optional
  - Human-readable name of the file/torrent
  - URL-encoded

- **tr** (TRacker) - Optional, can have multiple
  - Tracker announce URL
  - Can specify multiple trackers

- **ws** (Web Seed) - Optional
  - HTTP/FTP source for file
  - BEP 19 compatible

- **xl** (eXact Length) - Optional
  - Total size in bytes

### Example Magnet Links

```bash
# Minimal magnet link (info hash only)
magnet:?xt=urn:btih:a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0

# With display name
magnet:?xt=urn:btih:a1b2c3d4...&dn=Ubuntu+20.04+ISO

# With trackers
magnet:?xt=urn:btih:a1b2c3d4...&tr=udp://tracker.example.com:6881&tr=http://tracker2.example.com/announce

# Base32 info hash
magnet:?xt=urn:btih:ABCDEFGHIJKLMNOPQRSTUVWXYZ234567
```

## Usage

### Current Functionality

```bash
# Parse and display magnet link information
./torrent_client "magnet:?xt=urn:btih:a1b2c3d4..."
```

Output:
```
Detected magnet link
Magnet Link Info:
  Info Hash: a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0
  Name: Ubuntu 20.04 ISO
  Trackers: 2

Magnet link support is partially implemented.
To use magnet links, metadata download needs to be completed.
Currently only .torrent files are fully supported.
```

## Implementation Details

### 1. Magnet URI Parsing

```cpp
#include "magnet_uri.h"

// Parse magnet URI
std::string uri = "magnet:?xt=urn:btih:...";
torrent::MagnetURI magnet = torrent::MagnetURI::parse(uri);

if (magnet.isValid()) {
    auto info_hash = magnet.infoHash();        // 20-byte SHA1
    auto name = magnet.displayName();           // File/torrent name
    auto trackers = magnet.trackers();          // Tracker URLs
    std::string hex = magnet.infoHashHex();    // Hex representation
}
```

### 2. Extension Protocol (BEP 10)

```cpp
#include "extension_protocol.h"

// Create extension protocol handler
ExtensionProtocol ext_proto;

// Register ut_metadata extension
ext_proto.registerExtension("ut_metadata",
    [](uint8_t ext_id, const std::vector<uint8_t>& payload) {
        // Handle ut_metadata messages
    });

// Build extended handshake
auto handshake = ext_proto.buildExtendedHandshake();

// Parse peer's extended handshake
ext_proto.parseExtendedHandshake(peer_handshake);

// Check if peer supports ut_metadata
if (ext_proto.peerSupportsExtension("ut_metadata")) {
    // Request metadata
}
```

### 3. Metadata Exchange (BEP 9)

```cpp
#include "metadata_exchange.h"

// Create metadata exchange handler
int64_t metadata_size = 16384; // From extended handshake
MetadataExchange metadata(metadata_size,
    [](const std::vector<uint8_t>& data) {
        // Metadata download complete callback
    });

// Request next piece
int piece = metadata.getNextPieceToRequest();
if (piece >= 0) {
    auto request_msg = metadata.buildRequestMessage(piece);
    // Send to peer via extension protocol
}

// Handle received metadata piece
metadata.handleMessage(response_payload);

// Check progress
double progress = metadata.progress();  // 0-100%
```

### 4. Metadata Download Workflow

```
1. Parse Magnet URI
   └─> Extract info_hash, trackers, name

2. Find Peers
   ├─> Query DHT for info_hash
   └─> Query trackers

3. Connect to Peers
   └─> Send BitTorrent handshake

4. Send Extended Handshake
   ├─> Advertise ut_metadata support
   └─> Receive peer's extensions

5. Request Metadata
   ├─> Get metadata_size from peer
   ├─> Calculate num_pieces (metadata_size / 16KB)
   └─> Request pieces sequentially

6. Receive Metadata Pieces
   ├─> Validate piece data
   ├─> Store pieces
   └─> Track progress

7. Assemble Complete Metadata
   ├─> Concatenate all pieces
   ├─> Verify info_hash matches
   └─> Parse as bencode (info dict)

8. Create TorrentFile
   └─> Use metadata to build TorrentFile object

9. Start Regular Download
   └─> Switch to piece download mode
```

## Extension Protocol Messages

### Extended Handshake (msg_id=20, ext_id=0)

```json
{
  "m": {
    "ut_metadata": 3,
    "ut_pex": 2
  },
  "metadata_size": 16384,
  "v": "BitTorrent Client 1.0"
}
```

### ut_metadata Messages

**Request (msg_type=0):**
```json
{
  "msg_type": 0,
  "piece": 0
}
```

**Data (msg_type=1):**
```json
{
  "msg_type": 1,
  "piece": 0,
  "total_size": 16384
}
[piece data follows]
```

**Reject (msg_type=2):**
```json
{
  "msg_type": 2,
  "piece": 0
}
```

## Next Steps for Full Implementation

To complete magnet link support:

### 1. Integrate Extension Protocol with PeerConnection

- Modify `PeerConnection` to support extended messages
- Add extended handshake to connection initialization
- Handle message ID 20 (extended messages)

```cpp
// In peer_connection.h
class PeerConnection {
    // ...
    std::unique_ptr<ExtensionProtocol> extension_protocol_;

    void sendExtendedHandshake();
    void handleExtendedMessage(const std::vector<uint8_t>& payload);
};
```

### 2. Implement Metadata Download in MagnetDownloadManager

- Create peer connections with extension support
- Coordinate metadata piece requests across multiple peers
- Handle timeouts and retries
- Assemble and validate complete metadata

### 3. Create TorrentFile from Metadata

- Add constructor to `TorrentFile` that accepts info dict
- Validate metadata against info_hash
- Create TorrentFile object for download

```cpp
// In torrent_file.h
class TorrentFile {
public:
    // New constructor
    static TorrentFile fromMetadata(const std::vector<uint8_t>& info_hash,
                                   const std::vector<uint8_t>& metadata);
};
```

### 4. Transition to Regular Download

- Pass created TorrentFile to DownloadManager
- Continue with normal piece-based download
- Announce to DHT/trackers

## BEP References

- [BEP 9: Extension for Peers to Send Metadata Files](http://www.bittorrent.org/beps/bep_0009.html)
- [BEP 10: Extension Protocol](http://www.bittorrent.org/beps/bep_0010.html)
- [BEP 5: DHT Protocol](http://www.bittorrent.org/beps/bep_0005.html)

## Testing

Once fully implemented, test with:

```bash
# Test with popular Linux ISO magnet
./torrent_client "magnet:?xt=urn:btih:..."

# Test with multiple trackers
./torrent_client "magnet:?xt=urn:btih:...&tr=udp://tracker1.com:6881&tr=http://tracker2.com/announce"

# Test with DHT-only (no trackers)
./torrent_client "magnet:?xt=urn:btih:..."
```

## Current Limitations

1. **No Live Download**: Can parse magnet links but cannot download metadata yet
2. **PeerConnection Integration**: Extension protocol not integrated with existing peer system
3. **No Metadata Persistence**: Downloaded metadata not saved to disk
4. **Single-Threaded Metadata Download**: No parallel requests to multiple peers

## Future Enhancements

- [ ] Complete PeerConnection integration
- [ ] Parallel metadata download from multiple peers
- [ ] Metadata caching (save downloaded metadata)
- [ ] Resume metadata download
- [ ] Fast resume with partial metadata
- [ ] Support for hybrid torrents (metadata + pieces)
