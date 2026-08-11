# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(jamlink_core_tests jamlink_core_tests.cpp)
target_link_libraries(jamlink_core_tests PRIVATE JamLink::Core)
target_compile_features(jamlink_core_tests PRIVATE cxx_std_20)
jamlink_set_warnings(jamlink_core_tests)

add_test(NAME jamlink_core_tests COMMAND jamlink_core_tests)
set_tests_properties(jamlink_core_tests PROPERTIES TIMEOUT 30)

add_executable(jamlink_network_tests jamlink_network_tests.cpp)
target_link_libraries(jamlink_network_tests PRIVATE JamLink::Core)
target_compile_features(jamlink_network_tests PRIVATE cxx_std_20)
set_target_properties(jamlink_network_tests PROPERTIES CXX_EXTENSIONS OFF)
jamlink_set_warnings(jamlink_network_tests)

add_test(NAME jamlink_network_tests COMMAND jamlink_network_tests)
set_tests_properties(jamlink_network_tests PROPERTIES
    TIMEOUT 180 LABELS "network;impairment")

add_executable(jamlink_tuner_tests jamlink_tuner_tests.cpp)
target_link_libraries(jamlink_tuner_tests PRIVATE JamLink::Core)
target_compile_features(jamlink_tuner_tests PRIVATE cxx_std_20)
set_target_properties(jamlink_tuner_tests PROPERTIES CXX_EXTENSIONS OFF)
jamlink_set_warnings(jamlink_tuner_tests)

add_test(NAME jamlink_tuner_tests COMMAND jamlink_tuner_tests)
set_tests_properties(jamlink_tuner_tests PROPERTIES TIMEOUT 60 LABELS "audio;tuner")

add_executable(jamlink_recorder_tests jamlink_recorder_tests.cpp)
target_link_libraries(jamlink_recorder_tests PRIVATE JamLink::Core)
target_compile_features(jamlink_recorder_tests PRIVATE cxx_std_20)
set_target_properties(jamlink_recorder_tests PROPERTIES CXX_EXTENSIONS OFF)
jamlink_set_warnings(jamlink_recorder_tests)

add_test(NAME jamlink_recorder_tests COMMAND jamlink_recorder_tests)
set_tests_properties(jamlink_recorder_tests PROPERTIES TIMEOUT 90 LABELS "audio;record")

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
