#include "reloc.hpp"

#include <cstring>

namespace revenant::detail {

uint32_t SafeDecode(const uint8_t* p, rvlde_insn* ins) {
    __try {
        return rvlde_decode(p, ins);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

namespace {

size_t OpcodeOffset(const uint8_t* p, size_t len) {
    size_t i = 0;
    while (i < len) {
        const uint8_t b = p[i];
        if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x36 || b == 0x3E ||
            b == 0x26 || b == 0x64 || b == 0x65 || b == 0x66 || b == 0x67) {
            ++i;
            continue;
        }
        break;
    }
    if (i < len && (p[i] & 0xF0) == 0x40) ++i;
    return i;
}

// ret, jmp: no fallthrough
bool IsTerminator(const uint8_t* p, const rvlde_insn& ins) {
    if (ins.flags & (RVF_VEX | RVF_EVEX)) return false;
    if (ins.flags & RVF_JMP) return true;
    const size_t op = OpcodeOffset(p, ins.length);
    if (op >= ins.length) return false;
    const uint8_t b = p[op];
    if (b == 0xC3 || b == 0xC2 || b == 0xCB || b == 0xCA) return true;
    if (b == 0xFF && op + 1 < ins.length) {
        const uint8_t reg = (p[op + 1] >> 3) & 7;
        return reg == 4 || reg == 5;
    }
    return false;
}

// Emits `ins` from `src` at buf+out; buf will run at `base`. Branches are
// re-encoded in their rel32 or absolute form, prefixes dropped.
Status Relocate(const uint8_t* src, const rvlde_insn& ins, uint8_t* buf, size_t& out,
                size_t cap, uint8_t* base) {
    const size_t len = ins.length;
    uint8_t* const at = base + out;
    uint8_t* const dst = buf + out;
    const auto room = [&](size_t n) { return out + n <= cap; };

    if (ins.flags & RVF_LOOP) return Status::Unrelocatable;

    if (ins.flags & RVF_RIPREL) {
        if (!room(len)) return Status::Unrelocatable;
        std::memcpy(dst, src, len);
        const int32_t disp = *reinterpret_cast<const int32_t*>(src + ins.dispOffset);
        const uint8_t* abs = src + len + disp;
        const int64_t newDisp = Rel32(at, len, abs);
        if (!FitsRel32(newDisp)) return Status::Unrelocatable;
        *reinterpret_cast<int32_t*>(dst + ins.dispOffset) = static_cast<int32_t>(newDisp);
        out += len;
        return Status::Ok;
    }

    const auto* to = reinterpret_cast<const uint8_t*>(ins.relTarget);

    if (ins.flags & RVF_CALL) {
        const int64_t rel = Rel32(at, 5, to);
        if (FitsRel32(rel)) {
            if (!room(5)) return Status::Unrelocatable;
            dst[0] = 0xE8;
            *reinterpret_cast<int32_t*>(dst + 1) = static_cast<int32_t>(rel);
            out += 5;
        } else {
            // call [rip+2]; jmp +8; dq target
            if (!room(16)) return Status::Unrelocatable;
            const uint8_t seq[8] = { 0xFF, 0x15, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x08 };
            std::memcpy(dst, seq, sizeof(seq));
            *reinterpret_cast<uint64_t*>(dst + 8) = reinterpret_cast<uint64_t>(to);
            out += 16;
        }
        return Status::Ok;
    }

    if (ins.flags & RVF_JCC) {
        const size_t op = OpcodeOffset(src, len);
        const uint8_t cc = (ins.flags & RVF_REL8) ? (src[op] & 0x0F) : (src[op + 1] & 0x0F);
        const int64_t rel = Rel32(at, 6, to);
        if (FitsRel32(rel)) {
            if (!room(6)) return Status::Unrelocatable;
            dst[0] = 0x0F;
            dst[1] = static_cast<uint8_t>(0x80 | cc);
            *reinterpret_cast<int32_t*>(dst + 2) = static_cast<int32_t>(rel);
            out += 6;
        } else {
            // j!cc over an absolute jmp
            if (!room(2 + kAbsJmpSize)) return Status::Unrelocatable;
            dst[0] = static_cast<uint8_t>(0x70 | (cc ^ 1));
            dst[1] = static_cast<uint8_t>(kAbsJmpSize);
            EmitAbsJmp(dst + 2, reinterpret_cast<uintptr_t>(to));
            out += 2 + kAbsJmpSize;
        }
        return Status::Ok;
    }

    if (ins.flags & RVF_JMP) {
        const int64_t rel = Rel32(at, 5, to);
        if (FitsRel32(rel)) {
            if (!room(5)) return Status::Unrelocatable;
            dst[0] = 0xE9;
            *reinterpret_cast<int32_t*>(dst + 1) = static_cast<int32_t>(rel);
            out += 5;
        } else {
            if (!room(kAbsJmpSize)) return Status::Unrelocatable;
            EmitAbsJmp(dst, reinterpret_cast<uintptr_t>(to));
            out += kAbsJmpSize;
        }
        return Status::Ok;
    }

    if (!room(len)) return Status::Unrelocatable;
    std::memcpy(dst, src, len);
    out += len;
    return Status::Ok;
}

} // namespace

Status BuildTrampoline(SlotAllocator& slots, uint8_t* target, void* detour, size_t minBytes,
                       bool firstInsnOnly, CodeSlot& r) {
    uint8_t* slot = slots.Allocate(target);
    if (!slot) return Status::Unreachable;

    uint8_t buf[kSlotSize];
    std::memset(buf, 0xCC, sizeof(buf));
    const uint8_t relay[6] = { 0xFF, 0x25, 0x02, 0x00, 0x00, 0x00 };
    std::memcpy(buf, relay, sizeof(relay));
    *reinterpret_cast<uint64_t*>(buf + 8) = reinterpret_cast<uint64_t>(detour);

    uint8_t* const code = buf + kRelaySize;
    uint8_t* const codeBase = slot + kRelaySize;
    const size_t cap = kSlotSize - kRelaySize - kAbsJmpSize;
    size_t in = 0, out = 0;
    bool terminated = false;
    uintptr_t branchTargets[kMaxInsns];
    size_t branchCount = 0;
    Status st = Status::Ok;

    while (in < minBytes) {
        const uint8_t* src = target + in;
        if (terminated) {
            // Past a ret/jmp, only padding may be overwritten.
            if (!IsReadable(src, 1) || (*src != 0xCC && *src != 0x90 && *src != 0x00)) {
                st = Status::Unrelocatable;
                break;
            }
            ++in;
            continue;
        }
        rvlde_insn ins{};
        if (!SafeDecode(src, &ins) || ins.length == 0) { st = Status::Undecodable; break; }
        if (r.ipCount == kMaxInsns) { st = Status::Unrelocatable; break; }
        r.ipSrc[r.ipCount] = static_cast<uint8_t>(in);
        r.ipDst[r.ipCount] = static_cast<uint8_t>(out);
        ++r.ipCount;
        if (ins.flags & (RVF_JCC | RVF_JMP | RVF_CALL)) branchTargets[branchCount++] = ins.relTarget;
        st = Relocate(src, ins, code, out, cap, codeBase);
        if (st != Status::Ok) break;
        in += ins.length;
        if (IsTerminator(src, ins)) terminated = true;
    }

    if (st == Status::Ok) {
        const uintptr_t lo = reinterpret_cast<uintptr_t>(target);
        const uintptr_t hi = lo + (firstInsnOnly ? 1 : in);
        for (size_t i = 0; i < branchCount; ++i)
            if (branchTargets[i] >= lo && branchTargets[i] < hi) st = Status::Unrelocatable;
    }

    if (st == Status::Ok && !terminated) {
        uint8_t* back = target + in;
        const int64_t rel = Rel32(codeBase + out, 5, back);
        if (FitsRel32(rel)) {
            code[out] = 0xE9;
            *reinterpret_cast<int32_t*>(code + out + 1) = static_cast<int32_t>(rel);
            out += kJmpRel32Size;
        } else {
            EmitAbsJmp(code + out, reinterpret_cast<uintptr_t>(back));
            out += kAbsJmpSize;
        }
    }

    if (st == Status::Ok) {
        ScopedWritable w(slot, kSlotSize);
        if (!w) st = Status::AccessDenied;
        else std::memcpy(slot, buf, kSlotSize);
    }

    if (st != Status::Ok) {
        slots.Free(slot);
        r.ipCount = 0;
        return st;
    }
    r.slot = slot;
    r.codeSize = out;
    r.srcSize = in;
    return Status::Ok;
}

} // namespace revenant::detail
