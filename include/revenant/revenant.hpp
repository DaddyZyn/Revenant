// Revenant - x64 function hooking for Windows.
//
// Hook kinds:
//
//   Inline      5-byte rel32 jump to a relay next to the trampoline
//   Breakpoint  DR0-DR3 hardware execute breakpoint (no code is modified)
//   HotPatch    MSVC /hotpatch layout: 2-byte short jump into the padding
//   VTable      pointer swap of a single vtable slot
//
// Hooks are keyed by target (code address, or slot address for VTable) and
// are created disabled.
//
//   void* original = nullptr;
//   revenant::Create(&Target, &Detour, &original);
//   revenant::Enable(&Target);
//
// or, typed:
//
//   revenant::Hook<decltype(&Target)> hook(&Target, &Detour);
//   hook.Enable();
//   hook.Original()(args...);

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#if defined(REVENANT_STATIC)
#  define REVENANT_API
#elif defined(REVENANT_EXPORTS)
#  define REVENANT_API __declspec(dllexport)
#else
#  define REVENANT_API __declspec(dllimport)
#endif

#if !defined(_M_X64) && !defined(__x86_64__)
#  error "Revenant supports x64 only"
#endif

namespace revenant {

inline constexpr int kVersionMajor = 3;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

enum class Status : int {
    Ok = 0,
    AlreadyHooked,        // target already hooked with an incompatible kind
    NotHooked,            // no hook registered for this target
    AlreadyEnabled,
    AlreadyDisabled,
    InvalidParameter,
    NotExecutable,        // target is not committed executable memory
    Undecodable,          // prologue contains bytes the decoder can't parse
    Unrelocatable,        // prologue can't be moved (loop/jecxz, internal branch, rip-rel out of range)
    Unreachable,          // no trampoline slot within +-2 GB of the target
    NotHotPatchable,      // missing padding / 1-byte first instruction
    AccessDenied,         // VirtualProtect failed
    NoFreeDebugRegister,  // all four hardware breakpoints are in use
    ThreadEnumFailed,     // thread snapshot / suspension failed
    BatchActive,          // operation not allowed inside a batch
    NoBatch,              // CommitBatch without BeginBatch
    ModuleNotLoaded,      // graphics::Find: the API's DLL isn't loaded
    DeviceCreationFailed, // graphics::Find: throwaway device/swap chain failed
};

enum class Kind : std::uint8_t {
    Inline,
    Breakpoint,
    HotPatch,
    VTable,
};

[[nodiscard]] REVENANT_API const char* ToString(Status status) noexcept;
[[nodiscard]] REVENANT_API const char* ToString(Kind kind) noexcept;

// Kind::VTable takes the slot address (see VTableSlot). A second Inline hook on
// the same target is chained: the new detour runs first and receives the
// previous detour as its original. Remove pops the most recent detour from the
// chain, so chained hooks must be removed newest-first.
[[nodiscard]] REVENANT_API Status Create(void* target, void* detour, void** original,
                                         Kind kind = Kind::Inline) noexcept;

[[nodiscard]] REVENANT_API Status Enable(void* target) noexcept;
[[nodiscard]] REVENANT_API Status Disable(void* target) noexcept;
[[nodiscard]] REVENANT_API Status Remove(void* target) noexcept;

// Applied under a single thread freeze.
[[nodiscard]] REVENANT_API Status EnableAll() noexcept;
[[nodiscard]] REVENANT_API Status DisableAll() noexcept;
[[nodiscard]] REVENANT_API Status RemoveAll() noexcept;

// Enable/Disable calls between BeginBatch and CommitBatch are deferred and
// applied together.
[[nodiscard]] REVENANT_API Status BeginBatch() noexcept;
[[nodiscard]] REVENANT_API Status CommitBatch() noexcept;
REVENANT_API void AbortBatch() noexcept;

[[nodiscard]] REVENANT_API bool   IsHooked(void* target) noexcept;
[[nodiscard]] REVENANT_API bool   IsEnabled(void* target) noexcept;
[[nodiscard]] REVENANT_API Kind   KindOf(void* target) noexcept;
[[nodiscard]] REVENANT_API void*  OriginalOf(void* target) noexcept;   // trampoline / saved slot
[[nodiscard]] REVENANT_API size_t Count() noexcept;

// Debug registers are per thread. The DLL build arms new threads from
// DLL_THREAD_ATTACH; static builds must call this on threads created after
// a Breakpoint hook was enabled.
REVENANT_API void ApplyBreakpointsToCurrentThread() noexcept;

// Removes all hooks and the VEH. Call before unloading a module that contains detours.
REVENANT_API void Shutdown() noexcept;

// &(*(void***)object)[index]
[[nodiscard]] inline void* VTableSlot(void* object, size_t index) noexcept {
    return &(*static_cast<void***>(object))[index];
}


template <class Fn>
class Hook {
    static_assert(std::is_pointer_v<Fn> && std::is_function_v<std::remove_pointer_t<Fn>>,
                  "Hook<Fn>: Fn must be a function pointer type, e.g. decltype(&MessageBoxW)");

public:
    Hook() noexcept = default;

    Hook(Fn target, Fn detour, Kind kind = Kind::Inline) noexcept {
        Attach(reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour), kind);
    }

        Hook(void* target, Fn detour, Kind kind = Kind::Inline) noexcept {
        Attach(target, reinterpret_cast<void*>(detour), kind);
    }

    ~Hook() { Reset(); }

    Hook(Hook&& other) noexcept { *this = std::move(other); }
    Hook& operator=(Hook&& other) noexcept {
        if (this != &other) {
            Reset();
            m_target   = std::exchange(other.m_target, nullptr);
            m_original = std::exchange(other.m_original, nullptr);
            m_status   = std::exchange(other.m_status, Status::NotHooked);
        }
        return *this;
    }
    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;

    Status Enable() noexcept  { return m_target ? revenant::Enable(m_target)  : m_status; }
    Status Disable() noexcept { return m_target ? revenant::Disable(m_target) : m_status; }

    void Reset() noexcept {
        if (m_target) (void)revenant::Remove(m_target);
        m_target = nullptr;
        m_original = nullptr;
        m_status = Status::NotHooked;
    }

    [[nodiscard]] Fn     Original() const noexcept { return m_original; }
    [[nodiscard]] void*  Target() const noexcept { return m_target; }
    [[nodiscard]] Status Result() const noexcept { return m_status; }
    [[nodiscard]] bool   Enabled() const noexcept { return m_target && revenant::IsEnabled(m_target); }
    explicit operator bool() const noexcept { return m_target != nullptr; }

    template <class... Args>
    decltype(auto) CallOriginal(Args&&... args) const {
        return m_original(std::forward<Args>(args)...);
    }

private:
    void Attach(void* target, void* detour, Kind kind) noexcept {
        void* original = nullptr;
        m_status = revenant::Create(target, detour, &original, kind);
        if (m_status == Status::Ok) {
            m_target = target;
            m_original = reinterpret_cast<Fn>(original);
        }
    }

    void*  m_target = nullptr;
    Fn     m_original = nullptr;
    Status m_status = Status::NotHooked;
};

} // namespace revenant
