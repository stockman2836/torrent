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
uses the priority-aware selector. Raising or lowering priority does not preempt
already requested pieces; excluding whole pieces cancels their requests as
described below.

The CLI accepts priorities at session startup. To change CLI priorities, restart
with new flags; verified resume data is reused.

## Live desktop changes

While a torrent is downloading, select file rows and apply a priority using the
dropdown, or tick/untick files. These controls also work for ready queued jobs
and seeding sessions; destination folders remain locked during a session. The
desktop saves the new choices with the download list. The worker applies the
latest requested selection without recreating peer connections, and shows a
pending status while doing so.

Changing high/normal/low affects the next eligible requests. Already requested
pieces may finish first. Skipping a whole piece removes its session requests,
queues TCP CANCEL messages for every outstanding copy and discards unverified
assembly. Late replies cannot write excluded files. A skipped block also stops
waiting for a download-speed allowance, allowing the peer to request wanted data.
Shared boundary pieces remain wanted when they overlap another selected file.
Already queued HTTP seed requests may finish; unwanted results are discarded.

Verified pieces and existing payload files are retained. Enabling a file restores
its verified boundary bytes from the cache before treating them as available;
it does not download them again. Disabling a file closes its writable handle and
preserves shared verified boundaries. Whole skipped files stay on disk without
being copied wholesale into the cache. Progress is recalculated over the current
selection, so adding missing files can lower its percentage. Skipping all remaining
work completes the selected target; seeding policy remains independent of download
priorities.

Each active queue job retains ownership of paths selected during that session
until it stops. A live change that would write a path owned by another active job
is rejected. Queued writers wait until existing owners finish, including paths
just deselected while a worker is applying changes. Storage errors or changed
verified source bytes fail that job while preserving its data; restarting checks
the saved pieces again.

The API uses zero-based `Config::file_priorities` overrides with the `FilePriority`
enum. Like `file_indices`, these runtime overrides are not serialized into JSON.
The desktop list persists them separately. `DownloadQueue::setFilePriorities`
and `DownloadTask::setFilePriorities` accept zero-based overrides for the complete
file list; unmentioned files default to normal and replace an earlier selection.

## Verification

```sh
ctest --test-dir build -R "test_priority|test_file_selection|test_piece_manager|test_command_line" --output-on-failure
ctest --test-dir build -R "test_live_priorities|test_live_selection" --output-on-failure
```

Offline TCP tests check actual request order for sequential and default strategy
modes, boundary overlap, skipped files, an empty target, CLI validation, preview
and restarting with changed priorities. Unit tests cover rarity tie-breaking
and all strategy priority rules. Public-swarm behavior and web-seed transport
interoperability require broader integration coverage.

Live tests inspect wire request order, cancellation and late data, re-enabling
files, cached-boundary restoration, stop/resume, conflicting queue ownership,
endgame duplicates and cancellation while throttled at 1 B/s. A native-window
fixture changes a priority and unticks another file during an actual transfer.
