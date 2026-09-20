#include "h264_decoder.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include "../net/log.h"

namespace od::video {

H264Decoder::H264Decoder(Config cfg) : cfg_(std::move(cfg)) {}

H264Decoder::~H264Decoder() { Shutdown(); }

bool H264Decoder::Init() { return true; }

void H264Decoder::Shutdown() { ResetMft(); decoder_.Reset(); }

void H264Decoder::ResetMft() {
    decoder_.Reset();
    mftReady_ = false;
    sampleSize_ = 0;
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
    decoder_.Reset();

    IMFTransform* mft = nullptr;
    const HRESULT cr = ::CoCreateInstance(CLSID_MSH264DecoderMFT, nullptr,
                                          CLSCTX_INPROC_SERVER,
                                          __uuidof(IMFTransform),
                                          reinterpret_cast<void**>(&mft));
    if (FAILED(cr) || !mft) {
        LOG_ERROR("decoder: CoCreateInstance(CLSID_MSH264DecoderMFT) failed: "
                  "0x%lX",
                  static_cast<unsigned long>(cr));
        return false;
    }
    decoder_.Attach(mft);

    // Input type is raw H.264. The MFT offers a placeholder output type until
    // it has seen enough input to know the real format.
    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    if (FAILED(MFCreateMediaType(inputType.ReleaseAndGetAddressOf())))
        return false;
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(decoder_->SetInputType(0, inputType.Get(), 0))) {
        LOG_ERROR("decoder: SetInputType(H264) failed");
        return false;
    }
    if (!SetOutputNv12()) {
        LOG_ERROR("decoder: SetOutputNv12 (initial) failed");
        return false;
    }

    sampleSize_ = 0;
    mftReady_ = true;
    return true;
}

// Upload a decoded system-memory NV12 buffer to a fresh D3D11 texture.
bool H264Decoder::UploadNv12ToTexture(IMFMediaBuffer* buf,
                                      std::shared_ptr<DecodedFrame> frame) {
    const int w = frame->width, h = frame->height;
    const size_t ySize = (size_t)w * (size_t)h;
    const size_t uvSize = (size_t)w * (size_t)(h / 2);

    BYTE* scanline = nullptr;
    DWORD maxLen = 0, curLen = 0;
    if (FAILED(buf->Lock(&scanline, &maxLen, &curLen))) return false;
    const bool bigEnough = curLen >= (DWORD)(ySize + uvSize);

    bool ok = false;
    if (bigEnough) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = (UINT)w;
        desc.Height = (UINT)h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(cfg_.device->CreateTexture2D(&desc, nullptr, &tex))) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(cfg_.deviceCtx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                              &mapped))) {
                BYTE* dst = static_cast<BYTE*>(mapped.pData);
                for (int r = 0; r < h; ++r)
                    memcpy(dst + (size_t)r * mapped.RowPitch,
                           scanline + (size_t)r * w, (size_t)w);
                BYTE* uvSrc = scanline + ySize;
                BYTE* uvDst = dst + (size_t)mapped.RowPitch * h;
                for (int r = 0; r < h / 2; ++r)
                    memcpy(uvDst + (size_t)r * mapped.RowPitch,
                           uvSrc + (size_t)r * w, (size_t)w);
                cfg_.deviceCtx->Unmap(tex, 0);
                frame->texture.Attach(tex);
                frame->format = DXGI_FORMAT_NV12;
                ok = true;
            } else if (tex) {
                tex->Release();
            }
        }
    }
    buf->Unlock();
    return ok;
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

    if (cfg_.device && cfg_.deviceCtx && frame->width > 0 && frame->height > 0) {
        if (UploadNv12ToTexture(buf, frame) && cfg_.onFrame)
            cfg_.onFrame(frame);
    }
    buf->Release();
}

bool H264Decoder::FeedAccessUnit(const std::vector<uint8_t>& annexb) {
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
        const HRESULT hr = decoder_->ProcessInput(0, inSample.Get(), 0);
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
            LOG_ERROR("decoder: ProcessInput failed: 0x%lX",
                      static_cast<unsigned long>(hr));
            return false;
        }
    }

    // 2) Drain outputs until the decoder needs more input.
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
            SetOutputNv12();
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

    if (!FeedAccessUnit(s.annexb)) {
        decoderErrors_++;
        ResetMft();
        RequestKeyframe();
    }
}

} // namespace od::video

#endif  // _WIN32
