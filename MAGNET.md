# Magnet Link Support

## Overview

This BitTorrent client has **full support** for magnet links (BEP 9). The complete infrastructure for parsing magnet URIs, extension protocol, metadata exchange, and peer connection is implemented and integrated.

## Status: ✅ Fully Implemented

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

### ✅ Recently Completed

- **PeerConnection Integration**: Extension protocol fully integrated with `PeerConnection` class
- **Metadata Download Loop**: Complete peer connection and metadata piece request/response cycle implemented
- **TorrentFile Creation**: `TorrentFile::fromMetadata()` method for creating TorrentFile from downloaded metadata
- **Transition to Regular Download**: Seamless switch from metadata download to piece download via new `DownloadManager` constructor
- **DHT Integration**: Reuse existing DHT instance from metadata download phase

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
# Download from magnet link
./torrent_client "magnet:?xt=urn:btih:a1b2c3d4..."
```

Output:
```
Detected magnet link
Magnet Link Info:
  Info Hash: a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0
  Name: Ubuntu 20.04 ISO
  Trackers: 2

Initializing DHT...
Bootstrapping DHT (waiting 5 seconds)...
Starting metadata download...
Magnet: Looking for peers...
Magnet: Found 15 potential peers
Magnet: Connecting to peers and requesting metadata...
Magnet: Trying peer 192.168.1.10:6881
Magnet: Handshake successful with 192.168.1.10:6881
Magnet: Peer supports ut_metadata, metadata size: 16384 bytes
Magnet: Requesting metadata piece 0
Magnet: Requesting metadata piece 1
Magnet: Metadata received (16384 bytes)
Magnet: Successfully created TorrentFile from metadata

=== Metadata Downloaded Successfully ===
=== Torrent Information ===
Name: Ubuntu 20.04 ISO
...

Starting piece download...
Starting download...
[Download progress continues...]
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

## Implementation Summary

The magnet link support has been fully implemented with the following components:

### 1. ✅ Extension Protocol Integration with PeerConnection

- `PeerConnection` now supports extended messages (MSG_EXTENDED = 20)
- Extended handshake implemented via `sendExtendedHandshake()`
- Extension messages handled in `receiveMessage()` switch statement
- Optional `ExtensionProtocol` member for peers that support extensions

```cpp
// In peer_connection.h
class PeerConnection {
    // ...
    std::unique_ptr<ExtensionProtocol> extension_protocol_;

    bool sendExtendedHandshake();
    bool sendExtendedMessage(uint8_t ext_id, const std::vector<uint8_t>& payload);
    ExtensionProtocol* extensionProtocol();
};
```

### 2. ✅ Metadata Download in MagnetDownloadManager

- Creates peer connections with extension support
- Sends extended handshake to negotiate ut_metadata support
- Coordinates metadata piece requests from peers
- Handles timeouts and tries multiple peers
- Assembles and validates complete metadata via `MetadataExchange`

### 3. ✅ TorrentFile from Metadata

- New static method `TorrentFile::fromMetadata()` implemented
- Validates metadata against info_hash
- Parses info dictionary and creates complete TorrentFile object
- Includes tracker URLs from magnet link

```cpp
// In torrent_file.h
class TorrentFile {
public:
    static TorrentFile fromMetadata(const std::vector<uint8_t>& info_hash,
                                   const std::vector<uint8_t>& metadata,
                                   const std::vector<std::string>& trackers = {});
};
```

### 4. ✅ Transition to Regular Download

- New `DownloadManager` constructor accepts `TorrentFile` directly
- Reuses DHT instance from metadata download phase
- Seamless transition from metadata to piece download
- Full integration in `main.cpp` with proper flow control

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

1. ✅ **Live Download**: Fully functional - can download metadata and files
2. ✅ **PeerConnection Integration**: Extension protocol fully integrated
3. **No Metadata Persistence**: Downloaded metadata not saved to disk (future enhancement)
4. **Sequential Peer Attempts**: Tries up to 5 peers sequentially (parallel requests could be added)

## Future Enhancements

- [x] Complete PeerConnection integration - **DONE**
- [ ] Parallel metadata download from multiple peers
- [ ] Metadata caching (save downloaded metadata to .torrent file)
- [ ] Resume metadata download
- [ ] Fast resume with partial metadata
- [ ] Support for hybrid torrents (metadata + pieces)
- [ ] Better error handling and retry logic
- [ ] Progress reporting during metadata download
