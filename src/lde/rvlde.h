// rvlde - x64 instruction length decoder.
// Reports length plus relocation info: branch kind/target and rip-relative disp32 offset.

#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RVF_MODRM   = 1u << 0,
    RVF_IMM8    = 1u << 1,
    RVF_IMM16   = 1u << 2,
    RVF_IMM32   = 1u << 3,
    RVF_IMM64   = 1u << 4,
    RVF_REL8    = 1u << 5,
    RVF_REL32   = 1u << 6,
    RVF_RIPREL  = 1u << 7,   // modrm is rip-relative (disp32 patches at dispOffset)
    RVF_JCC     = 1u << 8,
    RVF_JMP     = 1u << 9,
    RVF_CALL    = 1u << 10,
    RVF_LOOP    = 1u << 11,  // rcx-dependent, cannot relocate
    RVF_VEX     = 1u << 12,
    RVF_EVEX    = 1u << 13,
    RVF_REX     = 1u << 14,
    RVF_OSZ     = 1u << 15,  // 66 prefix present
};

typedef struct {
    uint32_t length;       // total instruction length (0 = decode failure)
    uint32_t flags;
    uint64_t relTarget;    // for rel8/rel32: absolute target (base + len + disp)
    uint32_t dispOffset;   // for RVF_RIPREL: byte offset of the disp32 inside the insn
} rvlde_insn;

// returns instruction length in bytes, 0 on decode failure
uint32_t rvlde_decode(const uint8_t* p, rvlde_insn* out);

#ifdef __cplusplus
}
#endif
