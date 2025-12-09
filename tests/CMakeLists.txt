cmake_minimum_required(VERSION 3.15)

# Enable testing
enable_testing()

# Download and configure Google Test
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Create a library from the source files (excluding main.cpp)
add_library(torrent_lib STATIC
    ${PROJECT_SOURCE_DIR}/src/bencode.cpp
    ${PROJECT_SOURCE_DIR}/src/utils.cpp
    ${PROJECT_SOURCE_DIR}/src/torrent_file.cpp
    ${PROJECT_SOURCE_DIR}/src/tracker_client.cpp
    ${PROJECT_SOURCE_DIR}/src/peer_connection.cpp
    ${PROJECT_SOURCE_DIR}/src/piece_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/file_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/download_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/logger.cpp
)

target_link_libraries(torrent_lib
    PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    spdlog::spdlog
)

if(WIN32)
    target_link_libraries(torrent_lib PUBLIC ws2_32 wsock32)
endif()

# Helper macro to create test executables
macro(add_unit_test test_name test_file)
    add_executable(${test_name} ${test_file})
    target_link_libraries(${test_name}
        PRIVATE
        torrent_lib
        gtest
        gtest_main
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endmacro()

# Add unit tests
add_unit_test(test_bencode test_bencode.cpp)
add_unit_test(test_torrent_file test_torrent_file.cpp)
add_unit_test(test_piece_manager test_piece_manager.cpp)
add_unit_test(test_file_manager test_file_manager.cpp)

# Optional: Add coverage flags if needed
if(CMAKE_BUILD_TYPE STREQUAL "Coverage")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(torrent_lib PUBLIC --coverage -fprofile-arcs -ftest-coverage)
        target_link_options(torrent_lib PUBLIC --coverage)
    endif()
endif()
