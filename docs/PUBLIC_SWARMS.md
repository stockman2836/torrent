# Manual public-swarm verification

These checks contact real trackers, DHT routers and peers. They are explicit
manual runs, separate from CTest: an unavailable peer or a changed public release
must not break the offline regression suite. Use an official, freely distributed
image and obtain its checksum from the same project's HTTPS site.

`public_swarm_driver` uses the same `DownloadTask` as the desktop. It records
progress once per second in `<report>.jsonl`, stops at the requested time budget
and writes a final JSON report, including shutdown latency. Engine logs use
`log_file` from the configuration and rotate normally. The helper always disables
web seeds and post-download seeding so downloaded bytes must come from peers.
Keep separate report names for each run.

## Repeat a download and resume check

Build with `BUILD_TESTS=ON`, then build the manual helper:

```powershell
cmake --build build/verify --target public_swarm_driver --parallel
New-Item -ItemType Directory -Force build/public-swarm | Out-Null
```

Download an official `.torrent` and save it as `build/public-swarm/image.torrent`.
For the recorded Debian run below, the source was
[Debian's armhf torrent directory](https://cdimage.debian.org/debian-cd/current/armhf/bt-cd/).
That directory changes when Debian publishes a new release; select the matching
torrent and official ISO SHA256 rather than reusing an older checksum.

Save `build/public-swarm/config.json` with an available TCP port:

```json
{
  "enable_dht": true,
  "dht_port": 0,
  "enable_pex": true,
  "enable_lsd": false,
  "enable_webseeds": false,
  "enable_utp": false,
  "enable_encryption": false,
  "max_peers": 8,
  "listen_port": 28401,
  "max_download_speed": 2097152,
  "max_upload_speed": 131072,
  "download_dir": "./build/public-swarm/files",
  "log_file": "./build/public-swarm/client.log"
}
```

Run from the repository root. The first command normally exits with code `3`
after stopping a partial download; inspect the JSON report before continuing.
Use the same download directory for the second run:

```powershell
build/verify/tests/public_swarm_driver.exe build/public-swarm/image.torrent build/public-swarm/config.json build/public-swarm/partial.json 120
node tests/integration/verify_public_download.cjs build/public-swarm/image.torrent build/public-swarm/files build/public-swarm/partial-verified.json

build/verify/tests/public_swarm_driver.exe build/public-swarm/image.torrent build/public-swarm/config.json build/public-swarm/resumed.json 600
node tests/integration/verify_public_download.cjs build/public-swarm/image.torrent build/public-swarm/files build/public-swarm/complete-verified.json <official-iso-sha256>
```

The independent Node verifier supports single-file images. Without a SHA256
argument it reads the stopped resume bitfield and checks every claimed piece
against the torrent's SHA1 hashes. With an official SHA256 it checks **all**
pieces and the entire file, ignoring resume claims. Preallocated file size or
the application's progress alone is not proof of a correct download.

Driver exit codes: `0` complete (or successful metadata-only resolution), `1`
failure, `3` partial progress, `4` no progress. The time budget includes metadata
loading; shutdown joins the engine afterwards. DNS and individual network calls
can extend actual elapsed time beyond the budget. `stop_ms` makes that visible.

## Public DHT and magnet metadata

Use a separate available TCP port and log file if another run is active. Replace
the hash with the official torrent's info hash. Omit `tr` to exercise DHT-only
discovery; `--metadata-only` avoids allocating another image:

```powershell
build/verify/tests/public_swarm_driver.exe 'magnet:?xt=urn:btih:<info-hash>' build/public-swarm/config.json build/public-swarm/dht-metadata.json 60 --metadata-only
```

Require `outcome: metadata`, the expected `info_hash`, image name and total byte
count. The engine verifies received BEP 9 metadata against the magnet's SHA1
before publishing it. To test tracker-based metadata separately, add the
URL-encoded official `tr` parameter and use a different report name.

## Recorded run: 2026-11-11

Windows x64, shared desktop `DownloadTask` controller through the manual driver,
using plaintext TCP. Public DHT and PEX enabled; web seeds, LSD and uTP disabled.
Download limit 2 MiB/s, upload limit 128 KiB/s, at most eight active peers. No
router or firewall settings were changed. This run exercised the controller and
engine; the native window has its own existing local integration checks.

| Item | Observed value |
| --- | --- |
| Image | `debian-13.7.0-armhf-netinst.iso` |
| Length | 939,831,296 bytes |
| Info hash | `cc6464617ff7af0b9e598ca2244717cea8b899d9` |
| Official ISO SHA256 | `471528fbb8caee5f5eb385644bbc71847e607cd10d6668a4983cbe185faf6e09` |
| Official torrent SHA256 | `3374fdbbe4659ad7914cc23f95bf5dc29d7cd891cd0fb967ea90d83720f598d5` |
| Partial run | 120-second budget, 796 verified pieces / 208,666,624 bytes, 22.1974% |
| Stop | 276 ms, state `stopped` |
| Resumed run | Completed in 418.591 seconds, starting with the saved 796 pieces |
| Final verification | All 3,586 SHA1 piece hashes and the official ISO SHA256 matched |
| Magnet with official tracker | Verified metadata received; expected file name and length |
| Trackerless magnet | Verified metadata received through public DHT in 2.627 seconds |

The torrent and [checksums](https://cdimage.debian.org/debian-cd/current/armhf/bt-cd/SHA256SUMS)
came from Debian's HTTPS directory above. Local
evidence is retained under `build/public-swarm/`, including `debian-first.json`,
`debian-partial-verified.json`, `debian-resumed.json`, `debian-dht.json`, progress
records, `debian-complete-verified.json` and engine logs. These large/network-specific
artifacts are ignored by Git. All 115 existing CTest checks also passed; separate
small verifier checks rejected a corrupt saved piece and an incorrect final
SHA256, including when artifacts were stored under a Cyrillic path.

An earlier metadata-only attempt against the official Arch Linux
`archlinux-2026.11.01-x86_64.iso` torrent found addresses through DHT, but all five
attempted TCP peers timed out. Independent probes of three of those addresses
also timed out or were refused. That attempt did not establish a successful Arch
transfer. A public DHT response is not a guarantee that a returned peer is reachable.

These observations cover the named swarm and current machine. They do not
establish encrypted transfer, uTP, IPv6 DHT, sustained multi-day operation or
startup on a fresh Windows installation.
