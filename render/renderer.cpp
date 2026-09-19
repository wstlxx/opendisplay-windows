#include "renderer.h"

#ifdef _WIN32

#include <algorithm>

#include <d3dcompiler.h>

#include "../net/log.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace od::render {

namespace {

constexpr char kVertexShader[] = R"hlsl(
cbuffer PerFrame : register(b0) {
    float4 WinRect;     // letterboxed video rect in window pixels
    float4 TexRect;     // video crop in normalized texture coords
    float2 WindowSize;  // window client size in pixels
    float2 _pad;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Fullscreen triangle. The three t-vertices (0,0),(2,0),(0,2) form a triangle
// that fully covers the unit square [0,1]x[0,1] (every point in the square
// satisfies x>=0, y>=0, x+y<=2), so the letterbox rect is drawn completely.
// t maps onto both the window rect and the (cropped) texture rect.
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 t = (id == 1) ? float2(2,0) : (id == 2) ? float2(0,2) : float2(0,0);
    float2 win = WinRect.xy + t * WinRect.zw;
    o.pos = float4(win.x / WindowSize.x * 2.0 - 1.0,
                   -(win.y / WindowSize.y * 2.0 - 1.0), 0.0, 1.0);
    o.uv = TexRect.xy + t * TexRect.zw;
    return o;
}
)hlsl";

// The pixel shader needs the VS output struct defined in the same TU.
constexpr char kPixelShaderFull[] = R"hlsl(
struct VSIn {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
Texture2D Video : register(t0);
SamplerState Smp : register(s0);
float4 PSMain(VSIn in) : SV_Target {
    return Video.Sample(Smp, in.uv);
}
)hlsl";

struct PerFrame {
    float WinRect[4];
    float TexRect[4];
    float WindowSize[2];
    float pad;
};

} // namespace

bool Renderer::Init(HWND hwnd, Microsoft::WRL::ComPtr<ID3D11Device> device,
                    int clientW, int clientH) {
    clientW_ = clientW;
    clientH_ = clientH;
    swapHwnd_ = hwnd;
    device_ = std::move(device);
    device_->GetImmediateContext(ctx_.ReleaseAndGetAddressOf());

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2),
                                  reinterpret_cast<void**>(factory_.ReleaseAndGetAddressOf())))) {
        LOG_ERROR("renderer: CreateDXGIFactory1 failed");
        return false;
    }

    if (!CreateSwapChain(clientW, clientH)) return false;
    if (!CreateShaders()) return false;

    // Constant buffer (PerFrame).
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(PerFrame);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, constants_.ReleaseAndGetAddressOf()))) {
        LOG_ERROR("renderer: CreateBuffer failed");
        return false;
    }

    // Sampler: bilinear, clamp.
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = 16.0f;  // D3D11_MAX_LOD (absent in the CI runner's SDK)
    if (FAILED(device_->CreateSamplerState(&sd, sampler_.ReleaseAndGetAddressOf()))) {
        LOG_ERROR("renderer: CreateSamplerState failed");
        return false;
    }
    return true;
}

bool Renderer::CreateSwapChain(int w, int h) {
    rtv_.Reset();
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = w;
    scd.Height = h;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // (DXGI_SWAP_CHAIN_DESC1::OutputWindow is absent in the CI runner's SDK;
    //  the HWND is passed separately to CreateSwapChainForHwnd below.)

    if (FAILED(factory_->CreateSwapChainForHwnd(
            device_.Get(), swapHwnd_, &scd, nullptr, nullptr,
            swap_.ReleaseAndGetAddressOf()))) {
        LOG_ERROR("renderer: CreateSwapChainForHwnd failed");
        return false;
    }
    factory_->MakeWindowAssociation(swapHwnd_, 0);

    IDXGIResource* back = nullptr;
    if (FAILED(swap_->GetBuffer(0, __uuidof(IDXGIResource),
                                reinterpret_cast<void**>(&back)))) {
        LOG_ERROR("renderer: GetBuffer failed");
        return false;
    }
    ID3D11Texture2D* tex = nullptr;
    back->QueryInterface(__uuidof(ID3D11Texture2D),
                         reinterpret_cast<void**>(&tex));
    back->Release();
    if (!tex) return false;
    device_->CreateRenderTargetView(tex, nullptr, rtv_.ReleaseAndGetAddressOf());
    tex->Release();
    return true;
}

bool Renderer::CreateShaders() {
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errs = nullptr;
    HRESULT hr = D3DCompile(
        kVertexShader, sizeof(kVertexShader) - 1, "vs.hlsl", nullptr,
        nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &errs);
    if (FAILED(hr)) {
        if (errs) {
            LOG_ERROR("renderer: VS compile: %s", (char*)errs->GetBufferPointer());
            errs->Release();
        }
        return false;
    }
    hr = D3DCompile(kPixelShaderFull, sizeof(kPixelShaderFull) - 1, "ps.hlsl",
                    nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errs);
    if (FAILED(hr)) {
        if (errs) {
            LOG_ERROR("renderer: PS compile: %s", (char*)errs->GetBufferPointer());
            errs->Release();
        }
        vsBlob->Release();
        return false;
    }
    if (errs) errs->Release();

    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                           vsBlob->GetBufferSize(), nullptr,
                                           vs_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(),
                                          psBlob->GetBufferSize(), nullptr,
                                          ps_.ReleaseAndGetAddressOf()))) {
        LOG_ERROR("renderer: shader creation failed");
        return false;
    }
    vsBlob->Release();
    psBlob->Release();
    return true;
}

void Renderer::Resize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    clientW_ = w;
    clientH_ = h;
    ctx_->Flush();
    if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
        LOG_WARN("renderer: ResizeBuffers failed");
        return;
    }
    rtv_.Reset();
    IDXGIResource* back = nullptr;
    if (SUCCEEDED(swap_->GetBuffer(0, __uuidof(IDXGIResource),
                                    reinterpret_cast<void**>(&back)))) {
        ID3D11Texture2D* tex = nullptr;
        back->QueryInterface(__uuidof(ID3D11Texture2D),
                             reinterpret_cast<void**>(&tex));
        back->Release();
        if (tex) {
            device_->CreateRenderTargetView(tex, nullptr,
                                            rtv_.ReleaseAndGetAddressOf());
            tex->Release();
        }
    }
    // New back buffer: invalidate SRV cache.
    srv_.Reset();
    srvTexture_.Reset();
}

void Renderer::Present(const video::DecodedFrame* frame) {
    const float clear[4] = {0.06f, 0.06f, 0.08f, 1.0f};
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    ctx_->ClearRenderTargetView(rtv_.Get(), clear);

    if (frame && frame->texture && frame->width > 0 && frame->height > 0) {
        // Letterbox rect in window pixels.
        const double scale =
            std::min(double(clientW_) / frame->width,
                     double(clientH_) / frame->height);
        const double dw = frame->width * scale;
        const double dh = frame->height * scale;
        PerFrame pf{};
        pf.WinRect[0] = float((clientW_ - dw) / 2.0);
        pf.WinRect[1] = float((clientH_ - dh) / 2.0);
        pf.WinRect[2] = float(dw);
        pf.WinRect[3] = float(dh);

        // Crop rect in normalized texture coordinates.
        const double w = frame->width, h = frame->height;
        const double cl = std::clamp(frame->cropLeft, 0, int(w));
        const double ct = std::clamp(frame->cropTop, 0, int(h));
        const double cr = std::clamp(frame->cropRight, 0, int(w));
        const double cb = std::clamp(frame->cropBottom, 0, int(h));
        pf.TexRect[0] = float(cl / w);
        pf.TexRect[1] = float(ct / h);
        pf.TexRect[2] = float(std::max(1.0, w - cl - cr) / w);
        pf.TexRect[3] = float(std::max(1.0, h - ct - cb) / h);
        pf.WindowSize[0] = float(clientW_);
        pf.WindowSize[1] = float(clientH_);

        ctx_->UpdateSubresource(constants_.Get(), 0, nullptr, &pf, 0, 0);

        if (srvTexture_.Get() != frame->texture.Get()) {
            srv_.Reset();
            if (FAILED(device_->CreateShaderResourceView(frame->texture.Get(),
                                                         nullptr,
                                                         srv_.ReleaseAndGetAddressOf()))) {
                srvTexture_.Reset();
            } else {
                srvTexture_.Attach(frame->texture.Get());
            }
        }

        if (srv_) {
            ID3D11Buffer* cb = constants_.Get();
            ctx_->VSSetShader(vs_.Get(), nullptr, 0);
            ctx_->VSSetConstantBuffers(0, 1, &cb);
            ctx_->PSSetShader(ps_.Get(), nullptr, 0);
            ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
            ctx_->PSSetShaderResources(0, 1, srv_.GetAddressOf());
            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            UINT stride = 0, offset = 0;
            ctx_->IASetVertexBuffers(0, 0, nullptr, &stride, &offset);
            ctx_->Draw(3, 0);
        }
    }

    swap_->Present(1, 0);
}

} // namespace od::render

#endif  // _WIN32
