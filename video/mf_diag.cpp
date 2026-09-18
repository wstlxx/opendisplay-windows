// CI diagnostic (Windows only). Compiles a standalone translation unit that
// references the Media Foundation + D3D11 symbols we depend on. This isolates
// "the runner's SDK is broken" from "our include context is wrong": if this
// file fails to compile with the same 'undeclared' errors, the problem is the
// SDK/environment; if it compiles, the problem is how h264_decoder pulls them
// in. It is NOT linked into the app.
#include <windows.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <dxgi1_2.h>

int main() {
    // mfobjects.h
    IMFByteStream* bs = nullptr;
    IMFMediaType* mt = nullptr;
    // mfidl.h
    IMFMediaSource* ms = nullptr;
    IMFDXGIBuffer* dxgi = nullptr;
    IMFDXGIDeviceManager* dm = nullptr;
    // mfreadwrite.h
    IMFMediaSourceReader* sr = nullptr;
    MFCreateMediaSourceReader(nullptr, nullptr, &sr);
    MFCreateMediaSourceFromByteStream(nullptr, nullptr, &sr);
    // mfapi.h attribute GUIDs / enums
    (void)MF_DECODE_TO_DISPLAY;
    (void)MF_SOURCE_READER_D3DManager;
    (void)MF_ATTRIBUTE_VALUE_TYPE_INT32;
    (void)MF_MT_DEFAULT_CROP;
    (void)MFCreateAttributes;
    MFStreamStatus s = MF_STREAM_STATUS_NONE;
    MF_SEEK_ORIGIN o = MF_SEEK_ABSOLUTE;
    // d3d11.h
    ID3D11Texture2D* tex = nullptr;
    (void)bs; (void)mt; (void)ms; (void)dxgi; (void)dm;
    (void)s; (void)o; (void)tex;
    return 0;
}
