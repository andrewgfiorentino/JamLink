# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(jamlink_net_probe main.cpp)
target_link_libraries(jamlink_net_probe PRIVATE JamLink::PlatformAudio)
target_compile_features(jamlink_net_probe PRIVATE cxx_std_20)
set_target_properties(jamlink_net_probe PROPERTIES CXX_EXTENSIONS OFF)
jamlink_set_warnings(jamlink_net_probe)
