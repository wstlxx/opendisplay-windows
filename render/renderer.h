// D3D11 presentation: renders the latest decoded NV12 (or B8G8R8A8) texture
// letterboxed into the window client area.

#pragma once

#include <cstdint>
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
    // Wait for the display's vsync before Present. Off by default: for a
    // screen mirror the lowest latency wins (each Present otherwise waits up
    // to one refresh, adding up to a frame of latency + jitter). Pass true
    // (e.g. --vsync) for tear-free output instead.
    void SetVsync(bool on) { vsync_ = on; }
    // Letterboxed display rect of the last presented frame, in window client
    // pixels (for mapping window input to video coordinates). False if no
    // frame has been presented yet. Main thread only.
    bool VideoRect(int* outX, int* outY, int* outW, int* outH) const;
    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return ctx_.Get(); }

private:
    bool CreateSwapChain(int w, int h);
    bool CreateShaders();
    // Main thread only. (Re)create the persistent NV12 upload texture + its two
    // planar SRVs when the frame size changes.
    bool EnsureUploadTexture(int w, int h);
    // Main thread only. Copy CPU NV12 rows into the upload texture.
    void UploadNv12(const uint8_t* nv12, int w, int h);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;

    // The render thread owns a persistent NV12 upload texture (DYNAMIC). Each
    // frame's CPU NV12 (produced by the decode thread) is uploaded here on the
    // main thread -- a D3D11 immediate context must not be shared across
    // threads. NV12 is planar, so two planar SRVs (Y as R8 plane 0, UV as R8G8
    // plane 1) are made on it and YUV->RGB is done in the pixel shader.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> uploadTex_;
    int uploadW_ = 0, uploadH_ = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvY_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvUV_;

    HWND swapHwnd_ = nullptr;
    int clientW_ = 0;
    int clientH_ = 0;
    bool vsync_ = false;
    int lastVideoW_ = 0, lastVideoH_ = 0;  // set by Present, main thread
};

} // namespace od::render
