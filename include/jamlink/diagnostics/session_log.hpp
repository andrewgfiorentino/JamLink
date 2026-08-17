// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace jamlink::diagnostics {

// Append-only local log of what the connection actually did.
//
// An early two-person field test failed with no record of which step broke, so
// any later failure has to leave evidence behind. This records the connection
// lifecycle: binding, each router mapping protocol tried and its answer, public
// address discovery, handshake progress, and why a session ended.
//
// Three rules this file exists to enforce:
//   - The room secret is never written. An invite is redacted before logging.
//   - It stays on this machine. Nothing here transmits anything.
//   - It is never called from the audio callback. Writing touches a mutex and
//     the filesystem, which is forbidden on the realtime path; the network
//     worker and control threads log state changes only, never per packet.
class SessionLog final {
public:
    [[nodiscard]] static SessionLog& instance();

    // Control thread, before logging starts. An empty path disables the log.
    void open(const std::filesystem::path& directory);
    void close();

    void write(std::string_view category, std::string_view message) noexcept;

    [[nodiscard]] std::filesystem::path path() const;
    [[nodiscard]] bool enabled() const noexcept;

    // Keeps the address and port, drops the secret. Invite codes end with the
    // room key, and a log is exactly the sort of thing a user pastes into a
    // chat window when asking for help.
    [[nodiscard]] static std::string redactInvite(std::string_view invite);

private:
    SessionLog() = default;

    void rotateIfLarge();

    // Bounded so a long session cannot fill a disk.
    static constexpr std::uintmax_t maximumBytes = 2U * 1024U * 1024U;

    mutable std::mutex mutex_;
    std::filesystem::path path_;
    bool enabled_{false};
};

} // namespace jamlink::diagnostics

// Deliberately a function call rather than a stream: it keeps the call sites
// short and makes it obvious that a formatted string must be built first.
#define JAMLINK_LOG(category, message) \
    ::jamlink::diagnostics::SessionLog::instance().write((category), (message))
