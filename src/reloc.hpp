#pragma once

#include "alloc.hpp"
#include "lde/rvlde.h"

namespace revenant::detail {

// Slot layout (128 bytes):
//   +0   FF 25 02 00 00 00 CC CC   jmp [rip+2]
//   +8   dq detour                 swapped atomically when chaining
//   +16  relocated prologue        "original"
//        jmp target+srcSize
struct CodeSlot {
    uint8_t* slot = nullptr;
    size_t   codeSize = 0;            // bytes emitted at Trampoline(), excluding the jump back
    size_t   srcSize = 0;             // prologue bytes covered
    size_t   ipCount = 0;
    uint8_t  ipSrc[kMaxInsns] = {};   // instruction boundaries in the target...
    uint8_t  ipDst[kMaxInsns] = {};   // ...and where they ended up in the trampoline

    uint8_t* Trampoline() const { return slot + kRelaySize; }
};

// Copies at least minBytes of whole instructions from target into a new slot.
// Branches that land inside the overwritten range (or, with firstInsnOnly,
// back onto the first instruction) make the prologue unrelocatable.
Status BuildTrampoline(SlotAllocator& slots, uint8_t* target, void* detour, size_t minBytes,
                       bool firstInsnOnly, CodeSlot& out);

// rvlde_decode under SEH: running off the end of a mapping returns 0.
uint32_t SafeDecode(const uint8_t* p, rvlde_insn* ins);

} // namespace revenant::detail
