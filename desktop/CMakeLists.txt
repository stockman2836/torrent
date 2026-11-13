add_executable(torrent_desktop WIN32
    main.cpp
    main_window.cpp
    main_window_layout.cpp
    main_window_actions.cpp
    main_window_view.cpp
    main_window_smoke.cpp
    win32_helpers.cpp
    seeding_dialog.cpp
    network_dialog.cpp
    tracker_dialog.cpp
    network_settings_dialog.cpp
    global_speed_dialog.cpp
    resources.rc)
set_target_properties(torrent_desktop PROPERTIES OUTPUT_NAME TorrentClient)
target_compile_definitions(torrent_desktop PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(torrent_desktop PRIVATE torrent_app comctl32 comdlg32 shell32 ole32 uuid gdi32 uxtheme)
if(MINGW)
    target_compile_options(torrent_desktop PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_link_options(torrent_desktop PRIVATE -municode -static -static-libgcc -static-libstdc++)
    get_filename_component(toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET torrent_desktop POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP=$<TARGET_FILE:torrent_desktop>"
            "-DTOOLCHAIN_BIN=${toolchain_bin}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/copy_runtime.cmake"
        VERBATIM)
endif()
install(TARGETS torrent_desktop RUNTIME DESTINATION bin)
if(MINGW)
    install(DIRECTORY "$<TARGET_FILE_DIR:torrent_desktop>/" DESTINATION bin
        FILES_MATCHING PATTERN "*.dll")
endif()
