// Locates Present/ResizeBuffers (or equivalent) for a loaded graphics API.
//
//   revenant::graphics::Entries e;
//   if (revenant::graphics::Find(revenant::graphics::Api::D3D11, e) == revenant::Status::Ok)
//       revenant::Create(e.present, &MyPresent, &originalPresent);
//
// Creates a temporary device and swap chain on a hidden window and reads the
// vtable. The API's DLL must already be loaded; nothing is LoadLibrary'd.

#pragma once
#include "revenant.hpp"

namespace revenant::graphics {

enum class Api {
    D3D9,     // IDirect3DDevice9::Present [17], Reset [16]
    D3D11,    // IDXGISwapChain::Present [8], ResizeBuffers [13]
    D3D12,    // IDXGISwapChain::Present [8], ResizeBuffers [13] (D3D12 queue)
    OpenGL,   // wglSwapBuffers (resize = nullptr)
};

struct Entries {
    void* present = nullptr;
    void* resize = nullptr;
};

// Status::ModuleNotLoaded if the API's DLL isn't loaded in the process,
// Status::DeviceCreationFailed if a device or swap chain couldn't be created.
[[nodiscard]] REVENANT_API Status Find(Api api, Entries& out) noexcept;

} // namespace revenant::graphics
