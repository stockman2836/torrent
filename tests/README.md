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

## Suites

| Executable | Scope | Linked module |
| --- | --- | --- |
| `test_bencode` | Bencode parsing and round trips | Metainfo |
| `test_torrent_file` | Single/multi-file metadata and info hashes | Metainfo |
| `test_piece_manager` | Selection, block assembly, hash failures and resume state | Storage |
| `test_file_manager` | File initialization, piece boundaries and concurrent writes | Storage |
| `test_command_line` | Help, overrides, invalid numbers and overflow rejection | Application |
| `test_download` | Actual CLI download from a loopback tracker and TCP peer | CLI executable |

Each unit suite has a 30-second timeout. The integration test has a 55-second
CTest timeout and kills the child client after 45 seconds if it does not exit.

The integration test downloads two pieces split into multiple 16-KiB blocks,
checks the info hash in the handshake, validates block requests and compares
the output with the original 50,003-byte binary payload. It retains the client
output and files under `build/tests/download-test-*` for troubleshooting.

Run individual suites with `ctest --test-dir build -R test_command_line -V` or
`ctest --test-dir build -R test_download --output-on-failure`.

## Adding tests

Add `unit/test_<module>.cpp` and register it with `add_unit_test` in
`tests/CMakeLists.txt`, linking the narrowest module that provides the behavior.
Use Google Test assertions to check observable behavior and failure cases.
Integration checks belong under `integration/` and should not depend on public
trackers or external swarms.

The suite does not yet establish end-to-end behavior for magnet metadata,
public swarms, encryption, optional libutp or all cross-platform socket paths.
