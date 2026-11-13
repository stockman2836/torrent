add_library(torrent_app STATIC
    input.cpp
    download_task.cpp
    download_queue.cpp
    download_queue_state.cpp
    paths.cpp
    speed_limits.cpp
    seeding_options.cpp
    network_settings.cpp
    network_diagnostics.cpp
    tracker_diagnostics.cpp)
target_compile_features(torrent_app PUBLIC cxx_std_17)
target_include_directories(torrent_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(torrent_app PUBLIC Torrent::Session)
target_link_libraries(torrent_app PRIVATE nlohmann_json::nlohmann_json)

# CLI parsing and the console lifecycle are not dependencies of the desktop.
add_library(torrent_cli STATIC cli/application.cpp cli/command_line.cpp)
target_include_directories(torrent_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cli")
target_link_libraries(torrent_cli PUBLIC torrent_app)

add_executable(torrent_client cli/main.cpp)
target_link_libraries(torrent_client PRIVATE torrent_cli)
