param([string]$Root, [string]$InfoHash)
$ErrorActionPreference = 'Stop'

function Read-Exact($Stream, [int]$Length) {
    $bytes = [byte[]]::new($Length)
    $position = 0
    while ($position -lt $Length) {
        $count = $Stream.Read($bytes, $position, $Length - $position)
        if (!$count) { throw 'Unexpected EOF from BitTorrent client' }
        $position += $count
    }
    return ,$bytes
}

$server = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$server.Start()
try {
    [IO.File]::WriteAllText((Join-Path $Root 'fixture.port'), [string]$server.LocalEndpoint.Port)
    foreach ($mode in @('valid', 'corrupt', 'wrong-offset', 'oversized')) {
        for ($attempts = 0; !$server.Pending(); $attempts++) {
            if ($attempts -ge 200) { throw "No fixture connection for $mode" }
            Start-Sleep -Milliseconds 50
        }
        $client = $server.AcceptTcpClient()
        try {
            $client.ReceiveTimeout = 3000
            $client.SendTimeout = 3000
            $stream = $client.GetStream()
            $handshake = Read-Exact $stream 68
            $hex = ([BitConverter]::ToString($handshake, 28, 20)).Replace('-', '')
            if ($handshake[0] -ne 19 -or
                [Text.Encoding]::ASCII.GetString($handshake, 1, 19) -cne 'BitTorrent protocol' -or
                $hex -ne $InfoHash) { throw 'Invalid handshake from client' }
            [Text.Encoding]::ASCII.GetBytes('-FP0001-abcdefghijkl').CopyTo($handshake, 48)
            $stream.Write($handshake, 0, 12)
            $stream.Write($handshake, 12, 56)

            # Keep-alive, one-piece bitfield, and unchoke.
            $control = [byte[]]@(0,0,0,0, 0,0,0,2,5,128, 0,0,0,1,1)
            $stream.Write($control, 0, $control.Length)
            $interest = Read-Exact $stream 5
            if ([BitConverter]::ToString($interest) -ne '00-00-00-01-02') {
                throw 'Expected interested message'
            }
            $request = Read-Exact $stream 17
            if ([BitConverter]::ToString($request) -ne '00-00-00-0D-06-00-00-00-00-00-00-00-00-00-00-00-03') {
                throw 'Expected a request for all three payload bytes'
            }
            $response = [byte[]]@(0,0,0,12, 7, 0,0,0,0, 0,0,0,0, 97,98,99)
            switch ($mode) {
                'corrupt' { $response[15] = 100 }
                'wrong-offset' { $response[12] = 1 }
                'oversized' { $response = [byte[]]@(127,255,255,255) }
            }
            $stream.Write($response, 0, $response.Length)
        } finally { $client.Dispose() }
    }
} finally { $server.Stop() }
