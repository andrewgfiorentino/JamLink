# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(jamlink_core_stress main.cpp)
target_link_libraries(jamlink_core_stress PRIVATE JamLink::Core)
target_compile_features(jamlink_core_stress PRIVATE cxx_std_20)
jamlink_set_warnings(jamlink_core_stress)

if(JAMLINK_BUILD_TESTS)
    add_test(NAME jamlink_core_stress_quick COMMAND jamlink_core_stress 20000)
    set_tests_properties(jamlink_core_stress_quick PROPERTIES
        LABELS "stress"
        TIMEOUT 30
    )
endif()
