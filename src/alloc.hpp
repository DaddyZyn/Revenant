#pragma once

#include "common.hpp"

#include <vector>

namespace revenant::detail {

// 64 KB blocks within +-2 GB of the requesting target, split into 128-byte slots.
class SlotAllocator {
public:
    uint8_t* Allocate(const void* nearTo);
    void     Free(uint8_t* slot);
    bool     Contains(const void* p) const;
    void     ReleaseAll();

private:
    struct Block {
        uint8_t* base = nullptr;
        uint64_t bits[kSlotsPerBlock / 64] = {};
        size_t   used = 0;
    };

    static uint8_t* Take(Block& b);

    std::vector<Block> m_blocks;
};

} // namespace revenant::detail
