// Real-time H.264 decode via Media Foundation.
//
// Pipeline: the decode thread feeds each Annex-B access unit straight into the
// Media Foundation H.264 decoder MFT with ProcessInput, then drains decoded
// NV12 frames with ProcessOutput. Each decoded frame is uploaded to a D3D11
// texture and published via onFrame.
//
// Decoder choice: the HARDWARE H.264 decoder MFT (CLSID_CMSH264DecoderMFT,
// GPU decode) is tried first -- it is lower-latency and keeps the CPU free
// for rendering, which matters for a live screen mirror. The software MFT
// (CLSID_MSH264DecoderMFT) is the fallback when the hardware one is
// unavailable or refuses NV12 output.
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
#include <deque>
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
#include <dxgi1_2.h>
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
    // Decoded NV12 pixels in system memory (Y plane w*h, then UV plane w*h/2).
    // Filled on the decode thread (CPU only); the render thread uploads it to a
    // D3D11 texture. NV12 is planar, so the UV plane is interleaved U0 V0 U1 V1.
    std::vector<uint8_t> nv12;
    int width = 0;
    int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // MF_MT_DEFAULT_CROP (0 when absent).
    int cropLeft = 0, cropTop = 0, cropRight = 0, cropBottom = 0;
    // Sender-side capture time (wall-clock ms, SENDER's clock) from the
    // stream header; -1 when absent. Used for end-to-end latency stats.
    int64_t captureMs = -1;
    // Receiver-side socket arrival time (steady ms, OUR clock); 0 when
    // absent. Skew-free arrival->present latency stats.
    int64_t arrivalMs = 0;
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
    uint64_t AccessUnitsAccepted() const { return acceptedUnits_.load(); }
    uint64_t FramesDecoded() const { return framesDecoded_.load(); }
    uint64_t FramesPublished() const { return publishedFrames_.load(); }
    uint64_t Rebuilds() const { return rebuilds_.load(); }
    uint64_t DecoderErrors() const { return decoderErrors_.load(); }

private:
#ifdef _WIN32
    bool SetupMft();
    bool SetOutputNv12();
    bool FeedAccessUnit(const std::vector<uint8_t>& annexb, int64_t captureMs,
                        int64_t arrivalMs);
    bool DrainOutput();
    void PublishNv12(IMFSample* outSample);
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
    bool hwDecoder_ = false;  // true when the hardware MFT is in use
    // Sender capture times in input order (no B-frames in the stream, so
    // input order == output order); decoded frames pop the front.
    // (captureMs, arrivalMs) pairs, FIFO: pushed per accepted input AU, popped
    // per published frame. Pairs each decoded frame with its wire timestamps.
    std::deque<std::pair<int64_t, int64_t>> captureTimes_;
#endif

    std::atomic<uint64_t> samplesSubmitted_{0};
    std::atomic<uint64_t> framesDecoded_{0};
    std::atomic<uint64_t> rebuilds_{0};
    std::atomic<uint64_t> decoderErrors_{0};
    std::atomic<uint64_t> fedCount_{0};
    std::atomic<uint64_t> acceptedUnits_{0};
    std::atomic<uint64_t> publishedFrames_{0};
};

} // namespace od::video
