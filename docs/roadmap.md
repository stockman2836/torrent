# BitTorrent Client - ROADMAP

> Module paths were updated during the structural refactor. Future file suggestions
> are historical; place new code according to [the architecture](architecture.md).

## Project Status

This document tracks the development roadmap for the BitTorrent client.

### Current checkpoint — 2026-10-23

- [x] Structural refactor with explicit module dependencies.
- [x] Offline basic TCP download integration check.
- [x] Multi-peer download without duplicate block requests.
- [x] Resume a partial session without downloading verified pieces again.
- [x] Detect and redownload corrupted saved pieces.
- [x] Recover from a peer disconnecting during a piece.
- [x] Verify writes across multi-file piece boundaries.
- [x] Cancel idle TCP receive waits during shutdown.
- [x] Tracker-based magnet metadata exchange followed by piece downloading.
- [x] Verify metadata hash and fall back after rejects, bad hashes and idle peers.
- [x] Fall back from an unavailable magnet tracker.
- [x] Bounded redundant endgame block requests with CANCEL.
- [x] Ignore late cancelled blocks and retain shared data after disconnects.

Verification: seven unit suites and sixteen offline integration scenarios pass.
The historical completed-feature checklist below describes implementation scope,
not comprehensive interoperability coverage for every extension.

Next: selective file downloading through the CLI. DHT-only magnet coverage,
public swarm checks and UPnP/NAT-PMP remain later milestones.

### Completed Features 

- [x] Bencode parser
- [x] Torrent file parsing
- [x] HTTP & UDP tracker support
- [x] BitTorrent peer protocol
- [x] Piece manager with SHA1 verification
- [x] File manager (single & multi-file torrents)
- [x] Download manager with multi-threading
- [x] Upload & seeding mode
- [x] Bitfield management
- [x] Piece selection strategies
- [x] Speed limiting (download/upload)
- [x] Resume capability
- [x] Error handling & auto-reconnection
- [x] Logging system
- [x] JSON configuration
- [x] DHT (Distributed Hash Table) - BEP 5
- [x] Magnet link support (parsing & metadata exchange)
- [x] Extension Protocol - BEP 10
- [x] Metadata Exchange - BEP 9
- [x] Message Stream Encryption (MSE/RC4)
- [x] Basic unit tests

---

## Phase 2: Network & Performance =%

### 2.1 UPnP/NAT-PMP Port Forwarding

**Priority: High**
**Status: TODO**

Automatic port forwarding for better connectivity.

- [ ] Implement UPnP discovery
- [ ] Implement port mapping (UPnP)
- [ ] Implement NAT-PMP support
- [ ] Add configuration options
- [ ] Add automatic port mapping on startup
- [ ] Test on various routers

**New files:**

- `include/upnp.h`
- `src/upnp.cpp`
- `include/natpmp.h`
- `src/natpmp.cpp`

---

### 2.2 Improved Endgame Mode

**Priority: Medium**
**Status: DONE (offline verification)**

Better piece selection during endgame.

- [x] Implement redundant requests for last missing blocks
- [x] Send matching CANCELs when a block arrives
- [x] Bound requests to two peers per block and wait before duplicating
- [x] Test late responses, disconnects, exact cancellation fields and copy limits

**Files to modify:**

- `src/storage/piece_manager.cpp`
- `src/session/scheduling.cpp`
- `src/session/transfer.cpp`
- `src/session/peers.cpp`

---

### 2.3 Disk Cache System

**Priority: Medium**
**Status: TODO**

Reduce disk I/O with intelligent caching.

- [ ] Design cache architecture
- [ ] Implement read cache
- [ ] Implement write cache with coalescing
- [ ] Add cache statistics
- [ ] Add configuration for cache size
- [ ] Memory-mapped files consideration

**New files:**

- `include/disk_cache.h`
- `src/disk_cache.cpp`

---

### 2.4 Super-Seeding Mode

**Priority: Low**
**Status: TODO**

Efficient seeding for initial distributors.

- [ ] Implement super-seeding algorithm
- [ ] Track piece distribution
- [ ] Add configuration option
- [ ] Test super-seeding effectiveness

**Files to modify:**

- `src/session/download_manager.cpp`
- `include/torrent/session/download_manager.h`

---

### 2.5 Per-Torrent Rate Limiting

**Priority: Medium**
**Status: TODO**

Individual speed limits for each torrent.

- [ ] Add per-torrent speed limiters
- [ ] Implement global vs per-torrent limits
- [ ] Update configuration
- [ ] Add runtime speed adjustment

**Files to modify:**

- `src/session/download_manager.cpp`
- `include/torrent/core/config.h`

---

## Phase 3: Features & Usability <�

### 3.1 Multi-Torrent Support

**Priority: High**
**Status: TODO**

Download multiple torrents simultaneously.

- [ ] Design multi-torrent architecture
- [ ] Create torrent session manager
- [ ] Implement torrent queue
- [ ] Add/remove torrents dynamically
- [ ] Save/restore session state
- [ ] Update CLI for multi-torrent operations

**New files:**

- `include/session.h`
- `src/session.cpp`
- `include/torrent_handle.h`
- `src/torrent_handle.cpp`

---

### 3.2 Selective File Download

**Priority: High**
**Status: TODO**

Choose which files to download from multi-file torrents.

- [ ] Add file selection UI/CLI
- [ ] Implement piece filtering based on file selection
- [ ] Update piece manager for selective download
- [ ] Add file priority system
- [ ] Test with large multi-file torrents

**Files to modify:**

- `src/storage/piece_manager.cpp`
- `src/storage/file_manager.cpp`
- `include/torrent/metainfo/torrent_file.h`

---

### 3.3 File Priority System

**Priority: Medium**
**Status: TODO**

Prioritize important files for download.

- [ ] Add priority levels (skip, low, normal, high)
- [ ] Implement priority-based piece selection
- [ ] Add runtime priority adjustment
- [ ] Update CLI for priority management

**Files to modify:**

- `include/torrent/storage/piece_manager.h`
- `src/storage/piece_manager.cpp`

---

### 3.4 Torrent Creation Tool

**Priority: Medium**
**Status: TODO**

Create .torrent files from local data.

- [ ] Implement piece hashing
- [ ] Generate info dictionary
- [ ] Support single & multi-file torrents
- [ ] Add private flag support
- [ ] Add web seed URL support
- [ ] Create CLI tool for torrent creation

**New files:**

- `include/torrent_creator.h`
- `src/torrent_creator.cpp`
- `tools/make_torrent.cpp`

---

### 3.5 Watch Folder

**Priority: Low**
**Status: TODO**

Auto-add torrents from a folder.

- [ ] Implement directory monitoring
- [ ] Auto-add new .torrent files
- [ ] Auto-add magnet links from text files
- [ ] Add configuration for watch folder path

**New files:**

- `include/watch_folder.h`
- `src/watch_folder.cpp`

---

### 3.6 Web UI / REST API

**Priority: Medium**
**Status: TODO**

Web interface for remote management.

- [ ] Design REST API endpoints
- [ ] Implement HTTP server
- [ ] Add authentication
- [ ] Create basic web UI (HTML/CSS/JS)
- [ ] Add torrent management endpoints
- [ ] Add statistics endpoints

**New files:**

- `include/http_server.h`
- `src/http_server.cpp`
- `web/` directory for static files

---

### 3.7 Pause/Resume Individual Torrents

**Priority: High**
**Status: TODO**

Control individual torrent state.

- [ ] Add pause/resume functionality
- [ ] Save paused state to disk
- [ ] Update CLI commands
- [ ] Test pause/resume during active download

**Files to modify:**

- `src/session/download_manager.cpp`
- `include/torrent/session/download_manager.h`

---

### 3.8 Statistics & Metrics

**Priority: Medium**
**Status: TODO**

Detailed download/upload statistics.

- [ ] Track per-peer statistics
- [ ] Track per-torrent statistics
- [ ] Add session statistics
- [ ] Export statistics to JSON
- [ ] Add historical data tracking
- [ ] Create stats viewer tool

**New files:**

- `include/statistics.h`
- `src/statistics.cpp`

---

## Phase 4: Quality & Testing >�

### 4.1 Expanded Unit Tests

**Priority: High**
**Status: TODO**

Improve test coverage.

- [ ] Add tests for all BEPs
- [ ] Test edge cases in piece selection
- [ ] Test network error scenarios
- [ ] Test DHT operations
- [ ] Test encryption
- [ ] Achieve >80% code coverage

**Files to add:**

- `tests/test_pex.cpp`
- `tests/test_encryption.cpp`
- `tests/test_dht.cpp`
- More test files...

---

### 4.2 Integration Tests

**Priority: Medium**
**Status: TODO**

End-to-end testing.

- [ ] Create test torrents
- [ ] Set up local tracker
- [ ] Test full download cycle
- [ ] Test multi-peer scenarios
- [ ] Test resume functionality
- [ ] Automated integration test suite

**New directory:**

- `tests/integration/`

---

### 4.3 Performance Benchmarks

**Priority: Low**
**Status: TODO**

Measure and optimize performance.

- [ ] Create benchmark suite
- [ ] Measure download speed
- [ ] Measure memory usage
- [ ] Measure CPU usage
- [ ] Compare with other clients
- [ ] Profile and optimize hotspots

**New directory:**

- `benchmarks/`

---

### 4.4 Memory Optimization

**Priority: Medium**
**Status: TODO**

Reduce memory footprint.

- [ ] Profile memory usage
- [ ] Optimize buffer allocations
- [ ] Implement smart pointer usage review
- [ ] Reduce copy operations
- [ ] Add memory pool for frequent allocations

---

### 4.5 Documentation

**Priority: High**
**Status: TODO**

Improve project documentation.

- [ ] Add inline code documentation
- [ ] Create API documentation (Doxygen)
- [ ] Write user guide
- [ ] Add architecture diagrams
- [ ] Document all BEPs implemented
- [ ] Add troubleshooting guide

**New files:**

- `docs/` directory
- `Doxyfile`

---

## Phase 5: Platform & Distribution =�

### 5.1 Cross-Platform Testing

**Priority: High**
**Status: TODO**

Ensure compatibility across platforms.

- [ ] Set up CI/CD (GitHub Actions)
- [ ] Test on Windows 10/11
- [ ] Test on Ubuntu/Debian
- [ ] Test on macOS
- [ ] Fix platform-specific issues
- [ ] Document platform requirements

---

### 5.2 Docker Support

**Priority: Medium**
**Status: TODO**

Containerized deployment.

- [ ] Create Dockerfile
- [ ] Create docker-compose.yml
- [ ] Add volume mounts for downloads
- [ ] Add configuration via environment variables
- [ ] Publish to Docker Hub
- [ ] Add Docker documentation

**New files:**

- `Dockerfile`
- `docker-compose.yml`
- `.dockerignore`

---

### 5.3 Package Distribution

**Priority: Low**
**Status: TODO**

Easy installation across platforms.

- [ ] Create Linux packages (deb, rpm)
- [ ] Create Windows installer (NSIS/WiX)
- [ ] Create macOS package (.dmg)
- [ ] Add to package managers (apt, homebrew, chocolatey)
- [ ] Create release automation

---

### 5.4 Systemd Service (Linux)

**Priority: Low**
**Status: TODO**

Run as system service on Linux.

- [ ] Create systemd unit file
- [ ] Add user/group management
- [ ] Document service installation
- [ ] Test service start/stop/restart

**New files:**

- `torrent-client.service`
- `docs/systemd.md`

---

## Future Ideas =�

These are potential features for future consideration:

- [ ] Streaming mode (watch video while downloading)
- [ ] Encrypted database for torrent metadata
- [ ] Plugin system for extensions
- [ ] Mobile app (remote control)
- [ ] i2p/Tor support for anonymous torrenting
- [ ] Auto-bandwidth management based on network conditions
- [ ] Smart torrent search integration
- [ ] Cloud storage integration (Google Drive, Dropbox)
- [ ] Blockchain-based tracker (experimental)

---

## Contributing

Feel free to pick any task from this roadmap and contribute!

### Priority Legend:

- **High**: Important for core functionality
- **Medium**: Useful enhancements
- **Low**: Nice-to-have features

### Status Legend:

- **TODO**: Not started
- **IN PROGRESS**: Currently being worked on
- **DONE**: Completed (moved to "Completed Features")

---

Last updated: 2025-12-11
