param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('mytorrent-parallel-' + [guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $root
$fixture = $null
try {
    $source = Join-Path $root 'parallel.bin'
    $torrent = Join-Path $root 'parallel.torrent'
    $payload = [byte[]]::new(9 * 16384 + 73)
    [Random]::new(65).NextBytes($payload)
    [IO.File]::WriteAllBytes($source, $payload)
    $fixture = Start-Job -FilePath (Join-Path $PSScriptRoot 'parallel-fixture.ps1') -ArgumentList $root, $source, (Join-Path $PSScriptRoot 'peer-wire.ps1')
    $portPath = Join-Path $root 'tracker.port'
    for ($attempt = 0; !(Test-Path $portPath) -or !(Get-Item $portPath).Length; $attempt++) {
        if ($attempt -ge 100) { throw 'Tracker did not start' }
        Start-Sleep -Milliseconds 50
    }
    $port = [IO.File]::ReadAllText($portPath)
    & $Exe create $source --tracker "http://127.0.0.1:$port/announce" --out $torrent --piece-size 16384
    if ($LASTEXITCODE) { throw 'Cannot create parallel fixture torrent' }
    $inspection = & $Exe inspect $torrent
    if ($LASTEXITCODE) { throw 'Cannot inspect parallel fixture torrent' }
    $hashLine = $inspection | Where-Object { $_ -match '^Info hash: [0-9a-f]{40}$' }
    if (!$hashLine) { throw 'Missing parallel fixture info hash' }
    [IO.File]::WriteAllText((Join-Path $root 'info.hash'), $hashLine.Substring(11))

    $out = Join-Path $root 'download'
    & $Exe download $torrent --out $out --resume
    if ($LASTEXITCODE) { throw 'Parallel download failed' }
    if ((Get-FileHash -LiteralPath $source).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $out 'parallel.bin')).Hash) {
        throw 'Parallel output does not match source'
    }
    if (Get-ChildItem -Force -LiteralPath $out -Filter '*.part') { throw 'Completed transfer left a partial file' }
    $null = Wait-Job $fixture -Timeout 5
    if ($fixture.State -ne 'Completed') { throw "Parallel fixture ended with state $($fixture.State)" }
    Receive-Job $fixture
} catch {
    if ($fixture) { Receive-Job $fixture -ErrorAction Continue }
    throw
} finally {
    if ($fixture) { Remove-Job $fixture -Force }
    $resolved = (Resolve-Path -LiteralPath $root).ProviderPath
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^mytorrent-parallel-[0-9a-f-]{36}$') {
        throw "Unexpected test cleanup path: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
