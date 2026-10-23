foreach(name http_client tcp_socket handshake)
    add_executable(tool_${name} ${name}.cpp)
    target_link_libraries(tool_${name} PRIVATE Torrent::Network)
endforeach()
