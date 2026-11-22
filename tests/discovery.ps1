param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('mytorrent-discovery-' + [guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $root
$tracker = $null
$seed = $null
$download = $null

function Wait-Marker([string]$Name) {
    $file = Join-Path $root $Name
    for ($attempt = 0; $attempt -lt 160; $attempt++) {
        if ((Test-Path -LiteralPath $file) -and (Get-Item -LiteralPath $file).Length) {
            return [IO.File]::ReadAllText($file)
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out waiting for $Name"
}

try {
    $wire = Join-Path $PSScriptRoot 'peer-wire.ps1'
    $tracker = Start-Job -ArgumentList $root, $wire -ScriptBlock {
        param($root, $wire)
        $ErrorActionPreference = 'Stop'
        . $wire

        function Connect-Downloader($Query, [int]$Port) {
            $peer = [Net.Sockets.TcpClient]::new()
            try {
                $peer.ReceiveTimeout = 2500
                $peer.SendTimeout = 2500
                $peer.Connect([Net.IPAddress]::Loopback, $Port)
                $stream = $peer.GetStream()
                $handshake = [byte[]]::new(68)
                $handshake[0] = 19
                [Text.Encoding]::ASCII.GetBytes('BitTorrent protocol').CopyTo($handshake, 1)
                $hash = [byte[]]@($Query['info_hash'].Substring(1).Split('%') | ForEach-Object {
                    [Convert]::ToByte($_, 16)
                })
                $hash.CopyTo($handshake, 28)
                [Text.Encoding]::ASCII.GetBytes('-FIX001-InboundProbe').CopyTo($handshake, 48)
                $stream.Write($handshake, 0, $handshake.Length)
                $reply = Read-Exact $stream 68
                $peerId = '%' + ([BitConverter]::ToString($reply, 48, 20)).Replace('-', '%')
                if ($reply[0] -ne 19 -or
                    [Text.Encoding]::ASCII.GetString($reply, 1, 19) -cne 'BitTorrent protocol' -or
                    [BitConverter]::ToString($reply, 28, 20) -cne [BitConverter]::ToString($hash) -or
                    $peerId -ine $Query['peer_id']) {
                    throw 'Handshake did not match the tracker announce'
                }
                return $peer
            } catch {
                $peer.Dispose()
                throw
            }
        }
        $server = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $server.Start()
        [IO.File]::WriteAllText((Join-Path $root 'tracker.port'), [string]$server.LocalEndpoint.Port)
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $encoding = [Text.Encoding]::GetEncoding(28591)
        $downloader = ''
        $seedPort = $downloadPort = $retries = $last = 0
        $idlePeer = $null
        $completed = $false
        try {
            while (!(Test-Path (Join-Path $root 'tracker.stop'))) {
                if (!$server.Pending()) { Start-Sleep -Milliseconds 20; continue }
                $client = $server.AcceptTcpClient()
                try {
                    $client.ReceiveTimeout = 2500
                    $stream = $client.GetStream()
                    $reader = [IO.StreamReader]::new($stream)
                    $line = $reader.ReadLine()
                    do { $header = $reader.ReadLine() } while ($header)
                    if ($line -notmatch '^GET /announce\?([^ ]+) HTTP/') { throw "Invalid request: $line" }
                    $query = @{}
                    foreach ($pair in $Matches[1].Split('&')) {
                        $key, $value = $pair.Split('=', 2)
                        $query[$key] = $value
                    }
                    $body = 'd8:intervali1e5:peers0:e'
                    $announcePort = 0
                    if (!([int]::TryParse($query['port'], [ref]$announcePort)) -or
                        $announcePort -lt 1 -or $announcePort -gt 65535) {
                        $body = 'd14:failure reason20:Invalid tracker porte'
                    } elseif (!$downloader) {
                        if ($query['event'] -ne 'started' -or $query['left'] -ne '66019') {
                            throw 'Expected the downloader to announce first'
                        }
                        $downloader = $query['peer_id']
                        $downloadPort = $announcePort
                        $probe = Connect-Downloader $query $downloadPort
                        $probe.Dispose()
                        $last = $clock.ElapsedMilliseconds
                        [IO.File]::WriteAllText((Join-Path $root 'initial'), 'empty')
                    } elseif ($query['peer_id'] -cne $downloader) {
                        if ($query['left'] -ne '0' -or $announcePort -eq $downloadPort -or
                            (!$seedPort -and $query['event'] -ne 'started') -or
                            ($seedPort -and $seedPort -ne $announcePort)) {
                            throw 'Unexpected peer ID or seeder port'
                        }
                        $seedPort = $announcePort
                    } elseif ($announcePort -ne $downloadPort) {
                        throw 'Downloader changed ports between announces'
                    } elseif (!$query.ContainsKey('event')) {
                        $minimum = if ($retries -eq 0) { 1000 } else { 2000 }
                        $now = $clock.ElapsedMilliseconds
                        if ($now - $last -lt $minimum) { throw 'Retry ignored tracker interval' }
                        $last = $now
                        $retries++
                        if ($retries -eq 1) {
                            $body = 'd8:intervali2e5:peers0:e'
                        } else {
                            if (!$seedPort) { throw 'Seeder did not register' }
                            $packed = $encoding.GetString([byte[]]@(127,0,0,1,($seedPort -shr 8),($seedPort -band 255)))
                            $body = "d8:intervali2e5:peers6:${packed}e"
                        }
                    } elseif ($query['event'] -eq 'completed') {
                        if ($retries -ne 2 -or $query['left'] -ne '0' -or $query['downloaded'] -ne '66019') {
                            throw 'Unexpected completion event or counters'
                        }
                        $completed = $true
                    } elseif ($query['event'] -eq 'stopped') {
                        if (!$completed) { throw 'Stopped before completion' }
                        # Leave this peer idle to check that shutdown does not wait for its read timeout.
                        $idlePeer = Connect-Downloader $query $downloadPort
                        [IO.File]::WriteAllText((Join-Path $root 'finished'), 'complete')
                    } else { throw "Unexpected event: $line" }
                    $reply = $encoding.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n$body")
                    $stream.Write($reply, 0, $reply.Length)
                } finally { $client.Dispose() }
            }
        } finally {
            if ($idlePeer) { $idlePeer.Dispose() }
            $server.Stop()
        }
    }

    $port = Wait-Marker 'tracker.port'
    $payload = [byte[]]::new(66019)
    [Random]::new(107).NextBytes($payload)
    $source = Join-Path $root 'late.bin'
    $torrent = Join-Path $root 'late.torrent'
    $out = Join-Path $root 'received'
    [IO.File]::WriteAllBytes($source, $payload)
    & $Exe create $source --tracker "http://127.0.0.1:$port/announce" --out $torrent --piece-size 32768
    if ($LASTEXITCODE) { throw 'Cannot create test torrent' }
    $log = Join-Path $root 'download.log'
    $errors = Join-Path $root 'download.err'
    $download = Start-Process -FilePath $Exe -ArgumentList "download `"$torrent`" --out `"$out`"" `
        -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError $errors -PassThru
    $null = Wait-Marker 'initial'
    $seed = Start-Process -FilePath $Exe -ArgumentList "seed `"$torrent`" --file `"$source`" --port 0" `
        -WindowStyle Hidden -RedirectStandardOutput (Join-Path $root 'seed.log') -PassThru
    $null = Wait-Marker 'finished'
    if (!$download.WaitForExit(3000) -or $download.ExitCode -ne 0) { throw 'Late-seed transfer failed' }
    if ((Get-FileHash -LiteralPath $source).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $out 'late.bin')).Hash) { throw 'Output differs from source' }
    $messages = @(Select-String -LiteralPath $log -SimpleMatch 'Waiting for peers')
    if ($messages.Count -ne 2) { throw 'Expected two waiting messages' }
    Write-Output 'Strict tracker accepted the live download port and the downloader found the late seed.'
} catch {
    if ($tracker) { Receive-Job $tracker -ErrorAction Continue }
    if ($log -and (Test-Path -LiteralPath $log)) { Get-Content -LiteralPath $log }
    if ($errors -and (Test-Path -LiteralPath $errors)) { Get-Content -LiteralPath $errors }
    throw
} finally {
    foreach ($child in @($seed, $download)) {
        if ($child) {
            if (!$child.HasExited) { Stop-Process -Id $child.Id -ErrorAction SilentlyContinue }
            $child.WaitForExit()
            $child.Dispose()
        }
    }
    if ($tracker) {
        [IO.File]::WriteAllText((Join-Path $root 'tracker.stop'), 'stop')
        $null = Wait-Job $tracker -Timeout 4
        Remove-Job $tracker -Force
    }
    $resolved = (Resolve-Path -LiteralPath $root).ProviderPath
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^mytorrent-discovery-[0-9a-f-]{36}$') { throw 'Unexpected cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
