# MyTorrent

MyTorrent 0.1.0 is a Windows C++20 command-line client for creating, inspecting, verifying, seeding, and downloading single-file BitTorrent v1 torrents. It supports direct IPv4 TCP peers and HTTP or HTTPS tracker discovery.

Start with [the code walkthrough](docs/WALKTHROUGH.md) to follow a torrent from disk to the wire and back.

## Build and test

Use a C++20 compiler, CMake 3.20 or newer, and PowerShell for the integration tests. The protocol code has no third-party library dependency. It uses Windows CNG for hashing, Winsock for TCP, and WinHTTP for tracker requests.

From a Visual Studio Developer PowerShell with the C++ toolchain installed:

~~~powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
~~~

With MSYS2 UCRT64 GCC and Ninja on PATH:

~~~powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
~~~

Use distinct build directories if switching toolchains. For MinGW, keep the matching runtime DLLs available on PATH when running the executable.

## Try a transfer in Windows Command Prompt

Open two Command Prompt windows in the repository root. In both windows, set the path to the executable you built:

~~~bat
set "MYTORRENT=build\mytorrent.exe"
~~~

For a Visual Studio Release build, use this path instead:

~~~bat
set "MYTORRENT=build\Release\mytorrent.exe"
~~~

If you built with MSYS2 and its runtime DLLs are missing from PATH, run this in both windows. Adjust the folder if MSYS2 is installed elsewhere.

~~~bat
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
~~~

In the first window, create a sample torrent and leave the seeder running:

~~~bat
if not exist ".local" mkdir ".local"
echo Hello from MyTorrent>".local\example.txt"
"%MYTORRENT%" create ".local\example.txt" --tracker http://127.0.0.1:8080/announce --out ".local\example.torrent"
"%MYTORRENT%" inspect ".local\example.torrent"
"%MYTORRENT%" verify ".local\example.torrent" --file ".local\example.txt"
"%MYTORRENT%" seed ".local\example.torrent" --file ".local\example.txt" --port 6881 --no-tracker
~~~

In the second window, download directly from the seeder and compare the bytes:

~~~bat
"%MYTORRENT%" download ".local\example.torrent" --out ".local\received" --peer 127.0.0.1:6881
fc /b ".local\example.txt" ".local\received\example.txt"
~~~

Expect FC to report no differences. Stop the seeder with Ctrl+C when you are done. Use fresh torrent and destination paths to repeat the demo. This direct transfer skips tracker discovery, so the example tracker URL only needs to be stored in the torrent.

## Download through a tracker

Replace the sample tracker URL with an existing tracker reachable by the seeder and downloader. On the seeder, create a new torrent and start it with tracker announcements enabled:

~~~bat
"%MYTORRENT%" create ".local\example.txt" --tracker "http://tracker.example:8080/announce" --out ".local\tracked.torrent"
"%MYTORRENT%" seed ".local\tracked.torrent" --file ".local\example.txt" --port 6881
~~~

Give the recipient the torrent file and have them run:

~~~bat
"%MYTORRENT%" download ".local\tracked.torrent" --out ".local\tracked-download"
~~~

The downloader can use up to four tracker-discovered peers. If the tracker returns an empty peer list, MyTorrent waits for the advertised interval and asks again. Press Ctrl+C to cancel while waiting. The tracker must accept outbound-only clients announcing port 0, and the seeder must be reachable on its chosen TCP port.

## Resume an interrupted download

Add the resume flag on the initial run. After an interruption, rerun the same command with the same torrent and output directory:

~~~bat
"%MYTORRENT%" download ".local\example.torrent" --out ".local\resumable" --peer 127.0.0.1:6881 --resume
~~~

For a tracker-based transfer, add the same flag to the tracker download command.

MyTorrent keeps a .mytorrent-<info-hash>.part file in the output directory. It verifies the saved pieces again at startup, discards any incomplete or invalid tail, and continues from the first missing piece. The final file appears after all pieces pass verification. Remove the partial file to discard progress when no download is running.

## Release scope

- Single-file v1, nonempty payloads, IPv4 TCP peers. Seeded files must stay unchanged while serving.
- The seeder serves one connection at a time and announces periodically. The outbound-only downloader uses up to four peers concurrently from the first nonempty tracker response. An explicit peer address selects one connection. The seeder is a separate command after a successful download.
- Each connection pipelines up to eight block requests for its assigned piece. The scheduler buffers up to eight pieces near the write position, and an unfinished piece can be reassigned when a peer fails. Use the resume flag to preserve progress across runs.
- Multi-file torrents, private tracker flags, UDP trackers, IPv6 peers, magnet links, DHT, and v2 or hybrid torrents are future work.
- Verified on Windows using GCC and Ninja. The test tracker and peer are fixtures. Compatibility with public trackers and third-party desktop clients has yet to be tested.

## Protocol references

- [BEP 3 — BitTorrent v1](https://www.bittorrent.org/beps/bep_0003.html)
- [BEP 23 — Compact peer lists](https://www.bittorrent.org/beps/bep_0023.html)
