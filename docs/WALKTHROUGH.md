# Read the implementation in transfer order

## 1. Create a map of the file

Start with **src/main.cpp**. The entry point is wmain so Windows passes paths as Unicode. Options checks the command's allowed flags, rejects repeats, and validates numeric values. The entry point dispatches to the relevant module and converts exceptions into an error message and exit code 1.

**src/metainfo.cpp** starts with the file on disk. create_metainfo reads at most one piece at a time. The default piece size is 256 KiB. If the file has 600 KiB, it produces three hashes for pieces of 256, 256, and 88 KiB. Each digest occupies 20 raw bytes in the pieces field. hex is for display, and the digest field stays binary. BCrypt wraps Windows' cryptographic API, and peer_id creates a random identifier with an eight-byte client prefix.

**src/bencode.cpp** writes the outer tracker field and info dictionary. Bencode represents a byte string as its decimal byte length, a colon, and the bytes. For example, 3:cat is the string cat. Integers use i and e delimiters, while lists and dictionaries use l or d plus a closing e. Value stores those four types. Parser::parse consumes one value recursively, records its byte offsets, and limits the number of values and the nesting depth. The encoder visits those same four types and emits dictionary keys in order.

**Read the code:** Trace create in main.cpp into create_metainfo and encode. Then trace inspect through load_metainfo and parse_metainfo. The piece digest verifies payload bytes. The info hash identifies the torrent and hashes the exact encoded info value, including its delimiters. This is the hash used when talking to peers and the tracker.

## 2. Find a peer

**src/tracker.cpp** sends the info hash, peer ID, port, and transfer counters. escape_query percent-encodes each byte with two hex digits so arbitrary digest and peer ID bytes survive the URL. WinHTTP handles HTTP framing and HTTPS. parse_tracker_reply understands dictionary peer entries and compact records, where four bytes are the IPv4 address and two are the port in network byte order.

The seeder announces that it has zero bytes left. The tracker responds to a downloader with candidate endpoints. Files flow over peer connections. For a local experiment, an explicit --peer skips discovery entirely.

**src/discovery.cpp** handles an empty tracker response. It displays the suggested wait time, then announces again without repeating the started event. A new empty reply can supply a different interval. The scoped console interrupt handler lets Ctrl+C leave the wait, after which the download sends a stopped announce.

## 3. Connect and exchange messages

**src/net.cpp** wraps Winsock in Network and Socket. Constructors acquire resources and destructors release them. The sockets are nonblocking during transfer. wait_for waits until the socket is ready and shares a deadline across the partial reads or writes for one operation. receive_into reconstructs the requested byte span even when recv returns it in fragments.

**src/peer.cpp** sends and validates the 68-byte v1 handshake. Both sides must agree on the info hash. The seeder follows with a bitfield, a compact list of which pieces it has. The downloader declares interest and waits for an unchoke before requesting data. word and number encode and decode four-byte network-order integers. next reads the length prefix, rejects oversized frames, then reads the body. A zero-length frame is a keep-alive.

**Read the code:** Follow Downloader::get_piece. Each request names the piece index, the offset within that piece, and the number of bytes. The downloader keeps up to eight requests outstanding and sends another when a valid reply frees a slot. A full 256 KiB piece still takes sixteen 16 KiB requests. Sending several before waiting reduces the time spent waiting for round trips.

The response repeats the piece index and offset before the actual block. next_offset marks the first byte that has yet to be requested. An earlier, block-aligned offset refers to a request already sent for this piece. The completion bitmap tracks which slots already received data, so duplicate replies are rejected. A valid block is copied to its offset in the piece buffer, which restores the correct byte order even when replies arrive out of order.

## 4. Verify and publish the payload

Downloader::get_piece assembles blocks into one buffer and compares its SHA-1 digest with the expected piece digest. The scheduler receives that buffer after validation. The transfer entry points in **src/transfer.cpp** connect peer discovery, scheduling, and storage.

**src/storage.cpp** handles exact file reads, local verification, and output publishing. OutputFile uses an exclusive handle for the partial file. publish flushes and renames it while still holding that handle, with replacement disabled. The destructor removes incomplete output on ordinary failure unless resume was requested. The filename from metadata passes safe_name in metainfo.cpp before it becomes part of the destination path.

For uploads, seed verifies the full source once, then upload checks every requested range before seeking to the requested offset. A background thread handles tracker re-announces so a peer transfer can continue while the tracker responds.

## 5. Assign pieces to multiple peers

**src/scheduler.cpp** starts up to four workers. Each worker owns a TCP connection and reads the peer's bitfield before asking for work. The scheduler holds a mutex while choosing an unassigned piece that peer has. Workers download outside the mutex, so several peers can make progress at once.

The scheduler considers up to eight pieces starting at the next write position. This bounds the amount of out-of-order data in memory. If a later piece arrives first, it waits in its slot. When the gap closes, the scheduler writes the contiguous verified pieces in order. If a peer disconnects or supplies invalid data, the scheduler releases the assignment, allowing another peer that has the piece to claim it.

## 6. Pick up after a restart

The downloader writes pieces sequentially, so progress is the verified prefix at the start of the partial file. With the resume flag, the info hash gives the partial file a stable name for this torrent. A process opens it exclusively, so only one downloader can update that partial at a time.

OutputFile::recheck walks the file in piece order. The same piece hashes used during download decide which bytes can be reused. The first incomplete or mismatched piece ends the verified prefix. The function truncates everything after that boundary and positions the file handle for the next write. download starts from the index returned by recheck and announces only the remaining bytes. If every piece verifies, it publishes the completed partial without contacting peers.

## 7. Read the tests

- **tests/unit.cpp** covers binary bencoding, malformed metadata, UTF-8 and Windows filename checks, SHA-1, piece math, local verification, and tracker reply formats.
- **tests/loopback.ps1** generates its own files and runs the actual CLI. It checks direct transfer, tracker discovery, completion counters, SHA-256 equality, refusal to overwrite, and rejection of a mismatched handshake.
- **tests/fixture-peer.ps1** independently writes and reads wire messages. It checks a valid block, then sends a corrupt block, a wrong block offset, and an oversized length prefix. It also waits for eight requests before sending any data, verifies that the client refills the window, and sends replies out of order. The integration script checks byte-for-byte output and verifies that failures, including duplicate replies, leave the destination empty by default. Resume cases interrupt a transfer after one piece, check which piece is requested after restarting, and cover incomplete tails, corrupt saved data, exclusive access, and publication without a peer.

- **tests/parallel.ps1** uses three independent fixture peers. It withholds the first response until a second connection requests another piece, then drops the first peer and finishes through a backup. The surviving peers supply complementary pieces past the initial eight-piece window. The test checks the final file, tracker counters, and duplicate endpoint handling.

- **tests/discovery.ps1** starts the downloader before the seed, verifies two tracker intervals and the announce events, and compares the downloaded file with the source.

To see piece count change, create torrents for the same file with different --piece-size values and inspect both. Then modify one byte of a copy and run verify against the original torrent. The first failing piece index tells you which region changed.
