# Windows portable archive

The ZIP contains the Release desktop application and its recursively discovered
runtime DLLs. It requires no installation, compiler, CMake or Node.js on the
destination computer. Extract the whole archive before launching `TorrentClient.exe`.
Settings remain in `%LOCALAPPDATA%/TorrentClient`; downloads default to
`%USERPROFILE%/Downloads/TorrentClient`. Moving the application folder does not
move those files. Original `.torrent` sources must remain available for saved jobs.

The verified build uses Windows x64, MinGW GCC 13.2 and a Schannel-enabled curl.
Other Windows toolchains need separate validation. HTTPS trackers verify the
certificate chain and hostname. This curl build uses Windows certificate stores
and does not require a separate CA bundle. See [curl's Schannel behavior](https://curl.se/libcurl/c/CURLOPT_CAINFO.html).

## Build

Configure the ordinary OpenSSL, curl, spdlog and JSON dependencies first, as
described in the root README. For a single-configuration generator:

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TOOLS=OFF
cmake --build build/release --target package --parallel
```

With Visual Studio use `--config Release` when building the `package` target.
Packaging rejects other configurations. Outputs appear in `build/release/dist`:

```text
TorrentClient-1.0.0-windows-x64.zip
TorrentClient-1.0.0-windows-x64.zip.sha256
```

The archive contains one application folder with `TorrentClient.exe`, required
DLLs, a Russian `README.txt`, `BUILD-INFO.txt`, `THIRD_PARTY_NOTICES.txt`, supplied
upstream license files and `SHA256SUMS.txt`. CLI tools, tests, build files and user
data are excluded. CPack also writes the archive's SHA256 sidecar. Checksums detect
changes; the executable/archive are not signed by this workflow.

`PORTABLE_RUNTIME_DIRECTORIES` accepts additional DLL search directories as a
CMake list. Missing runtime dependencies make packaging fail. MinGW uses the
compiler's `objdump`; other Windows toolchains use CMake's runtime dependency
scanner. Available MSVC runtime redistributables are included separately.

`PORTABLE_LICENSE_FILES` accepts a CMake list of upstream notice/license files
from the selected dependency builds. They are copied under `licenses/<source-folder>`.
For example:

```powershell
cmake -S . -B build/release '-DPORTABLE_LICENSE_FILES=C:/deps/curl/COPYING;C:/deps/spdlog/LICENSE;C:/deps/json/LICENSE.MIT'
```

Pass the matching notices for all bundled dependencies. The reference build
includes curl, spdlog, JSON, OpenSSL, iconv, IDN2, unistring and zlib notices.
Recreate the archive after changing dependency binaries or notices. To package
already built binaries without rebuilding, run:

```powershell
cpack --config build/release/CPackConfig.cmake -C Release
```

The normal `cmake --install` layout under `bin/` remains available; ZIP staging
uses the explicit `Portable` component. Packaging does not publish a release or
install anything into the user's system.

## Verify the archive

With Node.js available on the build machine:

```powershell
node tests/integration/test_portable.cjs build/release/dist/TorrentClient-1.0.0-windows-x64.zip
```

An optional second argument selects `cmake.exe`. The verifier checks the archive
checksum, every file in the internal manifest, Release metadata, the GUI subsystem
and the absence of development files. It extracts into a path with spaces and
Cyrillic characters and starts the real window from a separate working directory.
Child processes receive a PATH containing only Windows and System32, excluding
the toolchain and dependency directories.

Local tracker/peer fixtures verify an ordinary file download and two queued
torrents, including uploads. The extracted application and results remain in
`build/tests/portable-*` for inspection. This checks relocation on the build
machine; validation on a fresh Windows installation is still a separate release
check.
