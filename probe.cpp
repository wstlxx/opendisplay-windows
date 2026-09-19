// Temporary compile probe: which MF/D3D symbols actually exist on the runner?
// Compiled with cl on windows-2022; each #ifdef block is guarded so a missing
// symbol produces a single, clear error we can read from the CI log.
#include <mfapi.h>
#include <mfobjects.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>

int main() {
#ifdef PROBE_CROP
    GUID g = MF_MT_DEFAULT_CROP;
    (void)g;
    std::printf("MF_MT_DEFAULT_CROP: OK\n");
#endif
#ifdef PROBE_AVT
    MF_ATTRIBUTE_VALUE_TYPE t = MF_ATTRIBUTE_VALUE_TYPE_INT32;
    (void)t;
    std::printf("MF_ATTRIBUTE_VALUE_TYPE_INT32: OK\n");
#endif
#ifdef PROBE_QD
    ID3D11Texture2D* tex = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    if (tex) tex->QueryDesc(&d);
    (void)d;
    std::printf("ID3D11Texture2D::QueryDesc: OK\n");
#endif
#ifdef PROBE_GD
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> mgr;
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    if (mgr) {
        mgr->GetDevice(__uuidof(ID3D11Device),
                       reinterpret_cast<void**>(dev.ReleaseAndGetAddressOf()),
                       0);
    }
    (void)dev;
    std::printf("IMFDXGIDeviceManager::GetDevice: OK\n");
#endif
    return 0;
}
