# Tests

Automated tests live in `unit/` and `integration/`. Manual network diagnostics
live in `../tools/` and are not registered as tests.

## Build and run

```sh
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For Visual Studio, build with `--config Release` and run CTest with `-C Release`.
Google Test is found as an installed package or fetched during configuration.
Node.js is optional; CMake registers the integration test when it finds `node`.

The CLI parser suite and desktop-controller fixtures that use CLI arguments link
`torrent_cli`. Queue/task fixtures link `torrent_app`; protocol suites link the
narrow engine module they exercise. The actual Windows window remains covered by
the portable GUI fixtures after changes to layout/actions/rendering or test hooks.

## Suites

| Executable | Scope | Linked module |
| --- | --- | --- |
| `test_bencode` | Bencode parsing and round trips | Metainfo |
| `test_torrent_file` | Single/multi-file metadata and info hashes | Metainfo |
| `test_piece_manager` | Selection, block assembly, hash failures and resume state | Storage |
| `test_file_manager` | File initialization, piece boundaries and concurrent writes | Storage |
| `test_command_line` | Help, overrides, invalid numbers and overflow rejection | Application |
| `test_extension_protocol` | Payload framing, ID dispatch and extension disable updates | Network |
| `test_metadata_exchange` | Binary assembly, size validation, rejects and callback locking | Network |
| `test_metadata_responder` | Exact blocks, rejects, per-peer budgets and malformed requests | Network |
| `test_file_selection` | File-to-piece masks, boundaries, empty files and invalid metadata | Metainfo |
| `test_download_task` | Background task state and cancellation | Application |
| `test_download_queue` | Scheduling, settings, persistence and file conflicts | Application |
| `test_speed_limits` | UI rate parsing and exact byte conversion | Application |
| `test_network_diagnostics` | Connection advice and report evidence | Application |
| `test_tracker_diagnostics` | Manual-update availability, timestamps and history report redaction | Application |
| `test_port_mapper` | Disabled defaults and bounded finite-lease options | Network |
| `test_token_bucket` | Throttling, live changes and cancellation | Core |
| `test_seeding_options` | Ratio/time limits and policy validation | Application |
| `test_peer_listener` | Port ownership and bitfield validation | Network |
| `test_dht` | UDP lifecycle, KRPC validation, unique transactions and bootstrap configuration | Network |
| `test_download` | Actual CLI download from a loopback tracker and TCP peer | CLI executable |
| `test_transfer_multi` | Two contributing seeds without duplicate block requests | CLI executable |
| `test_transfer_resume` | Stop a partial session and retain verified pieces after restart | Session + CLI |
| `test_transfer_corrupt` | Detect and redownload corrupted saved data | Session + CLI |
| `test_transfer_disconnect` | Recover after a seed disconnects mid-piece | CLI executable |
| `test_transfer_multifile` | Piece writes crossing files and a nested directory | CLI executable |
| `test_magnet_download` | Two-block metadata followed by file downloading | CLI executable |
| `test_magnet_reject` | Fallback after a metadata reject | CLI executable |
| `test_magnet_hash` | Reject wrong-hash metadata and try another peer | CLI executable |
| `test_magnet_tracker` | Fall back from an unavailable tracker | CLI executable |
| `test_magnet_invalid` | Invalid metadata never starts a file download | CLI executable |
| `test_magnet_stall` | Fall back from an idle metadata peer after its deadline | CLI executable |
| `test_magnet_list` | Acquire metadata and list files without payload requests | CLI executable |
| `test_magnet_fast` | Fast peer receives one HAVE_NONE before metadata negotiation | CLI executable |
| `test_endgame_cancel` | Rescue a stalled block and send a matching CANCEL | CLI executable |
| `test_endgame_late` | Ignore a late cancelled response without corrupting assembly | CLI executable |
| `test_endgame_cap` | Limit concurrent block copies with three connected seeds | CLI executable |
| `test_endgame_disconnect` | Retain valid partial data when a shared owner disconnects | CLI executable |
| `test_selection_list` | List IDs, sizes and paths without tracker or payload activity | CLI executable |
| `test_selection_single` | Download a middle file crossing skipped-file boundaries | CLI executable |
| `test_selection_multiple` | Combine disjoint files and skip unrelated pieces | CLI executable |
| `test_selection_zero` | Create a selected empty file without network activity | CLI executable |
| `test_selection_resume` | Reuse verified selected pieces after restarting | CLI executable |
| `test_selection_corrupt` | Detect modified selected bytes despite a valid boundary cache | CLI executable |
| `test_selection_changed` | Change selection and reuse previously verified data | CLI executable |
| `test_selection_invalid` | Reject out-of-range IDs before payload initialization | CLI executable |
| `test_selection_preserve` | Leave an existing skipped file unchanged across resume | CLI executable |
| `test_priority_order` | High then normal then low requests with sequential tie-breaking | CLI executable |
| `test_priority_default` | Priority order through random-first and rarest-first | CLI executable |
| `test_priority_boundary` | High-priority tail before lower-priority boundary pieces | CLI executable |
| `test_priority_allskip` | Skip everything without payload files or network activity | CLI executable |
| `test_priority_invalid` | Reject nonexistent priority file IDs before initialization | CLI executable |
| `test_priority_outside` | Reject priorities that expand an explicit file selection | CLI executable |
| `test_priority_resume` | Change startup priorities and reuse verified data | CLI executable |
| `test_priority_list` | Preview effective priorities without payload activity | CLI executable |

Each unit suite has a 30-second timeout. The integration test has a 55-second
CTest timeout and kills the child client after 45 seconds if it does not exit.
Each transfer scenario has a 40-second CTest timeout and a 15-second child
process timeout. Its seeds leave idle sockets open to check receive cancellation
during shutdown.
Magnet scenarios use a 20-second child-process timeout and a 30-second CTest
timeout. Each TCP metadata peer attempt has a 10-second deadline.
Endgame checks have an 8-second child-process timeout and a 15-second CTest
timeout. Their slow seed never supplies its held blocks normally, so completing
the file requires an endgame request to another seed.
Selection fixtures have a 12-second child timeout and 30-second CTest timeout.
Artifacts remain under `build/tests/selection-*`.
Priority fixtures use the same 12-second child/30-second CTest timeouts and
retain artifacts under `build/tests/priority-*`.

The integration test downloads two pieces split into multiple 16-KiB blocks,
checks the info hash in the handshake, validates block requests and compares
the output with the original 50,003-byte binary payload. It retains the client
output and files under `build/tests/download-test-*` for troubleshooting.

The transfer scenarios verify 229,499 bytes spanning eight pieces. They retain
artifacts under `build/tests/transfer-*`. Resume tests first run the
`partial_download` helper, which uses the production session API to stop after
a verified piece. They then start the ordinary CLI using the same files and
resume state. Valid saved pieces must not be requested again; deliberately
corrupted saved pieces must be downloaded again. The tracker also checks the
remaining byte count on the resumed announce.

Piece manager regressions check malformed and duplicate block rejection,
concurrent completion and discarding incomplete data for reassignment.

Magnet fixtures announce an extension ID different from the client's local ID,
send metadata in two blocks, and include strings containing Bencode delimiters.
An independent decoder checks the actual wire frames. Corrupt metadata remains
valid Bencode but has a wrong info hash, checking verification separately from
decoding. Logs remain in `build/tests/magnet-*`. Run these cases with
`ctest --test-dir build -R test_magnet --output-on-failure`.

`test_metadata_serving_*` checks the reverse transfer through the desktop queue
controller. It serves original, deliberately unsorted `info` bytes with an unknown
binary field in multiple blocks, then compares the full bytes and SHA1. Incoming
requests also work while the payload is incomplete; outgoing sessions cover a
seed, and a magnet input first acquires metadata/payload from a separate fixture
peer before serving it. Other cases verify malformed requests and invalid indices,
unknown message IDs, per-peer reply budgets, ID remapping/disable/re-enable,
budget preservation across renegotiation and cancellation during upload throttling.
Peers without BEP 10 receive ordinary payload messages without extended messages.
PEX and all public discovery are disabled. Run with
`ctest --test-dir build -R test_metadata_serving --output-on-failure`;
artifacts remain in `build/tests/metadata-serving-*`.

Endgame fixtures check exact `CANCEL` index/offset/length fields and compare the
finished 229,499-byte payload. The late-response case sends corrupt data after
`CANCEL` while another block remains pending; it must neither enter the piece
assembly nor inflate downloaded statistics. The disconnect case verifies that
an already received block is retained while another peer owns the missing one.
Logs remain under `build/tests/endgame-*`. Run these cases with
`ctest --test-dir build -R test_endgame --output-on-failure`.

Run individual suites with `ctest --test-dir build -R test_command_line -V` or
`ctest --test-dir build -R test_download --output-on-failure`.
Run transfer scenarios with `ctest --test-dir build -R test_transfer --output-on-failure`.

## Adding tests

Priority integration checks inspect the order of actual wire requests from one
peer. They verify final file bytes, skipped paths, tracker totals, boundary
pieces and restarting with changed priorities. Unit tests check maximum-overlap
priority mapping and each strategy's tie-break behavior. Live session changes
have a separate suite described below.

```sh
ctest --test-dir build -R test_priority --output-on-failure
```

Add `unit/test_<module>.cpp` and register it with `add_unit_test` in
`tests/CMakeLists.txt`, linking the narrowest module that provides the behavior.
Use Google Test assertions to check observable behavior and failure cases.
Integration checks belong under `integration/` and should not depend on public
trackers or external swarms.

The suite does not yet establish interoperability with public swarms,
encryption, optional libutp or all
cross-platform socket paths.

Queue scenarios also cover concurrency, file conflicts, removal, upload/download
limits, live changes and seeding policies. Incoming scenarios cover both transfer
directions, invalid requests, missing/corrupt data, peer caps, stalled handshakes,
IPv6, occupied ports and immediate restart. `test_incoming_fast` checks that a
complete incoming Fast connection receives HAVE_ALL exactly once. Existing queue
upload scenarios require an explicit UNCHOKE before sending upload requests.

## Independent client interoperability

These opt-in Windows checks require a separately installed qBittorrent 4.6.x.
They are not enabled or downloaded automatically. The verified reference is
qBittorrent 4.6.2 with libtorrent 1.2.19. Other versions need their own validation.

```powershell
cmake -S . -B build -DINTEROP_QBITTORRENT_EXECUTABLE="C:/Program Files/qBittorrent/qbittorrent.exe"
cmake --build build --parallel
ctest --test-dir build -L interop --output-on-failure
```

Unset the CMake variable to remove these checks. Ordinary fixtures continue to
run without an external client. Tests are serial and use a fresh profile under
`build/tests/interop-<scenario>-*`, distinct ports, a loopback-only API and local
tracker. DHT, PEX, LSD, encryption, uTP, GeoIP lookup, update checks and port mapping
are disabled. On Windows the GUI starts hidden and exits through its API.

| Scenario | Evidence |
| --- | --- |
| `download` | Our client downloads a multi-file torrent from qBittorrent |
| `upload-incoming` | qBittorrent initiates a TCP connection and downloads our verified pieces |
| `upload-outgoing` | Our seed connects to qBittorrent and uploads the full payload |
| `magnet` | Our client acquires metadata from qBittorrent before downloading files |
| `metadata-incoming` | qBittorrent starts with a magnet and obtains our metadata/files through its TCP connection |
| `metadata-outgoing` | Our seed connects to qBittorrent and serves its magnet metadata followed by files |
| `resume` | A partial desktop-controller session stops and the CLI resumes its verified pieces |

Each scenario checks 1,048,713 payload bytes and all piece hashes, including a
piece spanning two files. Resume additionally checks the stopped and restarted
tracker `left` values. Metadata-serving scenarios use more than 16 KiB of metadata
and a fresh qBittorrent profile without the `.torrent` input. Artifacts retain engine output, tracker announces,
qBittorrent's own logs and a `result.json` recording the reference build.

The fixture uses the documented [WebUI API](https://github.com/qbittorrent/qBittorrent/wiki/WebUI-API-%28qBittorrent-4.1%29)
and a separate [portable profile](https://github.com/qbittorrent/qBittorrent/wiki/How-to-use-portable-mode).
It establishes loopback plaintext TCP compatibility with that reference build;
public-swarm and encrypted transfers remain separate work.

## DHT interoperability

The ordinary suite does not require npm dependencies. It runs local UDP fixtures
for iterative lookup through another node, unavailable routers, KRPC errors,
forged replies, late responses, deduplication, token-based announces, occupied
ports and cancellation. `test_magnet_dht*` covers trackerless metadata/payload
downloads, an unavailable tracker, disabled DHT and a trackerless `.torrent` file.

To add checks against the independent `bittorrent-dht` 11.0.12 implementation:

```sh
cd tests/integration
npm ci --ignore-scripts
cd ../..
cmake -S . -B build -DENABLE_DHT_INTEROP_TESTS=ON
cmake --build build --parallel
ctest --test-dir build -R "test_dht|test_magnet_dht" --output-on-failure
```

The pinned dependency and its lockfile belong only to the integration tests.
Configuration never downloads it automatically. Two independent loopback DHT
nodes publish and serve a peer address with public bootstrap disabled.
`test_dht_reference` verifies our lookup and announce against them. When
`INTEROP_QBITTORRENT_EXECUTABLE` is also set, `test_interop_dht` acquires metadata
and a full multi-file payload from qBittorrent using a magnet without a tracker.
It rejects any tracker announce by our client; qBittorrent retains its local
fixture tracker and has its own DHT/PEX/LSD disabled.

This uses the reference library's public [DHT API](https://github.com/webtorrent/bittorrent-dht#api).
Both independent-client options enabled yield 195 CTest suites/scenarios, or 196
when the OpenSSL executable is available: 186 ordinary checks, an optional local
TLS certificate check and nine interoperability checks. Test artifacts stay
under `build/verify/tests/dht-*`, `magnet-*` or `build/tests/interop-*`, depending
on the build directory. No public tracker or DHT router participates in these tests.

TCP fixtures keep exclusive port-range leases until exit, in addition to probing
the TCP listener and adjacent DHT UDP ports on all IPv4 interfaces before starting
a child. This catches wildcard listeners owned by other applications as well as
loopback conflicts. Leases prevent parallel fixtures from claiming
the next torrent's port between that probe and its listener startup.

## Live network diagnostics

`test_network_status_*` checks the desktop controller's synchronized snapshots
against local HTTP/TCP/UDP fixtures. The transfer case observes outgoing and
incoming handshakes, choke state, pending requests, actual downloaded/uploaded
bytes, failed connections and removal after disconnect. It then stops and
resumes the verified payload. Other cases cover an initial tracker response
that is still pending, tracker errors and recovery, an empty peer list,
trackerless sessions and DHT node counts. Stopping retains the useful tracker
error; reloading clears it. These tests require no public network.

```sh
ctest --test-dir build -R test_network_status --output-on-failure
```

The native-window smoke checks also open and close the network-details dialog
after a completed transfer. They continue using isolated local fixtures.

## Saved network settings

`test_network_settings` validates integer input, port and peer-limit boundaries,
port-pair capacity and preservation of unrelated configuration values. Queue unit
checks verify that defaults apply on restart and to new jobs, invalid settings
leave defaults unchanged, and insufficient saved port ranges fail before restoring
any entries. `test_download_task` also verifies that a failed staged preferences
write preserves the previous Unicode-named config.

`test_network_settings_apply` keeps one transfer running while changing defaults,
starts another with the new port/DHT/PEX/peer cap, verifies its PEX handshake and
enforced incoming connection cap, then resumes and restores both jobs. It compares
the final payloads and checks that restoring the list does not start downloads.
`test_network_settings_magnet` loads metadata on a local DHT, changes its port and
later disables DHT before starting again. Real UDP probes verify that old metadata
sockets are released and that disabled DHT does not bind a replacement socket.
Both scenarios save and read preferences using the ordinary Config API.

```sh
ctest --test-dir build -R "test_network_settings|test_download_queue|test_download_task" --output-on-failure
```

Native-window smoke checks open and save the network-settings dialog before
starting downloads. Their fixtures keep public discovery disabled and leave
the user's preferences untouched.

## Live file priorities

`test_live_selection` verifies cached-span restoration, boundary preservation,
zero-byte files, excluding/re-enabling files without deleting data or copying
whole files to the cache, failed file activation and corrupt-data rejection.
It also checks that skipped unverified assemblies can be selected again while
verified pieces remain complete.

`test_live_priorities_*` uses local peers and actual wire requests to verify:

- high/normal/low order changes without recreating connections;
- CANCEL messages, late replies and re-enabling a skipped file;
- cached boundary reuse, saved choices and stop/resume without re-requesting verified pieces;
- rejecting active file conflicts and retaining ownership until stop;
- cancellation of duplicate endgame requests and resuming after skipping everything;
- abandoning a skipped block waiting at a 1 B/s limit and requesting wanted data.

```sh
ctest --test-dir build -R "test_live_priorities|test_live_selection" --output-on-failure
```

The manual Windows `ui` scenario drives the real priority controls during a
transfer, then checks request order, final selected bytes and an untouched skipped
file. Portable verification runs it from the extracted Release archive with a
system-only PATH:

```powershell
node tests/integration/test_live_priorities.cjs build/desktop/TorrentClient.exe ui
```

## Router port mapping

`test_port_mapping_*` uses a loopback NAT-PMP responder, SSDP responder, HTTP IGD
description and SOAP endpoint. Sixteen scenarios cover disabled defaults, TCP/DHT
UDP leases, renewal, alternate external ports, tracker/DHT advertisement, expiry
during an unanswered renewal, partial success, concurrent torrents, live toggling,
restoring idle entries, unsupported routers, lost create replies, cancellation,
foreign/replaced UPnP ownership, permanent-only devices and foreign control URLs.
The UPnP case verifies the local socket remains reserved during deletion. The
unsupported-router case completes and checks payload bytes. All protocol frames
and owned rules are recorded under `build/tests/port-mapping-*`.

The driver accepts only loopback overrides. The ordinary desktop smoke modes
explicitly clear inherited opt-in before loading fixture preferences. The portable
check runs a separate native-window mapping case with loopback ports fixed in its
test mode. It checks the actual settings checkbox, saved opt-in, diagnostics,
payload and cleanup. It never queries or changes the machine's real router.

```sh
ctest --test-dir build -R "test_port_mapping|test_port_mapper|test_network_settings|test_network_diagnostics" --output-on-failure
```

These checks establish local protocol/lifecycle behavior, not interoperability
with every hardware IGD/NAT-PMP implementation or public reachability. IPv4
default-gateway lookup is implemented for Windows and Linux. Runtime endpoint and
short-lease overrides are C++ fixture/embedding options and are not persisted.

## Connection diagnostics

`test_network_diagnostics` checks advice for lifecycle, port errors, peer permission,
pending blocks, missing discovery, empty tracker replies, connection failures,
HTTP transfers without peers, peer caps and effective speed limits. Reports retain
the distinction between local listening and Internet reachability, and omit tracker
credentials, path and query. The listener unit suite checks the OS listening flag,
address-family reporting and loopback classification including mapped IPv6.

The network-status integration cases additionally verify a port held by another
local listener, copied startup failures, successful incoming/outgoing handshakes,
loopback/mapped-IPv6/native-IPv6 evidence, rejection at the peer cap, failed
handshakes and retained results after disconnect/stop. Real IPv6 is exercised when
the listener is dual stack; IPv4 fallback is reported as such. No external port
checker or synthetic connection is used by the application's diagnostics.

```sh
ctest --test-dir build -R "test_network_diagnostics|test_network_status|test_peer_listener" --output-on-failure
```

Portable verification runs `test_download.cjs --desktop-network-ui`, opens the
real diagnosis during an active download and again after completion, verifies the
new fields render and checks final payload bytes. It leaves the user's clipboard
untouched; report content is tested through the shared application function.

## Tracker fallback

`test_tracker_history_*` exercises copied per-address history and the update
mailbox through a real background task. Nine scenarios verify independent
primary/backup results, untried rows, manual request coalescing, peer discovery
after an update, HTTP/UDP minimum intervals, cancellation during HTTP/UDP I/O,
the 16-event cap, final lifecycle notifications, restart/reload resets and invalid
minimum-interval fallback. A long Cyrillic error checks the 1024-byte UTF-8 bound.
Status reads and rejected manual requests must stay responsive during a pending
announce. Artifacts remain in `build/tests/tracker-history-*`.

The portable check also runs `test_download.cjs --desktop-trackers-ui`. It opens
the real tracker dialog, verifies displayed rows/history, invokes the update
button after its minimum interval and checks the exact wire sequence plus final
payload bytes. No clipboard is changed. Run history/controller checks with
`ctest --test-dir build -R "test_tracker_history|test_tracker_diagnostics" --output-on-failure`.

`test_torrent_file` covers tier boundaries, legacy precedence/fallback,
list-only torrents, duplicate/empty entries and ordered magnet trackers.
`test_tracker_fallback_*` uses the production task/session with isolated local
HTTP and UDP trackers. Eighteen scenarios cover failure reasons, HTTP errors,
invalid/empty bodies, timeouts, list-only metadata, UDP fallback in both directions,
promotion within a tier, interval handling, empty peer responses, switching during
a transfer, higher-tier recovery, all-failed backoff, cancellation in both
transports, completion timeouts and bounded final notifications. A 32-cycle
start/stop scenario checks that service workers wake without waiting for their
normal timers. Transfer cases
compare exact payload bytes; wire events distinguish registered trackers from
untried/unavailable backups. Artifact directories are `build/tests/tracker-fallback-*`.

```sh
ctest --test-dir build -R "test_tracker_fallback|test_torrent_file|test_network_status|test_tracker_tls" --output-on-failure
```

Portable verification additionally runs the native window through
`test_download.cjs --desktop-fallback-ui`: its first tracker returns HTTP 503,
the backup supplies the peer, and the fixture verifies final bytes and lifecycle
events. The window opens its network-details dialog after completion.

## Global speed limits

`test_token_bucket` checks combined local/shared budgets, independent session
buckets, cancellation, live rate removal and a slow local bucket leaving shared
capacity available. Queue checks preserve per-torrent caps, reject negative
limits, round-trip global preferences at byte precision and reject malformed,
fractional or overflowing JSON values. Older preferences default to unlimited.

`test_queue_global_*` runs two local torrents concurrently and verifies aggregate
download/upload timings, individual caps alongside a global cap, live removal,
stop/resume while waiting at 1 B/s and uploading from two complete seeds. Each
scenario compares payload bytes. The reference peers inspect actual upload frames;
download timing measures accepted payload through completion, not bytes already
buffered by the network stack. The limiter retains its bounded burst allowance.

```sh
ctest --test-dir build -R "test_queue_global|test_token_bucket|test_download_queue" --output-on-failure
```

The portable check also runs `test_download.cjs` with `--desktop-global-ui`.
It starts with a saved 1 B/s global cap, applies new rates using the real dialog
during a download, and verifies completed bytes and the saved global/individual
settings in the fixture's config. It does not write the user's preferences.

## Portable distribution

`test_tracker_tls` uses the OpenSSL command-line tool to generate a temporary
self-signed certificate. The same valid tracker payload succeeds over HTTP;
HTTPS with that untrusted certificate must fail before sending the request.
It does not modify the user's certificate stores.

`test_portable.cjs` checks a completed Release ZIP and runs the extracted native
window with only system directories on PATH. It verifies actual downloads and
uploads through local fixtures. See [distribution checks](../docs/DISTRIBUTION.md#verify-the-archive).

## Manual public-network checks

`public_swarm_driver` is built with the tests but never registered with CTest.
It runs the desktop controller with a time budget, records progress and stops
cleanly for resume. `verify_public_download.cjs` independently checks saved piece
hashes and an optional official image SHA256.
See [commands and recorded evidence](../docs/PUBLIC_SWARMS.md).
