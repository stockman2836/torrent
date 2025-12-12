# uTP (Micro Transport Protocol) - BEP 29

## Overview

uTP (Micro Transport Protocol) is a UDP-based transport protocol designed specifically for BitTorrent. It implements its own congestion control algorithm called LEDBAT (Low Extra Delay Background Transport) that aims to utilize available bandwidth while avoiding congestion and maintaining low latency for other applications.

This implementation uses **[libutp](https://github.com/bittorrent/libutp)**, the official uTP library maintained by BitTorrent Inc.

## Key Benefits

### Why uTP Instead of TCP?

1. **Better Network Behavior**
   - Yields to TCP traffic automatically
   - Doesn't cause bufferbloat
   - Maintains low latency for other applications (gaming, VoIP, web browsing)

2. **NAT Traversal**
   - Works better through NAT devices
   - Easier hole-punching
   - Better firewall compatibility

3. **Congestion Control**
   - LEDBAT algorithm detects congestion through delay measurements
   - Target: 100ms delay (maintains near-zero queue)
   - Automatically backs off when network is busy

4. **Bandwidth Utilization**
   - Uses all available bandwidth when network is idle
   - Shares bandwidth fairly with other uTP connections
   - Doesn't interfere with normal internet usage

### Performance Comparison

```
Scenario: Downloading while browsing/gaming

TCP BitTorrent:
  Download: 10 MB/s
  Web latency: 500-2000ms (bufferbloat!)
  Gaming ping: 300ms+
  Quality: Poor user experience

uTP BitTorrent:
  Download: 10 MB/s
  Web latency: 50-100ms
  Gaming ping: 50ms
  Quality: Excellent user experience
```

## Installation

### Option 1: Using Pre-built libutp (Recommended)

#### Linux (Ubuntu/Debian)

```bash
# Install from package manager (if available)
sudo apt-get install libutp-dev

# Or build from source
git clone https://github.com/bittorrent/libutp.git external/libutp
cd external/libutp
make
sudo make install
```

#### macOS

```bash
# Using Homebrew
brew install libutp

# Or build from source
git clone https://github.com/bittorrent/libutp.git external/libutp
cd external/libutp
make
sudo make install
```

#### Windows

```powershell
# Clone libutp
git clone https://github.com/bittorrent/libutp.git external/libutp

# Build with Visual Studio
cd external/libutp
# Open utp.sln in Visual Studio and build
```

### Option 2: Building with CMake

Our project supports optional uTP integration:

```bash
# Build with uTP support
mkdir build
cd build
cmake .. -DENABLE_UTP=ON
make

# Build without uTP (default)
cmake ..
make
```

**CMake will:**
1. Check if libutp is installed
2. If found: Enable uTP support with `-DHAVE_LIBUTP`
3. If not found: Compile without uTP (stub implementation)

### Verifying Installation

```bash
# Check if libutp is found
cmake .. -DENABLE_UTP=ON

# Output should show:
# -- Found libutp: /usr/local/lib/libutp.a
```

## Protocol Details

### BEP 29 Specification

uTP is defined in [BEP 29](https://www.bittorrent.org/beps/bep_0029.html).

#### Packet Header (20 bytes)

```
 0               8               16              24              32
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Type |  Ver  |   Extension   |      Connection ID              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   Timestamp Microseconds                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                Timestamp Difference Microseconds                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Window Size                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Sequence Number       |       Acknowledgment Number    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

#### Packet Types

| Type | Name | Description |
|------|------|-------------|
| 0 | ST_DATA | Regular data packet with payload |
| 1 | ST_FIN | Connection termination (like TCP FIN) |
| 2 | ST_STATE | ACK-only packet (no data) |
| 3 | ST_RESET | Force close (like TCP RST) |
| 4 | ST_SYN | Connection initiation (3-way handshake) |

### LEDBAT Congestion Control

**Core Algorithm:**

```
1. Measure one-way delay: our_delay = timestamp_diff - base_delay
2. Calculate deviation: off_target = TARGET (100ms) - our_delay
3. Adjust window:
   - If our_delay < 100ms: Increase window (more bandwidth)
   - If our_delay > 100ms: Decrease window (less bandwidth)
```

**Key Features:**
- **Target delay: 100ms** - maintains minimal queuing
- **Base delay tracking** - sliding minimum over 2 minutes
- **Delay-based** - uses delay, not loss, to detect congestion
- **Yields to TCP** - backs off when delay increases

### Connection State Machine

```
IDLE
  |
  | SYN sent
  v
SYN_SENT
  |
  | SYN-ACK received
  v
CONNECTED <---> DATA exchange
  |
  | FIN sent
  v
FIN_SENT
  |
  | ACK received
  v
DESTROY
```

## Configuration

### Config File (config.json)

```json
{
  "enable_utp": true,
  "prefer_utp": false
}
```

**Options:**
- `enable_utp`: Enable/disable uTP support (requires libutp)
- `prefer_utp`: Prefer uTP over TCP when both are available

### Runtime Configuration

```cpp
#include "config.h"

torrent::Config config;

// Enable uTP
config.enable_utp = true;

// Prefer uTP over TCP (recommended for better network behavior)
config.prefer_utp = true;

// Save configuration
config.saveToFile("config.json");
```

## Usage

### Basic Example

```cpp
#include "utp_socket.h"

// Check if uTP is available
if (!torrent::utp::UtpManager::isAvailable()) {
    std::cerr << "uTP not available (libutp not compiled)\n";
    return;
}

// Create uTP manager
torrent::utp::UtpManager utp_manager(6881);

// Create uTP socket
auto socket = utp_manager.createSocket();

// Set callbacks
socket->setOnConnect([]() {
    std::cout << "Connected via uTP!\n";
});

socket->setOnData([](const uint8_t* data, size_t length) {
    std::cout << "Received " << length << " bytes via uTP\n";
});

socket->setOnError([](int error_code) {
    std::cerr << "uTP error: " << error_code << "\n";
});

// Connect to peer
if (socket->connect("192.168.1.100", 6881)) {
    std::cout << "Connecting...\n";
}

// Event loop
while (true) {
    utp_manager.tick();  // Process uTP events
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
```

### Integration with BitTorrent Client

```cpp
#include "download_manager.h"
#include "config.h"

torrent::Config config;
config.enable_utp = true;
config.prefer_utp = true;

torrent::DownloadManager manager(
    "ubuntu.torrent",
    "./downloads",
    6881,
    0, 0,
    true,  // DHT
    true,  // PEX
    true,  // LSD
    true   // Web Seeding
);

// uTP will be used automatically for peer connections
// when both peers support it
manager.start();
```

### Checking uTP Availability at Runtime

```cpp
#include "utp_socket.h"

void checkUtpSupport() {
    if (torrent::utp::UtpManager::isAvailable()) {
        std::cout << "✓ uTP support enabled (libutp found)\n";
    } else {
        std::cout << "✗ uTP support disabled (libutp not found)\n";
        std::cout << "  Rebuild with libutp to enable uTP\n";
    }
}
```

## Peer Protocol Handshake

### Advertising uTP Support

BitTorrent extension handshake (BEP 10):

```cpp
// Extension dictionary
{
    "m": {
        "ut_pex": 1,
        "ut_metadata": 2
    },
    // Advertise uTP support
    "p": 6882  // uTP port
}
```

### Connection Preference

**Algorithm:**
1. If `prefer_utp = true`:
   - Try uTP first
   - Fall back to TCP if uTP fails
2. If `prefer_utp = false`:
   - Try TCP first
   - Use uTP only if TCP fails or peer requests it

```cpp
bool PeerConnection::connect(const Peer& peer) {
    if (config.prefer_utp && peer.supports_utp) {
        // Try uTP first
        if (connectUtp(peer)) {
            return true;
        }
    }

    // Fall back to TCP
    return connectTcp(peer);
}
```

## Performance Tuning

### Optimal Settings

```json
{
  "enable_utp": true,
  "prefer_utp": true,

  // Network settings that work well with uTP
  "max_peers": 100,
  "max_connections": 200,

  // Let uTP handle congestion control
  "max_download_speed": 0,  // unlimited
  "max_upload_speed": 0     // unlimited
}
```

### Monitoring uTP Performance

```cpp
// Get statistics
auto socket = utp_manager.createSocket();

std::cout << "Bytes sent: " << socket->getBytesSent() << "\n";
std::cout << "Bytes received: " << socket->getBytesReceived() << "\n";

// Manager statistics
std::cout << "Total uTP traffic:\n";
std::cout << "  Sent: " << utp_manager.getTotalBytesSent() << "\n";
std::cout << "  Received: " << utp_manager.getTotalBytesReceived() << "\n";
```

## Troubleshooting

### uTP Not Available

**Symptom:**
```
CMake Warning: libutp not found - uTP support will be disabled
```

**Solution:**
```bash
# Install libutp
git clone https://github.com/bittorrent/libutp.git external/libutp
cd external/libutp
make
sudo make install

# Reconfigure and rebuild
cd ../../build
cmake .. -DENABLE_UTP=ON
make
```

### Connection Failures

**Problem:** uTP connections failing to establish

**Checks:**
1. **Firewall**: Allow UDP traffic on your port
   ```bash
   # Linux
   sudo ufw allow 6881/udp

   # Windows
   netsh advfirewall firewall add rule name="BitTorrent uTP" protocol=UDP dir=in localport=6881 action=allow
   ```

2. **Router**: Enable UDP forwarding for your port

3. **NAT**: uTP may have issues with strict NAT
   - Enable UPnP/NAT-PMP
   - Or manually forward UDP port

### High Latency

**Problem:** uTP showing high latency

**Cause:** Network congestion or misconfigured LEDBAT

**Solution:**
- uTP automatically adapts - give it time
- Check if other applications are saturating bandwidth
- Verify base_delay is calculated correctly

### Falling Back to TCP

**Symptom:** Always using TCP despite `prefer_utp = true`

**Reasons:**
1. Peer doesn't support uTP
2. UDP blocked by firewall/router
3. NAT issues
4. libutp not compiled

**Debug:**
```cpp
if (socket->connect(host, port)) {
    if (socket->isConnected()) {
        std::cout << "Connected via uTP\n";
    } else {
        std::cout << "uTP failed, using TCP\n";
    }
}
```

## Advanced Topics

### Custom UDP Socket

For advanced users who need more control:

```cpp
// Create UDP socket manually
int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);

// Bind to port
struct sockaddr_in addr;
addr.sin_family = AF_INET;
addr.sin_port = htons(6881);
addr.sin_addr.s_addr = INADDR_ANY;
bind(udp_socket, (struct sockaddr*)&addr, sizeof(addr));

// Pass to libutp
// (requires lower-level libutp API)
```

### libutp API Reference

For direct libutp usage, see:
- [libutp GitHub](https://github.com/bittorrent/libutp)
- [libutp API Documentation](https://github.com/bittorrent/libutp/blob/master/utp.h)

## Comparison: TCP vs uTP

| Feature | TCP | uTP |
|---------|-----|-----|
| **Transport** | Kernel-level | User-space over UDP |
| **Congestion Control** | Loss-based (CUBIC, Reno) | Delay-based (LEDBAT) |
| **Latency Impact** | High (bufferbloat) | Low (yields to traffic) |
| **Bandwidth Usage** | Aggressive | Polite |
| **NAT Traversal** | Difficult | Easier |
| **Firewall Friendly** | Less | More |
| **Implementation** | OS kernel | libutp library |
| **Overhead** | Lower | Slightly higher |

## References

- [BEP 29: uTorrent Transport Protocol](https://www.bittorrent.org/beps/bep_0029.html)
- [LEDBAT RFC 6817](https://tools.ietf.org/html/rfc6817)
- [libutp GitHub Repository](https://github.com/bittorrent/libutp)
- [Bufferbloat Explanation](https://www.bufferbloat.net/)

## Conclusion

uTP provides:

- ✅ **Better network citizenship** - yields to other traffic
- ✅ **Low latency** - maintains <100ms queuing delay
- ✅ **Full bandwidth utilization** - when network is idle
- ✅ **NAT-friendly** - UDP-based, easier hole-punching
- ✅ **Production-ready** - used by µTorrent, BitTorrent, Transmission
- ✅ **Easy integration** - via libutp library

For BitTorrent traffic, **uTP is strongly recommended** over TCP to maintain good internet experience for all users.
