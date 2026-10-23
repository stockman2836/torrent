foreach(name bitfield_management message_parsing piece_assembly piece_request piece_selection)
    add_executable(example_${name} ${name}_example.cpp)
    target_link_libraries(example_${name} PRIVATE Torrent::Session)
endforeach()
