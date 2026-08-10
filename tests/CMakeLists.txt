# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(jamlink_core_tests jamlink_core_tests.cpp)
target_link_libraries(jamlink_core_tests PRIVATE JamLink::Core)
target_compile_features(jamlink_core_tests PRIVATE cxx_std_20)
jamlink_set_warnings(jamlink_core_tests)

add_test(NAME jamlink_core_tests COMMAND jamlink_core_tests)
set_tests_properties(jamlink_core_tests PROPERTIES TIMEOUT 30)
