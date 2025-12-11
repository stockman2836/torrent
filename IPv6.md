# IPv6 Support - BEP 7

## Overview

IPv6 support (BEP 7) extends the BitTorrent protocol to work with IPv6 addresses, enabling connectivity in modern networks. This implementation provides full dual-stack support, allowing peers to connect via both IPv4 and IPv6.

## Implementation

Our implementation follows [BEP 7 specification](http://www.bittorrent.org/beps/bep_0007.html) and [BEP 32](http://www.bittorrent.org/beps/bep_0032.html) for IPv6 tracker extensions.

## Key Features

1. **Dual Stack Support** - Seamless IPv4 and IPv6 connectivity
2. **Automatic Detection** - Automatically detect and handle both address types
3. **Compact Format** - IPv6 compact peer format (18 bytes per peer)
4. **Configuration** - Flexible IPv4/IPv6 preferences

## Configuration

### Config Options

```json
{
    "enable_ipv6": true,
    "ip_version": "dual"
}
```

### IP Version Modes

- `"ipv4"` - IPv4 only
- `"ipv6"` - IPv6 only
- `"dual"` - Dual stack, prefer IPv4
- `"dual_v6"` - Dual stack, prefer IPv6

### Usage

```cpp
torrent::Config config;
config.enable_ipv6 = true;
config.ip_version = "dual";  // Support both IPv4 and IPv6
```

## Address Formats

### IPv4 Compact Format
```
[4 bytes: IP address] [2 bytes: port]
Total: 6 bytes per peer
```

Example:
```
C0 A8 01 64 1A E1  ->  192.168.1.100:6881
```

### IPv6 Compact Format
```
[16 bytes: IP address] [2 bytes: port]
Total: 18 bytes per peer
```

Example:
```
20 01 0D B8 00 00 00 00 00 00 00 00 00 00 00 01 1A E1
-> 2001:db8::1:6881
```

## API Reference

### Utility Functions

#### detectIPVersion()
```cpp
IPVersion detectIPVersion(const std::string& ip);
```
Detects whether an IP string is IPv4, IPv6, or invalid.

**Returns:**
- `IPVersion::IPv4` - Valid IPv4 address
- `IPVersion::IPv6` - Valid IPv6 address
- `IPVersion::Unknown` - Invalid or unrecognized

**Example:**
```cpp
auto version = utils::detectIPVersion("2001:db8::1");
if (version == utils::IPVersion::IPv6) {
    std::cout << "IPv6 address\n";
}
```

#### isValidIPv4() / isValidIPv6()
```cpp
bool isValidIPv4(const std::string& ip);
bool isValidIPv6(const std::string& ipv6);
```
Validate IP address strings.

**Example:**
```cpp
if (utils::isValidIPv6("2001:db8::1")) {
    // Valid IPv6
}
```

#### ipv6ToCompact()
```cpp
std::vector<uint8_t> ipv6ToCompact(const std::string& ipv6);
```
Convert IPv6 string to 16-byte compact binary format.

**Example:**
```cpp
auto compact = utils::ipv6ToCompact("2001:db8::1");
// compact contains 16 bytes
```

#### compactToIPv6()
```cpp
std::string compactToIPv6(const uint8_t* data);
```
Convert 16-byte compact format to IPv6 string.

**Example:**
```cpp
uint8_t compact[16] = { /* ... */ };
std::string ipv6 = utils::compactToIPv6(compact);
// ipv6 = "2001:db8::1"
```

#### normalizeIPv6()
```cpp
std::string normalizeIPv6(const std::string& ipv6);
```
Convert IPv6 to canonical form (expand `::` notation).

**Example:**
```cpp
std::string normalized = utils::normalizeIPv6("::1");
// normalized = "0:0:0:0:0:0:0:1" or "::1" (system dependent)
```

### Peer Structure

The `Peer` struct now includes IPv6 support:

```cpp
struct Peer {
    std::string ip;
    uint16_t port;
    bool is_ipv6;  // Auto-detected from IP

    Peer(const std::string& ip, uint16_t port);

    // Factory methods
    static Peer fromCompactIPv4(const uint8_t* data);
    static Peer fromCompactIPv6(const uint8_t* data);

    // Convert to compact format
    std::vector<uint8_t> toCompact() const;
};
```

**Example:**
```cpp
// Create IPv6 peer
Peer peer("2001:db8::1", 6881);
std::cout << "Is IPv6: " << peer.is_ipv6 << "\n";  // true

// Convert to compact
auto compact = peer.toCompact();  // 18 bytes for IPv6

// Parse from compact
Peer peer2 = Peer::fromCompactIPv6(compact.data());
```

## Socket Support

### PeerConnection IPv6

The `PeerConnection` class automatically handles IPv6:

```cpp
// IPv6 connection
PeerConnection peer("2001:db8::1", 6881, info_hash, peer_id);
peer.connect();  // Automatically uses AF_INET6

// Check if IPv6
if (peer.is_ipv6_) {
    std::cout << "Connected via IPv6\n";
}
```

### Implementation Details

#### Socket Creation
```cpp
// Automatic address family selection
int address_family = is_ipv6_ ? AF_INET6 : AF_INET;
socket_fd_ = socket(address_family, SOCK_STREAM, IPPROTO_TCP);
```

#### Address Structures
```cpp
if (is_ipv6_) {
    // IPv6
    struct sockaddr_in6 addr6;
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(port);
    inet_pton(AF_INET6, ip.c_str(), &addr6.sin6_addr);
} else {
    // IPv4
    struct sockaddr_in addr4;
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr4.sin_addr);
}
```

## Tracker Support

### HTTP Tracker

HTTP trackers can return IPv6 peers in the response:

```python
# Tracker response (bencoded)
{
    "interval": 1800,
    "peers": "...",      # IPv4 peers (compact)
    "peers6": "..."      # IPv6 peers (compact)
}
```

### UDP Tracker

UDP trackers use action 4 for IPv6 announces:

```
Action 1: IPv4 announce
Action 4: IPv6 announce (BEP 15)
```

## PEX IPv6 Support

PEX messages now support IPv6 peers:

```python
# PEX message
{
    "added": "...",      # IPv4 peers (6 bytes each)
    "added.f": "...",    # IPv4 flags
    "added6": "...",     # IPv6 peers (18 bytes each)
    "added6.f": "...",   # IPv6 flags
    "dropped6": "..."    # Dropped IPv6 peers
}
```

**Note:** Full PEX IPv6 implementation is pending and marked for future development.

## Examples

### Basic IPv6 Connection

```cpp
#include "peer_connection.h"

// Connect to IPv6 peer
PeerConnection peer(
    "2001:db8:85a3::8a2e:370:7334",
    6881,
    info_hash,
    peer_id
);

if (peer.connect()) {
    std::cout << "Connected via IPv6!\n";
    peer.performHandshake();
}
```

### Dual Stack Configuration

```cpp
#include "config.h"
#include "download_manager.h"

torrent::Config config;
config.enable_ipv6 = true;
config.ip_version = "dual";  // Try both IPv4 and IPv6

// Will connect to both IPv4 and IPv6 peers
DownloadManager manager("file.torrent", "./downloads",
                        6881, 0, 0, true, true);
manager.start();
```

### IPv6 Peer Parsing

```cpp
// Parse IPv6 peers from tracker
std::vector<uint8_t> peers6_data = /* from tracker */;

std::vector<Peer> ipv6_peers;
for (size_t i = 0; i + 18 <= peers6_data.size(); i += 18) {
    Peer peer = Peer::fromCompactIPv6(&peers6_data[i]);
    ipv6_peers.push_back(peer);
}
```

### Address Validation

```cpp
#include "utils.h"

std::string addr = getUserInput();

if (utils::isValidIPv6(addr)) {
    // Valid IPv6
    auto compact = utils::ipv6ToCompact(addr);
    // Use compact format
} else if (utils::isValidIPv4(addr)) {
    // Valid IPv4
} else {
    // Invalid address
}
```

## Performance Considerations

### Memory Usage
- IPv4 peer: 6 bytes compact format
- IPv6 peer: 18 bytes compact format
- 3x memory for IPv6 peer lists

### Connection Priority
Dual stack mode connects to:
1. IPv4 peers (if `ip_version = "dual"`)
2. IPv6 peers (if `ip_version = "dual_v6"`)
3. Mix of both based on availability

### Bandwidth
IPv6 overhead is minimal:
- Same protocol, just different addressing
- No significant performance difference

## Platform Support

### Windows
- Requires Windows Vista or later for full IPv6 support
- Uses `winsock2.h` and `ws2tcpip.h`

### Linux/Unix
- Full IPv6 support on modern kernels
- Uses `netinet/in.h` and `arpa/inet.h`

### macOS
- Native IPv6 support
- Same headers as Linux

## Troubleshooting

### IPv6 Not Working

1. **Check OS Support**
   ```bash
   # Linux
   ip -6 addr show

   # Windows
   ipconfig /all
   ```

2. **Verify Configuration**
   ```cpp
   config.print();  // Should show "IPv6 enabled: yes"
   ```

3. **Test Connectivity**
   ```bash
   ping6 ipv6.google.com
   ```

### Connection Failures

1. **Firewall Rules**
   - Ensure IPv6 ports are open
   - Check ip6tables (Linux) or Windows Firewall

2. **Router Support**
   - Verify router has IPv6 enabled
   - Check for IPv6 NAT issues

3. **ISP Support**
   - Not all ISPs provide IPv6
   - May need tunnel broker (6to4, Teredo)

### Address Format Errors

```cpp
// Wrong:
Peer peer("2001:db8::1:6881", 0);  // Port in IP string

// Correct:
Peer peer("2001:db8::1", 6881);   // Separate port
```

## Future Enhancements

### Planned Features

1. **IPv6 Multicast**
   - Local peer discovery via IPv6 multicast
   - BEP 14 extension

2. **NAT64/DNS64**
   - Support for IPv6-only networks
   - Automatic IPv4-to-IPv6 translation

3. **Happy Eyeballs**
   - RFC 8305 implementation
   - Parallel IPv4/IPv6 connection attempts

4. **IPv6 Privacy Extensions**
   - Temporary addresses (RFC 4941)
   - Enhanced privacy for IPv6 peers

### PEX IPv6 (TODO)

Full implementation of IPv6 PEX fields:
- `added6` - Added IPv6 peers (18 bytes each)
- `added6.f` - Flags for IPv6 peers
- `dropped6` - Dropped IPv6 peers

## Testing

### Unit Tests

```cpp
// Test IPv6 address parsing
ASSERT_TRUE(utils::isValidIPv6("2001:db8::1"));
ASSERT_FALSE(utils::isValidIPv6("192.168.1.1"));

// Test compact format
auto compact = utils::ipv6ToCompact("2001:db8::1");
ASSERT_EQ(compact.size(), 16);
std::string ipv6 = utils::compactToIPv6(compact.data());
ASSERT_EQ(ipv6, "2001:db8::1");

// Test peer structure
Peer peer("2001:db8::1", 6881);
ASSERT_TRUE(peer.is_ipv6);
auto peer_compact = peer.toCompact();
ASSERT_EQ(peer_compact.size(), 18);
```

### Integration Tests

```cpp
// Test IPv6 connection
PeerConnection peer("::1", 6881, info_hash, peer_id);
ASSERT_TRUE(peer.connect());
ASSERT_TRUE(peer.is_ipv6_);

// Test dual stack
Config config;
config.ip_version = "dual";
// Should accept both IPv4 and IPv6 peers
```

## Security Considerations

### Privacy
- IPv6 addresses may reveal device info
- Consider using privacy extensions
- Temporary addresses recommended

### Attack Surface
- IPv6 expands address space
- More complex address validation
- Ensure proper input validation

### Best Practices
1. Validate all IPv6 addresses
2. Use canonical forms for comparison
3. Implement rate limiting per address
4. Monitor for IPv6-specific attacks

## References

- [BEP 7: IPv6 Tracker Extension](http://www.bittorrent.org/beps/bep_0007.html)
- [BEP 15: UDP Tracker Protocol for IPv6](http://www.bittorrent.org/beps/bep_0015.html)
- [BEP 32: IPv6 extension for DHT](http://www.bittorrent.org/beps/bep_0032.html)
- [RFC 4291: IPv6 Addressing Architecture](https://tools.ietf.org/html/rfc4291)
- [RFC 8305: Happy Eyeballs Version 2](https://tools.ietf.org/html/rfc8305)

## Conclusion

IPv6 support enables BitTorrent to function in modern IPv6-only and dual-stack networks. Our implementation provides:

- ✅ Full IPv6 address support
- ✅ Dual stack configuration options
- ✅ Compact format for efficient peer lists
- ✅ Automatic IPv4/IPv6 detection
- ✅ Platform-independent socket handling
- ✅ Utility functions for address manipulation

This ensures compatibility with the evolving internet infrastructure and future-proofs the BitTorrent client.
