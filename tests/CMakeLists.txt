find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest_main)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# Link each suite to the module it exercises.
function(add_unit_test test_name library)
    add_executable(${test_name} unit/${test_name}.cpp)
    target_link_libraries(${test_name}
        PRIVATE
        ${library}
        GTest::gtest_main
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# Add unit tests
add_unit_test(test_bencode Torrent::Metainfo)
add_unit_test(test_torrent_file Torrent::Metainfo)
add_unit_test(test_piece_manager Torrent::Storage)
add_unit_test(test_file_manager Torrent::Storage)
add_unit_test(test_command_line torrent_app)
add_unit_test(test_extension_protocol Torrent::Network)
add_unit_test(test_metadata_exchange Torrent::Network)

set_tests_properties(test_bencode test_torrent_file test_piece_manager test_file_manager test_command_line
    test_extension_protocol test_metadata_exchange
    PROPERTIES TIMEOUT 30)
find_program(NODE_EXECUTABLE NAMES node)
if(NODE_EXECUTABLE)
    add_executable(partial_download integration/partial_download.cpp)
    target_link_libraries(partial_download PRIVATE Torrent::Session)
    add_test(NAME test_download COMMAND "${NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_download.cjs" "$<TARGET_FILE:torrent_client>")
    set_tests_properties(test_download PROPERTIES TIMEOUT 55)
    foreach(scenario multi resume corrupt disconnect multifile)
        add_test(NAME test_transfer_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_transfer.cjs" ${scenario}
            "$<TARGET_FILE:torrent_client>" "$<TARGET_FILE:partial_download>")
        set_tests_properties(test_transfer_${scenario} PROPERTIES TIMEOUT 40)
    endforeach()
    foreach(scenario download reject hash tracker invalid stall)
        add_test(NAME test_magnet_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_magnet.cjs" ${scenario}
            "$<TARGET_FILE:torrent_client>")
        set_tests_properties(test_magnet_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
endif()

# Optional: Add coverage flags if needed
if(CMAKE_BUILD_TYPE STREQUAL "Coverage")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        foreach(target torrent_core torrent_metainfo torrent_storage torrent_network torrent_session)
            target_compile_options(${target} PUBLIC --coverage -fprofile-arcs -ftest-coverage)
            target_link_options(${target} PUBLIC --coverage)
        endforeach()
    endif()
endif()
