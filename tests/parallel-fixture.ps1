param([string]$Root, [string]$PayloadPath, [string]$WireScript)
$ErrorActionPreference = 'Stop'
. $WireScript

function Listener {
    $socket = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $socket.Start()
    return $socket
}

function Accept-Peer($Listener) {
    for ($attempt = 0; !$Listener.Pending(); $attempt++) {
        if ($attempt -ge 120) { throw 'Expected concurrent connection did not arrive' }
        Start-Sleep -Milliseconds 25
    }
    return $Listener.AcceptTcpClient()
}

function Announce($Tracker, [byte[]]$Peers, [string]$Event) {
    $client = Accept-Peer $Tracker
    try {
        $client.ReceiveTimeout = 4000
        $stream = $client.GetStream()
        $reader = [IO.StreamReader]::new($stream)
        $line = $reader.ReadLine()
        do { $header = $reader.ReadLine() } while ($header)
        if ($line -cnotmatch '^GET /announce\?[^ ]+ HTTP/' -or $line -cnotmatch "event=$Event HTTP/") {
            throw "Unexpected announce: $line"
        }
        $size = (Get-Item -LiteralPath $PayloadPath).Length
        if ($Event -eq 'completed' -and $line -cnotmatch "downloaded=$size&left=0&") {
            throw "Unexpected completion counters: $line"
        }
        $encoding = [Text.Encoding]::GetEncoding(28591)
        $body = "d8:intervali30e5:peers$($Peers.Length):" + $encoding.GetString($Peers) + 'e'
        $reply = $encoding.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n$body")
        $stream.Write($reply, 0, $reply.Length)
    } finally { $client.Dispose() }
}

$tracker = Listener
$first = Listener
$second = Listener
$backup = Listener
$clients = @()
try {
    [IO.File]::WriteAllText((Join-Path $Root 'tracker.port'), [string]$tracker.LocalEndpoint.Port)
    $peers = foreach ($listener in @($first, $second, $backup, $second)) {
        $port = $listener.LocalEndpoint.Port
        127, 0, 0, 1, ($port -shr 8), ($port -band 255)
    }
    Announce $tracker ([byte[]]$peers) 'started'
    $infoHash = [IO.File]::ReadAllText((Join-Path $Root 'info.hash'))
    $payload = [IO.File]::ReadAllBytes($PayloadPath)

    $one = Accept-Peer $first
    $clients += $one
    $a = Start-Peer $one $infoHash @(128,0) '-FP0001-aaaaaaaaaaaa'
    Read-Request $a 0 16384

    # Do not finish the first request. A sequential client cannot make it here.
    $two = Accept-Peer $second
    $clients += $two
    $b = Start-Peer $two $infoHash @(85,64) '-FP0001-bbbbbbbbbbbb'
    Read-Request $b 0 16384 1
    Send-Block $b $payload 0 16384 1

    # Have a different connection finish the outstanding first piece.
    $one.Dispose()
    $three = Accept-Peer $backup
    $clients += $three
    $c = Start-Peer $three $infoHash @(170,128) '-FP0001-cccccccccccc'
    Read-Request $c 0 16384
    Send-Block $c $payload 0 16384

    # The remaining peers advertise complementary pieces. Pass the initial eight slots.
    for ($piece = 2; $piece -lt 10; $piece++) {
        $stream = if ($piece % 2 -eq 0) { $c } else { $b }
        $length = [Math]::Min(16384, $payload.Length - $piece * 16384)
        Read-Request $stream 0 $length $piece
        Send-Block $stream $payload 0 $length $piece
    }

    Announce $tracker ([byte[]]@()) 'completed'
    Announce $tracker ([byte[]]@()) 'stopped'
    if ($second.Pending()) { throw 'Client connected to a duplicate tracker endpoint' }
} finally {
    foreach ($client in $clients) { $client.Dispose() }
    foreach ($listener in @($tracker, $first, $second, $backup)) { $listener.Stop() }
}
