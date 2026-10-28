// Handles legacy/REX/VEX/EVEX/XOP prefixes, 1-3 byte opcodes, ModRM/SIB/disp/imm.

#include "rvlde.h"
#include <cstring>

namespace {

enum : uint8_t {
    T_NONE = 0,
    T_MODRM = 1,
    T_IMM8 = 2,
    T_IMM16 = 4,
    T_IMM32 = 8,
    T_IMM64 = 16,
    T_REL8 = 32,
    T_REL32 = 64,
    T_ESC_0F = 128,   // handled in code
};

uint8_t tbl_primary[256];
uint8_t tbl_0f[256];
bool tables_ready = false;

void InitTables() {
    memset(tbl_primary, 0, sizeof(tbl_primary));
    memset(tbl_0f, 0, sizeof(tbl_0f));

    // 0x00-0x3F arithmetic groups: /r /r /r /r ib iz
    for (int base = 0x00; base <= 0x38; base += 8) {
        tbl_primary[base + 0] = T_MODRM;
        tbl_primary[base + 1] = T_MODRM;
        tbl_primary[base + 2] = T_MODRM;
        tbl_primary[base + 3] = T_MODRM;
        tbl_primary[base + 4] = T_IMM8;
        tbl_primary[base + 5] = T_IMM32;
    }
    tbl_primary[0x0F] = T_ESC_0F;
    tbl_primary[0x63] = T_MODRM;                       // movsxd
    tbl_primary[0x68] = T_IMM32;                       // push iv
    tbl_primary[0x69] = T_MODRM | T_IMM32;             // imul r,r/m,iz
    tbl_primary[0x6A] = T_IMM8;                        // push ib
    tbl_primary[0x6B] = T_MODRM | T_IMM8;
    for (int i = 0x70; i <= 0x7F; i++) tbl_primary[i] = T_REL8;
    tbl_primary[0x80] = T_MODRM | T_IMM8;
    tbl_primary[0x81] = T_MODRM | T_IMM32;
    tbl_primary[0x82] = T_MODRM | T_IMM8;
    tbl_primary[0x83] = T_MODRM | T_IMM8;
    for (int i = 0x84; i <= 0x8B; i++) tbl_primary[i] = T_MODRM;
    tbl_primary[0x8C] = T_MODRM;
    tbl_primary[0x8D] = T_MODRM;
    tbl_primary[0x8E] = T_MODRM;
    tbl_primary[0x8F] = T_MODRM;
    tbl_primary[0xA8] = T_IMM8;
    tbl_primary[0xA9] = T_IMM32;
    for (int i = 0xB0; i <= 0xB7; i++) tbl_primary[i] = T_IMM8;
    for (int i = 0xB8; i <= 0xBF; i++) tbl_primary[i] = T_IMM32;
    tbl_primary[0xC0] = T_MODRM | T_IMM8;
    tbl_primary[0xC1] = T_MODRM | T_IMM8;
    tbl_primary[0xC2] = T_IMM16;
    tbl_primary[0xC6] = T_MODRM | T_IMM8;
    tbl_primary[0xC7] = T_MODRM | T_IMM32;
    tbl_primary[0xC8] = T_IMM16 | T_IMM8;
    tbl_primary[0xCA] = T_IMM16;
    tbl_primary[0xCD] = T_IMM8;
    for (int i = 0xD0; i <= 0xD3; i++) tbl_primary[i] = T_MODRM;
    for (int i = 0xD8; i <= 0xDF; i++) tbl_primary[i] = T_MODRM;
    tbl_primary[0xE4] = T_IMM8;
    tbl_primary[0xE5] = T_IMM8;
    tbl_primary[0xE6] = T_IMM8;
    tbl_primary[0xE7] = T_IMM8;
    tbl_primary[0xFE] = T_MODRM;
    tbl_primary[0xFF] = T_MODRM;

    // 0F map
    tbl_0f[0x00] = T_MODRM;
    tbl_0f[0x01] = T_MODRM;
    tbl_0f[0x02] = T_MODRM;
    tbl_0f[0x03] = T_MODRM;
    tbl_0f[0x0D] = T_MODRM;
    tbl_0f[0x0F] = T_MODRM | T_IMM8;
    for (int i = 0x10; i <= 0x1F; i++) tbl_0f[i] = T_MODRM;
    for (int i = 0x20; i <= 0x23; i++) tbl_0f[i] = T_MODRM;
    for (int i = 0x28; i <= 0x2F; i++) tbl_0f[i] = T_MODRM;
    for (int i = 0x40; i <= 0x4F; i++) tbl_0f[i] = T_MODRM;   // cmovcc
    for (int i = 0x50; i <= 0x6F; i++) tbl_0f[i] = T_MODRM;
    tbl_0f[0x70] = T_MODRM | T_IMM8;
    tbl_0f[0x71] = T_MODRM | T_IMM8;
    tbl_0f[0x72] = T_MODRM | T_IMM8;
    tbl_0f[0x73] = T_MODRM | T_IMM8;
    for (int i = 0x74; i <= 0x79; i++) tbl_0f[i] = T_MODRM;
    tbl_0f[0x77] = T_NONE;                             // emms
    for (int i = 0x7C; i <= 0x7F; i++) tbl_0f[i] = T_MODRM;
    for (int i = 0x80; i <= 0x8F; i++) tbl_0f[i] = T_REL32;   // jcc
    for (int i = 0x90; i <= 0x9F; i++) tbl_0f[i] = T_MODRM;   // setcc
    tbl_0f[0xA3] = T_MODRM;                            // bt
    tbl_0f[0xA4] = T_MODRM | T_IMM8;                   // shld ib
    tbl_0f[0xA5] = T_MODRM;
    tbl_0f[0xA6] = T_MODRM;                            // via padlock (fixed C0-E8 modrm)
    tbl_0f[0xA7] = T_MODRM;
    tbl_0f[0xAB] = T_MODRM;
    tbl_0f[0xAC] = T_MODRM | T_IMM8;
    tbl_0f[0xAD] = T_MODRM;
    tbl_0f[0xAE] = T_MODRM;
    tbl_0f[0xAF] = T_MODRM;
    for (int i = 0xB0; i <= 0xB8; i++) tbl_0f[i] = T_MODRM;
    tbl_0f[0xBA] = T_MODRM | T_IMM8;
    for (int i = 0xBB; i <= 0xBF; i++) tbl_0f[i] = T_MODRM;
    tbl_0f[0xC0] = T_MODRM;
    tbl_0f[0xC1] = T_MODRM;
    tbl_0f[0xC2] = T_MODRM | T_IMM8;
    tbl_0f[0xC3] = T_MODRM;
    tbl_0f[0xC4] = T_MODRM | T_IMM8;
    tbl_0f[0xC5] = T_MODRM | T_IMM8;
    tbl_0f[0xC6] = T_MODRM | T_IMM8;
    tbl_0f[0xC7] = T_MODRM;
    for (int i = 0xD0; i <= 0xFE; i++) tbl_0f[i] = T_MODRM;
}

// decode modrm + sib + disp starting at p[pos]; returns false past 15 bytes.
// sets RVF_MODRM always, RVF_RIPREL + dispOffset for rip-relative.
bool DecodeModrm(const uint8_t* p, uint32_t& pos, uint32_t& flags, rvlde_insn* out, bool asz) {
    (void)asz;
    flags |= RVF_MODRM;
    uint8_t modrm = p[pos];
    uint32_t mod = modrm >> 6, rm = modrm & 7;
    pos++;
    if (mod == 3) return true;
    if (rm == 4) {
        uint8_t sib = p[pos++];
        if ((sib & 7) == 5 && mod == 0) pos += 4;
    } else if (rm == 5 && mod == 0) {
        flags |= RVF_RIPREL;
        if (out) out->dispOffset = pos;
        pos += 4;
    }
    if (mod == 1) pos += 1;
    else if (mod == 2) pos += 4;
    return pos <= 15;
}

} // namespace

uint32_t rvlde_decode(const uint8_t* p, rvlde_insn* out) {
    if (!tables_ready) { InitTables(); tables_ready = true; }
    if (out) { out->length = 0; out->flags = 0; out->relTarget = 0; out->dispOffset = 0; }

    uint32_t flags = 0;
    bool rexW = false, osz = false, asz = false, f2 = false;
    uint32_t pos = 0;
    uint32_t iflags = 0;   // post-modrm immediate flags

    while (pos < 15) {
        uint8_t b = p[pos];
        if (b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
            b == 0x64 || b == 0x65) {
            if (b == 0xF2) f2 = true;
            rexW = false; flags &= ~RVF_REX; pos++;     // rex only counts as the last prefix
        } else if (b == 0x66) {
            osz = true; flags |= RVF_OSZ; rexW = false; flags &= ~RVF_REX; pos++;
        } else if (b == 0x67) {
            asz = true; rexW = false; flags &= ~RVF_REX; pos++;
        } else if (b >= 0x40 && b <= 0x4F) {
            rexW = (b & 0x08) != 0; flags |= RVF_REX; pos++;
        } else break;
    }

    // VEX / EVEX (C4/C5/62 are always VEX/EVEX in 64-bit mode; seg/67 prefixes may precede)
    if (p[pos] == 0xC4 || p[pos] == 0xC5) {
        flags |= RVF_VEX;
        uint32_t map = 1;                               // 1 = 0F, 2 = 0F38, 3 = 0F3A
        if (p[pos] == 0xC4) {
            map = p[pos + 1] & 0x1F;                    // map is in the first payload byte
            pos += 3;                                   // C4 + 2 payload bytes
        } else {
            pos += 2;                                   // C5 + 1 payload byte
        }
        uint8_t opcode = p[pos++];
        if (map == 1 && opcode == 0x77) iflags = 0;     // vzeroupper: no operands
        else if (map == 1)        iflags = (tbl_0f[opcode] & T_IMM8) | T_MODRM;   // every vex op but 77 has modrm
        else if (map == 2)        iflags = T_MODRM;
        else if (map == 3)        iflags = T_MODRM | T_IMM8;
        else return 0;                                  // reserved map
        osz = false;                                    // vex.pp, not 66, selects operand size
    } else if (p[pos] == 0x8F && (p[pos + 1] & 0x1F) >= 8) {
        flags |= RVF_VEX;                               // amd xop (8F /0 pop has map bits < 8)
        uint32_t map = p[pos + 1] & 0x1F;
        pos += 4;                                       // 8F + 2 payload bytes + opcode
        if (map == 8)        iflags = T_MODRM | T_IMM8;
        else if (map == 9)   iflags = T_MODRM;
        else if (map == 10)  iflags = T_MODRM | T_IMM32;
        else return 0;
        osz = false;
    } else if (p[pos] == 0x62) {
        flags |= RVF_EVEX;
        uint32_t map = p[pos + 1] & 3;                  // EVEX.P0[1:0]
        if (map == 0) return 0;
        pos += 4;                                       // 62 + 3 payload bytes
        uint8_t opcode = p[pos++];
        if (map == 1)        iflags = (tbl_0f[opcode] & T_IMM8) | T_MODRM;   // every evex op has modrm
        else if (map == 2)   iflags = T_MODRM;
        else if (map == 3)   iflags = T_MODRM | T_IMM8;
        else return 0;
        osz = false;
    } else {
        uint8_t op = p[pos++];
        if (op == 0x0F) {
            uint8_t op2 = p[pos++];
            if (op2 == 0x38)      iflags = T_MODRM;
            else if (op2 == 0x3A) iflags = T_MODRM | T_IMM8;
            else if (op2 == 0x78 && (osz || f2) && (p[pos] >> 6) == 3) iflags = T_MODRM | T_IMM16;   // extrq/insertq ib,ib
            else                  iflags = tbl_0f[op2];
            if (op2 >= 0x80 && op2 <= 0x8F) flags |= RVF_JCC;
            if (op2 == 0x38 || op2 == 0x3A) { uint8_t op3 = p[pos++]; (void)op3; }
            if (op2 >= 0x20 && op2 <= 0x23) { iflags = 0; pos++; flags |= RVF_MODRM; }   // mov cr/dr: mod ignored, always reg form
        } else {
            uint32_t t = tbl_primary[op];
            if (t & T_ESC_0F) {
                uint8_t op2 = p[pos++];
                iflags = tbl_0f[op2];
                if (op2 >= 0x80 && op2 <= 0x8F) flags |= RVF_JCC;
            } else if (op >= 0xB8 && op <= 0xBF) {
                iflags = rexW ? T_IMM64 : (osz ? T_IMM16 : T_IMM32);
            } else if (op >= 0xA0 && op <= 0xA3) {
                iflags = asz ? T_IMM32 : T_IMM64; osz = false;   // moffs size follows 67 only
            } else if (op == 0xE8) {
                iflags = T_REL32; flags |= RVF_CALL;
            } else if (op == 0xE9) {
                iflags = T_REL32; flags |= RVF_JMP;
            } else if (op == 0xEB) {
                iflags = T_REL8; flags |= RVF_JMP;
            } else if (op >= 0x70 && op <= 0x7F) {
                iflags = T_REL8; flags |= RVF_JCC;
            } else if (op >= 0xE0 && op <= 0xE3) {
                iflags = T_REL8; flags |= RVF_LOOP;
            } else if (op == 0xF6) {
                iflags = ((p[pos] >> 3) & 7) < 2 ? T_MODRM | T_IMM8 : T_MODRM;    // grp3: only test /0-/1 has imm
            } else if (op == 0xF7) {
                iflags = ((p[pos] >> 3) & 7) < 2 ? T_MODRM | T_IMM32 : T_MODRM;   // (16 w66)
            } else {
                iflags = t;
            }
        }
    }

    if (iflags & T_MODRM) {
        flags |= RVF_MODRM;
        if (!DecodeModrm(p, pos, flags, out, asz)) return 0;
    }
    if (iflags & T_IMM8)  { pos += 1; flags |= RVF_IMM8; }
    if (iflags & T_IMM16) { pos += 2; flags |= RVF_IMM16; }
    if (iflags & T_IMM32) { pos += (osz && !rexW) ? 2 : 4; flags |= RVF_IMM32; }   // rex.w overrides 66
    if (iflags & T_IMM64) { pos += 8; flags |= RVF_IMM64; }
    if (iflags & T_REL8)  { pos += 1; flags |= RVF_REL8; }
    if (iflags & T_REL32) { pos += 4; flags |= RVF_REL32; }

    if (pos > 15) return 0;

    if (iflags & T_REL8) {
        flags |= RVF_REL8;
        if (out) out->relTarget = (uint64_t)p + pos + (int8_t)p[pos - 1];
    } else if (iflags & T_REL32) {
        flags |= RVF_REL32;
        if (out) out->relTarget = (uint64_t)p + pos + (int32_t)*(const uint32_t*)(p + pos - 4);
    }

    if (out) { out->length = pos; out->flags = flags; }
    return pos;
}
