# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(jamlink_core_tests jamlink_core_tests.cpp)
target_link_libraries(jamlink_core_tests PRIVATE JamLink::Core)
target_compile_features(jamlink_core_tests PRIVATE cxx_std_20)
jamlink_set_warnings(jamlink_core_tests)

add_test(NAME jamlink_core_tests COMMAND jamlink_core_tests)
set_tests_properties(jamlink_core_tests PROPERTIES TIMEOUT 30)

if(WIN32)
    add_executable(jamlink_peer_transport_tests peer_transport_tests.cpp)
    target_link_libraries(jamlink_peer_transport_tests PRIVATE JamLink::PlatformAudio)
    target_compile_features(jamlink_peer_transport_tests PRIVATE cxx_std_20)
    set_target_properties(jamlink_peer_transport_tests PROPERTIES CXX_EXTENSIONS OFF)
    jamlink_set_warnings(jamlink_peer_transport_tests)
    add_test(NAME jamlink_peer_transport_tests COMMAND jamlink_peer_transport_tests)
    set_tests_properties(jamlink_peer_transport_tests PROPERTIES
        TIMEOUT 10 LABELS "network;loopback")
endif()
