cmake_minimum_required(VERSION 3.15)
project(TorrentClient VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
if(MSVC)
    add_compile_options(/W4 /WX-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Source files
set(SOURCES
    src/main.cpp
    src/bencode.cpp
    src/utils.cpp
    src/torrent_file.cpp
    src/tracker_client.cpp
    src/udp_tracker.cpp
    src/peer_connection.cpp
    src/piece_manager.cpp
    src/file_manager.cpp
    src/download_manager.cpp
    src/logger.cpp
    src/config.cpp
)

# Header files
set(HEADERS
    include/bencode.h
    include/torrent_file.h
    include/tracker_client.h
    include/udp_tracker.h
    include/peer_connection.h
    include/piece_manager.h
    include/file_manager.h
    include/download_manager.h
    include/utils.h
    include/logger.h
    include/config.h
)

# Fetch external libraries
include(FetchContent)

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.12.0
)
FetchContent_MakeAvailable(spdlog)

# nlohmann/json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# Find dependencies
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# Executable
add_executable(torrent_client ${SOURCES} ${HEADERS})

# Link libraries
target_link_libraries(torrent_client
    PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(torrent_client PRIVATE ws2_32 wsock32)
endif()

# Tests (optional)
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
install(TARGETS torrent_client DESTINATION bin)
