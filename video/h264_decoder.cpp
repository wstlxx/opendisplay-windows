#include "h264_decoder.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "../protocol/sps.h"

#ifdef _WIN32
#include "../net/log.h"

namespace od::video {

// "Microsoft Hardware H.264 Decoder MFT" (msmh264dec.dll). Absent from the
// reduced SDK on CI, so define it when the SDK has not already.
#ifndef CLSID_CMSH264DecoderMFT
static const CLSID kClSIDHardwareH264 = {
    0x516630D3, 0x7C2B, 0x4C2F,
    {0x8A, 0x53, 0xFA, 0x06, 0xA1, 0x17, 0x17, 0xF0}};
#else
static const CLSID kClSIDHardwareH264 = CLSID_CMSH264DecoderMFT;
#endif

// MF_LOW_LATENCY / CODECAPI_AVLowLatencyMode. Define the documented GUID
// locally because the CI runner's reduced MF headers omit some GUID symbols.
static const GUID kMfLowLatency = {
    0x9c27891a, 0xed7a, 0x40e1,
    {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

H264Decoder::H264Decoder(Config cfg) : cfg_(std::move(cfg)) {}

H264Decoder::~H264Decoder() { Shutdown(); }

bool H264Decoder::Init() { return true; }

void H264Decoder::Shutdown() { ResetMft(); decoder_.Reset(); }

void H264Decoder::ResetMft() {
    decoder_.Reset();
    mftReady_ = false;
    sampleSize_ = 0;
    visibleW_ = visibleH_ = 0;
    captureTimes_.clear();
}

void H264Decoder::RequestKeyframe() {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (now - lastKfRequestMs < 500) return; // rate limit
    lastKfRequestMs = now;
    LOG_WARN("decoder: requesting keyframe from sender");
    if (cfg_.onRequestKeyframe) cfg_.onRequestKeyframe();
}

// Find the MFT's NV12 output type and select it. The H.264 decoder offers
// several pixel formats (NV12, I420, YUY2, YV12); NV12 maps 1:1 onto a D3D11
// texture, so we request it specifically.
bool H264Decoder::SetOutputNv12() {
    UINT32 index = 0;
    for (;;) {
        Microsoft::WRL::ComPtr<IMFMediaType> outputType;
        const HRESULT hr = decoder_->GetOutputAvailableType(
            0, index++, outputType.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;
        GUID guid = GUID_NULL;
        if (SUCCEEDED(outputType->GetGUID(MF_MT_SUBTYPE, &guid)) &&
            guid == MFVideoFormat_NV12) {
            return SUCCEEDED(decoder_->SetOutputType(0, outputType.Get(), 0));
        }
    }
}

bool H264Decoder::SetupMft() {
    // Hardware first (GPU decode: lower latency, CPU free for rendering),
    // software as the fallback. Input type is raw H.264; the MFT offers a
    // placeholder output type until it has seen enough input to know the real
    // format.
    static const CLSID* kCandidates[] = {&kClSIDHardwareH264,
                                         &CLSID_MSH264DecoderMFT};
    HRESULT lastCr = E_NOINTERFACE;
    for (const CLSID* clsid : kCandidates) {
        decoder_.Reset();
        IMFTransform* mft = nullptr;
        const HRESULT cr = ::CoCreateInstance(*clsid, nullptr,
                                              CLSCTX_INPROC_SERVER,
                                              __uuidof(IMFTransform),
                                              reinterpret_cast<void**>(&mft));
        const bool isHw = (clsid == &kClSIDHardwareH264);
        if (FAILED(cr) || !mft) {
            lastCr = cr;
            if (isHw)
                LOG_WARN("decoder: hardware H.264 MFT unavailable "
                         "(CoCreateInstance hr=0x%lX)",
                         static_cast<unsigned long>(cr));
            continue;
        }
        decoder_.Attach(mft);

        // The direct-MFT path does not inherit the source reader's low
        // latency setting. Configure it before negotiating media types so
        // the decoder does not keep reorder/display buffers unnecessarily.
        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        HRESULT lowHr = decoder_->GetAttributes(attrs.ReleaseAndGetAddressOf());
        if (SUCCEEDED(lowHr))
            lowHr = attrs ? attrs->SetUINT32(kMfLowLatency, TRUE) : E_POINTER;
        if (SUCCEEDED(lowHr)) {
            LOG_INFO("decoder: %s MFT low-latency mode enabled",
                     isHw ? "hardware" : "software");
        } else {
            LOG_WARN("decoder: %s MFT low-latency mode unavailable "
                     "(hr=0x%lX)", isHw ? "hardware" : "software",
                     static_cast<unsigned long>(lowHr));
        }

        Microsoft::WRL::ComPtr<IMFMediaType> inputType;
        if (FAILED(MFCreateMediaType(inputType.ReleaseAndGetAddressOf())))
            continue;
        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (FAILED(decoder_->SetInputType(0, inputType.Get(), 0))) {
            if (isHw)
                LOG_WARN("decoder: hardware H.264 MFT rejected H264 input");
            continue;
        }
        if (!SetOutputNv12()) {
            if (isHw)
                LOG_WARN("decoder: hardware H.264 MFT offered no NV12 "
                         "output type");
            continue;
        }

        sampleSize_ = 0;
        mftReady_ = true;
        hwDecoder_ = (clsid == &kClSIDHardwareH264);
        LOG_INFO("decoder: using %s H.264 decoder MFT",
                 hwDecoder_ ? "HARDWARE" : "software (fallback)");
        return true;
    }
    LOG_ERROR("decoder: no usable H.264 decoder MFT (last hr=0x%lX)",
              static_cast<unsigned long>(lastCr));
    return false;
}

void H264Decoder::PublishNv12(IMFSample* outSample) {
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(outSample->GetBufferByIndex(0, &buf)) || !buf) return;
    auto frame = std::make_shared<DecodedFrame>();

    Microsoft::WRL::ComPtr<IMFMediaType> type;
    if (SUCCEEDED(decoder_->GetOutputCurrentType(
            0, type.ReleaseAndGetAddressOf()))) {
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
            frame->width = static_cast<int>(w);
            frame->height = static_cast<int>(h);
        }
    }

    if (!captureTimes_.empty()) {
        frame->captureMs = captureTimes_.front().first;
        frame->arrivalMs = captureTimes_.front().second;
        captureTimes_.pop_front();
        if (captureTimes_.size() > 64) {
            // The MFT emitted fewer frames than it accepted (e.g. it
            // swallowed corrupted P-frames) -> the FIFO has drifted and
            // captureMs no longer matches the emitted frame. Reset the
            // e2e baseline rather than report a fake 20-second latency.
            LOG_WARN("decoder: capture-time drift (emitted < accepted "
                     "frames) -- resetting e2e baseline");
            captureTimes_.clear();
        }
    }
    if (frame->width > 0 && frame->height > 0) {
        // H.264 pads the coded raster to macroblock boundaries. For example,
        // a 1412px desktop can be decoded as 1424px. Crop that padding before
        // the renderer computes its letterbox rectangle and input mapping.
        if (visibleW_ > 0 && visibleW_ <= frame->width)
            frame->cropRight = frame->width - visibleW_;
        if (visibleH_ > 0 && visibleH_ <= frame->height)
            frame->cropBottom = frame->height - visibleH_;
        // Copy the decoded NV12 into the frame (CPU only). The D3D11 upload is
        // done on the render thread: a D3D11 immediate context is single-
        // threaded, so the decode thread must not call Map/Unmap on it.
        const size_t ySize = (size_t)frame->width * (size_t)frame->height;
        const size_t total = ySize + ySize / 2;  // Y (w*h) + UV (w*h/2)
        BYTE* scanline = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (SUCCEEDED(buf->Lock(&scanline, &maxLen, &curLen))) {
            if (curLen >= total) {
                frame->nv12.assign(scanline, scanline + total);
                frame->format = DXGI_FORMAT_NV12;
                uint64_t n = ++publishedFrames_;
                if (n <= 3 || (n % 250) == 0)
                    LOG_INFO("decoder: published frame #%llu %dx%d", n,
                             (unsigned long long)frame->width,
                             (unsigned long long)frame->height);
                if (cfg_.onFrame) cfg_.onFrame(frame);
            }
            buf->Unlock();
        }
    } else {
        LOG_WARN("decoder: no frame size on output type (w=%d h=%d)",
                 frame->width, frame->height);
    }
    buf->Release();
}

bool H264Decoder::FeedAccessUnit(const std::vector<uint8_t>& annexb,
                                 int64_t captureMs, int64_t arrivalMs) {
    // 1) Feed the whole access unit as a single input sample.
    {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> inBuf;
        if (FAILED(MFCreateMemoryBuffer((DWORD)annexb.size(),
                                        inBuf.ReleaseAndGetAddressOf())))
            return false;
        BYTE* chunk = nullptr;
        if (FAILED(inBuf->Lock(&chunk, nullptr, nullptr))) return false;
        memcpy(chunk, annexb.data(), annexb.size());
        inBuf->SetCurrentLength((DWORD)annexb.size());
        inBuf->Unlock();

        Microsoft::WRL::ComPtr<IMFSample> inSample;
        if (FAILED(MFCreateSample(inSample.ReleaseAndGetAddressOf())))
            return false;
        inSample->AddBuffer(inBuf.Get());
        HRESULT hr = decoder_->ProcessInput(0, inSample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            // The MFT still has output. Dropping this compressed AU would
            // break the reference chain and can leave the displayed picture
            // several actions behind until another IDR arrives.
            if (!DrainOutput()) return false;
            hr = decoder_->ProcessInput(0, inSample.Get(), 0);
        }
        uint64_t fc = ++fedCount_;
        if (FAILED(hr)) {
            LOG_ERROR("decoder: ProcessInput failed: 0x%lX",
                      static_cast<unsigned long>(hr));
            return false;
        }
        // Push for EVERY accepted AU (even captureMs<0) so the FIFO
        // stays aligned with the published frames.
        acceptedUnits_++;
        captureTimes_.push_back({captureMs, arrivalMs});
        if (fc <= 3 || (fc % 250) == 0)
            LOG_INFO("decoder: fed AU #%llu (accepted hr=0x%lX)", fc,
                     static_cast<unsigned long>(hr));
    }

    // 2) Drain outputs until the decoder needs more input.
    return DrainOutput();
}

bool H264Decoder::DrainOutput() {
    for (;;) {
        Microsoft::WRL::ComPtr<IMFSample> outSample;
        if (FAILED(MFCreateSample(outSample.ReleaseAndGetAddressOf()))) break;
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.pSample = outSample.Get();
        if (sampleSize_ > 0) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuf;
            if (FAILED(MFCreateMemoryBuffer(sampleSize_,
                                            outBuf.ReleaseAndGetAddressOf())))
                break;
            outSample->AddBuffer(outBuf.Get());
        }
        DWORD status = 0;
        const HRESULT hr = decoder_->ProcessOutput(0, 1, &ob, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!SetOutputNv12()) {
                LOG_ERROR("decoder: could not select NV12 after stream change");
                return false;
            }
            Microsoft::WRL::ComPtr<IMFMediaType> type;
            if (SUCCEEDED(decoder_->GetOutputCurrentType(
                    0, type.ReleaseAndGetAddressOf()))) {
                UINT32 ss = 0;
                if (SUCCEEDED(type->GetUINT32(MF_MT_SAMPLE_SIZE, &ss)))
                    sampleSize_ = ss;
                UINT32 w = 0, h = 0;
                if (SUCCEEDED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                                                 &w, &h))) {
                    if (w != lastW_ || h != lastH_) {
                        lastW_ = w;
                        lastH_ = h;
                        LOG_INFO("decoder: stream size %ux%u", w, h);
                        if (cfg_.onStreamSize) cfg_.onStreamSize(w, h);
                    }
                }
            }
            continue;
        }
        if (FAILED(hr)) {
            LOG_ERROR("decoder: ProcessOutput failed: 0x%lX",
                      static_cast<unsigned long>(hr));
            return false;
        }
        // S_OK: a decoded NV12 frame is in outSample.
        PublishNv12(outSample.Get());
        framesDecoded_++;
    }
    return true;
}

void H264Decoder::Submit(net::VideoSample&& s) {
    samplesSubmitted_++;
    if (s.annexb.empty()) return;

    // Build the MFT on a keyframe when it isn't ready yet, or reset it when
    // SPS/PPS changed (a resolution switch on the Mac).
    if (!mftReady_) {
        if (s.isKeyframe && SetupMft()) {
            lastSps_ = s.sps;
            lastPps_ = s.pps;
            rebuilds_++;
            LOG_INFO("decoder: H.264 MFT up (sps %zu bytes)", s.sps.size());
        } else {
            RequestKeyframe();
            return;
        }
    } else {
        const bool spsChanged = !s.sps.empty() && s.sps != lastSps_;
        const bool ppsChanged = !s.pps.empty() && s.pps != lastPps_;
        if ((spsChanged || ppsChanged) && s.isKeyframe) {
            ResetMft();
            if (SetupMft()) {
                lastSps_ = s.sps;
                lastPps_ = s.pps;
                rebuilds_++;
                LOG_INFO("decoder: H.264 MFT rebuilt (SPS/PPS change)");
            } else {
                RequestKeyframe();
                return;
            }
        }
    }

    if (s.isKeyframe && !s.sps.empty()) {
        const auto visible = od::ParseSpsDimensions(s.sps);
        if (visible) {
            visibleW_ = visible->width;
            visibleH_ = visible->height;
        }
    }

    if (!FeedAccessUnit(s.annexb, s.captureMs, s.arrivalMs)) {
        decoderErrors_++;
        ResetMft();
        RequestKeyframe();
    }
}

} // namespace od::video

#endif  // _WIN32
