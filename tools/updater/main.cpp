// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace {

struct Options final {
    std::filesystem::path archive;
    std::filesystem::path stage;
    std::filesystem::path target;
    std::uint32_t parentProcess{0U};
    bool restart{false};
};

[[nodiscard]] std::wstring quote(const std::filesystem::path& path) {
    std::wstring result = L"\"";
    for (const wchar_t character : path.wstring()) {
        result += character == L'\"' ? L"\\\"" : std::wstring(1U, character);
    }
    result += L'\"';
    return result;
}

[[nodiscard]] bool parse(int argc, wchar_t* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument(argv[index]);
        const auto value = [&]() -> const wchar_t* {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        if (argument == L"--archive") {
            const wchar_t* next = value();
            if (next == nullptr) return false;
            options.archive = next;
        } else if (argument == L"--stage") {
            const wchar_t* next = value();
            if (next == nullptr) return false;
            options.stage = next;
        } else if (argument == L"--target") {
            const wchar_t* next = value();
            if (next == nullptr) return false;
            options.target = next;
        } else if (argument == L"--parent") {
            const wchar_t* next = value();
            if (next == nullptr) return false;
            try {
                options.parentProcess = static_cast<std::uint32_t>(std::stoul(next));
            } catch (...) {
                return false;
            }
        } else if (argument == L"--restart") {
            options.restart = true;
        } else {
            return false;
        }
    }
    return !options.stage.empty() && !options.target.empty();
}

[[nodiscard]] bool safeDirectory(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    return !error && absolute.has_parent_path() && absolute != absolute.root_path()
        && absolute.parent_path() != absolute.root_path();
}

[[nodiscard]] bool waitForParent(std::uint32_t processId) {
    if (processId == 0U) {
        return true;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    const DWORD result = WaitForSingleObject(process, 120'000U);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
}

[[nodiscard]] bool runAndWait(std::wstring commandLine) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr, commandLine.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 120'000U);
    DWORD exitCode = 1U;
    if (wait == WAIT_OBJECT_0) {
        static_cast<void>(GetExitCodeProcess(process.hProcess, &exitCode));
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exitCode == 0U;
}

[[nodiscard]] bool extract(const Options& options) {
    if (options.archive.empty()) {
        return std::filesystem::exists(options.stage / L"JamLink.exe");
    }
    std::error_code error;
    std::filesystem::remove_all(options.stage, error);
    error.clear();
    std::filesystem::create_directories(options.stage, error);
    if (error) {
        return false;
    }
    wchar_t systemDirectory[MAX_PATH]{};
    if (GetSystemDirectoryW(systemDirectory, MAX_PATH) == 0U) {
        return false;
    }
    const std::filesystem::path tar = std::filesystem::path(systemDirectory) / L"tar.exe";
    std::wstring command = quote(tar) + L" -xf " + quote(options.archive)
        + L" -C " + quote(options.stage);
    return runAndWait(std::move(command));
}

[[nodiscard]] bool launch(const std::filesystem::path& executable) {
    std::wstring command = quote(executable);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            0U, nullptr, executable.parent_path().c_str(), &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    Options options;
    if (!parse(argc, argv, options) || !safeDirectory(options.stage)
        || !safeDirectory(options.target)) {
        std::wcerr << L"Invalid update paths or arguments\n";
        return 2;
    }
    options.stage = std::filesystem::absolute(options.stage).lexically_normal();
    options.target = std::filesystem::absolute(options.target).lexically_normal();
    if (options.stage == options.target || !std::filesystem::exists(options.target / L"JamLink.exe")
        || !waitForParent(options.parentProcess) || !extract(options)
        || !std::filesystem::exists(options.stage / L"JamLink.exe")
        || !std::filesystem::exists(options.stage / L"LICENSE")) {
        std::wcerr << L"Update validation or extraction failed\n";
        return 3;
    }

    std::filesystem::path backup = options.target;
    backup += L".update-backup";
    std::error_code error;
    std::filesystem::remove_all(backup, error);
    error.clear();
    std::filesystem::rename(options.target, backup, error);
    if (error) {
        std::wcerr << L"Could not preserve the current installation\n";
        return 4;
    }
    error.clear();
    std::filesystem::rename(options.stage, options.target, error);
    if (error) {
        std::error_code restoreError;
        std::filesystem::rename(backup, options.target, restoreError);
        std::wcerr << L"Could not activate the staged update\n";
        return 5;
    }
    if (options.restart && !launch(options.target / L"JamLink.exe")) {
        std::wcerr << L"Update installed, but JamLink could not restart\n";
        return 6;
    }
    return 0;
}
