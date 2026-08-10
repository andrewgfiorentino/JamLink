# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Qt6 6.10.3 REQUIRED COMPONENTS Core Gui Qml Quick)

qt_standard_project_setup(REQUIRES 6.10.3)

qt_add_executable(jamlink_desktop
    app_controller.cpp
    app_controller.hpp
    main.cpp
)

qt_add_qml_module(jamlink_desktop
    URI JamLinkDesktop
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/components/DeviceSelector.qml
        qml/components/IconButton.qml
        qml/components/JamButton.qml
        qml/components/JamCard.qml
        qml/components/JamIcon.qml
        qml/components/JamSlider.qml
        qml/components/JamSwitch.qml
        qml/components/LevelBar.qml
        qml/pages/HomePage.qml
        qml/pages/RoomPage.qml
        qml/pages/SettingsPage.qml
        qml/pages/SoundCheckPage.qml
    RESOURCES
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
        Qt6::Qml
        Qt6::Quick
)
target_compile_features(jamlink_desktop PRIVATE cxx_std_20)
set_target_properties(jamlink_desktop PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME JamLink
)
jamlink_set_warnings(jamlink_desktop)
if(MSVC)
    # Keep /W4 /WX for JamLink sources. Qt's generated QML cache translation
    # units instantiate code that emits the audited C4702 diagnostic internally.
    target_compile_options(jamlink_desktop PRIVATE /external:W0 /wd4702)
endif()

if(BUILD_TESTING)
    add_executable(jamlink_desktop_controller_tests
        app_controller.cpp
        app_controller.hpp
        controller_tests.cpp
    )
    target_link_libraries(
        jamlink_desktop_controller_tests
        PRIVATE JamLink::Core JamLink::PlatformAudio Qt6::Core Qt6::Gui Qt6::Qml)
    target_compile_features(jamlink_desktop_controller_tests PRIVATE cxx_std_20)
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
            --height 534
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-soundcheck.png"
    )
    add_test(
        NAME jamlink_desktop_visual_second_launch
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --width 532
            --height 534
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
            --height 534
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-settings.png"
    )
    add_test(
        NAME jamlink_desktop_visual_high_dpi
        COMMAND jamlink_desktop
            --visual-fixture
            --expect-restored
            --page soundcheck
            --width 532
            --height 534
            --expected-capture-width 798
            --expected-capture-height 801
            --preferences "${_jamlink_visual_preferences}"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/visual-smoke-high-dpi.png"
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
        jamlink_desktop_visual_high_dpi
        PROPERTIES
        TIMEOUT 30
        LABELS "gui;visual"
        ENVIRONMENT_MODIFICATION "${_jamlink_visual_environment}"
    )
    set_tests_properties(jamlink_desktop_controller_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_jamlink_qt_bin}")
    set_tests_properties(
        jamlink_desktop_visual_second_launch
        PROPERTIES DEPENDS jamlink_desktop_visual_first_launch)
    set_tests_properties(
        jamlink_desktop_visual_settings
        PROPERTIES DEPENDS jamlink_desktop_visual_second_launch)
    set_tests_properties(
        jamlink_desktop_visual_high_dpi
        PROPERTIES DEPENDS jamlink_desktop_visual_settings)
    set_property(
        TEST jamlink_desktop_visual_high_dpi APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "QT_SCREEN_SCALE_FACTORS=set:1.5")
endif()
