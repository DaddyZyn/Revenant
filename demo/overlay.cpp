// Minimal D3D11 app. The ImGui overlay is drawn entirely from a hooked
// IDXGISwapChain::Present; the app itself only clears and presents.

#include <revenant/revenant.hpp>
#include <revenant/graphics.hpp>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

revenant::Hook<PresentFn> g_presentHook;
bool     g_overlayReady = false;
bool     g_showDemoWindow = false;
uint64_t g_frames = 0;

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }
}

void ReleaseRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
    if (!g_overlayReady) {
        DXGI_SWAP_CHAIN_DESC desc{};
        swap->GetDesc(&desc);
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(desc.OutputWindow);
        ImGui_ImplDX11_Init(g_device, g_context);
        g_overlayReady = true;
    }

    ++g_frames;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::Begin("Revenant", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Present is hooked (%s)", revenant::ToString(revenant::KindOf(g_presentHook.Target())));
    ImGui::Text("frames through detour: %llu", static_cast<unsigned long long>(g_frames));
    ImGui::Text("active hooks: %zu", revenant::Count());
    ImGui::Separator();
    ImGui::Checkbox("ImGui demo window", &g_showDemoWindow);
    if (ImGui::Button("Unhook (overlay disappears)")) (void)g_presentHook.Disable();
    ImGui::End();
    if (g_showDemoWindow) ImGui::ShowDemoWindow(&g_showDemoWindow);

    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    return g_presentHook.Original()(swap, syncInterval, flags);
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    for (D3D_DRIVER_TYPE type : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP }) {
        if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels, 2,
                                                    D3D11_SDK_VERSION, &sd, &g_swapChain,
                                                    &g_device, nullptr, &g_context))) {
            CreateRenderTarget();
            return true;
        }
    }
    return false;
}

void DestroyDevice() {
    ReleaseRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_overlayReady && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return TRUE;
    switch (msg) {
    case WM_SIZE:
        if (g_swapChain && wParam != SIZE_MINIMIZED) {
            ReleaseRenderTarget();
            g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance,
                    nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"RevenantDemo", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Revenant - Present hook demo",
                                WS_OVERLAPPEDWINDOW, 100, 100, 1024, 640,
                                nullptr, nullptr, instance, nullptr);

    if (!CreateDevice(hwnd)) {
        MessageBoxW(hwnd, L"Could not create a D3D11 device.", L"Revenant", MB_ICONERROR);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, instance);
        return 1;
    }

    revenant::graphics::Entries entries;
    revenant::Status st = revenant::graphics::Find(revenant::graphics::Api::D3D11, entries);
    if (st == revenant::Status::Ok) {
        g_presentHook = revenant::Hook<PresentFn>(entries.present, &HookedPresent);
        st = g_presentHook ? g_presentHook.Enable() : g_presentHook.Result();
    }
    if (st != revenant::Status::Ok) {
        wchar_t text[128];
        wsprintfW(text, L"Hooking Present failed: %S", revenant::ToString(st));
        MessageBoxW(hwnd, text, L"Revenant", MB_ICONWARNING);
    }

    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        const float clear[4] = { 0.08f, 0.09f, 0.12f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        g_swapChain->Present(1, 0);
    }

    g_presentHook.Reset();
    if (g_overlayReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    DestroyDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
