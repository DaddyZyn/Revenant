#include <revenant/revenant.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace revenant;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

#define CHECK_OK(expr) CHECK((expr) == Status::Ok)


__declspec(noinline) int Add(int a, int b) {
    volatile int r = a + b;
    return r;
}
__declspec(noinline) int Mul(int a, int b) {
    volatile int r = a * b;
    return r;
}
__declspec(noinline) int Sub(int a, int b) {
    volatile int r = a - b;
    return r;
}
__declspec(noinline) int Square(int a) {
    volatile int r = a * a;
    return r;
}

using BinFn = int (*)(int, int);
using UnFn = int (*)(int);

BinFn g_addOriginal = nullptr;
int AddDetour(int a, int b) { return g_addOriginal(a, b) + 1000; }

BinFn g_chainA = nullptr;
BinFn g_chainB = nullptr;
int ChainA(int a, int b) { return g_chainA(a, b) * 10; }
int ChainB(int a, int b) { return g_chainB(a, b) + 1; }

BinFn g_bpOriginal = nullptr;
int SubDetour(int a, int b) { return g_bpOriginal(a, b) - 500; }

UnFn g_hpOriginal = nullptr;
int SquareDetour(int a) { return g_hpOriginal(a) + 7; }

struct Shape {
    virtual ~Shape() = default;
    virtual int Sides() const { return 0; }
};
struct Triangle : Shape {
    int Sides() const override { return 3; }
};
using SidesFn = int (*)(const Shape*);
SidesFn g_sidesOriginal = nullptr;
int SidesDetour(const Shape* self) { return g_sidesOriginal(self) + 40; }

using MulDivFn = int(WINAPI*)(int, int, int);
MulDivFn g_mulDivOriginal = nullptr;
int WINAPI MulDivDetour(int a, int b, int c) { return g_mulDivOriginal(a, b, c) + 1; }

// Copies raw code into an RX page.

struct Code {
    uint8_t* p = nullptr;
    explicit Code(std::initializer_list<uint8_t> bytes, size_t offset = 64) {
        p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        std::memset(p, 0xCC, 4096);
        std::memcpy(p + offset, bytes.begin(), bytes.size());
        p += offset;
        DWORD old;
        VirtualProtect(p - offset, 4096, PAGE_EXECUTE_READ, &old);
    }
    ~Code() { VirtualFree(p - 64, 0, MEM_RELEASE); }
    template <class Fn> Fn As() const { return reinterpret_cast<Fn>(p); }
};

int ReturnsFive() { return 5; }
using IntFn = int (*)();
using IntArgFn = int (*)(int);

IntFn g_genericIntOriginal = nullptr;
int GenericIntDetour() { return g_genericIntOriginal() + 100; }
IntArgFn g_genericArgOriginal = nullptr;
int GenericArgDetour(int x) { return g_genericArgOriginal(x) + 100; }


void TestInline() {
    std::printf("inline\n");
    CHECK(Add(2, 3) == 5);
    void* orig = nullptr;
    CHECK_OK(Create(&Add, &AddDetour, &orig));
    g_addOriginal = reinterpret_cast<BinFn>(orig);
    CHECK(IsHooked(&Add));
    CHECK(!IsEnabled(&Add));
    CHECK(KindOf(&Add) == Kind::Inline);
    CHECK(Add(2, 3) == 5);
    CHECK_OK(Enable(&Add));
    CHECK(Enable(&Add) == Status::AlreadyEnabled);
    CHECK(Add(2, 3) == 1005);
    CHECK(g_addOriginal(2, 3) == 5);
    CHECK_OK(Disable(&Add));
    CHECK(Add(2, 3) == 5);
    CHECK_OK(Enable(&Add));
    CHECK(Add(2, 3) == 1005);
    CHECK_OK(Remove(&Add));
    CHECK(!IsHooked(&Add));
    CHECK(Add(2, 3) == 5);
    CHECK(Remove(&Add) == Status::NotHooked);
}

void TestChain() {
    std::printf("chain\n");
    void* a = nullptr;
    void* b = nullptr;
    CHECK_OK(Create(&Mul, &ChainA, &a));
    g_chainA = reinterpret_cast<BinFn>(a);
    CHECK_OK(Enable(&Mul));
    CHECK(Mul(3, 4) == 120);
    CHECK_OK(Create(&Mul, &ChainB, &b));
    g_chainB = reinterpret_cast<BinFn>(b);
    CHECK(b == reinterpret_cast<void*>(&ChainA));
    // (3*4*10)+1
    CHECK(Mul(3, 4) == 121);
    CHECK(Create(&Mul, &ChainB, &b, Kind::Breakpoint) == Status::AlreadyHooked);
    CHECK_OK(Remove(&Mul));
    CHECK(Mul(3, 4) == 12);
}

void TestSystemFunction() {
    std::printf("system function (MulDiv)\n");
    void* orig = nullptr;
    auto target = reinterpret_cast<void*>(&MulDiv);
    CHECK_OK(Create(target, &MulDivDetour, &orig));
    g_mulDivOriginal = reinterpret_cast<MulDivFn>(orig);
    CHECK_OK(Enable(target));
    volatile MulDivFn call = &MulDiv;
    CHECK(call(10, 10, 4) == 26);
    CHECK_OK(Remove(target));
    CHECK(call(10, 10, 4) == 25);
}

std::atomic<int> g_threadResult{ 0 };

void TestBreakpoint() {
    std::printf("breakpoint\n");
    void* orig = nullptr;
    CHECK_OK(Create(&Sub, &SubDetour, &orig, Kind::Breakpoint));
    g_bpOriginal = reinterpret_cast<BinFn>(orig);
    CHECK(KindOf(&Sub) == Kind::Breakpoint);
    CHECK_OK(Enable(&Sub));

    uint8_t first = 0;
    std::memcpy(&first, reinterpret_cast<void*>(&Sub), 1);
    CHECK(first != 0xE9);
    CHECK(Sub(10, 3) == -493);

    std::thread([] {
        ApplyBreakpointsToCurrentThread();
        g_threadResult = Sub(10, 3);
    }).join();
    CHECK(g_threadResult == -493);

    CHECK_OK(Disable(&Sub));
    CHECK(Sub(10, 3) == 7);
    CHECK_OK(Enable(&Sub));
    CHECK(Sub(10, 3) == -493);
    CHECK_OK(Remove(&Sub));
    CHECK(Sub(10, 3) == 7);
}

void TestHotPatch() {
    std::printf("hot-patch\n");
    void* orig = nullptr;
    Status st = Create(&Square, &SquareDetour, &orig, Kind::HotPatch);
    if (st == Status::NotHotPatchable) {
        std::printf("  skipped: Square has no hot-patch padding in this build\n");
        return;
    }
    CHECK_OK(st);
    g_hpOriginal = reinterpret_cast<UnFn>(orig);
    CHECK_OK(Enable(&Square));
    const uint8_t* code = reinterpret_cast<const uint8_t*>(&Square);
    CHECK(code[0] == 0xEB && code[1] == 0xF9);
    CHECK(Square(6) == 43);
    CHECK_OK(Remove(&Square));
    CHECK(Square(6) == 36);
    CHECK(code[-5] == 0xCC || code[-5] == 0x90);
}

void TestHotPatchScratch() {
    std::printf("hot-patch (scratch)\n");
    // 5 bytes CC padding, then mov eax,5 ; ret
    Code code({ 0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3 });
    void* orig = nullptr;
    CHECK_OK(Create(code.p, &GenericIntDetour, &orig, Kind::HotPatch));
    g_genericIntOriginal = reinterpret_cast<IntFn>(orig);
    CHECK_OK(Enable(code.p));
    CHECK(code.As<IntFn>()() == 105);
    CHECK_OK(Remove(code.p));
    CHECK(code.As<IntFn>()() == 5);

    // 1-byte first instruction
    Code tiny({ 0x90, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3 });
    CHECK(Create(tiny.p, &GenericIntDetour, &orig, Kind::HotPatch) == Status::NotHotPatchable);
}

void TestVTable() {
    std::printf("vtable\n");
    Triangle tri;
    Shape* shape = &tri;
    void* slot = VTableSlot(shape, 1);
    void* orig = nullptr;
    CHECK_OK(Create(slot, &SidesDetour, &orig, Kind::VTable));
    g_sidesOriginal = reinterpret_cast<SidesFn>(orig);
    CHECK(shape->Sides() == 3);
    CHECK_OK(Enable(slot));
    CHECK(shape->Sides() == 43);
    CHECK_OK(DisableAll());
    CHECK(shape->Sides() == 3);
    CHECK_OK(EnableAll());
    CHECK(shape->Sides() == 43);
    CHECK_OK(RemoveAll());
    CHECK(shape->Sides() == 3);
    CHECK(Count() == 0);
}

void TestBatch() {
    std::printf("batch\n");
    void* a = nullptr;
    void* m = nullptr;
    CHECK_OK(Create(&Add, &AddDetour, &a));
    g_addOriginal = reinterpret_cast<BinFn>(a);
    CHECK_OK(Create(&Mul, &ChainA, &m));
    g_chainA = reinterpret_cast<BinFn>(m);

    CHECK_OK(BeginBatch());
    CHECK(BeginBatch() == Status::BatchActive);
    CHECK_OK(Enable(&Add));
    CHECK_OK(Enable(&Mul));
    CHECK(RemoveAll() == Status::BatchActive);
    CHECK(Add(1, 1) == 2);  // queued, not applied yet
    CHECK_OK(CommitBatch());
    CHECK(Add(1, 1) == 1002);
    CHECK(Mul(2, 2) == 40);

    CHECK_OK(BeginBatch());
    CHECK_OK(Disable(&Add));
    AbortBatch();
    CHECK(Add(1, 1) == 1002);
    CHECK(CommitBatch() == Status::NoBatch);

    CHECK_OK(RemoveAll());
    CHECK(Add(1, 1) == 2);
    CHECK(Mul(2, 2) == 4);
}

void TestTyped() {
    std::printf("typed Hook<>\n");
    {
        Hook<BinFn> hook(&Add, &AddDetour);
        CHECK(static_cast<bool>(hook));
        g_addOriginal = hook.Original();
        CHECK_OK(hook.Enable());
        CHECK(hook.Enabled());
        CHECK(Add(1, 2) == 1003);
        CHECK(hook.CallOriginal(1, 2) == 3);

        Hook<BinFn> moved = std::move(hook);
        CHECK(!hook);
        CHECK(Add(1, 2) == 1003);
    }
    CHECK(Add(1, 2) == 3);
    CHECK(!IsHooked(&Add));

    Hook<BinFn> bad(static_cast<BinFn>(nullptr), &AddDetour);
    CHECK(!bad);
    CHECK(bad.Result() == Status::InvalidParameter);
}

void TestRelocation() {
    std::printf("relocation\n");
    void* orig = nullptr;

    // lea rax,[rip+0x10] ; mov rax,[rax] ; ret   with qword 0x2A at the target
    {
        Code code({ 0x48, 0x8D, 0x05, 0x10, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x00, 0xC3,
                    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
                    0x2A, 0, 0, 0, 0, 0, 0, 0 });  // at rip(7) + 0x10 = 23
        CHECK(code.As<IntFn>()() == 42);
        CHECK_OK(Create(code.p, &GenericIntDetour, &orig));
        g_genericIntOriginal = reinterpret_cast<IntFn>(orig);
        CHECK_OK(Enable(code.p));
        CHECK(code.As<IntFn>()() == 142);
        CHECK(g_genericIntOriginal() == 42);
        CHECK_OK(Remove(code.p));
    }

    // test ecx,ecx ; je +6 ; mov eax,1 ; ret ; mov eax,2 ; ret
    {
        Code code({ 0x85, 0xC9, 0x74, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3,
                    0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3 });
        CHECK_OK(Create(code.p, &GenericArgDetour, &orig));
        g_genericArgOriginal = reinterpret_cast<IntArgFn>(orig);
        CHECK_OK(Enable(code.p));
        CHECK(code.As<IntArgFn>()(0) == 102);
        CHECK(code.As<IntArgFn>()(1) == 101);
        CHECK(g_genericArgOriginal(0) == 2);
        CHECK(g_genericArgOriginal(1) == 1);
        CHECK_OK(Remove(code.p));
    }

    // jmp +3 ; nop x3 ; mov eax,5 ; ret
    {
        Code code({ 0xEB, 0x03, 0x90, 0x90, 0x90, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3 });
        CHECK_OK(Create(code.p, &GenericIntDetour, &orig));
        g_genericIntOriginal = reinterpret_cast<IntFn>(orig);
        CHECK_OK(Enable(code.p));
        CHECK(code.As<IntFn>()() == 105);
        CHECK(g_genericIntOriginal() == 5);
        CHECK_OK(Remove(code.p));
    }

    // mov rax,imm64 ; sub rsp,28h ; call rax ; add rsp,28h ; ret
    {
        uint64_t fn = reinterpret_cast<uint64_t>(&ReturnsFive);
        Code code({ 0x48, 0xB8,
                    uint8_t(fn), uint8_t(fn >> 8), uint8_t(fn >> 16), uint8_t(fn >> 24),
                    uint8_t(fn >> 32), uint8_t(fn >> 40), uint8_t(fn >> 48), uint8_t(fn >> 56),
                    0x48, 0x83, 0xEC, 0x28, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0xC3 });
        CHECK_OK(Create(code.p, &GenericIntDetour, &orig));
        g_genericIntOriginal = reinterpret_cast<IntFn>(orig);
        CHECK_OK(Enable(code.p));
        CHECK(code.As<IntFn>()() == 105);
        CHECK_OK(Remove(code.p));
    }

    // xor eax,eax ; inc eax ; jmp back into the patched region (-> Unrelocatable)
    {
        Code code({ 0x31, 0xC0, 0xFF, 0xC0, 0xEB, 0xFC });
        CHECK(Create(code.p, &GenericIntDetour, &orig) == Status::Unrelocatable);
        CHECK(!IsHooked(code.p));
    }

    // ret followed by live code
    {
        Code code({ 0xC3, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3 });
        CHECK(Create(code.p, &GenericIntDetour, &orig) != Status::Ok);
    }
}

void TestErrors() {
    std::printf("errors\n");
    void* orig = nullptr;
    CHECK(Create(nullptr, &AddDetour, &orig) == Status::InvalidParameter);
    CHECK(Create(&Add, nullptr, &orig) == Status::InvalidParameter);
    CHECK(Create(&Add, &AddDetour, nullptr) == Status::InvalidParameter);
    static int data = 0;
    CHECK(Create(&data, &AddDetour, &orig) == Status::NotExecutable);
    CHECK(Enable(&Add) == Status::NotHooked);
    CHECK(std::strcmp(ToString(Status::Unreachable), "Unreachable") == 0);
    CHECK(std::strcmp(ToString(Kind::HotPatch), "HotPatch") == 0);
}

} // namespace

int main() {
    std::printf("Revenant %d.%d.%d tests\n", kVersionMajor, kVersionMinor, kVersionPatch);
    TestInline();
    TestChain();
    TestSystemFunction();
    TestBreakpoint();
    TestHotPatch();
    TestHotPatchScratch();
    TestVTable();
    TestBatch();
    TestTyped();
    TestRelocation();
    TestErrors();
    CHECK(Count() == 0);
    Shutdown();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
