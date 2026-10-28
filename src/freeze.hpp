#pragma once

#include "common.hpp"

#include <vector>

namespace revenant::detail {

// Suspends every other thread in the process until destroyed.
class Freeze {
public:
    Freeze() = default;
    ~Freeze();
    Freeze(const Freeze&) = delete;
    Freeze& operator=(const Freeze&) = delete;

    Status Begin();

    // f(DWORD64& rip) returns true if it changed rip.
    template <class F>
    void ForEachRip(F&& f) {
        for (HANDLE h : m_threads) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(h, &ctx)) continue;
            if (f(ctx.Rip)) SetThreadContext(h, &ctx);
        }
    }

private:
    std::vector<HANDLE> m_threads;
};

} // namespace revenant::detail
