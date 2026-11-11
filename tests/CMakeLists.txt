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
add_unit_test(test_file_selection Torrent::Metainfo)
add_unit_test(test_download_task torrent_app)
add_unit_test(test_download_queue torrent_app)
add_unit_test(test_speed_limits torrent_app)
add_unit_test(test_token_bucket Torrent::Core)
add_unit_test(test_seeding_options torrent_app)
add_unit_test(test_peer_listener Torrent::Network)
add_unit_test(test_dht Torrent::Network)
set_tests_properties(test_dht PROPERTIES TIMEOUT 15)
set_tests_properties(test_peer_listener PROPERTIES TIMEOUT 15)
set_tests_properties(test_speed_limits test_token_bucket PROPERTIES TIMEOUT 15)
target_link_libraries(test_download_queue PRIVATE nlohmann_json::nlohmann_json)
set_tests_properties(test_download_queue PROPERTIES TIMEOUT 30)
set_tests_properties(test_download_task PROPERTIES TIMEOUT 30)

set_tests_properties(test_bencode test_torrent_file test_piece_manager test_file_manager test_command_line
    test_extension_protocol test_metadata_exchange
    test_file_selection
    PROPERTIES TIMEOUT 30)
find_program(NODE_EXECUTABLE NAMES node)
set(INTEROP_QBITTORRENT_EXECUTABLE "" CACHE FILEPATH
    "Optional qBittorrent 4.6.x executable for isolated interoperability tests")
option(ENABLE_DHT_INTEROP_TESTS "Run local tests against the optional bittorrent-dht npm dependency" OFF)
if(NODE_EXECUTABLE)
    find_program(OPENSSL_PROGRAM NAMES openssl)
    if(OPENSSL_PROGRAM)
        add_executable(tracker_tls_driver integration/tracker_tls_driver.cpp)
        target_link_libraries(tracker_tls_driver PRIVATE Torrent::Network)
        add_test(NAME test_tracker_tls COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_tracker_tls.cjs"
            "$<TARGET_FILE:tracker_tls_driver>" "${OPENSSL_PROGRAM}")
        set_tests_properties(test_tracker_tls PROPERTIES TIMEOUT 20)
    endif()
    add_executable(dht_driver integration/dht_driver.cpp)
    target_link_libraries(dht_driver PRIVATE torrent_app)
    foreach(scenario direct chain fallback error spoof late cancel metadata-cancel occupied)
        add_test(NAME test_dht_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_dht.cjs" "$<TARGET_FILE:dht_driver>" ${scenario})
        set_tests_properties(test_dht_${scenario} PROPERTIES TIMEOUT 20)
    endforeach()
    if(ENABLE_DHT_INTEROP_TESTS)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/node_modules/bittorrent-dht/package.json")
            message(FATAL_ERROR "Run npm ci --ignore-scripts in tests/integration before enabling DHT interoperability tests")
        endif()
        add_test(NAME test_dht_reference COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_dht.cjs" "$<TARGET_FILE:dht_driver>" reference)
        set_tests_properties(test_dht_reference PROPERTIES TIMEOUT 20 LABELS interop)
    endif()
    add_executable(incoming_driver integration/incoming_driver.cpp)
    target_link_libraries(incoming_driver PRIVATE torrent_app)
    if(INTEROP_QBITTORRENT_EXECUTABLE)
        if(NOT WIN32)
            message(FATAL_ERROR "qBittorrent interoperability tests currently require Windows")
        endif()
        if(NOT EXISTS "${INTEROP_QBITTORRENT_EXECUTABLE}")
            message(FATAL_ERROR "INTEROP_QBITTORRENT_EXECUTABLE does not exist")
        endif()
        foreach(scenario download upload-incoming upload-outgoing magnet resume)
            add_test(NAME test_interop_${scenario} COMMAND "${NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_interop.cjs"
                "$<TARGET_FILE:incoming_driver>" "$<TARGET_FILE:torrent_client>"
                "${INTEROP_QBITTORRENT_EXECUTABLE}" ${scenario})
            set_tests_properties(test_interop_${scenario} PROPERTIES TIMEOUT 100 LABELS interop RUN_SERIAL TRUE)
        endforeach()
        if(ENABLE_DHT_INTEROP_TESTS)
            add_test(NAME test_interop_dht COMMAND "${NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_interop.cjs"
                "$<TARGET_FILE:incoming_driver>" "$<TARGET_FILE:torrent_client>"
                "${INTEROP_QBITTORRENT_EXECUTABLE}" dht)
            set_tests_properties(test_interop_dht PROPERTIES TIMEOUT 100 LABELS interop RUN_SERIAL TRUE)
        endif()
    endif()
    foreach(scenario transfer validate missing corrupt limit stop stalled cap restart occupied ipv6 multi receive fast)
        add_test(NAME test_incoming_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_incoming.cjs" "$<TARGET_FILE:incoming_driver>" ${scenario})
        set_tests_properties(test_incoming_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(queue_driver integration/queue_driver.cpp)
    target_link_libraries(queue_driver PRIVATE torrent_app)
    foreach(scenario serial parallel stop remove collision limited live limited-stop upload upload-live upload-stop
            seed-ratio seed-unlimited seed-disable seed-resume seed-collision)
        add_test(NAME test_queue_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_queue.cjs" "$<TARGET_FILE:queue_driver>" ${scenario})
        set_tests_properties(test_queue_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(desktop_driver integration/desktop_driver.cpp)
    target_link_libraries(desktop_driver PRIVATE torrent_app)
    add_executable(desktop_partial integration/desktop_driver.cpp)
    target_compile_definitions(desktop_partial PRIVATE TASK_PARTIAL)
    target_link_libraries(desktop_partial PRIVATE torrent_app)
    foreach(scenario resume corrupt)
        add_test(NAME test_desktop_stop_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_transfer.cjs" ${scenario}
            "$<TARGET_FILE:desktop_driver>" "$<TARGET_FILE:desktop_partial>")
        set_tests_properties(test_desktop_stop_${scenario} PROPERTIES TIMEOUT 40)
    endforeach()
    add_test(NAME test_desktop_download COMMAND "${NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_download.cjs" "$<TARGET_FILE:desktop_driver>")
    set_tests_properties(test_desktop_download PROPERTIES TIMEOUT 55)
    foreach(scenario single multiple zero resume corrupt changed invalid preserve seed)
        add_test(NAME test_desktop_selection_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_selection.cjs" ${scenario}
            "$<TARGET_FILE:desktop_driver>")
        set_tests_properties(test_desktop_selection_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
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
    foreach(scenario download reject hash tracker invalid stall list fast dht dht-chain dht-fallback dht-tracker dht-disabled dht-torrent)
        add_test(NAME test_magnet_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_magnet.cjs" ${scenario}
            "$<TARGET_FILE:torrent_client>")
        set_tests_properties(test_magnet_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    foreach(scenario cancel late cap disconnect)
        add_test(NAME test_endgame_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_endgame.cjs" ${scenario}
            "$<TARGET_FILE:torrent_client>")
        set_tests_properties(test_endgame_${scenario} PROPERTIES TIMEOUT 15)
    endforeach()
    foreach(scenario list single multiple zero resume corrupt changed invalid preserve)
        add_test(NAME test_selection_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_selection.cjs" ${scenario}
            "$<TARGET_FILE:torrent_client>")
        set_tests_properties(test_selection_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    foreach(scenario order default boundary allskip invalid outside resume list)
        add_test(NAME test_priority_${scenario} COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_priority.cjs" ${scenario}
            "$<TARGET_FILE:torrent_client>")
        set_tests_properties(test_priority_${scenario} PROPERTIES TIMEOUT 30)
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
