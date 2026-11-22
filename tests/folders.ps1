param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('mytorrent-folders-' + [guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $root
$seed = $null
$junction = $null

function Run-Cli([string[]]$Arguments, [int]$Expected = 0) {
    $output = & $Exe @Arguments 2>&1
    if ($LASTEXITCODE -ne $Expected) { throw "Unexpected exit code for $Arguments`: $output" }
    return ($output -join "`n")
}

function Write-Torrent([string]$Path, [string]$Files) {
    $info = "d5:files$($Files)4:name7:payload12:piece lengthi16384e6:pieces40:$($encoding.GetString($hashes))e"
    $torrentBytes = $encoding.GetBytes("d8:announce$($url.Length):$($url)4:info$($info)e")
    [IO.File]::WriteAllBytes($Path, $torrentBytes)
    return ([BitConverter]::ToString($sha.ComputeHash($encoding.GetBytes($info)))).Replace('-', '').ToLowerInvariant()
}

function Assert-Folder([string]$Destination) {
    $result = Join-Path $Destination 'payload'
    $null = Run-Cli @('verify', $torrent, '--folder', $result)
    foreach ($relative in @('z.bin', 'nested/a.bin', 'nested/empty.bin')) {
        $actual = (Get-FileHash -LiteralPath (Join-Path $result $relative)).Hash
        $expected = (Get-FileHash -LiteralPath (Join-Path $source $relative)).Hash
        if ($actual -ne $expected) { throw "Downloaded file differs: $relative" }
    }
    if (@(Get-ChildItem -Force -LiteralPath $Destination).Count -ne 1) {
        throw 'Folder publication left staging or partial data behind'
    }
}

try {
    $source = Join-Path $root 'payload'
    $nested = Join-Path $source 'nested'
    $null = New-Item -ItemType Directory -Path $nested -Force
    $first = [byte[]]::new(12000)
    $second = [byte[]]::new(6000)
    $random = [Random]::new(91)
    $random.NextBytes($first)
    $random.NextBytes($second)
    $secondPath = Join-Path $nested 'a.bin'
    [IO.File]::WriteAllBytes((Join-Path $source 'z.bin'), $first)
    [IO.File]::WriteAllBytes((Join-Path $nested 'empty.bin'), [byte[]]@())
    [IO.File]::WriteAllBytes($secondPath, $second)

    # The file order differs from the path order, and an empty file sits at the boundary.
    $entries = 'ld6:lengthi12000e4:pathl5:z.bineed6:lengthi0e4:pathl6:nested9:empty.bineed6:lengthi6000e4:pathl6:nested5:a.bineee'
    $encoding = [Text.Encoding]::GetEncoding(28591)
    $url = 'http://127.0.0.1:1/announce'
    $sha = [Security.Cryptography.SHA1]::Create()
    $payload = [byte[]]($first + $second)
    $hashes = [byte[]]($sha.ComputeHash($payload, 0, 16384) + $sha.ComputeHash($payload, 16384, $payload.Length - 16384))
    $torrent = Join-Path $root 'fixture.torrent'
    $infoHash = Write-Torrent $torrent $entries
    $inspection = Run-Cli @('inspect', $torrent)
    foreach ($line in @('Bytes: 18000', 'Pieces: 2', "Info hash: $infoHash", 'Files: 3', "Offset`tBytes`tPath",
            "0`t12000`tz.bin", "12000`t0`tnested/empty.bin", "12000`t6000`tnested/a.bin")) {
        if (($inspection -split "`n") -cnotcontains $line) { throw "Missing inspection line: $line" }
    }
    $null = Run-Cli @('verify', $torrent, '--folder', $source)
    $second[0] = $second[0] -bxor 255
    [IO.File]::WriteAllBytes($secondPath, $second)
    $failure = Run-Cli @('verify', $torrent, '--folder', $source) 1
    if ($failure -notmatch 'Piece hash mismatch at index 0') { throw "Wrong verification error: $failure" }
    $second[0] = $second[0] -bxor 255
    [IO.File]::WriteAllBytes($secondPath, $second)

    $invalid = Join-Path $root 'invalid.torrent'
    foreach ($invalidEntries in @(
            "$($entries)6:lengthi18000e",
            $entries.Replace('i12000e', 'i-1e'),
            $entries.Replace('i12000e', 'i9223372036854775807e'),
            $entries.Replace('l5:z.bine', 'le'),
            $entries.Replace('6:nested5:a.bin', '2:..5:a.bin'),
            $entries.Replace('6:nested5:a.bin', '6:Nested5:a.bin'),
            $entries.Replace('6:nested5:a.bin', '6:nested9:EMPTY.BIN'),
            $entries.Replace('6:nested5:a.bin', '5:z.bin5:a.bin'))) {
        $null = Write-Torrent $invalid $invalidEntries
        $null = Run-Cli @('inspect', $invalid) 1
    }

    $created = Join-Path $root 'created.torrent'
    $null = Run-Cli @('create', $source, '--tracker', $url, '--out', $created, '--piece-size', '16384')
    $createdInspection = Run-Cli @('inspect', $created)
    $null = Run-Cli @('verify', $created, '--folder', $source)
    $sortedPayload = [byte[]]($second + $first)
    $hashes = [byte[]]($sha.ComputeHash($sortedPayload, 0, 16384) +
        $sha.ComputeHash($sortedPayload, 16384, $sortedPayload.Length - 16384))
    $expectedEntries = 'ld6:lengthi6000e4:pathl6:nested5:a.bineed6:lengthi0e4:pathl6:nested9:empty.bineed6:lengthi12000e4:pathl5:z.bineee'
    $expected = Join-Path $root 'expected.torrent'
    $expectedHash = Write-Torrent $expected $expectedEntries
    if (($createdInspection -split "`n") -cnotcontains "Info hash: $expectedHash") {
        throw 'Folder creation did not hash files in the expected order'
    }

    $linked = Join-Path $root 'linked'
    $null = New-Item -ItemType Directory -Path $linked
    [IO.File]::WriteAllBytes((Join-Path $linked 'z.bin'), $first)
    $junction = Join-Path $linked 'nested'
    $null = New-Item -ItemType Junction -Path $junction -Target $nested
    $failure = Run-Cli @('verify', $torrent, '--folder', $linked) 1
    if ($failure -notmatch 'reparse points') { throw "Unexpected linked-folder error: $failure" }
    $null = Run-Cli @('create', $linked, '--tracker', $url, '--out', $invalid) 1
    [IO.Directory]::Delete($junction)
    $junction = $null

    $seedLog = Join-Path $root 'seed.log'
    $seedErrors = Join-Path $root 'seed.err'
    $seedArgs = "seed `"$torrent`" --folder `"$source`" --port 0 --no-tracker"
    $seed = Start-Process -FilePath $Exe -ArgumentList $seedArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $seedLog -RedirectStandardError $seedErrors
    $peer = ''
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        $line = Get-Content -Raw -LiteralPath $seedLog
        if ($line -match 'Listening on TCP port ([0-9]+)') {
            $peer = "127.0.0.1:$($Matches[1])"
            break
        }
        if ($seed.HasExited) { throw "Folder seeder exited: $([IO.File]::ReadAllText($seedErrors))" }
        Start-Sleep -Milliseconds 50
    }
    if (!$peer) { throw 'Timed out waiting for the folder seeder' }

    $destination = Join-Path $root 'received'
    $null = Run-Cli @('download', $torrent, '--out', $destination, '--peer', $peer)
    Assert-Folder $destination
    $failure = Run-Cli @('download', $torrent, '--out', $destination, '--peer', $peer) 1
    if ($failure -notmatch 'Output already exists') { throw "Expected overwrite protection: $failure" }
    Assert-Folder $destination

    $partialName = ".mytorrent-$infoHash.part"
    $resume = Join-Path $root 'resume'
    $null = New-Item -ItemType Directory -Path $resume
    [IO.File]::WriteAllBytes((Join-Path $resume $partialName), [byte[]]$payload[0..16387])
    $log = Run-Cli @('download', $torrent, '--out', $resume, '--peer', $peer, '--resume')
    if ($log -notmatch 'Reused 1 verified piece\(s\)') { throw "Folder resume lost its verified prefix: $log" }
    Assert-Folder $resume

    $complete = Join-Path $root 'complete'
    $null = New-Item -ItemType Directory -Path $complete
    [IO.File]::WriteAllBytes((Join-Path $complete $partialName), $payload)
    $null = Run-Cli @('download', $torrent, '--out', $complete, '--peer', '127.0.0.1:1', '--resume')
    Assert-Folder $complete
    $null = Run-Cli @('verify', $torrent, '--folder', $source)
    Write-Output 'Folder creation, cross-file verification, transfer, and resume passed.'
} finally {
    if ($seed) {
        if (!$seed.HasExited) { Stop-Process -Id $seed.Id -ErrorAction SilentlyContinue }
        $seed.WaitForExit()
        $seed.Dispose()
    }
    if ($junction -and (Test-Path -LiteralPath $junction)) { [IO.Directory]::Delete($junction) }
    if ($sha) { $sha.Dispose() }
    $resolved = (Resolve-Path -LiteralPath $root).ProviderPath
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^mytorrent-folders-[0-9a-f-]{36}$') { throw 'Unexpected cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
