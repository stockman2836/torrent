# Local Service Discovery (LSD) - BEP 14

## Overview

Local Service Discovery (LSD) is a peer discovery mechanism that uses IP multicast to find peers on the local network. This implementation follows [BEP 14 specification](http://www.bittorrent.org/beps/bep_0014.html).

Unlike trackers or DHT which discover peers across the internet, LSD discovers peers on the same local network (LAN), making it ideal for:
- Fast peer discovery without external dependencies
- Internal network file distribution
- Reducing external bandwidth usage
- Working behind restrictive firewalls

## Implementation

Our LSD implementation provides:
- **Multicast UDP** - Sends and receives announcements via multicast
- **Automatic peer discovery** - Finds peers on the local network automatically
- **Periodic announcements** - Announces torrents every 5 minutes
- **Thread-safe** - Safe for concurrent use
- **Callback-based** - Notifies application when peers are discovered

## How It Works

### Multicast Communication

LSD uses UDP multicast to broadcast torrent announcements to all BitTorrent clients on the local network:

1. **Multicast Group**: `239.192.152.143:6771` (IPv4)
2. **Message Format**: HTTP-like plaintext messages
3. **Discovery**: All clients listening on the multicast group receive announcements

```
Client A                    Multicast Group                  Client B
   |                        (239.192.152.143)                    |
   |--- Announce Torrent X --------->|<--------- Listening ------|
   |                                  |                           |
   |                                  |-------- Announce -------->|
   |<---------- Reply ----------------|                           |
```

### Message Format

LSD messages follow this format (BEP 14):

```
BT-SEARCH * HTTP/1.1
Host: 239.192.152.143:6771
Port: 6881
Infohash: 0123456789ABCDEF0123456789ABCDEF01234567

```

**Fields:**
- `BT-SEARCH * HTTP/1.1` - Cookie/identifier for LSD messages
- `Host` - Multicast address and port
- `Port` - Client's listening port for peer connections
- `Infohash` - SHA1 hash of the torrent (40 hex characters)

### Announcement Flow

1. **Client starts** and joins multicast group
2. **Announces torrents** every 5 minutes
3. **Listens for announcements** from other clients
4. **Parses messages** and extracts peer info
5. **Calls callback** to notify application of discovered peers

## Configuration

### Config Options

```json
{
    "enable_lsd": true
}
```

### Enabling LSD

```cpp
torrent::Config config;
config.enable_lsd = true;

// LSD is enabled by default
```

### Usage in DownloadManager

```cpp
// Enable LSD when creating DownloadManager
torrent::DownloadManager manager(
    "file.torrent",
    "./downloads",
    6881,        // listen_port
    0,           // max_download_speed
    0,           // max_upload_speed
    true,        // enable_dht
    true,        // enable_pex
    true         // enable_lsd (NEW!)
);

manager.start();
```

## API Reference

### LSD Class

The main class for Local Service Discovery.

```cpp
class LSD {
public:
    LSD(uint16_t listen_port = 6881, bool enable_ipv6 = false);
    ~LSD();

    // Control
    void start();
    void stop();
    bool isRunning() const;

    // Torrent management
    void announce(const std::vector<uint8_t>& info_hash);
    void stopAnnounce(const std::vector<uint8_t>& info_hash);

    // Peer discovery callback
    void setPeerCallback(LSDPeerCallback callback);

    // Statistics
    size_t getAnnouncedTorrentsCount() const;
    size_t getDiscoveredPeersCount() const;
};
```

### LSDPeer Structure

Represents a peer discovered via LSD.

```cpp
struct LSDPeer {
    std::string ip;
    uint16_t port;
    std::vector<uint8_t> info_hash;

    LSDPeer(const std::string& ip_, uint16_t port_,
            const std::vector<uint8_t>& info_hash_);
};
```

### LSDPeerCallback

Callback type for peer discovery notifications.

```cpp
using LSDPeerCallback = std::function<void(const std::vector<LSDPeer>&)>;
```

## Examples

### Basic Usage

```cpp
#include "lsd.h"
#include "utils.h"

// Create LSD instance
torrent::LSD lsd(6881);  // Use port 6881

// Set callback for discovered peers
lsd.setPeerCallback([](const std::vector<torrent::LSDPeer>& peers) {
    for (const auto& peer : peers) {
        std::cout << "Discovered peer: " << peer.ip << ":" << peer.port
                  << " for torrent " << utils::toHex(peer.info_hash).substr(0, 8)
                  << "\n";
    }
});

// Start LSD service
lsd.start();

// Announce a torrent
std::vector<uint8_t> info_hash = /* ... */;
lsd.announce(info_hash);

// LSD will now:
// 1. Send announcements every 5 minutes
// 2. Listen for announcements from other clients
// 3. Call the callback when peers are discovered

// Later, stop announcing
lsd.stopAnnounce(info_hash);

// Stop LSD
lsd.stop();
```

### Integration with DownloadManager

LSD is automatically integrated into `DownloadManager`:

```cpp
// Create manager with LSD enabled
torrent::DownloadManager manager("file.torrent", "./downloads", 6881,
                                 0, 0, true, true, true);

// Start downloads
manager.start();

// LSD automatically:
// - Announces all torrents being downloaded
// - Discovers peers on local network
// - Adds discovered peers to peer pool
// - Connects to discovered peers

// Stop
manager.stop();
```

### Custom Peer Handling

```cpp
torrent::LSD lsd(6881);

// Custom peer handler
lsd.setPeerCallback([](const std::vector<torrent::LSDPeer>& peers) {
    std::cout << "LSD discovered " << peers.size() << " peer(s)\n";

    for (const auto& peer : peers) {
        // Validate peer
        if (peer.port < 1024 || peer.port > 65535) {
            std::cerr << "Invalid peer port: " << peer.port << "\n";
            continue;
        }

        // Filter private IPs
        if (peer.ip.find("192.168.") == 0 ||
            peer.ip.find("10.") == 0 ||
            peer.ip.find("172.16.") == 0) {
            std::cout << "Local peer: " << peer.ip << ":" << peer.port << "\n";
            // Add to peer list...
        }
    }
});

lsd.start();
```

### Multiple Torrents

```cpp
torrent::LSD lsd(6881);
lsd.start();

// Announce multiple torrents
std::vector<std::vector<uint8_t>> torrents = {
    info_hash_1,
    info_hash_2,
    info_hash_3
};

for (const auto& info_hash : torrents) {
    lsd.announce(info_hash);
}

// Check statistics
std::cout << "Announcing " << lsd.getAnnouncedTorrentsCount() << " torrents\n";
std::cout << "Discovered " << lsd.getDiscoveredPeersCount() << " peers\n";

// Stop announcing specific torrent
lsd.stopAnnounce(info_hash_2);
```

## Architecture

### Threading Model

LSD uses two background threads:

1. **Announce Thread** (`announceLoop`)
   - Sends multicast announcements every 5 minutes
   - Announces all registered torrents
   - Runs until stopped

2. **Listen Thread** (`listenLoop`)
   - Receives multicast messages
   - Parses incoming announcements
   - Calls peer callback for discovered peers
   - Runs until stopped

### Message Flow

```
┌─────────────┐
│     LSD     │
└──────┬──────┘
       │
   ┌───┴────┐
   │ start()│
   └───┬────┘
       │
   ┌───┴──────────────┬──────────────┐
   │                  │              │
┌──▼──────────┐  ┌───▼──────┐  ┌───▼─────────┐
│announceLoop │  │listenLoop│  │Multicast Grp│
│             │  │          │  │239.192.152. │
│ Every 5min  │  │Receives  │  │   143:6771  │
│ ┌─────────┐ │  │messages  │  └──────────┬──┘
│ │Announce │ │  │          │             │
│ │Torrent A├─┼──┼──────────┼─────────────┤
│ │         │ │  │          │  Multicast  │
│ └─────────┘ │  │ ┌──────┐ │◄────────────┤
│             │  │ │Parse │ │             │
│             │  │ │Msg   │ │             │
│             │  │ └──┬───┘ │             │
│             │  │    │     │             │
│             │  │ ┌──▼────┐│             │
│             │  │ │Call   ││             │
│             │  │ │back   ││             │
│             │  │ └───────┘│             │
└─────────────┘  └──────────┘
```

### Socket Operations

LSD uses UDP multicast sockets with the following settings:

```cpp
// Create UDP socket
socket_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

// Allow multiple listeners (SO_REUSEADDR)
setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

// Bind to multicast port
bind(socket_fd_, &bind_addr, sizeof(bind_addr));

// Join multicast group (IP_ADD_MEMBERSHIP)
struct ip_mreq mreq;
mreq.imr_multiaddr = /* 239.192.152.143 */;
mreq.imr_interface = INADDR_ANY;
setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

// Set receive timeout
struct timeval timeout = {1, 0};  // 1 second
setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
```

## Network Configuration

### Firewall Rules

LSD requires the following firewall rules:

**Linux (iptables):**
```bash
# Allow outgoing multicast
iptables -A OUTPUT -d 239.192.152.143 -p udp --dport 6771 -j ACCEPT

# Allow incoming multicast
iptables -A INPUT -s 239.192.152.0/24 -p udp --dport 6771 -j ACCEPT
```

**Windows Firewall:**
```powershell
# Allow LSD multicast
New-NetFirewallRule -DisplayName "BitTorrent LSD" `
    -Direction Inbound -Protocol UDP -LocalPort 6771 -Action Allow
```

### Router Configuration

Most modern routers support multicast by default. For advanced setups:

1. **Enable IGMP** (Internet Group Management Protocol)
2. **Allow multicast forwarding** (if needed between VLANs)
3. **Check IGMP snooping** settings

### Network Topology

LSD works best on:
- **Flat networks** - All clients on same subnet
- **Home/Office LANs** - Typical residential/corporate networks
- **VPN tunnels** - If multicast is forwarded

LSD may not work on:
- **Public networks** - Most block multicast
- **Segmented VLANs** - Without multicast routing
- **Cloud environments** - Often no multicast support

## Performance

### Message Overhead

Each announcement message is ~100 bytes:
```
BT-SEARCH * HTTP/1.1\r\n
Host: 239.192.152.143:6771\r\n
Port: 6881\r\n
Infohash: 0123456789ABCDEF0123456789ABCDEF01234567\r\n
\r\n
```

With 5-minute interval:
- **1 torrent**: ~100 bytes / 5 min = 0.3 bytes/sec
- **10 torrents**: ~1000 bytes / 5 min = 3.3 bytes/sec
- **Negligible overhead** for most use cases

### Discovery Speed

- **First announcement**: Immediate on `announce()`
- **Subsequent announcements**: Every 5 minutes
- **Discovery latency**: < 1 second (local network)

### Scalability

LSD scales well on local networks:
- **10-100 clients**: Excellent performance
- **100-1000 clients**: Good performance (check network capacity)
- **1000+ clients**: May need multicast optimization

## Platform Support

### Windows

- **Windows Vista+** - Full multicast support
- **Winsock 2.2** - Used for socket operations
- **Firewall** - May need to allow UDP 6771

```cpp
#ifdef _WIN32
// Initialize Winsock
WSADATA wsa_data;
WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
```

### Linux

- **Kernel 2.6+** - Full multicast support
- **POSIX sockets** - Standard socket API
- **Firewall** - Configure iptables if needed

### macOS

- **macOS 10.5+** - Native multicast support
- **BSD sockets** - Standard socket API
- **Firewall** - Configure Application Firewall if needed

## Security Considerations

### Message Validation

LSD validates all incoming messages:

```cpp
// Check message starts with cookie
if (message.find(LSD_COOKIE) != 0) {
    return;  // Not an LSD message
}

// Validate info_hash length (must be 20 bytes)
if (info_hash.size() != 20) {
    return;  // Invalid info_hash
}

// Validate port range
if (port == 0 || port > 65535) {
    return;  // Invalid port
}

// Only accept peers for torrents we're interested in
if (!isInterestedInTorrent(info_hash)) {
    return;  // Not our torrent
}
```

### Attack Vectors

**Potential attacks:**
1. **Peer spoofing** - Fake peer announcements
2. **DoS via flood** - Excessive announcements
3. **Info leak** - Revealing active torrents on network

**Mitigations:**
- Only announce torrents we're downloading
- Validate all peer information
- Rate limit announcements (5-minute interval)
- Only trust peers we can connect to
- Don't announce private/sensitive torrents on untrusted networks

### Privacy

LSD reveals:
- **Active torrents** - Info hashes being downloaded
- **Client presence** - IP address and port on local network
- **Torrent interest** - What content you're accessing

**Privacy recommendations:**
- Disable LSD on untrusted networks
- Use VPN if privacy is critical
- Consider info_hash privacy implications

## Troubleshooting

### LSD Not Discovering Peers

1. **Check if LSD is running**
   ```cpp
   if (lsd.isRunning()) {
       std::cout << "LSD is running\n";
   }
   ```

2. **Verify multicast is supported**
   ```bash
   # Linux: Check multicast routes
   ip mroute show

   # Windows: Check multicast groups
   netsh interface ipv4 show joins
   ```

3. **Test multicast connectivity**
   ```bash
   # Send test multicast
   echo "test" | nc -u 239.192.152.143 6771
   ```

4. **Check firewall settings**
   - Allow UDP port 6771
   - Allow multicast group 239.192.152.143

### Firewall Blocking LSD

**Symptoms:**
- No peers discovered
- Announcements not received

**Solutions:**
```bash
# Linux: Check iptables
sudo iptables -L -n -v

# Add rule to allow LSD
sudo iptables -A INPUT -p udp --dport 6771 -j ACCEPT
```

### High CPU Usage

**Cause:** Excessive announcements

**Solution:**
```cpp
// LSD announces every 5 minutes by default
// This is reasonable and shouldn't cause high CPU
// If you see high CPU, check for:
// 1. Too many torrents announced (> 100)
// 2. Network flooding
// 3. Malformed messages
```

### No Peers on Local Network

**Check:**
1. Other clients are running LSD
2. Clients are on same subnet
3. Multicast is not blocked
4. Firewall allows LSD traffic

## Future Enhancements

### Planned Features

1. **IPv6 Support**
   - Multicast group: `ff15::efc0:988f:6771`
   - Dual-stack IPv4/IPv6 announcements
   - See: [IPv6.md](IPv6.md) for details

2. **Announce Optimization**
   - Adaptive announcement intervals
   - Suppress duplicates
   - Smart peer filtering

3. **Statistics Dashboard**
   - Track discovery success rate
   - Monitor network health
   - Peer quality metrics

4. **Cross-VLAN Discovery**
   - Support for segmented networks
   - Multicast routing configuration
   - IGMP snooping optimization

## Testing

### Unit Tests

```cpp
// Test LSD creation
torrent::LSD lsd(6881);
ASSERT_FALSE(lsd.isRunning());

// Test start/stop
lsd.start();
ASSERT_TRUE(lsd.isRunning());
lsd.stop();
ASSERT_FALSE(lsd.isRunning());

// Test announcement
std::vector<uint8_t> info_hash(20, 0xAB);
lsd.announce(info_hash);
ASSERT_EQ(lsd.getAnnouncedTorrentsCount(), 1);

lsd.stopAnnounce(info_hash);
ASSERT_EQ(lsd.getAnnouncedTorrentsCount(), 0);
```

### Integration Tests

```cpp
// Test peer discovery between two LSD instances
torrent::LSD lsd1(6881);
torrent::LSD lsd2(6882);

std::vector<torrent::LSDPeer> discovered_peers;

lsd2.setPeerCallback([&](const std::vector<torrent::LSDPeer>& peers) {
    discovered_peers = peers;
});

lsd1.start();
lsd2.start();

std::vector<uint8_t> info_hash(20, 0xCD);
lsd1.announce(info_hash);
lsd2.announce(info_hash);

// Wait for discovery
std::this_thread::sleep_for(std::chrono::seconds(2));

ASSERT_GT(discovered_peers.size(), 0);
ASSERT_EQ(discovered_peers[0].port, 6881);
```

## Best Practices

### When to Use LSD

**Use LSD when:**
- Distributing files on a local network
- Internal company file sharing
- LAN parties / gaming events
- Reducing internet bandwidth
- Network has multicast support

**Avoid LSD when:**
- On public/untrusted networks
- Privacy is critical
- Network blocks multicast
- Internet-only distribution

### Configuration Tips

```cpp
// For maximum peer discovery, enable all mechanisms:
config.enable_dht = true;   // DHT for internet-wide discovery
config.enable_pex = true;   // PEX for peer exchange
config.enable_lsd = true;   // LSD for local network discovery

// For local-only distribution:
config.enable_dht = false;
config.enable_pex = false;
config.enable_lsd = true;   // Only use LSD
```

### Debugging

Enable logging to debug LSD:

```cpp
// Set log level to debug
Logger::setLevel(LogLevel::DEBUG);

// LSD will log:
// - Multicast socket creation
// - Join/leave multicast group
// - Sent announcements
// - Received messages
// - Discovered peers
```

## References

- [BEP 14: Local Service Discovery](http://www.bittorrent.org/beps/bep_0014.html)
- [RFC 1112: IGMP](https://tools.ietf.org/html/rfc1112)
- [RFC 2365: Administratively Scoped IP Multicast](https://tools.ietf.org/html/rfc2365)
- [IANA Multicast Addresses](https://www.iana.org/assignments/multicast-addresses/multicast-addresses.xhtml)

## Conclusion

LSD provides efficient peer discovery on local networks:

- ✅ Zero external dependencies
- ✅ Fast discovery (< 1 second)
- ✅ Low overhead (< 5 bytes/sec per torrent)
- ✅ Multicast-based communication
- ✅ Thread-safe implementation
- ✅ Simple API
- ✅ Integrated with DownloadManager

LSD complements DHT and PEX to provide a comprehensive peer discovery system for BitTorrent clients.
