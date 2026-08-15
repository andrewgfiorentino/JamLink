// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/diagnostics/session_log.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace jamlink::diagnostics {
namespace {

[[nodiscard]] std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &time);
#else
    localtime_r(&time, &parts);
#endif
    std::ostringstream text;
    text << std::put_time(&parts, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << milliseconds.count();
    return text.str();
}

// A log is pasted into chat windows. Anything that looks like a room key is
// removed rather than trusted to be absent.
[[nodiscard]] std::string sanitise(std::string_view message) {
    std::string result;
    result.reserve(message.size());
    std::size_t hexRun = 0U;
    std::size_t hexStart = 0U;
    const auto isHex = [](char character) {
        return (character >= '0' && character <= '9')
            || (character >= 'a' && character <= 'f')
            || (character >= 'A' && character <= 'F');
    };
    for (std::size_t index = 0U; index < message.size(); ++index) {
        const char character = message[index];
        if (isHex(character)) {
            if (hexRun == 0U) {
                hexStart = result.size();
            }
            ++hexRun;
        } else {
            hexRun = 0U;
        }
        result.push_back(character == '\n' || character == '\r' ? ' ' : character);
        // A 32 character run of hex is a secret, not a word.
        if (hexRun == 32U) {
            result.resize(hexStart);
            result += "<redacted>";
            hexRun = 0U;
            // Skip the rest of the run.
            while (index + 1U < message.size() && isHex(message[index + 1U])) {
                ++index;
            }
        }
    }
    return result;
}

} // namespace

SessionLog& SessionLog::instance() {
    static SessionLog log;
    return log;
}

void SessionLog::open(const std::filesystem::path& directory) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (directory.empty()) {
        enabled_ = false;
        path_.clear();
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        enabled_ = false;
        path_.clear();
        return;
    }
    path_ = directory / "jamlink-session.log";
    enabled_ = true;
}

void SessionLog::close() {
    const std::lock_guard<std::mutex> guard(mutex_);
    enabled_ = false;
    path_.clear();
}

void SessionLog::rotateIfLarge() {
    std::error_code error;
    const auto size = std::filesystem::file_size(path_, error);
    if (error || size < maximumBytes) {
        return;
    }
    auto previous = path_;
    previous += ".1";
    std::filesystem::remove(previous, error);
    error.clear();
    std::filesystem::rename(path_, previous, error);
}

void SessionLog::write(std::string_view category, std::string_view message) noexcept {
    try {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (!enabled_ || path_.empty()) {
            return;
        }
        rotateIfLarge();
        std::ofstream file(path_, std::ios::app);
        if (!file) {
            return;
        }
        file << timestamp() << "  [" << sanitise(category) << "] "
             << sanitise(message) << '\n';
    } catch (...) {
        // Diagnostics must never take the application down with them.
    }
}

std::filesystem::path SessionLog::path() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return path_;
}

bool SessionLog::enabled() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return enabled_;
}

std::string SessionLog::redactInvite(std::string_view invite) {
    if (invite.empty()) {
        return "(none)";
    }
    const auto lastSeparator = invite.rfind('|');
    if (lastSeparator == std::string_view::npos) {
        return "(malformed)";
    }
    return std::string(invite.substr(0U, lastSeparator + 1U)) + "<secret withheld>";
}

} // namespace jamlink::diagnostics
