add_library(torrent_app STATIC application.cpp command_line.cpp input.cpp download_task.cpp download_queue.cpp speed_limits.cpp seeding_options.cpp network_settings.cpp)
target_compile_features(torrent_app PUBLIC cxx_std_17)
target_include_directories(torrent_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(torrent_app PUBLIC Torrent::Session)
target_link_libraries(torrent_app PRIVATE nlohmann_json::nlohmann_json)

add_executable(torrent_client main.cpp)
target_link_libraries(torrent_client PRIVATE torrent_app)
