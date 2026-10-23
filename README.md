# BitTorrent Client

A C++17 command-line BitTorrent client. Supports torrent metadata, HTTP/UDP
trackers, TCP peer transfers, piece verification, multi-file storage and resume
state. Additional modules implement DHT, magnet metadata exchange, PEX, MSE,
Fast Extension, IPv6, local discovery, web seeds and optional uTP.

The automated integration test covers the ordinary `.torrent` TCP download
path. The additional protocols still require broader interoperability testing.

## Project structure

```text
app/                     CLI parsing, input resolution and application lifecycle
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

- CMake 3.15+ and a C++17 compiler (MSVC, GCC or Clang).
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

An explicit `ENABLE_UTP=ON` requires libutp to be found; configuration fails with
an explanation if it is missing. Compile definitions and library settings apply
only to their owning targets.

## Usage

The executable is built under `build/app/` (or `build/app/Release/` with Visual
Studio). The default configuration path remains `./config.json`, relative to the
working directory. CLI values override configuration values.

```sh
./build/app/torrent_client example.torrent
./build/app/torrent_client "magnet:?xt=urn:btih:..."
./build/app/torrent_client example.torrent --config config/examples/minimal.json
./build/app/torrent_client example.torrent --download-dir ./downloads --max-download 500
./build/app/torrent_client --help
```

Options: `--config`, `--download-dir`, `--max-download` and `--max-upload` (KB/s,
zero means unlimited), `--port`, `--log-level` and `--help`. Invalid numeric
values produce an error before the download starts.

## Verification

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -R test_download --output-on-failure
```

Five unit-test executables cover metadata parsing, piece management, file I/O
and CLI parsing. With Node.js installed, six integration scenarios run against
local trackers and TCP seeds: basic download, multiple peers, partial download
followed by restart, corrupted saved data, peer disconnect and multi-file piece
boundaries. The multi-peer check rejects duplicate block requests. Each result
is compared byte for byte with its original payload; no public tracker is used.

Integration artifacts stay under `build/tests/download-test-*` and
`build/tests/transfer-*`. The partial-download helper exercises the public
session stop API, then resume checks restart the ordinary CLI. The tests also
check shutdown with idle peers and the resumed announce's remaining byte count.

See [test documentation](tests/README.md) for details and limitations.
