#include "gfx.h"

#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace gfx {

static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_context = nullptr;
static IDXGISwapChain*         g_swapchain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11SamplerState*     g_pointSampler = nullptr;
static ID3D11SamplerState*     g_linearSampler = nullptr;

ID3D11Device*           device() { return g_device; }
ID3D11DeviceContext*    context() { return g_context; }
IDXGISwapChain*         swapChain() { return g_swapchain; }
ID3D11RenderTargetView* renderTargetView() { return g_rtv; }

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
#ifdef _DEBUG
    // flags |= D3D11_CREATE_DEVICE_DEBUG;  // enable if the debug layer is present
#endif
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &featureLevel, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &featureLevel, &g_context);
    }
    if (FAILED(hr)) return false;

    createRenderTarget();
    createSamplers();
    return true;
}

void createSamplers() {
    if (!g_device) return;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    g_device->CreateSamplerState(&sd, &g_pointSampler);

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    g_device->CreateSamplerState(&sd, &g_linearSampler);
}

void bindPointSampler() {
    if (g_context && g_pointSampler)
        g_context->PSSetSamplers(0, 1, &g_pointSampler);
}

void bindLinearSampler() {
    if (g_context && g_linearSampler)
        g_context->PSSetSamplers(0, 1, &g_linearSampler);
}

void cleanupDevice() {
    cleanupRenderTarget();
    if (g_pointSampler) { g_pointSampler->Release(); g_pointSampler = nullptr; }
    if (g_linearSampler) { g_linearSampler->Release(); g_linearSampler = nullptr; }
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }
}

void cleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

void resizeBuffers(UINT width, UINT height) {
    if (!g_swapchain) return;
    cleanupRenderTarget();
    g_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
}

void present(bool vsync) {
    if (g_swapchain) g_swapchain->Present(vsync ? 1 : 0, 0);
}

// ---- Texture --------------------------------------------------------------

Texture::~Texture() { release(); }

void Texture::release() {
    if (srv_) { srv_->Release(); srv_ = nullptr; }
    if (tex_) { tex_->Release(); tex_ = nullptr; }
    w_ = h_ = 0;
}

void Texture::ensure(int width, int height) {
    if (tex_ && width == w_ && height == h_) return;
    release();
    if (width <= 0 || height <= 0 || !g_device) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &tex_))) {
        tex_ = nullptr;
        return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    if (FAILED(g_device->CreateShaderResourceView(tex_, &srvDesc, &srv_))) {
        release();
        return;
    }
    w_ = width;
    h_ = height;
}

void Texture::update(const uint32_t* pixels, int width, int height) {
    ensure(width, height);
    if (!tex_ || !pixels) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;

    auto* dst = (uint8_t*)mapped.pData;
    const auto* src = (const uint8_t*)pixels;
    const size_t rowBytes = (size_t)width * 4;
    for (int y = 0; y < height; ++y) {
        memcpy(dst + (size_t)y * mapped.RowPitch, src + (size_t)y * rowBytes, rowBytes);
    }
    g_context->Unmap(tex_, 0);
}

}  // namespace gfx
