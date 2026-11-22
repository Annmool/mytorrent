#include "console_interrupt.hpp"

#include <windows.h>
#include <system_error>

namespace bt {
namespace {
std::atomic_bool interrupted{};

BOOL WINAPI on_console(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    interrupted = true;
    return TRUE;
}
}

ConsoleInterrupt::ConsoleInterrupt() {
    interrupted = false;
    if (!SetConsoleCtrlHandler(on_console, TRUE)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Installing console interrupt handler");
    }
}

ConsoleInterrupt::~ConsoleInterrupt() {
    SetConsoleCtrlHandler(on_console, FALSE);
}

const std::atomic_bool& ConsoleInterrupt::requested() const {
    return interrupted;
}
}
