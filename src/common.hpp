#pragma once

#include <revenant/revenant.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>

namespace revenant::detail {

constexpr size_t   kSlotSize      = 128;
constexpr size_t   kRelaySize     = 16;
constexpr size_t   kBlockSize     = 0x10000;
constexpr size_t   kSlotsPerBlock = kBlockSize / kSlotSize;
constexpr size_t   kJmpRel32Size  = 5;
constexpr size_t   kAbsJmpSize    = 14;
constexpr size_t   kMaxInsns      = 16;
constexpr size_t   kHotPatchPad   = 5;
constexpr intptr_t kMaxReach      = 0x7FF00000;   // 2 GB minus 1 MB slack

inline bool IsReadableProtect(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protect & 0xFF) {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

inline bool IsReadable(const void* p, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || !IsReadableProtect(mbi.Protect)) return false;
        a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

inline bool IsExecutable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || !IsReadableProtect(mbi.Protect)) return false;
    switch (mbi.Protect & 0xFF) {
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

inline bool FitsRel32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

inline int64_t Rel32(const void* from, size_t insnLen, const void* to) {
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(to)) -
           static_cast<int64_t>(reinterpret_cast<uintptr_t>(from) + insnLen);
}

inline bool InReach(uintptr_t a, uintptr_t b) {
    const intptr_t d = static_cast<intptr_t>(a - b);
    return d > -kMaxReach && d < kMaxReach;
}

// jmp [rip+0]; dq to
inline void EmitAbsJmp(uint8_t* p, uintptr_t to) {
    p[0] = 0xFF; p[1] = 0x25;
    *reinterpret_cast<uint32_t*>(p + 2) = 0;
    *reinterpret_cast<uint64_t*>(p + 6) = to;
}

class ScopedWritable {
public:
    ScopedWritable(void* addr, size_t size) : m_addr(addr), m_size(size) {
        m_ok = VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &m_old) != FALSE;
    }
    ~ScopedWritable() {
        if (!m_ok) return;
        DWORD tmp = 0;
        VirtualProtect(m_addr, m_size, m_old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), m_addr, m_size);
    }
    explicit operator bool() const { return m_ok; }
    ScopedWritable(const ScopedWritable&) = delete;
    ScopedWritable& operator=(const ScopedWritable&) = delete;

private:
    void*  m_addr;
    size_t m_size;
    DWORD  m_old = 0;
    bool   m_ok = false;
};

} // namespace revenant::detail
