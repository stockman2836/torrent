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

# Tests link the same library as the CLI, including all protocol modules.

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

set_tests_properties(test_bencode test_torrent_file test_piece_manager test_file_manager
    PROPERTIES TIMEOUT 30)
find_program(NODE_EXECUTABLE NAMES node)
if(NODE_EXECUTABLE)
    add_test(NAME test_download COMMAND "${NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_download.cjs" "$<TARGET_FILE:torrent_client>")
    set_tests_properties(test_download PROPERTIES TIMEOUT 55)
endif()

# Optional: Add coverage flags if needed
if(CMAKE_BUILD_TYPE STREQUAL "Coverage")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(torrent_lib PUBLIC --coverage -fprofile-arcs -ftest-coverage)
        target_link_options(torrent_lib PUBLIC --coverage)
    endif()
endif()
