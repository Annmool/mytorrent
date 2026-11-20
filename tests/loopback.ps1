param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('mytorrent-test-' + [guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $root
$seed = $null
$tracker = $null
$fixture = $null
function Run([string[]]$Arguments, [int]$Expected = 0) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne $Expected) { throw "Unexpected exit code for $Arguments" }
}
function Wait-File([string]$Path) {
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if ((Test-Path $Path) -and (Get-Item $Path).Length -gt 0) { return }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Path"
}
function Assert-Hash([string]$Path, [string]$Hash) {
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Hash) {
        throw "Different payload in $Path"
    }
}
try {
    # The fixture tracker talks HTTP independently of the C++ client.
    $tracker = Start-Job -ArgumentList $root -ScriptBlock {
        param($root)
        $server = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $server.Start()
        [IO.File]::WriteAllText((Join-Path $root 'tracker.port'), [string]$server.LocalEndpoint.Port)
        $seedPort = 0
        $infoHash = ''
        $encoding = [Text.Encoding]::GetEncoding(28591)
        try {
            while (!(Test-Path (Join-Path $root 'tracker.stop'))) {
                if (!$server.Pending()) { Start-Sleep -Milliseconds 50; continue }
                $client = $server.AcceptTcpClient()
                try {
                    $client.ReceiveTimeout = 3000
                    $stream = $client.GetStream()
                    $reader = [IO.StreamReader]::new($stream)
                    $line = $reader.ReadLine()
                    do { $header = $reader.ReadLine() } while ($header)
                    if ($line -notmatch '^GET /announce\?([^ ]+) HTTP/') { throw "Malformed request: $line" }
                    $query = @{}
                    foreach ($pair in $Matches[1].Split('&')) {
                        $key, $value = $pair.Split('=', 2)
                        $query[$key] = $value
                    }
                    if ($query['info_hash'] -notmatch '^(%[0-9A-F]{2}){20}$' -or
                        $query['peer_id'] -notmatch '^(%[0-9A-F]{2}){20}$') { throw 'Binary query was not preserved' }
                    if ($query['event'] -eq 'started' -and $query['left'] -eq '0') {
                        $seedPort = [int]$query['port']
                        $infoHash = $query['info_hash']
                        [IO.File]::WriteAllText((Join-Path $root 'seed.registered'), [string]$seedPort)
                    }
                    if ($query['event'] -eq 'completed' -and $query['left'] -eq '0') {
                        [IO.File]::WriteAllText((Join-Path $root 'download.completed'), $query['downloaded'])
                    }
                    $body = 'd8:intervali30e5:peers0:e'
                    if ($seedPort -and $query['info_hash'] -eq $infoHash -and $query['left'] -ne '0') {
                        $packed = $encoding.GetString([byte[]]@(127, 0, 0, 1, ($seedPort -shr 8), ($seedPort -band 255)))
                        $body = "d8:intervali30e5:peers6:${packed}e"
                    }
                    $reply = $encoding.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n$body")
                    $stream.Write($reply, 0, $reply.Length)
                } finally { $client.Dispose() }
            }
        } finally { $server.Stop() }
    }
    $portFile = Join-Path $root 'tracker.port'
    Wait-File $portFile
    $trackerPort = [IO.File]::ReadAllText($portFile)
    $source = Join-Path $root 'example file.bin'
    $torrent = Join-Path $root 'example.torrent'
    $payload = [byte[]]::new(82513)
    [Random]::new(17).NextBytes($payload)
    [IO.File]::WriteAllBytes($source, $payload)
    $hash = (Get-FileHash -LiteralPath $source).Hash
    $url = "http://127.0.0.1:$trackerPort/announce"
    Run @('create', $source, '--tracker', $url, '--out', $torrent, '--piece-size', '32768')
    Run @('inspect', $torrent)
    Run @('verify', $torrent, '--file', $source)
    Run @('create', $source, '--tracker', $url, '--out', $torrent) 1
    $bad = Join-Path $root 'bad.bin'
    $payload[100] = $payload[100] -bxor 255
    [IO.File]::WriteAllBytes($bad, $payload)
    Run @('verify', $torrent, '--file', $bad) 1
    Run @('seed', $torrent, '--file', $bad, '--no-tracker') 1

    $log = Join-Path $root 'seed.log'
    $errors = Join-Path $root 'seed-error.log'
    $seed = Start-Process -FilePath $Exe -ArgumentList "seed `"$torrent`" --file `"$source`" --port 0" `
        -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError $errors -PassThru
    $registered = Join-Path $root 'seed.registered'
    Wait-File $registered
    $peer = '127.0.0.1:' + [IO.File]::ReadAllText($registered)

    $direct = Join-Path $root 'direct'
    Run @('download', $torrent, '--out', $direct, '--peer', $peer)
    Assert-Hash (Join-Path $direct 'example file.bin') $hash
    Run @('download', $torrent, '--out', $direct, '--peer', $peer) 1
    Assert-Hash (Join-Path $direct 'example file.bin') $hash

    $tracked = Join-Path $root 'tracked'
    Run @('download', $torrent, '--out', $tracked)
    Assert-Hash (Join-Path $tracked 'example file.bin') $hash
    $completed = Join-Path $root 'download.completed'
    Wait-File $completed
    if ([int][IO.File]::ReadAllText($completed) -ne $payload.Length) { throw 'Incorrect download counter' }

    # Valid metadata for a different torrent must fail at the peer handshake.
    $other = Join-Path $root 'other.torrent'
    Run @('create', $bad, '--tracker', $url, '--out', $other)
    $failed = Join-Path $root 'failed'
    Run @('download', $other, '--out', $failed, '--peer', $peer) 1
    if (Get-ChildItem -Force $failed) { throw 'Failed download left output behind' }

    $small = Join-Path $root 'small.bin'
    $smallTorrent = Join-Path $root 'small.torrent'
    [IO.File]::WriteAllBytes($small, [byte[]]@(97,98,99))
    Run @('create', $small, '--tracker', $url, '--out', $smallTorrent)
    $inspection = & $Exe inspect $smallTorrent
    if ($LASTEXITCODE) { throw 'Cannot inspect fixture torrent' }
    $hashLine = $inspection | Where-Object { $_ -match '^Info hash: [0-9a-f]{40}$' }
    if (!$hashLine) { throw 'Missing info hash' }
    $fixture = Start-Job -FilePath (Join-Path $PSScriptRoot 'fixture-peer.ps1') -ArgumentList $root, $hashLine.Substring(11)
    $fixturePort = Join-Path $root 'fixture.port'
    Wait-File $fixturePort
    $fixturePeer = '127.0.0.1:' + [IO.File]::ReadAllText($fixturePort)
    foreach ($mode in @('valid', 'corrupt', 'wrong-offset', 'oversized')) {
        $out = Join-Path $root $mode
        if ($mode -eq 'valid') {
            Run @('download', $smallTorrent, '--out', $out, '--peer', $fixturePeer)
            Assert-Hash (Join-Path $out 'small.bin') (Get-FileHash -LiteralPath $small).Hash
        } else {
            Run @('download', $smallTorrent, '--out', $out, '--peer', $fixturePeer) 1
            if (Get-ChildItem -Force $out) { throw "$mode left output behind" }
        }
    }
    $null = Wait-Job $fixture -Timeout 5
    if ($fixture.State -ne 'Completed') { throw "Fixture peer failed with state $($fixture.State)" }
} catch {
    if ($fixture) { Receive-Job $fixture -ErrorAction Continue }
    if ($tracker) { Receive-Job $tracker -ErrorAction Continue }
    if ($errors -and (Test-Path $errors)) { Get-Content $errors }
    throw
} finally {
    if ($fixture) { Remove-Job $fixture -Force }
    if ($seed) { Stop-Process -Id $seed.Id -ErrorAction SilentlyContinue; $seed.WaitForExit(); $seed.Dispose() }
    if ($tracker) {
        [IO.File]::WriteAllText((Join-Path $root 'tracker.stop'), 'stop')
        $null = Wait-Job $tracker -Timeout 5
        Remove-Job $tracker -Force
    }
    $resolved = (Resolve-Path -LiteralPath $root).ProviderPath
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^mytorrent-test-[0-9a-f-]{36}$') {
        throw "Unexpected test cleanup path: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
