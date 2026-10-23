add_library(torrent_core STATIC core/utils.cpp core/crypto.cpp core/logger.cpp core/config.cpp)
torrent_target(torrent_core Core)
target_link_libraries(torrent_core PUBLIC Threads::Threads spdlog::spdlog
    PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(torrent_core PUBLIC ws2_32)
endif()

add_library(torrent_metainfo STATIC metainfo/bencode.cpp metainfo/torrent_file.cpp metainfo/magnet_uri.cpp)
torrent_target(torrent_metainfo Metainfo)
target_link_libraries(torrent_metainfo PUBLIC Torrent::Core)

add_library(torrent_storage STATIC storage/file_manager.cpp storage/piece_manager.cpp)
torrent_target(torrent_storage Storage)
target_link_libraries(torrent_storage PUBLIC Torrent::Metainfo)

add_library(torrent_network STATIC
    network/tracker/tracker_client.cpp
    network/tracker/udp_tracker.cpp
    network/dht/dht_node.cpp
    network/dht/dht_krpc.cpp
    network/dht/dht_routing_table.cpp
    network/dht/dht_manager.cpp
    network/peer/connection.cpp
    network/peer/handshake.cpp
    network/peer/messages.cpp
    network/peer/send.cpp
    network/peer/requests.cpp
    network/peer/extensions.cpp
    network/peer/extension_protocol.cpp
    network/peer/metadata_exchange.cpp
    network/peer/mse_handshake.cpp
    network/discovery/pex_manager.cpp
    network/discovery/lsd.cpp
    network/transport/web_seed.cpp
    network/transport/utp_socket.cpp)
torrent_target(torrent_network Network)
target_link_libraries(torrent_network PUBLIC Torrent::Metainfo PRIVATE CURL::libcurl)
if(ENABLE_UTP)
    target_include_directories(torrent_network PUBLIC "${LIBUTP_INCLUDE_DIR}")
    target_compile_definitions(torrent_network PUBLIC HAVE_LIBUTP)
    target_link_libraries(torrent_network PRIVATE "${LIBUTP_LIBRARY}")
endif()

add_library(torrent_session STATIC
    session/download_manager.cpp
    session/peers.cpp
    session/transfer.cpp
    session/scheduling.cpp
    session/tracker.cpp
    session/resume.cpp
    session/discovery.cpp
    session/magnet_download_manager.cpp)
torrent_target(torrent_session Session)
target_link_libraries(torrent_session PUBLIC Torrent::Storage Torrent::Network)
