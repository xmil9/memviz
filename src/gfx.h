#pragma once

#include <cstdint>

#include <windows.h>
#include <d3d11.h>

#include "imgui.h"

// Thin DirectX 11 helper: device/swapchain lifecycle plus a dynamic texture that
// backs the memory-grid image shown through ImGui::Image.
namespace gfx {

bool createDevice(HWND hwnd);
void cleanupDevice();
void createRenderTarget();
void cleanupRenderTarget();
void resizeBuffers(UINT width, UINT height);
void present(bool vsync);

ID3D11Device*           device();
ID3D11DeviceContext*    context();
IDXGISwapChain*         swapChain();
ID3D11RenderTargetView* renderTargetView();

// GPU texture that mirrors a CPU RGBA pixel buffer.
class Texture {
public:
    ~Texture();
    // (Re)create when dimensions change; upload the pixel buffer (row-major RGBA).
    void update(const uint32_t* pixels, int width, int height);
    void release();
    ImTextureID id() const { return (ImTextureID)srv_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void ensure(int width, int height);
    ID3D11Texture2D*          tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    int w_ = 0;
    int h_ = 0;
};

}  // namespace gfx
