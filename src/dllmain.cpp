// DLL build only.

#include <revenant/revenant.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_THREAD_ATTACH:
        revenant::ApplyBreakpointsToCurrentThread();
        break;
    case DLL_PROCESS_DETACH:
        // FreeLibrary (not process exit): unhook while the code is still mapped.
        if (!reserved) revenant::Shutdown();
        break;
    default:
        break;
    }
    return TRUE;
}
