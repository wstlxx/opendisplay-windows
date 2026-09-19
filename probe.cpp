// Probe #3: exercise the D3D11 device/context/resource APIs the renderer uses,
// to predict whether od_render compiles on this runner's SDK.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>

static void test(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* t = nullptr;
    dev->CreateTexture2D(&td, nullptr, &t);

    D3D11_TEXTURE2D_DESC dd{};
    if (t) t->GetDesc(&dd);                       // SDK's GetDesc (void)
    ID3D11ShaderResourceView* srv = nullptr;
    if (t) dev->CreateShaderResourceView(t, nullptr, &srv);

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT; bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = (void*)""; init.SysMemPitch = 16;
    ID3D11Buffer* vb = nullptr;
    dev->CreateBuffer(&bd, &init, &vb);

    D3D11_MAPPED_SUBRESOURCE mr{};
    ctx->Map(vb, 0, D3D11_MAP_WRITE, 0, &mr);
    ctx->Unmap(vb, 0);
    ctx->UpdateSubresource(t, 0, nullptr, (void*)"", 1, 1);

    D3D11_VIEWPORT vp{};
    ctx->RSSetViewports(1, &vp);
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, (const FLOAT[]){});

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    UINT stride = 16, off = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    ID3D11Buffer* cb = nullptr;
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->Draw(3, 0);

    ID3D11BlendState* bs = nullptr;
    D3D11_BLEND_DESC bld{};
    dev->CreateBlendState(&bld, &bs);
    ID3D11RasterizerState* rs = nullptr;
    D3D11_RASTERIZER_DESC rd{};
    dev->CreateRasterizerState(&rd, &rs);
    ID3D11SamplerState* ss = nullptr;
    D3D11_SAMPLER_DESC sd{};
    dev->CreateSamplerState(&sd, &ss);

    const void* sh = nullptr;
    size_t sl = 0;
    ID3D11VertexShader* vs2 = nullptr;
    dev->CreateVertexShader(sh, sl, nullptr, &vs2);
    ID3D11PixelShader* ps2 = nullptr;
    dev->CreatePixelShader(sh, sl, nullptr, &ps2);
    const D3D11_INPUT_ELEMENT_DESC* ie = nullptr;
    ID3D11InputLayout* il = nullptr;
    dev->CreateInputLayout(ie, 0, sh, sl, &il);

    ID3D11DeviceContext* c2 = nullptr;
    dev->GetImmediateContext(&c2);
}

int main() { return 0; }
