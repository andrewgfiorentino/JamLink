# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 6.10.3 REQUIRED COMPONENTS Core Gui Network Qml Quick)

qt_standard_project_setup(REQUIRES 6.10.3)

set_source_files_properties(qml/components/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(jamlink_desktop WIN32
    app_controller.cpp
    firewall_access.cpp
    firewall_access.hpp
    main.cpp
    private_room_directory.cpp
    private_room_directory.hpp
    update_manager.cpp
    update_manager.hpp
)

qt_add_qml_module(jamlink_desktop
    URI JamLinkDesktop
    VERSION 1.0
    SOURCES
        app_controller.hpp
    QML_FILES
        qml/Main.qml
        qml/components/DeviceSelector.qml
        qml/components/ClipLatch.qml
        qml/components/IconButton.qml
        qml/components/JamButton.qml
        qml/components/JamCard.qml
        qml/components/JamIcon.qml
        qml/components/JamSlider.qml
        qml/components/JamSwitch.qml
        qml/components/LevelBar.qml
        qml/components/ProfileAvatar.qml
        qml/components/Theme.qml
        qml/pages/HomePage.qml
        qml/pages/ProfilePage.qml
        qml/pages/RoomPage.qml
        qml/pages/SettingsPage.qml
        qml/pages/SoundCheckPage.qml
        qml/pages/TunerPage.qml
    RESOURCES
        assets/avatar_bass.svg
        assets/avatar_drums.svg
        assets/avatar_guitar_acoustic.svg
        assets/avatar_guitar_electric.svg
        assets/avatar_keys.svg
        assets/avatar_listener.svg
        assets/avatar_synth.svg
        assets/avatar_vocals.svg
        assets/arrow_back.svg
        assets/check_circle.svg
        assets/close.svg
        assets/expand_more.svg
        assets/headphones.svg
        assets/mic.svg
        assets/music_note.svg
        assets/settings.svg
        assets/tune.svg
        assets/volume_up.svg
)

target_link_libraries(jamlink_desktop
    PRIVATE
        JamLink::Core
        JamLink::PlatformAudio
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Qml
        Qt6::Quick
)
target_compile_features(jamlink_desktop PRIVATE cxx_std_20)
target_compile_definitions(jamlink_desktop
    PRIVATE
        JAMLINK_VERSION_STRING="${PROJECT_VERSION}"
        JAMLINK_BUILD_IDENTITY_STRING="${JAMLINK_BUILD_IDENTITY}"
        JAMLINK_RELEASE_CHANNEL_STRING="${JAMLINK_RELEASE_CHANNEL}"
        JAMLINK_DIRECTORY_URL_STRING="${JAMLINK_DIRECTORY_URL}"
)
set_target_properties(jamlink_desktop PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME JamLink
)
jamlink_set_warnings(jamlink_desktop)
if(TARGET jamlink_updater)
    add_dependencies(jamlink_desktop jamlink_updater)
    add_custom_command(TARGET jamlink_desktop POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:jamlink_updater>"
            "$<TARGET_FILE_DIR:jamlink_desktop>/JamLinkUpdater.exe"
    )
endif()
if(MSVC)
    # Keep /W4 /WX for JamLink sources. Qt's generated QML cache translation
    # units instantiate code that emits the audited C4702 diagnostic internally.
    target_compile_options(jamlink_desktop PRIVATE /external:W0 /wd4702)
endif()

if(BUILD_TESTING)
    add_executable(jamlink_desktop_updater_tests
        update_manager.cpp
        update_manager.hpp
        updater_tests.cpp
    )
    target_link_libraries(
        jamlink_desktop_updater_tests
        PRIVATE Qt6::Core Qt6::Network Qt6::Qml)
    target_compile_features(jamlink_desktop_updater_tests PRIVATE cxx_std_20)
    set_target_properties(jamlink_desktop_updater_tests PROPERTIES CXX_EXTENSIONS OFF)
    jamlink_set_warnings(jamlink_desktop_updater_tests)
    if(MSVC)
        target_compile_options(jamlink_desktop_updater_tests PRIVATE /external:W0)
    endif()
    add_test(NAME jamlink_desktop_updater_tests COMMAND jamlink_desktop_updater_tests)
    set_tests_properties(jamlink_desktop_updater_tests PROPERTIES
        TIMEOUT 30 LABELS "gui;update")

    if(Python3_Interpreter_FOUND)
        add_executable(jamlink_private_room_directory_tests
            private_room_directory.cpp
            private_room_directory.hpp
            private_room_directory_tests.cpp
        )
        target_link_libraries(
            jamlink_private_room_directory_tests PRIVATE Qt6::Core Qt6::Network)
        target_compile_features(jamlink_private_room_directory_tests PRIVATE cxx_std_20)
        set_target_properties(
            jamlink_private_room_directory_tests PROPERTIES CXX_EXTENSIONS OFF)
        jamlink_set_warnings(jamlink_private_room_directory_tests)
        if(MSVC)
            target_compile_options(
                jamlink_private_room_directory_tests PRIVATE /external:W0)
        endif()
        add_test(
            NAME jamlink_private_room_directory_tests
            COMMAND jamlink_private_room_directory_tests
                "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/services/directory/jamlink_directory.py"
        )
        set_tests_properties(jamlink_private_room_directory_tests PROPERTIES
            TIMEOUT 20 LABELS "gui;service;security")
    endif()

    add_executable(jamlink_desktop_controller_tests
        app_controller.cpp
        app_controller.hpp
        controller_tests.cpp
        firewall_access.cpp
        firewall_access.hpp
        private_room_directory.cpp
        private_room_directory.hpp
        update_manager.cpp
        update_manager.hpp
    )
    target_link_libraries(
        jamlink_desktop_controller_tests
        PRIVATE JamLink::Core JamLink::PlatformAudio Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml)
    target_compile_features(jamlink_desktop_controller_tests PRIVATE cxx_std_20)
    target_compile_definitions(jamlink_desktop_controller_tests
        PRIVATE
            JAMLINK_VERSION_STRING="${PROJECT_VERSION}"
            JAMLINK_BUILD_IDENTITY_STRING="${JAMLINK_BUILD_IDENTITY}"
            JAMLINK_RELEASE_CHANNEL_STRING="${JAMLINK_RELEASE_CHANNEL}"
            JAMLINK_DIRECTORY_URL_STRING="${JAMLINK_DIRECTORY_URL}"
    )
    set_target_properties(jamlink_desktop_controller_tests PROPERTIES CXX_EXTENSIONS OFF)
    jamlink_set_warnings(jamlink_desktop_controller_tests)
    if(MSVC)
        target_compile_options(jamlink_desktop_controller_tests PRIVATE /external:W0 /wd4702)
    endif()
    add_test(NAME jamlink_desktop_controller_tests COMMAND jamlink_desktop_controller_tests)
    set_tests_properties(jamlink_desktop_controller_tests PROPERTIES TIMEOUT 30 LABELS "gui")

    set(_jamlink_visual_preferences "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-preferences.jlpf")
    add_test(
        NAME jamlink_desktop_visual_first_launch
        COMMAND jamlink_desktop
            --visual-fixture
            --reset-preferences
            --width 532
            --height 562
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-soundcheck.png"
    )
    add_test(
        NAME jamlink_desktop_visual_second_launch
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --width 532
            --height 562
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-home.png"
    )
    add_test(
        NAME jamlink_desktop_visual_settings
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page settings
            --width 532
            --height 562
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-settings.png"
    )
    add_test(
        NAME jamlink_desktop_visual_room
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page room
            --width 532
            --height 728
            --minimum-capture-height 728
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-room.png"
    )
    add_test(
        NAME jamlink_desktop_visual_named_room_create
        COMMAND jamlink_desktop
            --visual-fixture --expect-restored --page home
            --width 532 --height 680 --minimum-capture-height 680
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-named-create.png"
    )
    add_test(
        NAME jamlink_desktop_visual_named_room_guest_waiting
        COMMAND jamlink_desktop
            --visual-fixture --expect-restored --page room
            --width 532 --height 620 --minimum-capture-height 620
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-named-guest-waiting.png"
    )
    add_test(
        NAME jamlink_desktop_visual_named_room_host_drawer
        COMMAND jamlink_desktop
            --visual-fixture --expect-restored --page room
            --width 532 --height 728 --minimum-capture-height 728
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-named-host-drawer.png"
    )
    add_test(
        NAME jamlink_desktop_visual_named_room_host_drawer_wide
        COMMAND jamlink_desktop
            --visual-fixture --expect-restored --page room
            --width 980 --height 760 --minimum-capture-width 980 --minimum-capture-height 760
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-named-host-drawer-wide.png"
    )
    add_test(
        NAME jamlink_desktop_visual_named_room_recording_queue
        COMMAND jamlink_desktop
            --visual-fixture --expect-restored --page room
            --width 532 --height 728 --minimum-capture-height 728
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-named-recording-queue.png"
    )
    add_test(
        NAME jamlink_desktop_visual_tuner
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page tuner
            --width 532
            --height 588
            --minimum-capture-height 588
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-tuner.png"
    )
    add_test(
        NAME jamlink_desktop_visual_four_musician_room
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page room
            --width 532
            --height 1040
            --minimum-capture-height 1040
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-room-four.png"
    )
    add_test(
        NAME jamlink_desktop_visual_four_musician_room_wide
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page room
            --width 980
            --height 760
            --minimum-capture-width 980
            --minimum-capture-height 760
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-room-four-wide.png"
    )
    add_test(
        NAME jamlink_desktop_visual_high_dpi
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page soundcheck
            --width 532
            --height 562
            --expected-capture-width 798
            --expected-capture-height 843
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-high-dpi.png"
    )
    add_test(
        NAME jamlink_desktop_visual_clipping
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page soundcheck
            --width 532
            --height 562
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-clipping.png"
    )
    add_test(
        NAME jamlink_desktop_visual_update_prompt
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page home
            --width 532
            --height 562
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-update.png"
    )
    add_test(
        NAME jamlink_desktop_visual_200_percent
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page home
            --width 532
            --height 562
            --expected-capture-width 1064
            --expected-capture-height 1124
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-200-percent.png"
    )
    get_target_property(_jamlink_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_jamlink_qt_bin "${_jamlink_qmake}" DIRECTORY)
    # ENVIRONMENT_MODIFICATION takes one list value. Passing the entries as
    # separate arguments makes set_tests_properties read the second entry as the
    # next property name, which silently drops the offscreen/software settings
    # and leaves the captures dependent on the developer's real display.
    set(_jamlink_visual_environment
        "PATH=path_list_prepend:${_jamlink_qt_bin}"
        "QT_QPA_PLATFORM=set:offscreen"
        "QSG_RHI_BACKEND=set:software"
    )
    set_tests_properties(
        jamlink_desktop_visual_first_launch
        jamlink_desktop_visual_second_launch
        jamlink_desktop_visual_settings
        jamlink_desktop_visual_room
        jamlink_desktop_visual_named_room_create
        jamlink_desktop_visual_named_room_guest_waiting
        jamlink_desktop_visual_named_room_host_drawer
        jamlink_desktop_visual_named_room_host_drawer_wide
        jamlink_desktop_visual_named_room_recording_queue
        jamlink_desktop_visual_tuner
        jamlink_desktop_visual_four_musician_room
        jamlink_desktop_visual_four_musician_room_wide
        jamlink_desktop_visual_high_dpi
        jamlink_desktop_visual_clipping
        jamlink_desktop_visual_update_prompt
        jamlink_desktop_visual_200_percent
        PROPERTIES
        TIMEOUT 30
        LABELS "gui;visual"
        ENVIRONMENT_MODIFICATION "${_jamlink_visual_environment}"
    )
    set_tests_properties(jamlink_desktop_controller_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_jamlink_qt_bin}")
    set_tests_properties(jamlink_desktop_updater_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_jamlink_qt_bin}")
    if(TARGET jamlink_private_room_directory_tests)
        set_tests_properties(jamlink_private_room_directory_tests PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_jamlink_qt_bin}")
    endif()
    set_tests_properties(
        jamlink_desktop_visual_second_launch
        PROPERTIES DEPENDS jamlink_desktop_visual_first_launch)
    set_tests_properties(
        jamlink_desktop_visual_settings
        PROPERTIES DEPENDS jamlink_desktop_visual_second_launch)
    set_tests_properties(
        jamlink_desktop_visual_room
        PROPERTIES DEPENDS jamlink_desktop_visual_settings)
    set_tests_properties(
        jamlink_desktop_visual_tuner
        PROPERTIES DEPENDS jamlink_desktop_visual_room)
    set_tests_properties(
        jamlink_desktop_visual_named_room_create
        jamlink_desktop_visual_named_room_guest_waiting
        jamlink_desktop_visual_named_room_host_drawer
        jamlink_desktop_visual_named_room_host_drawer_wide
        jamlink_desktop_visual_named_room_recording_queue
        PROPERTIES DEPENDS jamlink_desktop_visual_room)
    set_property(TEST jamlink_desktop_visual_named_room_create APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "JAMLINK_VISUAL_PRIVATE_ROOM=set:create")
    set_property(TEST jamlink_desktop_visual_named_room_guest_waiting APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "JAMLINK_VISUAL_PRIVATE_ROOM=set:guest-waiting")
    set_property(TEST jamlink_desktop_visual_named_room_host_drawer APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "JAMLINK_VISUAL_PRIVATE_ROOM=set:host-drawer")
    set_property(TEST jamlink_desktop_visual_named_room_host_drawer_wide APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "JAMLINK_VISUAL_PRIVATE_ROOM=set:host-drawer")
    set_property(TEST jamlink_desktop_visual_named_room_recording_queue APPEND PROPERTY
        ENVIRONMENT_MODIFICATION
            "JAMLINK_VISUAL_PRIVATE_ROOM=set:host-drawer"
            "JAMLINK_VISUAL_RECORDING=set:1")
    set_tests_properties(
        jamlink_desktop_visual_four_musician_room
        PROPERTIES DEPENDS jamlink_desktop_visual_room)
    set_property(
        TEST jamlink_desktop_visual_four_musician_room APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "JAMLINK_VISUAL_ROOM_SIZE=set:4")
    set_tests_properties(
        jamlink_desktop_visual_four_musician_room_wide
        PROPERTIES DEPENDS jamlink_desktop_visual_four_musician_room)
    set_property(
        TEST jamlink_desktop_visual_four_musician_room_wide APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "JAMLINK_VISUAL_ROOM_SIZE=set:4")
    set_tests_properties(
        jamlink_desktop_visual_clipping
        PROPERTIES
        DEPENDS jamlink_desktop_visual_first_launch)
    set_property(
        TEST jamlink_desktop_visual_clipping APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "JAMLINK_VISUAL_CLIP=set:1")
    set_property(
        TEST jamlink_desktop_visual_update_prompt APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "JAMLINK_VISUAL_UPDATE=set:1")
    set_tests_properties(jamlink_desktop_visual_update_prompt PROPERTIES
        DEPENDS jamlink_desktop_visual_second_launch)
    set_tests_properties(jamlink_desktop_visual_200_percent PROPERTIES
        DEPENDS jamlink_desktop_visual_second_launch)
    set_property(
        TEST jamlink_desktop_visual_200_percent APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "QT_SCREEN_SCALE_FACTORS=set:2")
    add_test(
        NAME jamlink_desktop_visual_profile
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page profile
            --width 532
            --height 808
            --minimum-capture-height 808
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-profile.png"
    )
    set_tests_properties(jamlink_desktop_visual_profile PROPERTIES
        TIMEOUT 30
        LABELS "gui;visual"
        ENVIRONMENT_MODIFICATION "${_jamlink_visual_environment}"
        DEPENDS jamlink_desktop_visual_four_musician_room_wide)
    set_tests_properties(
        jamlink_desktop_visual_high_dpi
        PROPERTIES DEPENDS jamlink_desktop_visual_profile)
    set_property(
        TEST jamlink_desktop_visual_high_dpi APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "QT_SCREEN_SCALE_FACTORS=set:1.5")
endif()
