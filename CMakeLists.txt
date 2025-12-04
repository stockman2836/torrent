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
    src/peer_connection.cpp
    src/piece_manager.cpp
    src/file_manager.cpp
    src/download_manager.cpp
)

# Header files
set(HEADERS
    include/bencode.h
    include/torrent_file.h
    include/tracker_client.h
    include/peer_connection.h
    include/piece_manager.h
    include/file_manager.h
    include/download_manager.h
    include/utils.h
)

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
