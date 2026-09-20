// Real-time H.264 decode via Media Foundation.
//
// Pipeline: the decode thread feeds each Annex-B access unit straight into the
// Media Foundation H.264 decoder MFT (CLSID_MSH264DecoderMFT) with
// ProcessInput, then drains decoded NV12 frames with ProcessOutput. Each
// decoded frame is uploaded to a D3D11 texture and published via onFrame.
//
// Why a decoder MFT and not a source reader over a byte stream: on Windows,
// MFCreateSourceReaderFromByteStream returns MF_E_UNSUPPORTED_MEDIA_TYPE for
// raw (non-container) Annex-B H.264. Feeding the decoder MFT directly is the
// approach the reference implementation uses and it works for a live stream.
//
// Rebuild policy:
//   * SPS/PPS byte-change (screen resolution switched on the Mac) -> tear
//     down the MFT, create a fresh one, feed the keyframe that carried the
//     new SPS.
//   * Decoder error (corrupt frame) -> reset the MFT, request a keyframe
//     (rate-limited) and wait for one.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "../net/session.h"
#include "../win32_compat.h"

#ifdef _WIN32
// Include order matters: mfobjects.h (IMFMediaType) must precede mfidl.h /
// mftransform.h (IMFTransform, MFT_OUTPUT_DATA_BUFFER).
#include <mfapi.h>
#include <mfobjects.h>
#include <mfidl.h>
#include <mferror.h>

// mftransform.h provides MFT_OUTPUT_DATA_BUFFER + MF_E_TRANSFORM_* constants.
// The reduced Windows SDK on CI may lack it, so guard and fall back.
#if defined(__has_include)
#  if __has_include(<mftransform.h>)
#    include <mftransform.h>
#    define OD_HAVE_MFTRANSFORM_H 1
#  else
#    define OD_HAVE_MFTRANSFORM_H 0
#  endif
#else
#  include <mftransform.h>
#  define OD_HAVE_MFTRANSFORM_H 1
#endif

#include <d3d11.h>
#include <wrl/client.h>

// Reduced-SDK fallback: provide the MFT pieces we use, but only if no header
// already defined them (mfidl.h may carry them in some SDK builds).
#ifndef MF_E_TRANSFORM_STREAM_CHANGE
#define MF_E_TRANSFORM_STREAM_CHANGE 0xC00D36B5
#define MF_E_TRANSFORM_NEED_MORE_INPUT 0xC00D36B6
#ifndef MF_E_NOTACCEPTING
#define MF_E_NOTACCEPTING 0xC00D36B7
#endif
struct IMMediaEvent;
typedef struct MFT_OUTPUT_DATA_BUFFER {
    IMFSample* pSample;
    DWORD dwStatus;
    IMMediaEvent* pEvent;
} MFT_OUTPUT_DATA_BUFFER;
#endif
#endif

namespace od::video {

struct DecodedFrame {
#ifdef _WIN32
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
#endif
    int width = 0;
    int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // MF_MT_DEFAULT_CROP (0 when absent).
    int cropLeft = 0, cropTop = 0, cropRight = 0, cropBottom = 0;
};

class H264Decoder {
public:
    struct Config {
#ifdef _WIN32
        // Non-owning: the app owns these; the decoder uses them to upload
        // decoded (system-memory) NV12 frames to a D3D11 texture.
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> deviceCtx;
#endif
        // Called on the decode thread once per decoded frame.
        std::function<void(std::shared_ptr<DecodedFrame>)> onFrame;
        // Called on the decode thread when the stream's coded size changes.
        std::function<void(int width, int height)> onStreamSize;
        // Called on the decode thread; the app should send a "kf" message.
        std::function<void()> onRequestKeyframe;
    };

    explicit H264Decoder(Config cfg);
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    bool Init();
    // Called on the decode thread only.
    void Submit(net::VideoSample&& sample);
    void Shutdown();

    // Diagnostics (touched from more than one thread; lock-free).
    uint64_t SamplesSubmitted() const { return samplesSubmitted_.load(); }
    uint64_t FramesDecoded() const { return framesDecoded_.load(); }
    uint64_t Rebuilds() const { return rebuilds_.load(); }
    uint64_t DecoderErrors() const { return decoderErrors_.load(); }

private:
#ifdef _WIN32
    bool SetupMft();
    bool SetOutputNv12();
    bool FeedAccessUnit(const std::vector<uint8_t>& annexb);
    void PublishNv12(IMFSample* outSample);
    bool UploadNv12ToTexture(IMFMediaBuffer* buf,
                             std::shared_ptr<DecodedFrame> frame);
    void ResetMft();
    void RequestKeyframe();
#endif

    Config cfg_;
#ifdef _WIN32
    Microsoft::WRL::ComPtr<IMFTransform> decoder_;
    bool mftReady_ = false;
    DWORD sampleSize_ = 0;   // NV12 output size, 0 until the first stream change
    UINT32 lastW_ = 0, lastH_ = 0;
    std::vector<uint8_t> lastSps_;
    std::vector<uint8_t> lastPps_;
    int64_t lastKfRequestMs = 0;
#endif

    std::atomic<uint64_t> samplesSubmitted_{0};
    std::atomic<uint64_t> framesDecoded_{0};
    std::atomic<uint64_t> rebuilds_{0};
    std::atomic<uint64_t> decoderErrors_{0};
};

} // namespace od::video
