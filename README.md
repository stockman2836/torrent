# BitTorrent Client

A C++17 BitTorrent client with a native Windows desktop application and CLI. Supports torrent metadata, HTTP/UDP
trackers, TCP peer transfers, piece verification, multi-file storage and resume
state and selective file downloads. Additional modules implement DHT, magnet metadata exchange, PEX, MSE,
Fast Extension, IPv6, local discovery, web seeds and optional uTP.

Automated integration tests cover TCP transfers, tracker-based magnets and the
desktop controller. Optional checks exchange files with an independently
installed qBittorrent. Other protocols still require broader interoperability testing.

## Project structure

```text
app/                     Shared background controller, CLI and input resolution
desktop/                 Native Windows interface without a console window
include/torrent/         Public headers organized by module
  core/                  Configuration, logging, crypto and shared helpers
  metainfo/              Bencode, torrent metadata, magnet URIs and block types
  storage/               Piece assembly, file I/O and resume state
  network/               Peer protocol, trackers, DHT, discovery and transports
  session/               Download coordination and metadata acquisition
src/                     Implementations mirroring the public module layout
cmake/                   Dependency discovery
config/examples/         Full and minimal example configurations
examples/                Standalone API examples (optional build)
tools/                   Manual network diagnostics (optional build)
tests/unit/              Google Test suites
tests/integration/       Offline tracker/peer download test
docs/architecture.md     Module boundaries and development rules
docs/protocols/          Protocol-specific notes
docs/roadmap.md          Future work
```

The application and tests use the same libraries. Storage and networking are
independent modules; the session coordinates them. See
[architecture](docs/architecture.md) for the dependency diagram and ownership
rules.

## Requirements

- CMake 3.16+ and a C++17 compiler (MSVC, GCC or Clang).
- OpenSSL, libcurl and platform thread/socket libraries.
- spdlog, nlohmann/json and Google Test: installed CMake packages are preferred;
  missing packages are fetched at the pinned versions during configuration.
- Node.js is optional and enables the offline end-to-end test.
- libutp is optional and required only with `ENABLE_UTP=ON`.

On Windows, dependencies can be installed through vcpkg:

```powershell
vcpkg install openssl curl spdlog nlohmann-json gtest
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Linux/macOS, install OpenSSL and libcurl development packages before building:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use `CMAKE_PREFIX_PATH` for dependencies installed in a custom prefix. On Windows
with MinGW/Ninja, prefer an ASCII checkout path when the toolchain has trouble
with non-ASCII dependency paths.

## Build options

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_TESTS` | `ON` | Unit tests and optional Node.js integration test |
| `BUILD_EXAMPLES` | `OFF` | Standalone API examples |
| `BUILD_TOOLS` | `OFF` | Manual HTTP, socket and handshake diagnostics |
| `ENABLE_UTP` | `OFF` | Link libutp and enable its transport implementation |
| `BUILD_DESKTOP` | `ON` on Windows | Build the native Windows application |
| `INTEROP_QBITTORRENT_EXECUTABLE` | Empty | Opt into Windows interoperability checks against qBittorrent 4.6.x |

An explicit `ENABLE_UTP=ON` requires libutp to be found; configuration fails with
an explanation if it is missing. Compile definitions and library settings apply
only to their owning targets.

## Usage

On Windows, open `build/desktop/TorrentClient.exe` (or
`build/desktop/Release/TorrentClient.exe` with Visual Studio). Select a `.torrent`
file or paste a magnet link and click **Add**. Select a torrent in the list on the
left, choose its destination and check the files to download. Select file rows
and apply a priority using the dropdown. Each torrent keeps its own choices. The window
shows progress and transfer speeds. **Stop** saves verified pieces; **Continue**
starts a session that verifies and reuses them. Closing the window waits for this
cleanup; network timeouts can delay stopping. **Download** adds the selected
torrent to the queue. The active download limit defaults to one and can be set
from 1 to 8. Lowering the limit lets existing transfers finish before starting
more. Jobs writing to the same files wait for each other even when the limit is
higher. By default each session stops when its selected files finish. **Remove from list**
stops the job and removes its row while keeping downloaded files and resume data.

The list, file priorities, destinations and concurrency limit are saved in
`%LOCALAPPDATA%/TorrentClient/downloads.json`. After reopening the app, select a
torrent and click **Download** to resume it; payload downloads do not start
automatically. Existing pieces are checked when the session starts. Keep original
`.torrent` files available so their metadata can be read again. Magnet metadata
is resolved again when the saved list is opened.

Each torrent has separate download and upload limits in **KiB/s** (1 KiB = 1024
bytes). Enter a non-negative number and click **Apply limits**; `0` means
unlimited. Decimal values accept a dot or comma. Limits can be changed while a
torrent is downloading or waiting in the queue. Starting a torrent also applies
the values currently entered in its fields. Applied limits are saved with the
download list and restored independently for each torrent. Limits apply to
payload data; short bursts and protocol traffic can exceed the displayed value.

Click **Seeding** for a torrent to continue sharing after it finishes. Set a ratio
limit and a whole number of hours; `0` disables that limit. The first reached
limit stops sharing. Changes apply to active sessions, and **Stop** ends sharing
manually. The ratio is bytes uploaded during the current session divided by the
full torrent's verified size, including uploads made during downloading. The
timer starts when the full torrent finishes. Counters restart when a new session
starts; the settings are saved with each torrent.

Seeding requires a fully verified, nonempty torrent. A selected subset completes
and stops when the rest is unavailable. A seeding job frees its download slot so
the queue can start the next torrent; tasks writing to its files still wait.
Each active session accepts incoming TCP peers on its announced port as well as
initiating outgoing connections. The port is shown with the selected torrent.
Different torrents use separate ports, and incoming handshakes must match the
torrent's info hash. Connections work over IPv4 and, when enabled, IPv6. Unknown
torrents and self connections are rejected; only verified pieces are uploaded.
The listener closes on stop or removal. Incoming handshakes use a five-second
deadline and count toward the peer limit. The application does not configure
router port forwarding or firewall rules. These controls apply to desktop jobs;
the CLI keeps its download-and-exit behavior.

Preferences and the desktop log live in `%LOCALAPPDATA%/TorrentClient`; the default
download folder is `%USERPROFILE%/Downloads/TorrentClient`. Resume data stays with
the downloaded files. The Windows interface uses system controls and requires no
additional GUI framework. MinGW builds link the C++ runtime statically and copy
remaining third-party DLLs beside the executable. Keep these DLLs with the app
when moving it to another folder.

The desktop controller is covered by unit tests and local tracker/peer integration
tests in CTest. To exercise the actual window in hidden mode, run:

```powershell
node tests/integration/test_download.cjs build/desktop/TorrentClient.exe --desktop-ui
node tests/integration/test_queue.cjs build/desktop/TorrentClient.exe ui
```

The remaining examples below use the CLI.

The executable is built under `build/app/` (or `build/app/Release/` with Visual
Studio). The default configuration path remains `./config.json`, relative to the
working directory. CLI values override configuration values.

```sh
./build/app/torrent_client example.torrent
./build/app/torrent_client "magnet:?xt=urn:btih:..."
./build/app/torrent_client example.torrent --config config/examples/minimal.json
./build/app/torrent_client example.torrent --download-dir ./downloads --max-download 500
./build/app/torrent_client example.torrent --list-files
./build/app/torrent_client example.torrent --files 2,4
./build/app/torrent_client example.torrent --priority "2:high,4:low,5:skip"
./build/app/torrent_client --help
```

Options: `--config`, `--download-dir`, `--max-download` and `--max-upload` (KB/s,
zero means unlimited), `--port`, `--log-level` and `--help`. Invalid numeric
values produce an error before the download starts.

`--list-files` prints one-based file numbers, byte sizes and paths without
starting payload downloads. `--files 2,4` downloads only those files; omitting
it downloads everything. Magnet inputs support the same flags after metadata
acquisition. Skipped payload files are not created or modified.

`--priority "2:high,4:low,5:skip"` assigns startup priorities; unmentioned files
default to `normal`. `--list-files` also displays effective priorities. When
combined with `--files`, priorities apply within that selection; they cannot
silently enable an excluded file. Pieces shared by multiple selected files use
their highest priority. Existing in-flight work is not preempted. See
[file priorities](docs/FILE_PRIORITIES.md).

Pieces overlapping selected and skipped files must still be downloaded and
SHA1-verified in full. Their data is retained in a hidden per-torrent boundary
cache for resume verification and reads, without creating skipped payload files.
See [selective downloads](docs/SELECTIVE_DOWNLOADS.md).

## Verification

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -R test_download --output-on-failure
```

Unit suites cover metadata, storage, peer messages and application controls.
With Node.js installed, integration scenarios run against local trackers and
TCP peers, checking downloads, resume, magnets, file selection, the queue, rate
limits, seeding and incoming connections. Payloads are compared byte for byte;
no public tracker is used.

Windows interoperability checks against qBittorrent 4.6.x are optional and must
be enabled explicitly with `INTEROP_QBITTORRENT_EXECUTABLE`. They use a disposable
profile and a loopback tracker, leaving the installed client's normal profile
alone. See [running interoperability checks](tests/README.md#independent-client-interoperability).

Integration artifacts stay under `build/tests/download-test-*` and
`build/tests/transfer-*`. The partial-download helper exercises the public
session stop API, then resume checks restart the ordinary CLI. The tests also
check shutdown with idle peers and the resumed announce's remaining byte count.

Magnet scenarios cover two-block metadata, different local/remote extension IDs,
metadata hash verification, rejection or timeout of one peer, tracker fallback,
and transition into ordinary file downloading. Unverified metadata cannot create
download files. The fixtures use tracker-discovered loopback peers; they do not
establish public-swarm or DHT-only interoperability.

Endgame automatically lets idle peers rescue the last delayed blocks. Copies
are limited to two peers per block; other requests are cancelled after a block
arrives. Offline checks cover wire `CANCEL` messages, late cancelled responses,
copy limits and preserving shared partial data after a disconnect. See
[endgame policy](docs/protocols/ENDGAME.md) for thresholds and ownership rules.

See [test documentation](tests/README.md) for details and limitations.
