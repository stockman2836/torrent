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
| `test_extension_protocol` | Payload framing, ID dispatch and extension disable updates | Network |
| `test_metadata_exchange` | Binary assembly, size validation, rejects and callback locking | Network |
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

Each unit suite has a 30-second timeout. The integration test has a 55-second
CTest timeout and kills the child client after 45 seconds if it does not exit.
Each transfer scenario has a 40-second CTest timeout and a 15-second child
process timeout. Its seeds leave idle sockets open to check receive cancellation
during shutdown.
Magnet scenarios use a 20-second child-process timeout and a 30-second CTest
timeout. Each TCP metadata peer attempt has a 10-second deadline.

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

Run individual suites with `ctest --test-dir build -R test_command_line -V` or
`ctest --test-dir build -R test_download --output-on-failure`.
Run transfer scenarios with `ctest --test-dir build -R test_transfer --output-on-failure`.

## Adding tests

Add `unit/test_<module>.cpp` and register it with `add_unit_test` in
`tests/CMakeLists.txt`, linking the narrowest module that provides the behavior.
Use Google Test assertions to check observable behavior and failure cases.
Integration checks belong under `integration/` and should not depend on public
trackers or external swarms.

The suite does not yet establish interoperability with public swarms, DHT-only
magnet discovery, metadata seeding, encryption, optional libutp or all
cross-platform socket paths.
