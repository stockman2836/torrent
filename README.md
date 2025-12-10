# BitTorrent Client

A full-featured BitTorrent client in C++17 with support for core BitTorrent protocol features.

## Features

- .torrent file parsing (Bencode format)
- HTTP and UDP tracker communication
- BitTorrent peer protocol
- **DHT (Distributed Hash Table) support** - Trackerless operation
- Piece-based download management
- Multithreaded architecture
- Support for single-file and multi-file torrents
- SHA1 verification of downloaded data
- Resume capability
- Logging system
- JSON configuration
- CLI interface

## Architecture

The project is divided into several core components:

### Main Modules:

1. **Bencode Parser** (`bencode.h/cpp`)

   - Parsing Bencode format (used in .torrent files)
   - Support for integers, strings, lists, dictionaries
   - Encoding/Decoding

2. **Torrent File** (`torrent_file.h/cpp`)

   - .torrent file parsing
   - Metadata extraction (announce URL, file sizes, info hash)
   - Support for single-file and multi-file modes

3. **Tracker Client** (`tracker_client.h/cpp`, `udp_tracker.h/cpp`)

   - HTTP and UDP tracker support
   - Peer list retrieval
   - Tracker response parsing (compact/dictionary format)

4. **Peer Connection** (`peer_connection.h/cpp`)

   - BitTorrent peer protocol implementation
   - Peer handshake
   - Message exchange (choke, interested, request, piece, etc.)

5. **Piece Manager** (`piece_manager.h/cpp`)

   - File piece download management
   - Piece verification via SHA1
   - Bitfield tracking of downloaded pieces
   - Download strategies

6. **File Manager** (`file_manager.h/cpp`)

   - File system operations
   - Reading/writing pieces to files
   - Multi-file torrent support

7. **Download Manager** (`download_manager.h/cpp`)

   - Overall download process coordination
   - Thread management
   - Statistics (speed, progress)

8. **Utils** (`utils.h/cpp`)
   - SHA1 hashing (OpenSSL)
   - URL encoding
   - Hex conversion
   - Peer ID generation
   - Output formatting

9. **DHT Manager** (`dht_manager.h/cpp`, `dht_node.h/cpp`, `dht_krpc.h/cpp`, `dht_routing_table.h/cpp`)

   - Kademlia-based DHT implementation (BEP 5)
   - KRPC protocol (ping, find_node, get_peers, announce_peer)
   - K-bucket routing table
   - Bootstrap from well-known nodes
   - Peer discovery without trackers
   - Token-based announce_peer validation

10. **Logger** (`logger.h/cpp`)
    - Multi-level logging (trace, debug, info, warn, error)
    - File and console output
    - Log rotation

11. **Config** (`config.h/cpp`)
    - JSON-based configuration
    - CLI argument override
    - Configurable DHT, speed limits, ports, etc.

## Requirements

- C++17 or higher
- CMake 3.15+
- OpenSSL (for SHA1)
- Compiler: GCC, Clang, or MSVC

### Windows:

- Visual Studio 2019+ or MinGW
- OpenSSL (can be installed via vcpkg)

### Linux/macOS:

- GCC 7+ or Clang 5+
- OpenSSL (usually pre-installed)

## Building

### Windows (Visual Studio):

```bash
# Install OpenSSL via vcpkg (optional)
vcpkg install openssl

# Build
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[path to vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

### Windows (MinGW):

```bash
mkdir build
cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
```

### Linux/macOS:

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install build-essential cmake libssl-dev

# Build
mkdir build
cd build
cmake ..
make -j$(nproc)
```

## Usage

```bash
# Basic usage
./torrent_client example.torrent

# With custom download directory
./torrent_client example.torrent ./my_downloads
```

### Options:

- `<torrent_file>` - Path to .torrent file (required)
- `[download_dir]` - Directory to save files (default: ./downloads)
