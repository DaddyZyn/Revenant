#include "alloc.hpp"
#include "freeze.hpp"
#include "hwbp.hpp"
#include "reloc.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace revenant {
namespace {

using namespace detail;

struct Record : CodeSlot {
    Kind     kind = Kind::Inline;
    uint8_t* target = nullptr;            // code address, or slot address for VTable
    std::vector<void*> chain;             // back() is the active detour
    bool     enabled = false;
    uint8_t  saved[32] = {};              // original bytes (hot-patch: from target-5)
    int      dr = -1;
    void*    savedPointer = nullptr;      // VTable

    void* Detour() const { return chain.back(); }
};

struct Change {
    Record* rec;
    bool    enable;
    bool    applied = false;
};

struct State {
    std::mutex mutex;
    std::unordered_map<void*, std::unique_ptr<Record>> hooks;
    SlotAllocator slots;
    PVOID veh = nullptr;
    bool  batch = false;
    std::vector<std::pair<void*, bool>> pending;
};

// Intentionally leaked: static destructors in other modules may still run through hooks.
State& S() {
    static State* s = new State;
    return *s;
}

Record* Lookup(State& s, void* key) {
    auto it = s.hooks.find(key);
    return it == s.hooks.end() ? nullptr : it->second.get();
}

// Follow incremental-link and import thunks to the real body. Stops at an existing hook.
uint8_t* Resolve(State& s, uint8_t* p) {
    for (int depth = 0; depth < 8; ++depth) {
        if (Lookup(s, p) || !IsReadable(p, 7)) return p;
        if (p[0] == 0xE9) {
            p = p + 5 + *reinterpret_cast<int32_t*>(p + 1);
        } else if (p[0] == 0xEB) {
            p = p + 2 + static_cast<int8_t>(p[1]);
        } else if ((p[0] == 0xFF && p[1] == 0x25) || (p[0] == 0x48 && p[1] == 0xFF && p[2] == 0x25)) {
            const size_t len = p[0] == 0x48 ? 7 : 6;
            uint8_t* ptr = p + len + *reinterpret_cast<int32_t*>(p + len - 4);
            if (!IsReadable(ptr, sizeof(void*))) return p;
            uint8_t* next = *reinterpret_cast<uint8_t**>(ptr);
            if (!next || next == p) return p;
            p = next;
        } else {
            return p;
        }
    }
    return p;
}

Record* Find(State& s, void* target) {
    if (!target) return nullptr;
    if (Record* r = Lookup(s, target)) return r;
    return Lookup(s, Resolve(s, static_cast<uint8_t*>(target)));
}

bool UpdateRelay(Record& r) {
    auto* qword = reinterpret_cast<volatile LONG64*>(r.slot + 8);
    ScopedWritable w(r.slot, kRelaySize);
    if (!w) return false;
    InterlockedExchange64(qword, reinterpret_cast<LONG64>(r.Detour()));
    return true;
}

bool EnsureExceptionHandler(State& s) {
    if (!s.veh) s.veh = AddVectoredExceptionHandler(1, BreakpointHandler);
    return s.veh != nullptr;
}

bool HotPatchNeedsFreeze(const Record& r) {
    return (reinterpret_cast<uintptr_t>(r.target) & 1) != 0;   // unaligned 2-byte store is not atomic
}

bool NeedsFreeze(const Record& r) {
    return r.kind == Kind::Inline || (r.kind == Kind::HotPatch && HotPatchNeedsFreeze(r));
}

Status SetInline(Record& r, bool enable) {
    ScopedWritable w(r.target, r.srcSize);
    if (!w) return Status::AccessDenied;
    if (enable) {
        r.target[0] = 0xE9;
        *reinterpret_cast<int32_t*>(r.target + 1) =
            static_cast<int32_t>(Rel32(r.target, kJmpRel32Size, r.slot));
        std::memset(r.target + kJmpRel32Size, 0xCC, r.srcSize - kJmpRel32Size);
    } else {
        std::memcpy(r.target, r.saved, r.srcSize);
    }
    return Status::Ok;
}

Status SetHotPatch(Record& r, bool enable) {
    uint8_t* pad = r.target - kHotPatchPad;
    ScopedWritable w(pad, kHotPatchPad + 2);
    if (!w) return Status::AccessDenied;
    uint16_t word;
    if (enable) {
        pad[0] = 0xE9;
        *reinterpret_cast<int32_t*>(pad + 1) = static_cast<int32_t>(Rel32(pad, kJmpRel32Size, r.slot));
        word = 0xF9EB;                                   // jmp short -7 (into the pad)
    } else {
        std::memcpy(&word, r.saved + kHotPatchPad, 2);
    }
    if (HotPatchNeedsFreeze(r)) std::memcpy(r.target, &word, 2);
    else InterlockedExchange16(reinterpret_cast<volatile SHORT*>(r.target), static_cast<SHORT>(word));
    return Status::Ok;
}

Status SetBreakpoint(Record& r, bool enable) {
    if (enable) {
        int idx = -1;
        for (int i = 0; i < 4; ++i) if (!g_bp[i].reserved) { idx = i; break; }
        if (idx < 0) return Status::NoFreeDebugRegister;
        g_bp[idx].reserved = true;
        g_bp[idx].detour.store(r.Detour(), std::memory_order_release);
        g_bp[idx].target.store(r.target, std::memory_order_release);
        g_bpActive.fetch_add(1, std::memory_order_acq_rel);
        if (!ApplyDrToAllThreads(idx, r.target, true)) {
            ApplyDrToAllThreads(idx, nullptr, false);
            g_bp[idx].target.store(nullptr, std::memory_order_release);
            g_bpActive.fetch_sub(1, std::memory_order_acq_rel);
            g_bp[idx].reserved = false;
            return Status::ThreadEnumFailed;
        }
        r.dr = idx;
    } else {
        // Clear DR7 before the slot so an in-flight #DB still finds its detour.
        ApplyDrToAllThreads(r.dr, nullptr, false);
        g_bp[r.dr].target.store(nullptr, std::memory_order_release);
        g_bp[r.dr].detour.store(nullptr, std::memory_order_release);
        g_bpActive.fetch_sub(1, std::memory_order_acq_rel);
        g_bp[r.dr].reserved = false;
        r.dr = -1;
    }
    return Status::Ok;
}

Status SetVTable(Record& r, bool enable) {
    ScopedWritable w(r.target, sizeof(void*));
    if (!w) return Status::AccessDenied;
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(r.target),
                               enable ? r.Detour() : r.savedPointer);
    return Status::Ok;
}

Status SetOne(Record& r, bool enable) {
    switch (r.kind) {
    case Kind::Inline:     return SetInline(r, enable);
    case Kind::HotPatch:   return SetHotPatch(r, enable);
    case Kind::Breakpoint: return SetBreakpoint(r, enable);
    case Kind::VTable:     return SetVTable(r, enable);
    }
    return Status::InvalidParameter;
}

// Translate RIP between the original prologue and its relocated copy.
bool FixIp(const Record& r, bool enabled, DWORD64& rip) {
    if (r.kind != Kind::Inline) return false;
    const auto ip = reinterpret_cast<uint8_t*>(rip);
    if (enabled) {
        if (ip < r.target || ip >= r.target + r.srcSize) return false;
        const size_t off = static_cast<size_t>(ip - r.target);
        for (size_t i = 0; i < r.ipCount; ++i)
            if (r.ipSrc[i] == off) { rip = reinterpret_cast<DWORD64>(r.Trampoline() + r.ipDst[i]); return true; }
    } else {
        if (ip < r.Trampoline() || ip >= r.Trampoline() + r.codeSize) return false;
        const size_t off = static_cast<size_t>(ip - r.Trampoline());
        for (size_t i = 0; i < r.ipCount; ++i)
            if (r.ipDst[i] == off) { rip = reinterpret_cast<DWORD64>(r.target + r.ipSrc[i]); return true; }
    }
    return false;
}

// Code patches share a single freeze; breakpoint/vtable changes run after it.
Status Apply(std::vector<Change>& changes) {
    Status result = Status::Ok;
    const auto note = [&](Status st) { if (result == Status::Ok) result = st; };

    bool anyFreeze = false;
    for (auto& c : changes)
        if (c.rec->enabled != c.enable && NeedsFreeze(*c.rec)) anyFreeze = true;

    if (anyFreeze) {
        Freeze freeze;
        if (freeze.Begin() != Status::Ok) return Status::ThreadEnumFailed;
        for (auto& c : changes) {
            if (c.rec->enabled == c.enable || !NeedsFreeze(*c.rec)) continue;
            const Status st = SetOne(*c.rec, c.enable);
            if (st != Status::Ok) { note(st); continue; }
            c.rec->enabled = c.enable;
            c.applied = true;
        }
        freeze.ForEachRip([&](DWORD64& rip) {
            for (auto& c : changes)
                if (c.applied && FixIp(*c.rec, c.enable, rip)) return true;
            return false;
        });
    }

    // SetBreakpoint suspends threads itself, so it must run outside the freeze.
    for (auto& c : changes) {
        if (c.rec->enabled == c.enable || NeedsFreeze(*c.rec)) continue;
        const Status st = SetOne(*c.rec, c.enable);
        if (st != Status::Ok) { note(st); continue; }
        c.rec->enabled = c.enable;
        c.applied = true;
    }
    return result;
}

Status ApplyOne(Record& r, bool enable) {
    std::vector<Change> changes{ Change{ &r, enable } };
    return Apply(changes);
}

// Disables, then evicts threads from relays/trampolines before freeing slots.
Status RemoveRecords(State& s, const std::vector<Record*>& recs) {
    std::vector<Change> changes;
    changes.reserve(recs.size());
    for (Record* r : recs) if (r->enabled) changes.push_back(Change{ r, false });
    Status result = Apply(changes);

    std::vector<Record*> removable;
    removable.reserve(recs.size());
    for (Record* r : recs) if (!r->enabled) removable.push_back(r);

    bool anyCode = false;
    for (Record* r : removable) if (r->slot) anyCode = true;
    if (anyCode) {
        Freeze freeze;
        if (freeze.Begin() == Status::Ok) {
            freeze.ForEachRip([&](DWORD64& rip) {
                const auto ip = reinterpret_cast<uint8_t*>(rip);
                for (Record* r : removable) {
                    if (!r->slot) continue;
                    if (ip >= r->slot && ip < r->Trampoline()) {
                        rip = reinterpret_cast<DWORD64>(r->Detour());
                        return true;
                    }
                    if (ip >= r->Trampoline() && ip < r->slot + kSlotSize) {
                        const size_t off = static_cast<size_t>(ip - r->Trampoline());
                        for (size_t i = 0; i < r->ipCount; ++i)
                            if (r->ipDst[i] == off) {
                                rip = reinterpret_cast<DWORD64>(r->target + r->ipSrc[i]);
                                return true;
                            }
                    }
                    if (r->kind == Kind::HotPatch && ip >= r->target - kHotPatchPad && ip < r->target) {
                        rip = reinterpret_cast<DWORD64>(r->target);
                        return true;
                    }
                }
                return false;
            });
            for (Record* r : removable) {
                if (r->kind != Kind::HotPatch) continue;
                ScopedWritable w(r->target - kHotPatchPad, kHotPatchPad);
                if (w) std::memcpy(r->target - kHotPatchPad, r->saved, kHotPatchPad);
            }
        } else if (result == Status::Ok) {
            result = Status::ThreadEnumFailed;
        }
    }

    for (Record* r : removable) {
        if (r->slot) s.slots.Free(r->slot);
        s.hooks.erase(r->target);
    }
    return result;
}

std::vector<Record*> AllRecords(State& s) {
    std::vector<Record*> recs;
    recs.reserve(s.hooks.size());
    for (auto& [key, rec] : s.hooks) recs.push_back(rec.get());
    return recs;
}

} // namespace

const char* ToString(Status status) noexcept {
    switch (status) {
    case Status::Ok:                  return "Ok";
    case Status::AlreadyHooked:       return "AlreadyHooked";
    case Status::NotHooked:           return "NotHooked";
    case Status::AlreadyEnabled:      return "AlreadyEnabled";
    case Status::AlreadyDisabled:     return "AlreadyDisabled";
    case Status::InvalidParameter:    return "InvalidParameter";
    case Status::NotExecutable:       return "NotExecutable";
    case Status::Undecodable:         return "Undecodable";
    case Status::Unrelocatable:       return "Unrelocatable";
    case Status::Unreachable:         return "Unreachable";
    case Status::NotHotPatchable:     return "NotHotPatchable";
    case Status::AccessDenied:        return "AccessDenied";
    case Status::NoFreeDebugRegister: return "NoFreeDebugRegister";
    case Status::ThreadEnumFailed:    return "ThreadEnumFailed";
    case Status::BatchActive:         return "BatchActive";
    case Status::NoBatch:             return "NoBatch";
    case Status::ModuleNotLoaded:     return "ModuleNotLoaded";
    case Status::DeviceCreationFailed: return "DeviceCreationFailed";
    }
    return "Unknown";
}

const char* ToString(Kind kind) noexcept {
    switch (kind) {
    case Kind::Inline:     return "Inline";
    case Kind::Breakpoint: return "Breakpoint";
    case Kind::HotPatch:   return "HotPatch";
    case Kind::VTable:     return "VTable";
    }
    return "Unknown";
}

Status Create(void* target, void* detour, void** original, Kind kind) noexcept {
    if (!target || !detour || !original) return Status::InvalidParameter;
    try {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);

        if (kind == Kind::VTable) {
            if (Lookup(s, target)) return Status::AlreadyHooked;
            if (reinterpret_cast<uintptr_t>(target) % alignof(void*) != 0 ||
                !IsReadable(target, sizeof(void*)))
                return Status::InvalidParameter;
            auto rec = std::make_unique<Record>();
            rec->kind = kind;
            rec->target = static_cast<uint8_t*>(target);
            rec->chain.push_back(detour);
            rec->savedPointer = *static_cast<void**>(target);
            *original = rec->savedPointer;
            s.hooks.emplace(target, std::move(rec));
            return Status::Ok;
        }

        uint8_t* code = Resolve(s, static_cast<uint8_t*>(target));
        if (Record* existing = Lookup(s, code)) {
            if (existing->kind != Kind::Inline || kind != Kind::Inline) return Status::AlreadyHooked;
            void* previous = existing->Detour();
            existing->chain.push_back(detour);
            if (!UpdateRelay(*existing)) {
                existing->chain.pop_back();
                return Status::AccessDenied;
            }
            *original = previous;
            return Status::Ok;
        }

        if (!IsExecutable(code)) return Status::NotExecutable;

        auto rec = std::make_unique<Record>();
        rec->kind = kind;
        rec->target = code;
        rec->chain.push_back(detour);

        Status st = Status::Ok;
        switch (kind) {
        case Kind::Inline:
            st = BuildTrampoline(s.slots, code, detour, kJmpRel32Size, false, *rec);
            if (st == Status::Ok) std::memcpy(rec->saved, code, rec->srcSize);
            break;
        case Kind::Breakpoint:
            if (!EnsureExceptionHandler(s)) return Status::ThreadEnumFailed;
            st = BuildTrampoline(s.slots, code, detour, 1, true, *rec);
            break;
        case Kind::HotPatch: {
            if (!IsReadable(code - kHotPatchPad, kHotPatchPad)) return Status::NotHotPatchable;
            for (size_t i = 1; i <= kHotPatchPad; ++i)
                if (code[-static_cast<ptrdiff_t>(i)] != 0xCC && code[-static_cast<ptrdiff_t>(i)] != 0x90)
                    return Status::NotHotPatchable;
            rvlde_insn first{};
            if (!SafeDecode(code, &first)) return Status::Undecodable;
            if (first.length < 2) return Status::NotHotPatchable;
            st = BuildTrampoline(s.slots, code, detour, 2, false, *rec);
            if (st == Status::Ok) std::memcpy(rec->saved, code - kHotPatchPad, kHotPatchPad + 2);
            break;
        }
        default:
            return Status::InvalidParameter;
        }
        if (st != Status::Ok) return st;

        *original = rec->Trampoline();
        s.hooks.emplace(code, std::move(rec));
        return Status::Ok;
    } catch (...) {
        return Status::InvalidParameter;
    }
}

namespace {
Status SetState(void* target, bool enable) noexcept {
    try {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        Record* r = Find(s, target);
        if (!r) return Status::NotHooked;
        if (s.batch) {
            s.pending.emplace_back(r->target, enable);
            return Status::Ok;
        }
        if (r->enabled == enable) return enable ? Status::AlreadyEnabled : Status::AlreadyDisabled;
        return ApplyOne(*r, enable);
    } catch (...) {
        return Status::InvalidParameter;
    }
}

Status SetAll(bool enable) noexcept {
    try {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.batch) return Status::BatchActive;
        std::vector<Change> changes;
        for (Record* r : AllRecords(s))
            if (r->enabled != enable) changes.push_back(Change{ r, enable });
        return Apply(changes);
    } catch (...) {
        return Status::InvalidParameter;
    }
}
} // namespace

Status Enable(void* target) noexcept  { return SetState(target, true); }
Status Disable(void* target) noexcept { return SetState(target, false); }
Status EnableAll() noexcept  { return SetAll(true); }
Status DisableAll() noexcept { return SetAll(false); }

Status Remove(void* target) noexcept {
    try {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.batch) return Status::BatchActive;
        Record* r = Find(s, target);
        if (!r) return Status::NotHooked;
        return RemoveRecords(s, { r });
    } catch (...) {
        return Status::InvalidParameter;
    }
}

Status RemoveAll() noexcept {
    try {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.batch) return Status::BatchActive;
        return RemoveRecords(s, AllRecords(s));
    } catch (...) {
        return Status::InvalidParameter;
    }
}

Status BeginBatch() noexcept {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.batch) return Status::BatchActive;
    s.batch = true;
    s.pending.clear();
    return Status::Ok;
}

Status CommitBatch() noexcept {
    try {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.batch) return Status::NoBatch;
        s.batch = false;
        std::vector<Change> changes;
        for (auto& [target, enable] : s.pending) {
            Record* r = Lookup(s, target);
            if (!r) continue;
            auto it = std::find_if(changes.begin(), changes.end(),
                                   [&](const Change& c) { return c.rec == r; });
            if (it != changes.end()) it->enable = enable;   // last request wins
            else changes.push_back(Change{ r, enable });
        }
        s.pending.clear();
        return Apply(changes);
    } catch (...) {
        return Status::InvalidParameter;
    }
}

void AbortBatch() noexcept {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.batch = false;
    s.pending.clear();
}

bool IsHooked(void* target) noexcept {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return Find(s, target) != nullptr;
}

bool IsEnabled(void* target) noexcept {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    const Record* r = Find(s, target);
    return r && r->enabled;
}

Kind KindOf(void* target) noexcept {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    const Record* r = Find(s, target);
    return r ? r->kind : Kind::Inline;
}

void* OriginalOf(void* target) noexcept {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    const Record* r = Find(s, target);
    if (!r) return nullptr;
    if (r->kind == Kind::VTable) return r->savedPointer;
    return r->chain.size() > 1 ? r->chain[r->chain.size() - 2] : r->Trampoline();
}

size_t Count() noexcept {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.hooks.size();
}

void Shutdown() noexcept {
    try {
        State& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.batch = false;
        s.pending.clear();
        (void)RemoveRecords(s, AllRecords(s));
        if (s.hooks.empty()) {
            if (s.veh) {
                RemoveVectoredExceptionHandler(s.veh);
                s.veh = nullptr;
            }
            s.slots.ReleaseAll();
        }
    } catch (...) {
    }
}

} // namespace revenant
