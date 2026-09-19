// Probe #2: D3D headers FIRST, then MF. Tests whether include order fixes
// QueryDesc / GetDevice, and re-confirms the two MF symbols.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <cstdio>

int main() {
    ID3D11Texture2D* tex = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    if (tex) tex->QueryDesc(&d);  // TEST 1: QueryDesc with D3D included first
    (void)d;

    MF_ATTRIBUTE_VALUE_TYPE t = MF_ATTRIBUTE_VALUE_TYPE_INT32;  // TEST 2
    (void)t;

    GUID g = MF_MT_DEFAULT_CROP;  // TEST 3
    (void)g;

    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> mgr;
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    if (mgr)
        mgr->GetDevice(__uuidof(ID3D11Device),
                       reinterpret_cast<void**>(dev.ReleaseAndGetAddressOf()),
                       0);  // TEST 4
    (void)dev;
    return 0;
}
