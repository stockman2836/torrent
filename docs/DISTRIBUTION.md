# Windows distribution

The Windows x64 installer places the application in
`%LOCALAPPDATA%\Programs\TorrentClient` by default and adds a **TorrentClient**
Start menu shortcut. It runs under the current account. Remove it through
**Settings → Apps → Installed apps**. Settings, the saved queue and downloaded
files are kept on uninstall.

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

Configure the ordinary OpenSSL, curl, spdlog, JSON and TinyXML2 dependencies first, as
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
The statically linked TinyXML2 10.0.0 notice is included automatically.
Recreate the archive after changing dependency binaries or notices. To package
already built binaries without rebuilding, run:

```powershell
cpack --config build/release/CPackConfig.cmake -C Release
```

The normal `cmake --install` layout under `bin/` remains available; ZIP staging
uses the explicit `Portable` component. Packaging does not publish a release or
install anything into the user's system.

## Build the installer

Build the x64 ZIP first. With Inno Setup 6.7 installed, run the following from the
repository root. Use `-Compiler <path-to-ISCC.exe>` for a non-default installation.

```powershell
./packaging/build-installer.ps1 -Archive build/release/dist/TorrentClient-1.0.0-windows-x64.zip
```

The script verifies the ZIP checksum and Release metadata, then compiles the
installer from its extracted contents, including DLLs and notices. The resulting
`-setup.exe` and its SHA256 file appear beside the ZIP.

## Verify the archive

For a CI build, open **Actions → Windows CI**, select the successful run for the
commit you want to test and download **TorrentClient-windows-x64** under
**Artifacts**. Extract the artifact to get the installer, portable ZIP and
their SHA256 files.

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

Local trackers and peers exercise downloads, queueing and uploads. The extracted
application and results remain in `build/tests/portable-*` for inspection.

## Verify installation and restart

CI downloads the packages on a fresh Windows runner in a separate job. To run
the installer check locally, use a Windows account without an existing TorrentClient
installation or Start menu shortcut.

```powershell
./tests/integration/test_installer.ps1 -Archive build/release/dist/TorrentClient-1.0.0-windows-x64.zip
```

The check installs into a test folder, verifies the uninstall entry, shortcut
target and installed file hashes, and runs the installed GUI with a system-only
PATH. A local peer serves one piece before the window closes. A second process
restores the saved queue and requests only the remaining pieces. The test checks
the completed payload byte for byte, uninstalls the client and verifies that
the queue, configuration and download survived. Logs are under `build/tests/installer-*`.

## Publish a release

When a release is ready, push a tag matching the CMake project version, such
as `v1.0.0`. The tag triggers the same build and checks as a branch push.
After the verification job succeeds, the release job checks both SHA256 files
and creates a GitHub Release for that tag with the installer, ZIP and checksums.
