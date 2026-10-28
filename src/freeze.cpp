#include "freeze.hpp"

#include <tlhelp32.h>

namespace revenant::detail {

Status Freeze::Begin() {
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return Status::ThreadEnumFailed;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
                tids.push_back(te.th32ThreadID);
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    // Reserve before suspending: a suspended thread may own the heap lock.
    m_threads.reserve(tids.size());
    for (DWORD tid : tids) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                              FALSE, tid);
        if (!h) continue;   // thread exited after the snapshot
        if (SuspendThread(h) == static_cast<DWORD>(-1)) {
            CloseHandle(h);
            continue;
        }
        m_threads.push_back(h);
    }
    return Status::Ok;
}

Freeze::~Freeze() {
    for (HANDLE h : m_threads) {
        ResumeThread(h);
        CloseHandle(h);
    }
}

} // namespace revenant::detail
