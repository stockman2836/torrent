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
    src/dht_node.cpp
    src/dht_krpc.cpp
    src/dht_routing_table.cpp
    src/dht_manager.cpp
    src/magnet_uri.cpp
    src/extension_protocol.cpp
    src/metadata_exchange.cpp
    src/magnet_download_manager.cpp
    src/pex_manager.cpp
    src/crypto.cpp
    src/mse_handshake.cpp
    src/lsd.cpp
    src/web_seed.cpp
    src/utp_socket.cpp
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
    include/dht_node.h
    include/dht_krpc.h
    include/dht_routing_table.h
    include/dht_manager.h
    include/magnet_uri.h
    include/extension_protocol.h
    include/metadata_exchange.h
    include/magnet_download_manager.h
    include/pex_manager.h
    include/crypto.h
    include/mse_handshake.h
    include/lsd.h
    include/web_seed.h
    include/utp_socket.h
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

# Optional: libutp for uTP support (BEP 29)
option(ENABLE_UTP "Enable uTP support (requires libutp)" OFF)

if(ENABLE_UTP)
    # Try to find libutp
    find_path(LIBUTP_INCLUDE_DIR utp.h
        HINTS
        ${CMAKE_SOURCE_DIR}/external/libutp
        /usr/include
        /usr/local/include
    )

    find_library(LIBUTP_LIBRARY
        NAMES utp libutp
        HINTS
        ${CMAKE_SOURCE_DIR}/external/libutp
        /usr/lib
        /usr/local/lib
    )

    if(LIBUTP_INCLUDE_DIR AND LIBUTP_LIBRARY)
        message(STATUS "Found libutp: ${LIBUTP_LIBRARY}")
        add_definitions(-DHAVE_LIBUTP)
        include_directories(${LIBUTP_INCLUDE_DIR})
        set(UTP_LIBRARIES ${LIBUTP_LIBRARY})
    else()
        message(WARNING "libutp not found - uTP support will be disabled")
        message(WARNING "To enable uTP:")
        message(WARNING "  1. Clone libutp: git clone https://github.com/bittorrent/libutp.git external/libutp")
        message(WARNING "  2. Build libutp")
        message(WARNING "  3. Reconfigure CMake with -DENABLE_UTP=ON")
        set(UTP_LIBRARIES "")
    endif()
else()
    message(STATUS "uTP support disabled (use -DENABLE_UTP=ON to enable)")
    set(UTP_LIBRARIES "")
endif()

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
    ${UTP_LIBRARIES}
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
