# Roadmap

Last updated: 2026-11-13.

The main application is the native Windows desktop client. The CLI and automated
fixtures use the same engine. Follow [architecture](architecture.md) when adding
code; prefer small concrete modules and keep network work off the window thread.

## Delivered

- [x] Module structure with explicit CMake dependencies.
- [x] TCP downloads with SHA1 verification, multi-file storage and resume.
- [x] Multi-peer scheduling and bounded endgame requests with cancellation.
- [x] Tracker-based magnet metadata acquisition with hash verification and fallback.
- [x] File selection and startup priorities, including shared boundary pieces.
- [x] Live file priorities and selection with cancellation, cached-data reuse and queue ownership checks.
- [x] Native Windows interface with background loading and responsive shutdown.
- [x] Multiple torrents with FIFO queue, concurrency limits and file conflict handling.
- [x] Saved download list, destinations, file choices and per-torrent settings.
- [x] Stop and continue individual jobs using saved verified pieces.
- [x] Live per-torrent upload and download limits.
- [x] Saved global download/upload budgets shared by all desktop sessions with live updates.
- [x] Tracker-tier fallback for HTTP/HTTPS and IPv4 UDP with current-URL diagnostics and bounded final notifications.
- [x] Optional seeding with ratio and time limits.
- [x] Incoming IPv4/IPv6 TCP peers with handshake deadlines, a peer cap and verified uploads.
- [x] Live peer connections, transfer totals, tracker responses/errors and waiting hints in the desktop.
- [x] Local listening-port diagnostics, incoming handshake evidence, contextual advice and a copyable report.
- [x] Opt-in NAT-PMP/UPnP finite port mappings with renewal, ownership checks, cleanup and diagnostics.
- [x] Saved network settings for the first port, per-torrent peer cap, DHT and PEX, applied on the next session.
- [x] Offline tracker/peer regression tests for transfers and application lifecycle.

Protocol modules also exist for UDP trackers, PEX, local discovery, web
seeds, MSE and optional uTP. Their presence does not establish interoperability
coverage. The ordinary suite currently has nineteen unit suites and 148
offline integration scenarios, plus a certificate check when the OpenSSL tool
is available. See [tests](../tests/README.md) for verification.

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

This optional suite covers local plaintext TCP and tracker-based discovery.
Public-swarm evidence is recorded separately below. Encryption and metadata
serving by our client require separate verification.

## Verified: trackerless magnets

- [x] Add deterministic DHT-only discovery and magnet download checks.
- [x] Verify iterative lookup, announce, unavailable-node fallback and cancellation.
- [x] Check interoperability with an independent DHT implementation.

Reference: bittorrent-dht 11.0.12 on a private loopback DHT. A separate check
downloads metadata and payload from qBittorrent through this DHT without a
tracker in the magnet. Public DHT metadata resolution is checked below.

## Verified: public-swarm smoke checks

- [x] Download an official image from public peers with web seeds disabled.
- [x] Stop, independently verify saved pieces and resume to completion.
- [x] Match every final piece hash and the official whole-image SHA256.
- [x] Resolve public magnet metadata through both a tracker and trackerless DHT.
- [x] Keep time-limited manual runs and their reports separate from CTest.

On 2026-11-11, Debian 13.7.0 armhf netinst completed all 939,831,296 bytes after
stopping at 796 saved pieces. All 3,586 piece hashes and the official image
SHA256 matched. Trackerless metadata resolution used the public IPv4 DHT.
See [repeatable procedure and evidence](PUBLIC_SWARMS.md).

This covers one public swarm on the current Windows machine. Longer operation,
encrypted/uTP transfers and other network environments still need validation.

## Desktop distribution

- [x] Produce a portable release archive containing the executable and required DLLs.
- [x] Verify startup, download and upload in a clean destination folder with only system directories on PATH.
- [x] Check package integrity and reject untrusted HTTPS tracker certificates.
- [ ] Verify the archive on a fresh Windows installation.
- [ ] Add a Windows installer and release automation when portable builds are reliable.

## Later improvements

- Verify public port reachability from another network; local socket diagnostics are already available.
- Check port mapping compatibility on physical routers and across upstream NAT/firewall configurations.
- Serve verified magnet metadata to peers.
- Richer per-tracker history and peer statistics beyond the current connection snapshots.
- Windows and POSIX CI plus broader protocol and encryption checks.
- Profile disk I/O and memory usage before adding caches or tuning performance.

Torrent creation, watch folders and remote management are separate future
features. They are not prerequisites for the desktop transfer workflow.
