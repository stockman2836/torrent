# Roadmap

Last updated: 2026-11-11.

The main application is the native Windows desktop client. The CLI and automated
fixtures use the same engine. Follow [architecture](architecture.md) when adding
code; prefer small concrete modules and keep network work off the window thread.

## Delivered

- [x] Module structure with explicit CMake dependencies.
- [x] TCP downloads with SHA1 verification, multi-file storage and resume.
- [x] Multi-peer scheduling and bounded endgame requests with cancellation.
- [x] Tracker-based magnet metadata acquisition with hash verification and fallback.
- [x] File selection and startup priorities, including shared boundary pieces.
- [x] Native Windows interface with background loading and responsive shutdown.
- [x] Multiple torrents with FIFO queue, concurrency limits and file conflict handling.
- [x] Saved download list, destinations, file choices and per-torrent settings.
- [x] Stop and continue individual jobs using saved verified pieces.
- [x] Live per-torrent upload and download limits.
- [x] Optional seeding with ratio and time limits.
- [x] Incoming IPv4/IPv6 TCP peers with handshake deadlines, a peer cap and verified uploads.
- [x] Offline tracker/peer regression tests for transfers and application lifecycle.

Protocol modules also exist for UDP trackers, PEX, local discovery, web
seeds, MSE and optional uTP. Their presence does not establish interoperability
coverage. The ordinary suite currently has fifteen unit suites and ninety-two
offline integration scenarios. See [tests](../tests/README.md) for verification.

## Verified: independent client interoperability

Use a separately installed qBittorrent with a disposable profile, loopback peers
and a local tracker. Compare final payload bytes and piece hashes.

- [x] Download files from qBittorrent.
- [x] Upload to qBittorrent through incoming and outgoing TCP connections.
- [x] Resolve a magnet's metadata from qBittorrent and download its files.
- [x] Stop a partial download and reuse verified pieces after restarting.
- [x] Keep these checks reproducible as optional integration tests.

Reference: qBittorrent 4.6.2 with libtorrent 1.2.19 on Windows. All five
interoperability scenarios pass. The tests are opt-in and do not replace the
ordinary offline regression suite.

This stage covers plaintext TCP and tracker-based discovery. Public swarms,
encryption and metadata serving by our client require separate evidence.

## Verified: trackerless magnets

- [x] Add deterministic DHT-only discovery and magnet download checks.
- [x] Verify iterative lookup, announce, unavailable-node fallback and cancellation.
- [x] Check interoperability with an independent DHT implementation.

Reference: bittorrent-dht 11.0.12 on a private loopback DHT. A separate check
downloads metadata and payload from qBittorrent through this DHT without a
tracker in the magnet. Public DHT/swarm behavior remains to be validated.

## Next: desktop distribution

- [ ] Produce a portable release archive containing the executable and required DLLs.
- [ ] Verify startup and download in a clean destination folder.
- [ ] Add a Windows installer and release automation when portable builds are reliable.

## Later improvements

- Network settings and useful port/connectivity diagnostics.
- Optional UPnP/NAT-PMP port mapping with explicit user controls.
- Serve verified magnet metadata to peers.
- Change file priorities during an active download.
- Global speed limits in addition to the existing per-torrent limits.
- Better peer/tracker statistics and diagnostics in the desktop interface.
- Windows and POSIX CI plus broader protocol and encryption checks.
- Profile disk I/O and memory usage before adding caches or tuning performance.

Torrent creation, watch folders and remote management are separate future
features. They are not prerequisites for the desktop transfer workflow.
