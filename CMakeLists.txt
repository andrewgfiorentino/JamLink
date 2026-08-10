# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

project(
    JamLink
    VERSION 0.2.0
    DESCRIPTION "Low-latency remote music collaboration"
    HOMEPAGE_URL "https://github.com/andrewgfiorentino/JamLink"
    LANGUAGES CXX
)

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
    src/audio/private_soundcheck_processor.cpp
    src/clock/clock_domain_controller.cpp
    src/control/readiness_tracker.cpp
    src/network/audio_stream_receiver.cpp
    src/preferences/preferences_store.cpp
)

add_library(JamLink::Core ALIAS jamlink_core)

target_compile_features(jamlink_core PUBLIC cxx_std_20)
set_target_properties(jamlink_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(jamlink_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
jamlink_set_warnings(jamlink_core)

if(WIN32)
    add_library(jamlink_platform_audio STATIC
        src/platform/windows/wasapi_soundcheck_audio_service.cpp
        src/platform/windows/windows_peer_audio_transport.cpp
    )
    target_link_libraries(jamlink_platform_audio
        PUBLIC JamLink::Core
        PRIVATE avrt bcrypt ole32 oleaut32 propsys ws2_32
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
    add_subdirectory(tests)
endif()

if(JAMLINK_BUILD_TOOLS)
    add_subdirectory(tools/core_stress)
endif()

if(JAMLINK_BUILD_DESKTOP)
    add_subdirectory(apps/desktop)
endif()
