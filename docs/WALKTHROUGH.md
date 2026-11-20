# Read the implementation in transfer order

## 1. Create a map of the file

Start with **src/main.cpp**. The entry point is wmain so Windows passes paths as Unicode. Options checks the command's allowed flags, rejects repeats, and validates numeric values. The entry point dispatches to the relevant module and converts exceptions into an error message and exit code 1.

**src/metainfo.cpp** starts with the file on disk. create_metainfo reads at most one piece at a time. The default piece size is 256 KiB. If the file has 600 KiB, it produces three hashes for pieces of 256, 256, and 88 KiB. Each digest occupies 20 raw bytes in the pieces field. hex is for display, and the digest field stays binary. BCrypt wraps Windows' cryptographic API, and peer_id creates a random identifier with an eight-byte client prefix.

**src/bencode.cpp** writes the outer tracker field and info dictionary. Bencode represents a byte string as its decimal byte length, a colon, and the bytes. For example, 3:cat is the string cat. Integers use i and e delimiters, while lists and dictionaries use l or d plus a closing e. Value stores those four types. Parser::parse consumes one value recursively, records its byte offsets, and limits the number of values and the nesting depth. The encoder visits those same four types and emits dictionary keys in order.

**Read the code:** Trace create in main.cpp into create_metainfo and encode. Then trace inspect through load_metainfo and parse_metainfo. The piece digest verifies payload bytes. The info hash identifies the torrent and hashes the exact encoded info value, including its delimiters. This is the hash used when talking to peers and the tracker.

## 2. Find a peer

**src/tracker.cpp** sends the info hash, peer ID, port, and transfer counters. escape_query percent-encodes each byte with two hex digits so arbitrary digest and peer ID bytes survive the URL. WinHTTP handles HTTP framing and HTTPS. parse_tracker_reply understands dictionary peer entries and compact records, where four bytes are the IPv4 address and two are the port in network byte order.

The seeder announces that it has zero bytes left. The tracker responds to a downloader with candidate endpoints. Files flow over peer connections. For a local experiment, an explicit --peer skips discovery entirely.

## 3. Connect and exchange messages

**src/net.cpp** wraps Winsock in Network and Socket. Constructors acquire resources and destructors release them. The sockets are nonblocking during transfer. wait_for waits until the socket is ready and shares a deadline across the partial reads or writes for one operation. receive_into reconstructs the requested byte span even when recv returns it in fragments.

**src/transfer.cpp** sends and validates the 68-byte v1 handshake. Both sides must agree on the info hash. The seeder follows with a bitfield, a compact list of which pieces it has. The downloader declares interest and waits for an unchoke before requesting data. word and number encode and decode four-byte network-order integers. next reads the length prefix, rejects oversized frames, then reads the body. A zero-length frame is a keep-alive.

**Read the code:** Follow Downloader::block. A request contains the piece index, the offset within that piece, and the number of bytes. The response repeats the piece index and offset before the actual block. The downloader checks that all three match its pending request. A 256 KiB piece takes sixteen 16 KiB requests with the default sizes.

## 4. Verify and publish the payload

Downloader::get_piece assembles blocks into one buffer and compares its SHA-1 digest with the expected piece digest. download writes that buffer only after validation and retries the current piece with another candidate after peer failure. It carries the verified prefix across those attempts.

**src/storage.cpp** handles exact file reads, local verification, and output publishing. OutputFile creates a unique temporary sibling using exclusive creation. publish flushes and closes the file, then renames it without overwriting an existing destination. The destructor removes incomplete output on ordinary failure. The filename from metadata passes safe_name in metainfo.cpp before it becomes part of the destination path.

For uploads, seed verifies the full source once, then upload checks every requested range before seeking to the requested offset. A background thread handles tracker re-announces so a peer transfer can continue while the tracker responds.

## 5. Read the tests

- **tests/unit.cpp** covers binary bencoding, malformed metadata, UTF-8 and Windows filename checks, SHA-1, piece math, local verification, and tracker reply formats.
- **tests/loopback.ps1** generates its own files and runs the actual CLI. It checks direct transfer, tracker discovery, completion counters, SHA-256 equality, refusal to overwrite, and rejection of a mismatched handshake.
- **tests/fixture-peer.ps1** independently writes and reads wire messages. It checks a valid block, then sends a corrupt block, a wrong block offset, and an oversized length prefix. The main integration script verifies that every failed transfer leaves the destination empty.

To see piece count change, create torrents for the same file with different --piece-size values and inspect both. Then modify one byte of a copy and run verify against the original torrent. The first failing piece index tells you which region changed.
