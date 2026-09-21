// D3D11 presentation: renders the latest decoded NV12 (or B8G8R8A8) texture
// letterboxed into the window client area.

#pragma once

#include <memory>

#include "../video/h264_decoder.h"
#include "../win32_compat.h"

#ifdef _WIN32
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#endif

namespace od::render {

class Renderer {
public:
    // Takes an existing device (shared with the Media Foundation decoder via
    // the DXGI device manager) plus the window to present into.
    bool Init(HWND hwnd, Microsoft::WRL::ComPtr<ID3D11Device> device,
              int clientW, int clientH);
    void Resize(int w, int h);
    // Draw the frame (or just the background) and present. Main thread only.
    void Present(const video::DecodedFrame* frame);
    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return ctx_.Get(); }

private:
    bool CreateSwapChain(int w, int h);
    bool CreateShaders();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;

    // SRV cache for the most recent NV12 texture. NV12 is planar, so a
    // null-description SRV is NOT pixel-shader-sampleable; we make two planar
    // SRVs -- Y (R8_UNORM, plane 0) and UV (R8G8_UNORM, plane 1) -- and do the
    // YUV->RGB conversion in the pixel shader.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> srvTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvY_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvUV_;

    HWND swapHwnd_ = nullptr;
    int clientW_ = 0;
    int clientH_ = 0;
};

} // namespace od::render
