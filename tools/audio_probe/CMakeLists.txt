# Copyright (c) 2026 Andrew Fiorentino
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(jamlink_audio_probe main.cpp)
target_link_libraries(jamlink_audio_probe PRIVATE JamLink::PlatformAudio)
target_compile_features(jamlink_audio_probe PRIVATE cxx_std_20)
set_target_properties(jamlink_audio_probe PROPERTIES CXX_EXTENSIONS OFF)
jamlink_set_warnings(jamlink_audio_probe)
