#include "storage.hpp"
#include "bencode.hpp"
#include "metainfo.hpp"
#include "tracker.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void check(bool condition, const char* failure) {
    if (!condition) throw std::runtime_error(failure);
}

template<class Action>
void rejects(Action action, const char* failure) {
    try { action(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error(failure);
}
}

int main() {
    try {
        using namespace bt;
        using namespace std::string_literals;
        check(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d",
              "SHA-1 known answer");
        const auto valid = "d1:ai-3e1:bl0:3:x\0ye1:zd1:k3:valee"s;
        check(encode(decode(valid)) == valid, "Binary bencode round trip");
        for (std::string_view bad : {"", "i03e", "i-0e", "i9223372036854775808e", "1:a0:",
              "2:a", "d1:bi1e1:ai2ee", "d1:ai1e1:ai2ee", "l"}) {
            rejects([&] { decode(bad); }, "Accepted malformed bencode");
        }
        rejects([] { decode(std::string(50, 'l') + std::string(50, 'e')); }, "Accepted excess nesting");
        for (std::string_view bad : {"../x", "dir\\x", "C:x", "NUL.txt", "COM1.txt",
                                     "LPT1 .txt", "name.", "trailing ", "\xff"}) {
            rejects([&] { safe_name(bad); }, "Accepted unsafe filename");
        }
        const auto info = "d6:lengthi3e4:name5:x.txt12:piece lengthi16384e6:pieces20:" + sha1("abc") + "e";
        const auto torrent = "d8:announce18:http://localhost/a4:info" + info + "e";
        const auto meta = parse_metainfo(torrent);
        check(meta.info_hash == sha1(info), "Info hash excludes outer dictionary");
        check(meta.count() == 1 && meta.size(0) == 3 && meta.offset(0) == 0, "Final piece length");
        rejects([&] { meta.size(1); }, "Accepted out of range piece index");
        std::istringstream correct("abc"), corrupt("abd"), oversized("abcd");
        verify_file(meta, correct);
        rejects([&] { verify_file(meta, corrupt); }, "Corrupt file verified");
        rejects([&] { verify_file(meta, oversized); }, "Oversized file verified");

        check(escape_query("\0\xff+"s) == "%00%FF%2B", "Binary tracker query escaping");
        const auto reply = parse_tracker_reply("d8:intervali60e5:peers6:\x7f\0\0\x01\x1a\xe1" "e"s);
        check(reply.interval == 60 && reply.peers.size() == 1 && reply.peers[0].ip == "127.0.0.1" &&
              reply.peers[0].port == 6881, "Compact IPv4 peer decoding");
        const auto list = parse_tracker_reply("d8:intervali60e5:peersld2:ip9:127.0.0.14:porti6881eeee");
        check(list.peers.size() == 1 && list.peers[0].port == 6881, "Dictionary peer decoding");
        rejects([] { parse_tracker_reply("d8:intervali1e5:peers1:xe"); }, "Accepted partial compact address");
        rejects([] { parse_tracker_reply("d14:failure reason4:nopee"); }, "Ignored tracker failure");
        std::cout << "Unit checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
