# Magnet download support

The tracked magnet path is covered by offline end-to-end tests: tracker
peer discovery, extension negotiation, metadata acquisition, info-hash
verification and transition into the ordinary file download session.

## Protocol flow

1. Parse a v1 `magnet:?xt=urn:btih:<hash>` URI. The parser accepts hex and base32
   hashes with optional display names and trackers.
2. Query listed trackers and deduplicate peers. If one tracker fails, try the
   others. DHT is another implemented discovery path; DHT-only magnets are not
   part of the offline integration coverage.
3. Perform the BitTorrent handshake and advertise a local `ut_metadata` ID.
4. Read the remote extension map and metadata size. Outgoing requests use the
   remote ID; responses are dispatched through the advertised local ID.
5. Request 16-KiB metadata blocks. Parse the Bencode message header and preserve
   the binary bytes following it.
6. Validate total size, block index and exact block length. Assemble the blocks
   and verify SHA1 against the magnet info hash before creating a `TorrentFile`.
7. Start the same `DownloadManager` lifecycle used by `.torrent` files, retaining
   the working tracker and any existing DHT instance.

Wire headers are added exactly once by `PeerConnection`. Extension codecs
produce Bencode bodies and dispatch already-separated extension IDs. Completion
callbacks run outside the metadata assembler's mutex.

## Bounds and failure handling

The client accepts metadata up to 4 MiB. This is a project resource limit, not a
BEP requirement. A TCP metadata peer attempt has a 10-second deadline, and up to
five discovered peers are tried. A reject, invalid size, wrong metadata hash or
stalled connection ends the current attempt so another peer can be tried.
Unknown metadata message types are ignored.

Only verified metadata reaches the application. If every attempt fails, the
CLI exits with an error without creating payload files. Progress is exposed
atomically; the exchange object stays private to its worker's peer attempt.

## Usage

```sh
./build/app/torrent_client "magnet:?xt=urn:btih:<40_HEX_CHARACTERS>&tr=<URL_ENCODED_TRACKER>"
```

Configuration controls DHT and other download services. Input resolution lives
in `app/input.cpp`; acquisition lives in
`src/session/magnet_download_manager.cpp`. Extension and block codecs live under
`src/network/peer/`.

## Verification

```sh
ctest --test-dir build -R "test_magnet|test_metadata_exchange|test_extension_protocol" --output-on-failure
```

Fixtures verify two-block binary metadata and the final payload. They cover
different local/remote IDs, metadata rejects, valid Bencode with a wrong hash,
an unavailable tracker and an idle peer. Invalid metadata must not start file
downloading. Logs and artifacts remain under `build/tests/magnet-*`.

These checks do not establish interoperability with every public client or
tracker. Public-swarm checks, DHT-only discovery, v2 magnets, direct `x.pe`
sources and metadata seeding require separate work or coverage. Incoming
metadata requests are not served by the download-side assembler.

## References

- [BEP 9: metadata exchange](https://www.bittorrent.org/beps/bep_0009.html)
- [BEP 10: extension protocol](https://www.bittorrent.org/beps/bep_0010.html)
