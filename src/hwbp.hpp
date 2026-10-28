#pragma once

#include "common.hpp"

#include <atomic>

namespace revenant::detail {

// Read by the VEH without locks; `reserved` is guarded by the engine mutex.
struct BreakpointSlot {
    std::atomic<void*> target{ nullptr };
    std::atomic<void*> detour{ nullptr };
    bool reserved = false;
};

extern BreakpointSlot   g_bp[4];
extern std::atomic<int> g_bpActive;

void ApplyDr(CONTEXT& ctx, int index, void* address, bool enable);
bool ApplyDrToAllThreads(int index, void* address, bool enable);
LONG CALLBACK BreakpointHandler(PEXCEPTION_POINTERS ep);

} // namespace revenant::detail
