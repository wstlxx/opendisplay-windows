// Real-time H.264 decode via Media Foundation.
//
// Pipeline (per PROTOCOL.md / research):
//   decode thread --(mutex)--> ByteStreamSource (IMFByteStream)
//     --> MFCreateMediaSourceFromByteStream
//     --> media source reader (sync ReadSample on a dedicated reader thread)
//     --> output IMFSample (DXVA texture on the shared D3D11 device)
//     --> onFrame callback (publishes DecodedFrame to the render thread)
//
// Rebuild policy (the two traps from the research notes):
//   * SPS/PPS byte-change (screen resolution switched on the Mac) -> tear
//     down source+reader, create a fresh pair, feed the keyframe that
//     carried the new SPS.
//   * Decoder error (corrupt frame, dropped stream) -> wait for a keyframe
//     (requesting "kf" in the meantime, rate-limited) and rebuild.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../net/session.h"
#include "../win32_compat.h"

#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfobjects.h>
#include <mferror.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
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

// The live Annex-B byte source. Fed by the decode thread, consumed by the
// MF H.264 parser on its own thread.
class ByteStreamSource;

class H264Decoder {
public:
    struct Config {
#ifdef _WIN32
        Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiManager;
#endif
        // Called on the reader thread once per decoded frame.
        std::function<void(std::shared_ptr<DecodedFrame>)> onFrame;
        // Called on the decode thread when a new SPS arrives with new
        // dimensions (coded size before cropping).
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

    // Diagnostics.
    uint64_t SamplesSubmitted() const { return samplesSubmitted_; }
    uint64_t FramesDecoded() const { return framesDecoded_; }
    uint64_t Rebuilds() const { return rebuilds_; }
    uint64_t DecoderErrors() const { return decoderErrors_; }

private:
#ifdef _WIN32
    void ReaderLoop();
    void PublishSample(IMFSample* sample);
    bool CreatePipeline(const net::VideoSample* firstKeyframe);
    void StopReader();
    void RequestKeyframe();
#endif

    Config cfg_;

    // ---- pipeline objects (owned during a generation) ----
#ifdef _WIN32
    Microsoft::WRL::ComPtr<ByteStreamSource> stream_;
    Microsoft::WRL::ComPtr<IMFMediaSource> source_;
    Microsoft::WRL::ComPtr<IMFMediaSourceReader> reader_;
    Microsoft::WRL::ComPtr<IMFMediaType> currentOutputType_;
    std::thread readerThread_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> pipelineReady_{false};
    std::mutex rebuildMutex_;  // serializes Submit-driven rebuilds
    std::vector<uint8_t> lastSps_;
    std::vector<uint8_t> lastPps_;
    int64_t lastKfRequestMs = 0;
#endif

    // Diagnostic counters are touched from more than one thread (Submit on the
    // decode thread, ReaderLoop on the MF reader thread, stats elsewhere), so
    // keep them lock-free atomic.
    std::atomic<uint64_t> samplesSubmitted_{0};
    std::atomic<uint64_t> framesDecoded_{0};
    std::atomic<uint64_t> rebuilds_{0};
    std::atomic<uint64_t> decoderErrors_{0};
};

} // namespace od::video
