#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace torrent {

constexpr size_t BLOCK_SIZE = 16384;

struct Block {
    uint32_t piece_index;
    uint32_t offset;
    uint32_t length;
    std::vector<uint8_t> data;
    bool downloaded = false;

    Block(uint32_t index, uint32_t begin, uint32_t size)
        : piece_index(index), offset(begin), length(size) {}
};

} // namespace torrent
