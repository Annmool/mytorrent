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
$exe = '.\build\Release\mytorrent.exe'
~~~

With MSYS2 UCRT64 GCC and Ninja on PATH:

~~~powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
$exe = '.\build\mytorrent.exe'
~~~

Use distinct build directories if switching toolchains. For MinGW, keep the matching runtime DLLs available on PATH when running the executable.

## Try a transfer locally

In the first PowerShell window, set the executable path from the build step and create a sample file:

~~~powershell
New-Item -ItemType Directory -Force .local
Set-Content -LiteralPath .local\example.txt -Value 'Hello from MyTorrent'
& $exe create .local\example.txt --tracker http://127.0.0.1:8080/announce --out .local\example.torrent
& $exe inspect .local\example.torrent
& $exe verify .local\example.torrent --file .local\example.txt
& $exe seed .local\example.torrent --file .local\example.txt --port 6881 --no-tracker
~~~

In another PowerShell window in the repository, set the same executable path and run:

~~~powershell
& $exe download .local\example.torrent --out .local\received --peer 127.0.0.1:6881
Get-FileHash .local\example.txt,.local\received\example.txt
~~~

The hashes should match. Stop the seeder with Ctrl+C. Use a fresh output directory on subsequent runs or move the existing download first.

## Discover a seed through a tracker

Set the tracker URL at creation time to an existing tracker reachable from both machines. Start the seeder without the direct-only flag, give the recipient the .torrent file, and run download without an explicit peer. The seeder must be reachable on its chosen TCP port. A tracker that accepts outbound-only peers announcing port 0 is required for tracker downloads with this release. Direct peer mode works independently of tracker availability.

## Release scope

- Single-file v1, nonempty payloads, IPv4 TCP peers. Seeded files must stay unchanged while serving.
- The seeder serves one connection at a time and announces periodically. The downloader is outbound-only and tries the initial tracker peer list serially. The seeder is a separate command after a successful download.
- One in-flight block request. Verified pieces remain available for fallback to the next peer within the same run. Across runs a download starts fresh.
- Multi-file torrents, private tracker flags, UDP trackers, IPv6 peers, magnet links, DHT, and v2 or hybrid torrents are future work.
- Verified on Windows using GCC and Ninja. The test tracker and peer are fixtures. Compatibility with public trackers and third-party desktop clients has yet to be tested.

## Protocol references

- [BEP 3 — BitTorrent v1](https://www.bittorrent.org/beps/bep_0003.html)
- [BEP 23 — Compact peer lists](https://www.bittorrent.org/beps/bep_0023.html)
