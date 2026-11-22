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

function Read-Request($Stream, [int]$Offset, [int]$Length, [int]$Piece = 0) {
    $bytes = Read-Exact $Stream 17
    if ([Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($bytes, 0)) -ne 13 -or
        $bytes[4] -ne 6 -or [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($bytes, 5)) -ne $Piece -or
        [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($bytes, 9)) -ne $Offset -or
        [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($bytes, 13)) -ne $Length) {
        throw "Expected request for length $Length at offset $Offset"
    }
}

function Send-Block($Stream, [byte[]]$Payload, [int]$Offset, [int]$Length, [int]$Piece = 0, [int]$PieceLength = 16384) {
    $header = [byte[]]::new(13)
    [BitConverter]::GetBytes([Net.IPAddress]::HostToNetworkOrder([int](9 + $Length))).CopyTo($header, 0)
    $header[4] = 7
    [BitConverter]::GetBytes([Net.IPAddress]::HostToNetworkOrder($Piece)).CopyTo($header, 5)
    [BitConverter]::GetBytes([Net.IPAddress]::HostToNetworkOrder($Offset)).CopyTo($header, 9)
    $Stream.Write($header, 0, $header.Length)
    $Stream.Write($Payload, $Piece * $PieceLength + $Offset, $Length)
}

function Start-Peer($Client, [string]$InfoHash, [byte[]]$Bitfield, [string]$PeerId) {
    $Client.ReceiveTimeout = 4000
    $Client.SendTimeout = 4000
    $stream = $Client.GetStream()
    $handshake = Read-Exact $stream 68
    $hex = ([BitConverter]::ToString($handshake, 28, 20)).Replace('-', '')
    if ($handshake[0] -ne 19 -or $hex -ne $InfoHash -or
        [Text.Encoding]::ASCII.GetString($handshake, 1, 19) -cne 'BitTorrent protocol') {
        throw 'Invalid handshake from client'
    }
    [Text.Encoding]::ASCII.GetBytes($PeerId).CopyTo($handshake, 48)
    $stream.Write($handshake, 0, 12)
    $stream.Write($handshake, 12, 56)

    # Keep-alive, bitfield, and unchoke.
    $control = [byte[]](@(0,0,0,0, 0,0,0,(1 + $Bitfield.Length),5) + $Bitfield + @(0,0,0,1,1))
    $stream.Write($control, 0, $control.Length)
    $interest = Read-Exact $stream 5
    if ([BitConverter]::ToString($interest) -ne '00-00-00-01-02') {
        throw 'Expected interested message'
    }
    return $stream
}
