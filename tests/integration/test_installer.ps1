param([Parameter(Mandatory)][string]$Archive)

$ErrorActionPreference = 'Stop'
$zip = Get-Item -LiteralPath $Archive
$setup = Get-Item -LiteralPath (Join-Path $zip.DirectoryName ($zip.BaseName + '-setup.exe'))
$registry = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Stockman2836.TorrentClient_is1'
if (Test-Path -LiteralPath $registry) { throw 'TorrentClient is already installed for this user. Run this test in a fresh profile.' }

$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'TorrentClient.lnk'
if (Test-Path -LiteralPath $shortcut) { throw 'An existing TorrentClient shortcut belongs to this user' }
$work = New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot ("../../build/tests/installer-" + [guid]::NewGuid())) -Force
$appDir = Join-Path $work.FullName 'Installed application'
$app = Join-Path $appDir 'TorrentClient.exe'
$node = (Get-Command node.exe).Source
foreach ($file in $zip, $setup) {
    $expected = (Get-Content -LiteralPath ($file.FullName + '.sha256') -Raw).Split(' ')[0]
    if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne $expected) { throw "Checksum mismatch for $($file.Name)" }
}
Expand-Archive -LiteralPath $zip.FullName -DestinationPath $work.FullName
$portable = Join-Path $work.FullName $zip.BaseName

function Run-Setup([string]$Program, [string[]]$Options) {
    $process = Start-Process -FilePath $Program -WindowStyle Hidden -Wait -PassThru -ArgumentList $Options
    if ($process.ExitCode) { throw "$Program returned $($process.ExitCode)" }
}
$silent = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
try {
    Run-Setup $setup.FullName ($silent + @("/DIR=`"$appDir`"", "/LOG=`"$($work.FullName)\setup.log`""))
    $entry = Get-ItemProperty -LiteralPath $registry
    if ($entry.InstallLocation.TrimEnd('\') -ne $appDir -or $entry.DisplayName -notlike 'TorrentClient*' -or
        $zip.BaseName -ne "TorrentClient-$($entry.DisplayVersion)-windows-x64") { throw 'Wrong Windows uninstall entry' }
    if (!(Test-Path -LiteralPath $shortcut)) { throw 'Start menu shortcut missing' }
    $shell = New-Object -ComObject WScript.Shell
    if ($shell.CreateShortcut($shortcut).TargetPath -ne $app) { throw 'Start menu shortcut points to another program' }

    foreach ($file in (Get-ChildItem -LiteralPath $portable -Recurse -File)) {
        $relative = $file.FullName.Substring($portable.Length + 1)
        $installed = Join-Path $appDir $relative
        if ((Get-FileHash -LiteralPath $installed).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
            throw "Installed file differs from portable release: $relative"
        }
    }

    $previousPath = $env:Path
    Push-Location $work.FullName
    try {
        $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
        & $node (Join-Path $PSScriptRoot 'test_transfer.cjs') desktop-restart $app
        if ($LASTEXITCODE) { throw 'Installed GUI restart test failed' }
    } finally {
        $env:Path = $previousPath
        Pop-Location
    }
    $fixture = Get-Item (Join-Path $work.FullName 'transfer-desktop-restart-*')
    $userFiles = @('downloads.json', 'config.json', 'files/payload.bin') | ForEach-Object {
        Get-FileHash -LiteralPath (Join-Path $fixture.FullName $_)
    }
} finally {
    if (Test-Path -LiteralPath $registry) {
        $entry = Get-ItemProperty -LiteralPath $registry
        $uninstall = $entry.UninstallString.Trim('"')
        if ($entry.InstallLocation.TrimEnd('\') -ne $appDir -or (Split-Path -Parent $uninstall) -ne $appDir) {
            throw 'Unexpected uninstall location'
        }
        Run-Setup $uninstall $silent
    }
}
if ((Test-Path -LiteralPath $appDir) -or (Test-Path -LiteralPath $shortcut) -or (Test-Path -LiteralPath $registry)) {
    throw 'Uninstall left application files, a shortcut or a registry entry'
}
foreach ($file in $userFiles) {
    if ((Get-FileHash -LiteralPath $file.Path).Hash -ne $file.Hash) { throw "Uninstall changed $($file.Path)" }
}
Write-Output 'PASS installer: per-user install, shortcut, verified restart and uninstall with user data preserved.'
