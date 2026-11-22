#pragma once

#include <atomic>

namespace bt {
class ConsoleInterrupt {
public:
    ConsoleInterrupt();
    ~ConsoleInterrupt();
    ConsoleInterrupt(const ConsoleInterrupt&) = delete;
    ConsoleInterrupt& operator=(const ConsoleInterrupt&) = delete;

    const std::atomic_bool& requested() const;
};
}
