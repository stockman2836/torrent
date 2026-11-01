# File download priorities

```powershell
.\build\app\torrent_client.exe example.torrent --priority "2:high,4:low,5:skip"
.\build\app\torrent_client.exe example.torrent --list-files --priority "2:high,4:low"
.\build\app\torrent_client.exe example.torrent --files "2,4" --priority "2:high,4:low"
```

The local verification executable is under `build/verify/app/`. File numbers
start at 1 and match `--list-files`. Levels are `skip`, `low`, `normal` and
`high`; unmentioned selected files default to `normal`. Repeated entries use
the last value. Unsupported levels and invalid file numbers are errors.

## Selection and storage

Without `--files`, priorities apply to all files. With an explicit selection,
priorities apply inside it. Setting a non-skip priority for an excluded file is
rejected rather than changing that selection. `skip` removes a file from the
current download target and does not delete existing data. An entirely skipped
target completes without fetching payload pieces or creating payload paths.

Each piece gets the maximum priority of the selected nonempty files it overlaps.
A low-priority file sharing a piece with a high-priority file can therefore
receive some data early. Full-piece verification and the boundary cache follow
the same [selective download rules](SELECTIVE_DOWNLOADS.md).

## Scheduling semantics

The highest priority is preferred among eligible, available, unreserved pieces.
Sequential selection orders equal priorities by index. Random-first chooses
randomly within the highest priority. Rarest-first compares rarity within the
highest available priority. Idle peers can still work on lower priorities when
higher-priority work is already reserved or unavailable from that peer.

Endgame rescue respects priorities among eligible blocks while retaining its
request-copy limits and cancellation policy. Web-seed candidate selection also
uses the priority-aware selector. Existing requests are not preempted.

The CLI accepts priorities at session startup. To change them, restart with new
flags; verified resume data is reused. Live reprioritization and changes to
storage masks during an active session are not exposed yet.

The API uses zero-based `Config::file_priorities` overrides with the `FilePriority`
enum. Like `file_indices`, these runtime overrides are not serialized into JSON.

## Verification

```sh
ctest --test-dir build -R "test_priority|test_file_selection|test_piece_manager|test_command_line" --output-on-failure
```

Offline TCP tests check actual request order for sequential and default strategy
modes, boundary overlap, skipped files, an empty target, CLI validation, preview
and restarting with changed priorities. Unit tests cover rarity tie-breaking
and all strategy priority rules. Public-swarm behavior and web-seed transport
interoperability require broader integration coverage.
