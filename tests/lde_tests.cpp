#include "lde/rvlde.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum Branch { None, Jcc, Jmp, Call, Loop };

struct Case {
    const char* hex;
    uint32_t    len;
    int         disp;   // rip-relative disp32 offset, -1 if none
    Branch      br;
};

// Expected values checked against Capstone 5.0.
const Case kCases[] = {
    // common prologues
    { "F3 0F 1E FA",                      4, -1, None },   // endbr64
    { "40 53",                            2, -1, None },
    { "48 89 5C 24 08",                   5, -1, None },
    { "48 81 EC 00 01 00 00",             7, -1, None },
    { "C8 20 00 00",                      4, -1, None },   // enter
    { "0F 1F 84 00 00 00 00 00",          8, -1, None },
    { "65 48 8B 04 25 30 00 00 00",       9, -1, None },   // mov rax, gs:[30h]

    // rip-relative
    { "48 8B 05 00 00 00 00",             7,  3, None },
    { "4C 8D 05 F0 FF FF FF",             7,  3, None },
    { "FF 25 00 00 00 00",                6,  2, None },
    { "FF 15 02 00 00 00",                6,  2, None },
    { "48 FF 25 00 00 00 00",             7,  3, None },
    { "67 8B 05 10 00 00 00",             7,  3, None },
    { "65 48 8B 05 10 00 00 00",          8,  4, None },

    // rip-relative + immediate
    { "C7 05 10 00 00 00 78 56 34 12",   10,  2, None },
    { "80 3D 10 00 00 00 00",             7,  2, None },
    { "66 C7 05 10 00 00 00 34 12",       9,  3, None },
    { "F6 05 10 00 00 00 01",             7,  2, None },
    { "F7 05 10 00 00 00 78 56 34 12",   10,  2, None },
    { "48 81 3D 10 00 00 00 00 10 00 00",11,  3, None },
    { "48 83 3D 10 00 00 00 00",          8,  3, None },
    { "66 81 3D 10 00 00 00 34 12",       9,  3, None },
    { "48 69 05 10 00 00 00 78 56 34 12",11,  3, None },
    { "66 0F 70 05 10 00 00 00 1B",       9,  4, None },
    { "66 0F 3A 63 05 10 00 00 00 10",   10,  5, None },   // pcmpistri

    // immediate sizing
    { "6A 05",                            2, -1, None },
    { "F7 D8",                            2, -1, None },   // neg, no imm
    { "F6 C1 01",                         3, -1, None },
    { "66 48 C7 C0 78 56 34 12",          8, -1, None },   // rex.w beats 66
    { "48 66 B8 34 12",                   5, -1, None },   // rex not last, ignored
    { "48 A1 88 77 66 55 44 33 22 11",   10, -1, None },
    { "66 A1 88 77 66 55 44 33 22 11",   10, -1, None },
    { "67 A1 44 33 22 11",                6, -1, None },

    // sse / vex / evex
    { "F3 0F 6F 05 10 00 00 00",          8,  4, None },
    { "0F 77",                            2, -1, None },
    { "C5 F8 77",                         3, -1, None },   // vzeroupper
    { "C5 FC 28 05 10 00 00 00",          8,  4, None },
    { "65 C5 FC 28 05 10 00 00 00",       9,  5, None },
    { "C4 E3 7D 18 05 10 00 00 00 01",   10,  5, None },
    { "62 F1 7C 48 28 05 10 00 00 00",   10,  6, None },
    { "62 F3 75 48 25 05 10 00 00 00 FF",11,  6, None },
    { "62 F1 FE 48 6F 84 24 00 01 00 00",11, -1, None },

    // branches
    { "EB FE",                            2, -1, Jmp  },
    { "E9 F0 FF FF FF",                   5, -1, Jmp  },
    { "F2 E9 00 00 00 00",                6, -1, Jmp  },   // bnd jmp
    { "E8 00 00 00 00",                   5, -1, Call },
    { "74 05",                            2, -1, Jcc  },
    { "0F 84 00 01 00 00",                6, -1, Jcc  },
    { "E2 FE",                            2, -1, Loop },
    { "E3 05",                            2, -1, Loop },   // jrcxz
};

size_t ParseHex(const char* s, uint8_t* out) {
    size_t n = 0;
    while (*s) {
        char* end;
        out[n++] = static_cast<uint8_t>(std::strtoul(s, &end, 16));
        s = end;
        while (*s == ' ') ++s;
    }
    return n;
}

Branch BranchOf(uint32_t flags) {
    if (flags & RVF_LOOP) return Loop;
    if (flags & RVF_CALL) return Call;
    if (flags & RVF_JMP)  return Jmp;
    if (flags & RVF_JCC)  return Jcc;
    return None;
}

} // namespace

int main() {
    int failures = 0;
    for (const Case& c : kCases) {
        uint8_t buf[32];
        std::memset(buf, 0xCC, sizeof(buf));
        ParseHex(c.hex, buf);

        rvlde_insn insn{};
        const uint32_t len = rvlde_decode(buf, &insn);
        const int disp = (insn.flags & RVF_RIPREL) ? static_cast<int>(insn.dispOffset) : -1;
        const Branch br = BranchOf(insn.flags);

        if (len != c.len || disp != c.disp || br != c.br) {
            ++failures;
            std::printf("FAIL %-36s len %u/%u disp %d/%d br %d/%d\n", c.hex, len, c.len, disp, c.disp, br, c.br);
        }
    }
    std::printf("%zu cases, %d failed\n", sizeof(kCases) / sizeof(kCases[0]), failures);
    return failures ? 1 : 0;
}
