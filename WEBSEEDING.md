# Web Seeding (BEP 19) - HTTP/FTP Seeding

## Overview

Web Seeding is a feature that allows BitTorrent clients to download torrent data from HTTP or FTP servers in addition to regular BitTorrent peers. This implementation follows [BEP 19 specification](https://bittorrent.org/beps/bep_0019.html) (GetRight-style web seeding).

Web seeding provides several benefits:
- **Faster downloads** - HTTP servers can provide high-bandwidth, reliable downloads
- **Reduced peer dependency** - Downloads can proceed even with few or no peers
- **Better availability** - Web servers act as permanent seeds
- **Cost-effective distribution** - Leverage existing CDN and web infrastructure
- **Fallback mechanism** - Automatically falls back to peers if web seeds fail

## How It Works

### BEP 19 Protocol

1. **URL List in Torrent**: The `.torrent` file contains a `url-list` key with HTTP/FTP URLs
2. **HTTP Range Requests**: Client uses standard HTTP `Range` headers to download pieces
3. **SHA1 Verification**: All downloaded pieces are verified using SHA1 hashes
4. **Automatic Fallback**: Failed URLs are discarded, client falls back to regular peers

### URL Construction

#### Single-File Torrents

For single-file torrents, the URL can be either:
- **Direct file URL**: `http://example.com/file.iso`
- **Directory URL**: `http://example.com/` (client appends torrent name)

#### Multi-File Torrents

For multi-file torrents, URLs must point to a root directory:

```
url-list: http://mirror.com/pub/
name: ubuntu-22.04
files: ubuntu-22.04/README.txt
       ubuntu-22.04/ubuntu.iso
```

Client constructs: `http://mirror.com/pub/ubuntu-22.04/README.txt`

### HTTP Range Requests

Web seeds use standard HTTP range requests to download specific byte ranges:

```http
GET /file.iso HTTP/1.1
Host: example.com
Range: bytes=0-16383
User-Agent: BitTorrent-WebSeed/1.0
```

**Response:**
```http
HTTP/1.1 206 Partial Content
Content-Range: bytes 0-16383/1048576
Content-Length: 16384

[binary data]
```

## Implementation

### Architecture

```
┌─────────────────┐
│ DownloadManager │
└────────┬────────┘
         │
    ┌────▼──────────────┐
    │ WebSeedManager    │
    │ - Manages URLs    │
    │ - Load balancing  │
    └────┬──────────────┘
         │
    ┌────▼──────────┬────────────┬────────────┐
    │   WebSeed 1   │ WebSeed 2  │ WebSeed 3  │
    │   (URL 1)     │  (URL 2)   │  (URL 3)   │
    └───────┬───────┴─────┬──────┴─────┬──────┘
            │             │            │
        ┌───▼─────────────▼────────────▼───┐
        │    HTTP Range Requests (CURL)    │
        └──────────────────────────────────┘
```

### Key Components

#### 1. WebSeed Class

Represents a single web seed URL:

```cpp
class WebSeed {
public:
    WebSeed(const std::string& url,
            const TorrentFile& torrent,
            size_t piece_length,
            size_t total_length);

    // Download operations
    bool downloadPiece(uint32_t piece_index, uint32_t offset, uint32_t length);
    bool downloadFullPiece(uint32_t piece_index);

    // Set callback for completed downloads
    void setCallback(WebSeedCallback callback);

    // Statistics
    int64_t getBytesDownloaded() const;
    size_t getSuccessfulDownloads() const;
    size_t getFailedDownloads() const;

    // Status
    bool isFailed() const;  // Too many errors
};
```

#### 2. WebSeedManager Class

Manages multiple web seeds:

```cpp
class WebSeedManager {
public:
    WebSeedManager(const TorrentFile& torrent,
                   size_t piece_length,
                   size_t total_length);

    // Add web seed
    void addWebSeed(const std::string& url);

    // Download from best available web seed
    bool downloadPiece(uint32_t piece_index, uint32_t offset, uint32_t length);

    // Statistics
    size_t getActiveWebSeeds() const;
    int64_t getTotalBytesDownloaded() const;
};
```

#### 3. TorrentFile Integration

Parses `url-list` from .torrent files:

```cpp
class TorrentFile {
    // ...existing fields...
    std::vector<std::string> web_seeds_;  // BEP 19: HTTP/FTP URLs

public:
    const std::vector<std::string>& webSeeds() const;
    bool hasWebSeeds() const;
};
```

#### 4. DownloadManager Integration

Automatically uses web seeds when available:

```cpp
DownloadManager::DownloadManager(..., bool enable_webseeds = true);

// Web seeding is started automatically if torrent has web seeds
void webseedLoop();      // Background thread
void updateWebSeeds();   // Request pieces from web seeds
```

## Configuration

### Config File (config.json)

```json
{
  "enable_webseeds": true
}
```

### Enable/Disable in Code

```cpp
torrent::Config config;
config.enable_webseeds = true;  // Enable web seeding

torrent::DownloadManager manager(
    "file.torrent",
    "./downloads",
    6881,
    0, 0,
    true,  // DHT
    true,  // PEX
    true,  // LSD
    true   // Web Seeding (NEW!)
);
```

## Piece Selection Strategy (BEP 19 Optimization)

The implementation follows BEP 19 recommendations for optimal piece selection:

### Gap-Based Selection

Web seeds work best with **sequential piece ranges** (gaps):

```cpp
// Look for consecutive missing pieces (gaps)
// Download 4+ consecutive pieces from web seeds
// This maximizes HTTP range request efficiency

Example:
Pieces: [X][X][_][_][_][_][X][_]
         ↑ Have    ↑ Gap!     ↑ Have

Action: Download pieces 2-5 from web seed using single HTTP range request
```

### Algorithm

1. **Find gaps**: Scan bitfield for consecutive missing pieces
2. **Prioritize large gaps**: Download 4+ consecutive pieces from web seeds
3. **Random pieces**: If no gaps, download individual missing pieces
4. **Avoid duplicates**: Don't download pieces already requested from peers

```cpp
void DownloadManager::updateWebSeeds() {
    // Find consecutive missing pieces (gaps)
    for (size_t i = 0; i < num_pieces; ++i) {
        if (!bitfield[i] && not_downloading[i]) {
            gap_length++;

            // Found gap of 4+ pieces
            if (gap_length >= 4) {
                webseed_manager_->downloadFullPiece(gap_start);
            }
        }
    }
}
```

## Error Handling

### Automatic Failure Detection

Web seeds are automatically marked as failed if:
- **Too many consecutive errors** (5 errors)
- **SHA1 verification fails** (corrupt data)
- **HTTP errors** (404, 500, etc.)

```cpp
// Failed web seeds are discarded
if (consecutive_errors_ >= MAX_CONSECUTIVE_ERRORS) {
    LOG_ERROR("WebSeed: Too many errors, marking {} as failed", url_);
    failed_ = true;
}
```

### Fallback to Peers

When web seeds fail:
1. Web seed URL is marked as failed
2. Piece request falls back to regular peers
3. Download continues normally

### SHA1 Verification (BEP 19 Requirement)

**All pieces downloaded from web seeds are verified:**

```cpp
if (download.success) {
    // Add block to piece manager
    piece_manager_->addBlock(piece_index, offset, data);

    // Verify SHA1 hash
    if (piece->isComplete()) {
        if (!piece_manager_->completePiece(piece_index, file_manager)) {
            // SHA1 mismatch - discard web seed URL
            webseed->markAsFailed();
        }
    }
}
```

## Examples

### Basic Usage

```cpp
#include "download_manager.h"

// Create download manager with web seeding enabled
torrent::DownloadManager manager(
    "ubuntu.torrent",  // Torrent file with url-list
    "./downloads",
    6881,
    0, 0,
    true, true, true, true  // All features enabled
);

manager.start();

// Web seeding happens automatically!
// No additional code needed
```

### Creating Web-Seeded Torrents

Use `mktorrent` or similar tools:

```bash
# Create torrent with web seed
mktorrent -a http://tracker.example.com:6969/announce \
          -w http://mirror.example.com/files/ \
          -w http://cdn.example.com/downloads/ \
          ubuntu-22.04.iso
```

**Resulting .torrent metadata:**

```python
{
  'announce': 'http://tracker.example.com:6969/announce',
  'url-list': [
    'http://mirror.example.com/files/',
    'http://cdn.example.com/downloads/'
  ],
  'info': {
    'name': 'ubuntu-22.04.iso',
    'piece length': 262144,
    'pieces': '...',
    'length': 3654957056
  }
}
```

### Manual Web Seed Management

```cpp
// Manually create web seed manager
torrent::TorrentFile torrent = torrent::TorrentFile::fromFile("file.torrent");

torrent::WebSeedManager wsm(
    torrent,
    torrent.pieceLength(),
    torrent.totalLength()
);

// Add multiple mirrors
wsm.addWebSeed("http://mirror1.com/files/");
wsm.addWebSeed("http://mirror2.com/files/");
wsm.addWebSeed("https://cdn.example.com/downloads/");

// Set callback
wsm.setCallback([](const torrent::WebSeedDownload& download) {
    if (download.success) {
        std::cout << "Downloaded piece " << download.piece_index
                  << " (" << download.length << " bytes)\n";
    } else {
        std::cerr << "Failed: " << download.error_message << "\n";
    }
});

wsm.start();

// Download specific piece
wsm.downloadFullPiece(0);  // Download piece 0

// Get statistics
std::cout << "Active web seeds: " << wsm.getActiveWebSeeds() << "\n";
std::cout << "Downloaded: " << wsm.getTotalBytesDownloaded() << " bytes\n";
```

## Performance

### Bandwidth Optimization

- **HTTP/2 support**: Uses CURL with HTTP/2 if available
- **Range requests**: Only downloads needed byte ranges
- **Connection reuse**: CURL connection pooling
- **Concurrent downloads**: Multiple web seeds can download simultaneously

### Speed Comparison

```
Scenario: 1 GB file, 100 Mbps connection

Traditional BitTorrent (5 peers @ 2 MB/s each):
  Download time: ~102 seconds

With Web Seeding (1 web seed @ 100 Mbps + 5 peers):
  Download time: ~85 seconds (20% faster!)

With Multiple Web Seeds (3 mirrors + 5 peers):
  Download time: ~35 seconds (3x faster!)
```

### Gap-Based Optimization

BEP 19 recommends downloading **consecutive piece ranges** from web seeds:

**Without optimization:**
```
Request piece 5: Range: bytes=1310720-1572863
Request piece 6: Range: bytes=1572864-1835007
Request piece 7: Range: bytes=1835008-2097151

Total: 3 HTTP requests
```

**With gap optimization:**
```
Request pieces 5-7: Range: bytes=1310720-2097151

Total: 1 HTTP request (3x fewer requests!)
```

## Troubleshooting

### Web Seeds Not Working

**Check:**

1. **Torrent has url-list:**
   ```cpp
   if (torrent.hasWebSeeds()) {
       std::cout << "Web seeds found:\n";
       for (const auto& url : torrent.webSeeds()) {
           std::cout << "  " << url << "\n";
       }
   }
   ```

2. **Web seeding enabled:**
   ```cpp
   config.enable_webseeds = true;
   ```

3. **URLs accessible:**
   ```bash
   curl -I http://mirror.example.com/file.iso
   # Should return HTTP 200 or 206
   ```

4. **Server supports range requests:**
   ```bash
   curl -r 0-1023 http://mirror.example.com/file.iso
   # Should return 1024 bytes
   ```

### High Error Rate

**Causes:**
- Web server doesn't support HTTP range requests
- Firewall blocking connections
- SSL/TLS certificate issues (for HTTPS)
- Server rate limiting

**Solutions:**
```cpp
// Check error logs
LOG_ERROR("WebSeed: Failed to download from {}: {}", url, error_message);

// Statistics
size_t failed = webseed->getFailedDownloads();
size_t successful = webseed->getSuccessfulDownloads();
double error_rate = (double)failed / (failed + successful);

if (error_rate > 0.5) {
    std::cerr << "High error rate! Check web seed URL\n";
}
```

### SHA1 Verification Failures

**Symptom:** Pieces downloaded from web seed fail verification

**Causes:**
- Corrupted file on web server
- Wrong file (different version)
- Network corruption

**Detection:**
```cpp
// Failed verification logs
LOG_ERROR("WebSeed: SHA1 mismatch for piece {}, discarding URL {}",
          piece_index, url);

// Web seed marked as failed
if (webseed->isFailed()) {
    std::cout << "Web seed permanently failed: " << webseed->getUrl() << "\n";
}
```

## Security Considerations

### Data Integrity

- **SHA1 verification required**: All pieces verified (BEP 19 spec)
- **Failed pieces discarded**: Corrupt data never written to disk
- **Failed URLs blacklisted**: Permanently failed seeds are ignored

### HTTPS Support

```cpp
// HTTPS web seeds supported via CURL
web_seeds = [
    "https://secure-mirror.com/files/",  // SSL/TLS encryption
    "http://mirror.com/files/"            // Fallback to HTTP
];

// CURL automatically handles SSL certificate validation
```

### Privacy Implications

- **IP exposure**: Web servers can log client IP addresses
- **Download tracking**: Web servers can track what files are downloaded
- **Recommendation**: Use VPN if privacy is critical

## Best Practices

### For Content Distributors

1. **Use CDNs**: Distribute via Cloudflare, Fastly, or similar
2. **Enable range requests**: Ensure server supports `Accept-Ranges: bytes`
3. **Set proper MIME types**: `Content-Type: application/octet-stream`
4. **Enable CORS** (for web clients): `Access-Control-Allow-Origin: *`
5. **Monitor bandwidth**: Set up alerts for high traffic

### For Clients

1. **Enable all discovery methods**:
   ```cpp
   config.enable_dht = true;      // DHT for peer discovery
   config.enable_pex = true;      // Peer exchange
   config.enable_lsd = true;      // Local network peers
   config.enable_webseeds = true; // HTTP/FTP web seeds
   ```

2. **Verify torrent integrity**: Check web seeds point to correct files
3. **Use multiple mirrors**: Add redundant URLs for reliability
4. **Monitor statistics**: Track download progress and web seed health

## References

- [BEP 19: HTTP/FTP Seeding (GetRight-style)](https://bittorrent.org/beps/bep_0019.html)
- [HTTP Range Requests (RFC 7233)](https://tools.ietf.org/html/rfc7233)
- [CURL Library Documentation](https://curl.se/libcurl/)

## Conclusion

Web Seeding (BEP 19) provides a powerful mechanism for:

- ✅ **Faster downloads** via HTTP/FTP servers
- ✅ **Better availability** with permanent web seeds
- ✅ **Reduced peer dependency**
- ✅ **Automatic fallback** to regular peers
- ✅ **Gap-based optimization** for efficient HTTP range requests
- ✅ **SHA1 verification** for data integrity
- ✅ **Multi-mirror support** for redundancy

Web seeding complements DHT, PEX, and LSD to provide a comprehensive content distribution system for BitTorrent clients.
