# Copyright (c) 2026 Andrew Fiorentino
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

project(
    JamLink
    VERSION 0.4.1
    DESCRIPTION "Low-latency remote music collaboration"
    HOMEPAGE_URL "https://github.com/andrewgfiorentino/JamLink"
    LANGUAGES CXX
)

set(JAMLINK_BUILD_IDENTITY "source")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE JAMLINK_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    string(LENGTH "${JAMLINK_GIT_COMMIT}" JAMLINK_GIT_COMMIT_LENGTH)
    if(JAMLINK_GIT_COMMIT_LENGTH EQUAL 40
        AND JAMLINK_GIT_COMMIT MATCHES "^[0-9a-fA-F]+$")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=all
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE JAMLINK_GIT_STATUS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(JAMLINK_GIT_STATUS STREQUAL "")
            set(JAMLINK_BUILD_IDENTITY "${JAMLINK_GIT_COMMIT}")
        else()
            # A dirty build must never negotiate as the clean commit it was
            # configured from. The desktop deliberately refuses to host when
            # the identity is not one exact 40-character commit.
            set(JAMLINK_BUILD_IDENTITY "${JAMLINK_GIT_COMMIT}-dirty")
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" ls-files --cached --others --exclude-standard
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE JAMLINK_GIT_INPUTS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT JAMLINK_GIT_INPUTS STREQUAL "")
            string(REPLACE "\n" ";" JAMLINK_GIT_INPUTS "${JAMLINK_GIT_INPUTS}")
            list(TRANSFORM JAMLINK_GIT_INPUTS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                ${JAMLINK_GIT_INPUTS})
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE JAMLINK_GIT_HEAD_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE JAMLINK_GIT_HEAD_REF
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT JAMLINK_GIT_HEAD_REF STREQUAL "")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path "${JAMLINK_GIT_HEAD_REF}"
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE JAMLINK_GIT_HEAD_REF_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${JAMLINK_GIT_HEAD_PATH}" "${JAMLINK_GIT_HEAD_REF_PATH}")
    endif()
endif()
set(JAMLINK_RELEASE_CHANNEL "test" CACHE STRING "JamLink release channel")
set(JAMLINK_DIRECTORY_URL "" CACHE STRING
    "HTTPS URL for the optional JamLink private-room/directory service")

set(JAMLINK_LICENSE_ID "GPL-3.0-or-later")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

option(JAMLINK_BUILD_TESTS "Build automated tests" ON)
option(JAMLINK_BUILD_TOOLS "Build engineering tools" ON)
option(JAMLINK_BUILD_DESKTOP "Build the Qt desktop application" OFF)
option(JAMLINK_WARNINGS_AS_ERRORS "Treat JamLink warnings as errors" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(JamLinkWarnings)

add_library(jamlink_core STATIC
    src/audio/audio_route_graph.cpp
    src/audio/instrument_tuner.cpp
    src/audio/opus_stream_codec.cpp
    src/audio/private_soundcheck_processor.cpp
    src/clock/clock_domain_controller.cpp
    src/control/readiness_tracker.cpp
    src/control/session_conductor.cpp
    src/control/room_authority.cpp
    src/diagnostics/session_log.cpp
    src/diagnostics/support_bundle.cpp
    src/network/audio_stream_receiver.cpp
    src/network/connection_preflight.cpp
    src/network/peer_audio_codec.cpp
    src/preferences/preferences_store.cpp
    src/record/session_recorder.cpp
)

add_library(JamLink::Core ALIAS jamlink_core)

target_compile_features(jamlink_core PUBLIC cxx_std_20)
set_target_properties(jamlink_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(jamlink_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
# Opus is linked into the core rather than the platform layer: the codec is
# portable, and keeping it here lets the impairment tests exercise the real
# encoder rather than a stand-in.
target_link_libraries(jamlink_core PRIVATE opus)
jamlink_set_warnings(jamlink_core)

# Opus carries the audio on the wire. Built here rather than found on the
# system so the corresponding-source archive, which is git archive HEAD, holds
# everything that is statically linked into the executable.
# Opus 1.4 declares a minimum older than current CMake will accept. Raising the
# floor here keeps the vendored tree byte-identical to upstream, which is worth
# more than avoiding one variable.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
# Opus builds its own test executables whenever BUILD_TESTING is set, which
# CTest sets for JamLink's tests. Shadow it across the subdirectory only.
set(_jamlink_build_testing_was ${BUILD_TESTING})
set(BUILD_TESTING OFF)
add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)
set(BUILD_TESTING ${_jamlink_build_testing_was})
# Upstream code is not held to JamLink's warning settings.
if(MSVC)
    target_compile_options(opus PRIVATE /W0)
else()
    target_compile_options(opus PRIVATE -w)
endif()

if(WIN32)
    add_library(jamlink_asio_sdk STATIC
        third_party/asio-sdk/common/asio.cpp
        third_party/asio-sdk/host/asiodrivers.cpp
        third_party/asio-sdk/host/pc/asiolist.cpp
    )
    target_compile_features(jamlink_asio_sdk PUBLIC cxx_std_20)
    set_target_properties(jamlink_asio_sdk PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(jamlink_asio_sdk
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-sdk/common"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-sdk/host"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-sdk/host/pc"
    )
    target_compile_definitions(jamlink_asio_sdk PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(jamlink_asio_sdk PRIVATE /W0)
    endif()

    add_library(jamlink_platform_audio STATIC
        src/platform/windows/asio_soundcheck_audio_service.cpp
        src/platform/windows/wasapi_soundcheck_audio_service.cpp
        src/platform/windows/wasapi_secondary_capture.cpp
        src/platform/windows/windows_soundcheck_audio_service.cpp
        src/platform/windows/windows_peer_audio_transport.cpp
    )
    target_link_libraries(jamlink_platform_audio
        PUBLIC JamLink::Core
        PRIVATE jamlink_asio_sdk advapi32 avrt bcrypt iphlpapi ole32 oleaut32 propsys ws2_32
    )
else()
    add_library(jamlink_platform_audio STATIC
        src/platform/null_soundcheck_audio_service.cpp
        src/platform/null_peer_audio_transport.cpp
    )
    target_link_libraries(jamlink_platform_audio PUBLIC JamLink::Core)
endif()
add_library(JamLink::PlatformAudio ALIAS jamlink_platform_audio)
target_compile_features(jamlink_platform_audio PUBLIC cxx_std_20)
set_target_properties(jamlink_platform_audio PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(jamlink_platform_audio
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
jamlink_set_warnings(jamlink_platform_audio)

if(JAMLINK_BUILD_TESTS)
    include(CTest)
    enable_testing()
    # Keeps the dual-licensed Steinberg SDK reachable from one file, so which
    # licence JamLink ships under stays a licensing decision and never becomes
    # an architectural one.
    add_test(
        NAME jamlink_asio_isolation_tests
        COMMAND "${CMAKE_COMMAND}"
            "-DJAMLINK_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckAsioIsolation.cmake"
    )
    set_tests_properties(jamlink_asio_isolation_tests PROPERTIES
        TIMEOUT 60 LABELS "licensing")
    add_subdirectory(tests)
    find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME jamlink_directory_tests
            COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_directory.py
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/services/directory"
        )
        set_tests_properties(jamlink_directory_tests PROPERTIES
            TIMEOUT 30 LABELS "service;directory")
    endif()
endif()

if(JAMLINK_BUILD_TOOLS)
    add_subdirectory(tools/core_stress)
    if(WIN32)
        add_subdirectory(tools/audio_probe)
        add_subdirectory(tools/net_probe)
    endif()
endif()

if(WIN32 AND JAMLINK_BUILD_DESKTOP)
    add_executable(jamlink_updater tools/updater/main.cpp)
    target_compile_features(jamlink_updater PRIVATE cxx_std_20)
    set_target_properties(jamlink_updater PROPERTIES
        CXX_EXTENSIONS OFF
        OUTPUT_NAME JamLinkUpdater
    )
    jamlink_set_warnings(jamlink_updater)
endif()

if(JAMLINK_BUILD_DESKTOP)
    add_subdirectory(apps/desktop)
endif()

if(WIN32 AND JAMLINK_BUILD_DESKTOP)
    if(BUILD_TESTING)
        add_test(
            NAME jamlink_updater_helper_tests
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/updater_helper_test.ps1"
                -Updater "$<TARGET_FILE:jamlink_updater>"
                -TestRoot "${CMAKE_CURRENT_BINARY_DIR}/tests/updater-helper-fixture"
        )
        set_tests_properties(jamlink_updater_helper_tests PROPERTIES
            TIMEOUT 30 LABELS "update")
    endif()
endif()
