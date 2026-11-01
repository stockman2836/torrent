# Selective file downloads

```powershell
.\build\app\torrent_client.exe example.torrent --list-files
.\build\app\torrent_client.exe example.torrent --files 2,4
```

Use `build/verify/app/` instead if using the local verification build. Visual
Studio builds put the executable under the corresponding configuration folder.

## CLI behavior

`--list-files` prints file numbers starting at 1, exact byte sizes and paths. For
a `.torrent` file it does not contact trackers or create payload files. For a
magnet input it acquires and verifies metadata first, then lists files without
starting payload transfers.

`--files 2,4` selects those file numbers. Omit the option to download all files.
Comma-separated positive numbers are accepted; duplicates are deduplicated.
Ranges and an empty list are not accepted. Out-of-range numbers fail before
payload initialization. Existing skipped files are left unchanged.

Use `--priority "2:high,4:low"` to order files within the selected set, or
`--priority "2:skip"` to exclude a file. Priorities do not expand an explicit
`--files` selection. Effective values are shown by `--list-files`. See
[priority rules](FILE_PRIORITIES.md).

CLI numbers are one-based. The `Config::file_indices` API uses zero-based
metadata indices, with an empty vector meaning all files. This runtime choice
is not serialized into the JSON configuration file.

## Shared pieces and storage

BitTorrent verifies complete pieces, so a selected file may require bytes from
adjacent skipped files. The client requests the pieces intersecting selected
nonempty files, verifies their complete SHA1 hashes, and writes only selected
payload spans. Pieces wholly outside the selection are not requested.

Boundary pieces also go into
`<download-dir>/.parts_<full-info-hash>/<piece-index>.bin`. They retain the
complete verified piece, including bytes belonging to skipped files. This
keeps skipped payload paths absent while allowing resume checks and valid
piece reads/uploads. These internal files are retained between runs.

Readback overlays current selected-file data on boundary cache bytes, so a
modified selected file cannot be hidden by an intact cache. Existing unselected
files may be read without opening them for writing when reconstructing data
from an earlier selection. Missing boundary data or corrupted selected bytes
invalidate the corresponding resume piece and cause it to be downloaded again.

## Progress and protocol state

Progress counts only required pieces. Selecting an empty file needs no pieces
and completes after creating the empty selected path. Resume bits and peer
bitfields continue to represent data actually verified and readable on disk.
Skipped pieces are never marked complete just to advance progress.

Tracker `left` still describes the entire torrent. A finished selection can
therefore have positive torrent bytes remaining. It does not announce full
completion or advertise HAVE_ALL. Endgame uses the remaining required-piece
count. Both `.torrent` and magnet downloads use the same selection/storage path.

## Verification

```sh
ctest --test-dir build -R "test_selection|test_file_selection|test_command_line" --output-on-failure
```

Offline scenarios verify a middle file spanning two boundaries, disjoint files,
an empty file, restarting with cached boundaries, detecting selected-file
corruption, changing selection, rejected IDs and preserving an existing skipped
file. Listing `.torrent` files is checked for zero tracker activity; a separate
magnet listing scenario checks metadata-only acquisition. These tests do not
replace large public-swarm or cross-platform filesystem validation.
