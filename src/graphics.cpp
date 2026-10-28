// All entry points are resolved with GetProcAddress; no graphics import libs.

#include <revenant/graphics.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

namespace revenant::graphics {
namespace {

template <class T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

class DummyWindow {
public:
    DummyWindow() {
        m_wc.cbSize = sizeof(m_wc);
        m_wc.lpfnWndProc = DefWindowProcW;
        m_wc.hInstance = GetModuleHandleW(nullptr);
        m_wc.lpszClassName = L"RevenantDummyWindow";
        m_registered = RegisterClassExW(&m_wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (m_registered)
            m_hwnd = CreateWindowExW(0, m_wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                     0, 0, 8, 8, nullptr, nullptr, m_wc.hInstance, nullptr);
    }
    ~DummyWindow() {
        if (m_hwnd) DestroyWindow(m_hwnd);
        if (m_registered) UnregisterClassW(m_wc.lpszClassName, m_wc.hInstance);
    }
    HWND Get() const { return m_hwnd; }

private:
    WNDCLASSEXW m_wc{};
    HWND m_hwnd = nullptr;
    bool m_registered = false;
};

template <class Fn>
Fn Proc(HMODULE m, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

void ReadEntries(void* object, size_t presentIdx, size_t resizeIdx, Entries& out) {
    void** vtable = *static_cast<void***>(object);
    out.present = vtable[presentIdx];
    out.resize = vtable[resizeIdx];
}

Status FindD3D9(Entries& out) {
    HMODULE mod = GetModuleHandleW(L"d3d9.dll");
    if (!mod) return Status::ModuleNotLoaded;
    auto create = Proc<IDirect3D9*(WINAPI*)(UINT)>(mod, "Direct3DCreate9");
    if (!create) return Status::ModuleNotLoaded;

    DummyWindow wnd;
    if (!wnd.Get()) return Status::DeviceCreationFailed;
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) return Status::DeviceCreationFailed;

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd.Get();
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IDirect3DDevice9* device = nullptr;
    Status st = Status::DeviceCreationFailed;
    for (D3DDEVTYPE type : { D3DDEVTYPE_HAL, D3DDEVTYPE_NULLREF }) {
        if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, type, wnd.Get(),
                                        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                                        &pp, &device)) && device) {
            ReadEntries(device, 17, 16, out);
            st = Status::Ok;
            break;
        }
    }
    SafeRelease(device);
    SafeRelease(d3d);
    return st;
}

DXGI_SWAP_CHAIN_DESC SwapChainDesc(HWND hwnd, DXGI_SWAP_EFFECT effect, UINT buffers) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = buffers;
    sd.BufferDesc.Width = 8;
    sd.BufferDesc.Height = 8;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = effect;
    return sd;
}

Status FindD3D11(Entries& out) {
    HMODULE mod = GetModuleHandleW(L"d3d11.dll");
    if (!mod) return Status::ModuleNotLoaded;
    auto create = Proc<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(mod, "D3D11CreateDeviceAndSwapChain");
    if (!create) return Status::ModuleNotLoaded;

    DummyWindow wnd;
    if (!wnd.Get()) return Status::DeviceCreationFailed;
    const DXGI_SWAP_CHAIN_DESC sd = SwapChainDesc(wnd.Get(), DXGI_SWAP_EFFECT_DISCARD, 1);
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    for (D3D_DRIVER_TYPE type : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP }) {
        IDXGISwapChain* swap = nullptr;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        if (SUCCEEDED(create(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                             &swap, &device, nullptr, &ctx)) && swap) {
            ReadEntries(swap, 8, 13, out);
            SafeRelease(ctx);
            SafeRelease(swap);
            SafeRelease(device);
            return Status::Ok;
        }
        SafeRelease(ctx);
        SafeRelease(swap);
        SafeRelease(device);
    }
    return Status::DeviceCreationFailed;
}

Status FindD3D12(Entries& out) {
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!d3d12 || !dxgi) return Status::ModuleNotLoaded;
    auto createDevice = Proc<PFN_D3D12_CREATE_DEVICE>(d3d12, "D3D12CreateDevice");
    auto createFactory = Proc<HRESULT(WINAPI*)(REFIID, void**)>(dxgi, "CreateDXGIFactory1");
    if (!createDevice || !createFactory) return Status::ModuleNotLoaded;

    DummyWindow wnd;
    if (!wnd.Get()) return Status::DeviceCreationFailed;

    IDXGIFactory1* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain* swap = nullptr;
    Status st = Status::DeviceCreationFailed;

    if (SUCCEEDED(createFactory(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
            DXGI_SWAP_CHAIN_DESC sd = SwapChainDesc(wnd.Get(), DXGI_SWAP_EFFECT_FLIP_DISCARD, 2);
            if (SUCCEEDED(factory->CreateSwapChain(queue, &sd, &swap)) && swap) {
                ReadEntries(swap, 8, 13, out);
                st = Status::Ok;
            }
        }
    }
    SafeRelease(swap);
    SafeRelease(queue);
    SafeRelease(device);
    SafeRelease(factory);
    return st;
}

Status FindOpenGL(Entries& out) {
    HMODULE mod = GetModuleHandleW(L"opengl32.dll");
    if (!mod) return Status::ModuleNotLoaded;
    void* swap = reinterpret_cast<void*>(GetProcAddress(mod, "wglSwapBuffers"));
    if (!swap) return Status::ModuleNotLoaded;
    out.present = swap;
    out.resize = nullptr;
    return Status::Ok;
}

} // namespace

Status Find(Api api, Entries& out) noexcept {
    out = Entries{};
    try {
        switch (api) {
        case Api::D3D9:   return FindD3D9(out);
        case Api::D3D11:  return FindD3D11(out);
        case Api::D3D12:  return FindD3D12(out);
        case Api::OpenGL: return FindOpenGL(out);
        }
    } catch (...) {
    }
    return Status::InvalidParameter;
}

} // namespace revenant::graphics
