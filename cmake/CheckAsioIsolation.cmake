# Copyright (c) 2026 Andrew Fiorentino
# SPDX-License-Identifier: GPL-3.0-or-later

# Keeps the Steinberg ASIO SDK behind one file.
#
# The SDK is dual-licensed: GPL version 3, or a proprietary agreement that has
# to be signed by Steinberg before anything using it may be published. Whichever
# JamLink ends up under, that choice must stay a licensing decision rather than
# turning into an architectural one. It only stays cheap while the SDK is
# reachable from a single translation unit hidden behind ISoundcheckAudioService.
#
# This test fails the build the moment a second file starts including it, which
# is the point at which the boundary would otherwise quietly disappear.

set(JAMLINK_ASIO_PERMITTED
    "src/platform/windows/asio_soundcheck_audio_service.cpp")

set(JAMLINK_ASIO_HEADERS
    "asio.h" "asiosys.h" "asiodrivers.h" "asiolist.h" "iasiodrv.h" "asiodrvr.h")

file(GLOB_RECURSE JAMLINK_SOURCES
    "${JAMLINK_SOURCE_DIR}/src/*.cpp"
    "${JAMLINK_SOURCE_DIR}/src/*.hpp"
    "${JAMLINK_SOURCE_DIR}/include/*.hpp"
    "${JAMLINK_SOURCE_DIR}/apps/*.cpp"
    "${JAMLINK_SOURCE_DIR}/apps/*.hpp"
    "${JAMLINK_SOURCE_DIR}/tools/*.cpp"
    "${JAMLINK_SOURCE_DIR}/tests/*.cpp")

set(JAMLINK_VIOLATIONS "")
foreach(source IN LISTS JAMLINK_SOURCES)
    file(RELATIVE_PATH relative "${JAMLINK_SOURCE_DIR}" "${source}")
    if(relative IN_LIST JAMLINK_ASIO_PERMITTED)
        continue()
    endif()
    file(READ "${source}" contents)
    foreach(header IN LISTS JAMLINK_ASIO_HEADERS)
        # Only an actual include counts. Prose about ASIO is expected and fine.
        if(contents MATCHES "#[ \t]*include[ \t]*[<\"]${header}[>\"]")
            list(APPEND JAMLINK_VIOLATIONS "${relative} includes ${header}")
        endif()
    endforeach()
endforeach()

if(JAMLINK_VIOLATIONS)
    message(STATUS "The Steinberg ASIO SDK must stay behind one file.")
    foreach(violation IN LISTS JAMLINK_VIOLATIONS)
        message(STATUS "  ${violation}")
    endforeach()
    message(FATAL_ERROR
        "ASIO SDK isolation broken. Route the need through "
        "ISoundcheckAudioService instead, so relicensing stays a licensing "
        "decision rather than a rewrite.")
endif()

message(STATUS "ASIO SDK isolation intact.")
