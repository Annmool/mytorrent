#include "discovery.hpp"
#include "console_interrupt.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bt {
std::vector<Endpoint> wait_for_tracker_peers(const Metainfo& meta, std::string_view id,
                                             const Announce& state, unsigned interval) {
    using Clock = std::chrono::steady_clock;
    const ConsoleInterrupt interrupt;
    while (!interrupt.requested()) {
        const auto next_announce = Clock::now() + std::chrono::seconds(interval);
        std::cout << "Waiting for peers. Asking the tracker again in " << interval
                  << " second(s). Press Ctrl+C to cancel." << std::endl;
        auto now = Clock::now();
        while (now < next_announce && !interrupt.requested()) {
            std::this_thread::sleep_until(std::min(next_announce, now + std::chrono::milliseconds(100)));
            now = Clock::now();
        }
        if (interrupt.requested()) break;

        auto reply = announce(meta, id, state);
        if (interrupt.requested()) break;
        if (!reply.peers.empty()) return std::move(reply.peers);
        interval = reply.interval;
    }
    throw std::runtime_error("Download cancelled while waiting for peers");
}
}
