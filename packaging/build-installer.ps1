param(
    [Parameter(Mandatory)][string]$Archive,
    [string]$Compiler = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
)

$ErrorActionPreference = 'Stop'
$zip = Get-Item -LiteralPath $Archive
if ($zip.Name -notmatch '^TorrentClient-(\d+\.\d+\.\d+)-windows-x64\.zip$') {
    throw "Expected an x64 portable release ZIP, got $($zip.Name)"
}
$version = $Matches[1]
$digest = (Get-Content -LiteralPath ($zip.FullName + '.sha256') -Raw).Split(' ')[0]
if ((Get-FileHash -LiteralPath $zip.FullName).Hash -ne $digest) { throw 'Portable ZIP checksum mismatch' }

$tempRoot = [IO.Path]::GetTempPath()
$stage = Join-Path $tempRoot ('torrent-installer-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    Expand-Archive -LiteralPath $zip.FullName -DestinationPath $stage
    $payload = Join-Path $stage $zip.BaseName
    $metadata = Get-Content -LiteralPath (Join-Path $payload 'BUILD-INFO.txt')
    if ($metadata -notcontains "TorrentClient $version" -or $metadata -notcontains 'Configuration: Release' -or
        $metadata -notcontains 'Architecture: x64') { throw 'Unexpected portable build metadata' }

    & $Compiler "/DAppVersion=$version" "/DPayload=$payload" "/O$($zip.DirectoryName)" (Join-Path $PSScriptRoot 'windows.iss')
    if ($LASTEXITCODE) { throw "Inno Setup failed with exit code $LASTEXITCODE" }
    $setup = Get-Item -LiteralPath (Join-Path $zip.DirectoryName ($zip.BaseName + '-setup.exe'))
    $hash = (Get-FileHash -LiteralPath $setup.FullName).Hash.ToLowerInvariant()
    Set-Content -LiteralPath ($setup.FullName + '.sha256') -Encoding ascii -Value "$hash  $($setup.Name)"
} finally {
    if ((Split-Path -Parent ([IO.Path]::GetFullPath($stage))).TrimEnd('\') -eq $tempRoot.TrimEnd('\')) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
