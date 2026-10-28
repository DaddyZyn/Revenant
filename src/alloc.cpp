#include "alloc.hpp"

namespace revenant::detail {
namespace {

uintptr_t FindPrevFree(uintptr_t addr, uintptr_t minAddr, uintptr_t gran) {
    uintptr_t tryAddr = addr - addr % gran;
    if (tryAddr < minAddr + gran) return 0;
    tryAddr -= gran;
    while (tryAddr >= minAddr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(tryAddr), &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_FREE) return tryAddr;
        const uintptr_t allocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        if (allocBase < minAddr + gran) break;
        tryAddr = allocBase - allocBase % gran - gran;
    }
    return 0;
}

uintptr_t FindNextFree(uintptr_t addr, uintptr_t maxAddr, uintptr_t gran) {
    uintptr_t tryAddr = addr - addr % gran + gran;
    while (tryAddr <= maxAddr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(tryAddr), &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_FREE) return tryAddr;
        tryAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        tryAddr = (tryAddr + gran - 1) - (tryAddr + gran - 1) % gran;
    }
    return 0;
}

uint8_t* TryCommit(uintptr_t at) {
    return static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), kBlockSize,
                                              MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READ));
}

// Alternate below/above the target so the block lands as close as possible.
uint8_t* AllocateBlockNear(uintptr_t target) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    uintptr_t minAddr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    if (target > static_cast<uintptr_t>(kMaxReach) && target - kMaxReach > minAddr)
        minAddr = target - kMaxReach;
    if (maxAddr - target > static_cast<uintptr_t>(kMaxReach))
        maxAddr = target + kMaxReach - kBlockSize;

    uintptr_t down = target, up = target;
    bool downDone = false, upDone = false;
    while (!downDone || !upDone) {
        if (!downDone) {
            down = FindPrevFree(down, minAddr, gran);
            if (!down) downDone = true;
            else if (uint8_t* p = TryCommit(down)) return p;
        }
        if (!upDone) {
            up = FindNextFree(up, maxAddr, gran);
            if (!up) upDone = true;
            else if (uint8_t* p = TryCommit(up)) return p;
        }
    }
    return nullptr;
}

} // namespace

uint8_t* SlotAllocator::Allocate(const void* nearTo) {
    const uintptr_t target = reinterpret_cast<uintptr_t>(nearTo);
    for (auto& b : m_blocks) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(b.base);
        if (b.used == kSlotsPerBlock) continue;
        if (!InReach(base, target) || !InReach(base + kBlockSize, target)) continue;
        if (uint8_t* s = Take(b)) return s;
    }
    uint8_t* mem = AllocateBlockNear(target);
    if (!mem) return nullptr;
    m_blocks.push_back(Block{ mem });
    return Take(m_blocks.back());
}

void SlotAllocator::Free(uint8_t* slot) {
    for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
        if (slot < it->base || slot >= it->base + kBlockSize) continue;
        const size_t idx = static_cast<size_t>(slot - it->base) / kSlotSize;
        it->bits[idx / 64] &= ~(1ULL << (idx % 64));
        if (--it->used == 0) {
            VirtualFree(it->base, 0, MEM_RELEASE);
            m_blocks.erase(it);
        }
        return;
    }
}

bool SlotAllocator::Contains(const void* p) const {
    for (auto& b : m_blocks)
        if (p >= b.base && p < b.base + kBlockSize) return true;
    return false;
}

void SlotAllocator::ReleaseAll() {
    for (auto& b : m_blocks) VirtualFree(b.base, 0, MEM_RELEASE);
    m_blocks.clear();
}

uint8_t* SlotAllocator::Take(Block& b) {
    for (size_t w = 0; w < kSlotsPerBlock / 64; ++w) {
        if (b.bits[w] == ~0ULL) continue;
        for (size_t bit = 0; bit < 64; ++bit) {
            if (b.bits[w] & (1ULL << bit)) continue;
            b.bits[w] |= 1ULL << bit;
            ++b.used;
            return b.base + (w * 64 + bit) * kSlotSize;
        }
    }
    return nullptr;
}

} // namespace revenant::detail
