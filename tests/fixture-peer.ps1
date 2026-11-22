param([string]$Root, [string]$InfoHash, [string]$PipelineHash, [string]$PipelineFile, [string]$ResumeHash, [string]$ResumeFile, [string]$WireScript)
$ErrorActionPreference = 'Stop'

. $WireScript

$server = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$server.Start()
try {
    [IO.File]::WriteAllText((Join-Path $Root 'fixture.port'), [string]$server.LocalEndpoint.Port)
    foreach ($mode in @('valid', 'corrupt', 'wrong-offset', 'oversized', 'pipelined', 'duplicate', 'interrupt', 'resumed', 'repair')) {
        for ($attempts = 0; !$server.Pending(); $attempts++) {
            if ($attempts -ge 200) { throw "No fixture connection for $mode" }
            Start-Sleep -Milliseconds 50
        }
        $client = $server.AcceptTcpClient()
        try {
            $pipeline = $mode -in @('pipelined', 'duplicate')
            $resume = $mode -in @('interrupt', 'resumed', 'repair')
            $expectedHash = if ($resume) { $ResumeHash } elseif ($pipeline) { $PipelineHash } else { $InfoHash }
            $bits = if ($resume) { 192 } else { 128 }
            $stream = Start-Peer $client $expectedHash $bits '-FP0001-abcdefghijkl'
            if ($resume) {
                $payload = [IO.File]::ReadAllBytes($ResumeFile)
                if ($mode -ne 'resumed') {
                    Read-Request $stream 0 16384
                    Send-Block $stream $payload 0 16384
                }
                $tail = $payload.Length - 16384
                Read-Request $stream 0 $tail 1
                if ($mode -ne 'interrupt') { Send-Block $stream $payload 0 $tail 1 }
                continue
            }
            if ($pipeline) {
                $payload = [IO.File]::ReadAllBytes($PipelineFile)
                $blockSize = 16384
                # Wait for the entire first window before sending any data.
                for ($i = 0; $i -lt 8; $i++) { Read-Request $stream ($i * $blockSize) $blockSize }
                Send-Block $stream $payload (7 * $blockSize) $blockSize
                $tailOffset = 8 * $blockSize
                $tailLength = $payload.Length - $tailOffset
                # The client must refill the slot while earlier replies are pending.
                Read-Request $stream $tailOffset $tailLength
                if ($mode -eq 'duplicate') {
                    Send-Block $stream $payload (7 * $blockSize) $blockSize
                    continue
                }
                Send-Block $stream $payload $tailOffset $tailLength
                for ($i = 6; $i -ge 0; $i--) {
                    Send-Block $stream $payload ($i * $blockSize) $blockSize
                }
                continue
            }
            Read-Request $stream 0 3
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
