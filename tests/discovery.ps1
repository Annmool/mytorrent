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
    $tracker = Start-Job -ArgumentList $root -ScriptBlock {
        param($root)
        $ErrorActionPreference = 'Stop'
        $server = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $server.Start()
        [IO.File]::WriteAllText((Join-Path $root 'tracker.port'), [string]$server.LocalEndpoint.Port)
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $encoding = [Text.Encoding]::GetEncoding(28591)
        $downloader = ''
        $seedPort = $retries = $last = 0
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
                    if ($query['port'] -ne '0') {
                        $seedPort = [int]$query['port']
                    } elseif ($query['event'] -eq 'started') {
                        if ($downloader -or $seedPort) { throw 'Expected one started event before seeding' }
                        $downloader = $query['peer_id']
                        $last = $clock.ElapsedMilliseconds
                        [IO.File]::WriteAllText((Join-Path $root 'initial'), 'empty')
                    } elseif ($query['peer_id'] -cne $downloader) {
                        throw 'Peer ID changed between announces'
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
                        [IO.File]::WriteAllText((Join-Path $root 'finished'), 'complete')
                    } else { throw "Unexpected event: $line" }
                    $reply = $encoding.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n$body")
                    $stream.Write($reply, 0, $reply.Length)
                } finally { $client.Dispose() }
            }
        } finally { $server.Stop() }
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
    Write-Output 'Downloader found the late seed without restarting and honored both tracker intervals.'
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
