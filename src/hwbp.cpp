#include "hwbp.hpp"

#include <tlhelp32.h>

namespace revenant::detail {

BreakpointSlot   g_bp[4];
std::atomic<int> g_bpActive{ 0 };

namespace {
DWORD64& DrReg(CONTEXT& ctx, int index) {
    switch (index) {
    case 0:  return ctx.Dr0;
    case 1:  return ctx.Dr1;
    case 2:  return ctx.Dr2;
    default: return ctx.Dr3;
    }
}
} // namespace

void ApplyDr(CONTEXT& ctx, int index, void* address, bool enable) {
    const DWORD64 enableBit = 1ULL << (index * 2);
    const DWORD64 condMask = 0xFULL << (16 + index * 4);   // RW=00 LEN=00: execute
    if (enable) {
        DrReg(ctx, index) = reinterpret_cast<DWORD64>(address);
        ctx.Dr7 = (ctx.Dr7 & ~condMask) | enableBit;
    } else {
        DrReg(ctx, index) = 0;
        ctx.Dr7 &= ~(enableBit | condMask);
    }
}

namespace {

bool ApplyDrToThread(HANDLE thread, bool isSelf, int index, void* address, bool enable) {
    if (!isSelf && SuspendThread(thread) == static_cast<DWORD>(-1)) return false;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = GetThreadContext(thread, &ctx) != FALSE;
    if (ok) {
        ApplyDr(ctx, index, address, enable);
        ok = SetThreadContext(thread, &ctx) != FALSE;
    }
    if (!isSelf) ResumeThread(thread);
    return ok;
}

} // namespace

bool ApplyDrToAllThreads(int index, void* address, bool enable) {
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool selfOk = false;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == self) {
                selfOk = ApplyDrToThread(GetCurrentThread(), true, index, address, enable);
            } else if (HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                             THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID)) {
                ApplyDrToThread(h, false, index, address, enable);
                CloseHandle(h);
            }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return selfOk;
}

LONG CALLBACK BreakpointHandler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (g_bpActive.load(std::memory_order_acquire) == 0) return EXCEPTION_CONTINUE_SEARCH;
    void* const addr = ep->ExceptionRecord->ExceptionAddress;
    for (auto& bp : g_bp) {
        if (bp.target.load(std::memory_order_acquire) != addr) continue;
        void* detour = bp.detour.load(std::memory_order_acquire);
        if (!detour) continue;
        // The detour reaches the original through the trampoline, never through
        // the breakpointed address, so there is nothing to step over.
        ep->ContextRecord->Dr6 = 0;
        ep->ContextRecord->Rip = reinterpret_cast<DWORD64>(detour);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace revenant::detail

namespace revenant {

void ApplyBreakpointsToCurrentThread() noexcept {
    using namespace detail;
    if (g_bpActive.load(std::memory_order_acquire) == 0) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(GetCurrentThread(), &ctx)) return;
    for (int i = 0; i < 4; ++i)
        if (void* t = g_bp[i].target.load(std::memory_order_acquire)) ApplyDr(ctx, i, t, true);
    SetThreadContext(GetCurrentThread(), &ctx);
}

} // namespace revenant
