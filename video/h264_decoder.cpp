#include "h264_decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32

#include "../net/log.h"
#include "../protocol/sps.h"

namespace od::video {

// ---------------------------------------------------------------------------
// ByteStreamSource: IMFByteStream over a grow-only ring fed by the decode
// thread. The H.264 parser reads from it on its own thread; EOS unblocks a
// parser that is waiting for data (used for pipeline rebuilds and shutdown).
// ---------------------------------------------------------------------------

class ByteStreamSource : public IMFByteStream {
public:
    std::deque<uint8_t> data;
    std::mutex mutex;
    std::condition_variable cv;
    bool eos = false;
    // Cumulative bytes ever written; GetLength reports this so the parser
    // sees the live stream as growing (Read consumes `data`, so size() alone
    // would shrink and look like an ending stream).
    std::atomic<long long> totalBytes_{0};

    bool Write(const uint8_t* p, size_t n) {
        {
            std::lock_guard lock(mutex);
            if (eos) return false;
            data.insert(data.end(), p, p + n);
            totalBytes_.fetch_add(static_cast<long long>(n),
                                  std::memory_order_relaxed);
        }
        cv.notify_all();
        return true;
    }

    void SetEos() {
        {
            std::lock_guard lock(mutex);
            eos = true;
        }
        cv.notify_all();
    }

    // ---- IUnknown ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFByteStream)) {
            *ppv = static_cast<IMFByteStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return ++refcount_;
    }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refcount_;
        if (n == 0) delete this;
        return n;
    }

    // ---- IMFByteStream ----
    STDMETHODIMP GetLength(LONGLONG* length) override {
        if (!length) return E_POINTER;
        *length = static_cast<LONGLONG>(
            totalBytes_.load(std::memory_order_relaxed));
        return S_OK;
    }
    STDMETHODIMP SetCurrentPosition(const PROPVARIANT* newValue) override {
        (void)newValue;
        return S_OK; // position is always "now"
    }
    STDMETHODIMP GetCurrentPosition(PROPVARIANT* currentValue) override {
        if (!currentValue) return E_POINTER;
        PropVariantInit(currentValue);
        currentValue->vt = VT_I8;
        currentValue->llVal = 0;
        return S_OK;
    }
    STDMETHODIMP IsEndOfStream(BOOL* value) override {
        if (!value) return E_POINTER;
        std::lock_guard lock(mutex);
        *value = eos ? TRUE : FALSE;
        return S_OK;
    }
    STDMETHODIMP Read(BYTE* pBuffer, ULONG cBuffer, ULONG* pbRead) override {
        if (!pBuffer || !pbRead) return E_POINTER;
        *pbRead = 0;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] { return eos || !data.empty(); });
            if (data.empty()) {
                // EOS with nothing left: signal end of stream so the parser
                // finalizes (returning S_OK/0 here can make it busy-poll).
                return MF_E_END_OF_STREAM;
            }
            const size_t n = std::min<size_t>(cBuffer, data.size());
            for (size_t i = 0; i < n; ++i) {
                pBuffer[i] = data.front();
                data.pop_front();
            }
            *pbRead = static_cast<ULONG>(n);
        }
        return S_OK;
    }
    STDMETHODIMP SetLength(LONGLONG) override { return E_NOTIMPL; }
    STDMETHODIMP Seek(const MFGUID*, const PROPVARIANT*, const PROPVARIANT*) override {
        return MF_E_INVALIDREQUEST; // live stream: no seeking
    }
    STDMETHODIMP IsCurrentPositionSupported(MFStreamStatus* pStreamStatus) override {
        if (!pStreamStatus) return E_POINTER;
        *pStreamStatus = eos ? MF_STREAM_STATUS_ENDED : MF_STREAM_STATUS_READING;
        return S_OK;
    }
    STDMETHODIMP GetStreamStatus(MFStreamStatus* pStreamStatus) override {
        return IsCurrentPositionSupported(pStreamStatus);
    }

private:
    std::atomic<ULONG> refcount_{1};
};

// ---------------------------------------------------------------------------

H264Decoder::H264Decoder(Config cfg) : cfg_(std::move(cfg)) {
    stream_ = new ByteStreamSource();
}

H264Decoder::~H264Decoder() { Shutdown(); }

bool H264Decoder::Init() { return true; }

void H264Decoder::Shutdown() {
    std::lock_guard lock(rebuildMutex_);
    StopReader();
    source_.Reset();
    reader_.Reset();
    currentOutputType_.Reset();
    lastSps_.clear();
    lastPps_.clear();
    stream_.Reset();
}

void H264Decoder::StopReader() {
    generation_++;
    running_ = false;
    pipelineReady_ = false;
    // Unblock a parser parked inside the current stream's Read().
    if (stream_) stream_->SetEos();
    if (readerThread_.joinable()) {
        readerThread_.join();
    }
    readerThread_ = std::thread();
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

bool H264Decoder::CreatePipeline(const net::VideoSample* firstKeyframe) {
    HRESULT hr;

    // A fresh byte source per pipeline generation. Reusing one across
    // generations risks the finalizing old parser and the new parser reading
    // the same stream concurrently.
    stream_ = new ByteStreamSource();
    if (firstKeyframe && !firstKeyframe->annexb.empty()) {
        stream_->Write(firstKeyframe->annexb.data(), firstKeyframe->annexb.size());
    }

    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    hr = MFCreateAttributes(attrs.ReleaseAndGetAddressOf(), 4);
    if (FAILED(hr)) {
        LOG_ERROR("decoder: MFCreateAttributes failed: 0x%lX", hr);
        return false;
    }

    // Decode to the D3D11 device (hardware decode) with low latency.
    attrs->SetUINT32(MF_DECODE_TO_DISPLAY, TRUE);
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (cfg_.dxgiManager) {
        attrs->SetUnknown(MF_SOURCE_READER_D3DManager, cfg_.dxgiManager.Get());
    }

    Microsoft::WRL::ComPtr<ByteStreamSource> streamCom(stream_.get());
    hr = MFCreateMediaSourceFromByteStream(streamCom.Get(), attrs.Get(),
                                           source_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("decoder: MFCreateMediaSourceFromByteStream failed: 0x%lX",
                  hr);
        source_.Reset();
        return false;
    }

    hr = MFCreateMediaSourceReader(source_.Get(), attrs.Get(),
                                   reader_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("decoder: MFCreateMediaSourceReader failed: 0x%lX", hr);
        reader_.Reset();
        source_.Reset();
        return false;
    }

    // Ask for NV12 output (what the hardware decoder produces). If the
    // request is rejected, accept the decoder's native output.
    Microsoft::WRL::ComPtr<IMFMediaType> nv12;
    if (SUCCEEDED(MFCreateMediaType(nv12.ReleaseAndGetAddressOf()))) {
        nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (FAILED(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                nullptr, nv12.Get()))) {
            LOG_DEBUG("decoder: NV12 output type rejected, using native");
        }
    }
    reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                 currentOutputType_.ReleaseAndGetAddressOf());

    generation_++;
    running_ = true;
    readerThread_ = std::thread([this] { ReaderLoop(); });
    pipelineReady_ = true;
    return true;
}

void H264Decoder::Submit(net::VideoSample&& s) {
    samplesSubmitted_++;
    if (s.annexb.empty()) return;

    // 1) Stream not yet up (or failed): only keyframes can (re)start it.
    if (!pipelineReady_) {
        if (s.isKeyframe && !s.sps.empty()) {
            std::lock_guard lock(rebuildMutex_);
            if (!pipelineReady_ && CreatePipeline(&s)) {
                lastSps_ = s.sps;
                lastPps_ = s.pps;
                rebuilds_++;
                LOG_INFO("decoder: pipeline up (sps %zu bytes)", s.sps.size());
                return;
            }
        }
        RequestKeyframe();
        return;
    }

    // 2) SPS/PPS changed: rebuild on this (key)frame.
    const bool spsChanged = !s.sps.empty() && s.sps != lastSps_;
    const bool ppsChanged = !s.pps.empty() && s.pps != lastPps_;
    if ((spsChanged || ppsChanged) && s.isKeyframe) {
        std::lock_guard lock(rebuildMutex_);
        StopReader();
        source_.Reset();
        reader_.Reset();
        currentOutputType_.Reset();
        if (CreatePipeline(&s)) {
            lastSps_ = s.sps;
            lastPps_ = s.pps;
            rebuilds_++;
            auto dims = od::ParseSpsDimensions(s.sps);
            if (dims && cfg_.onStreamSize) {
                LOG_INFO("decoder: stream size changed to %dx%d (coded)",
                         dims->width, dims->height);
                cfg_.onStreamSize(dims->width, dims->height);
            }
            return;
        }
        RequestKeyframe();
        return;
    }

    // 3) Steady state: feed the byte stream.
    if (!stream_->Write(s.annexb.data(), s.annexb.size())) {
        // EOS was set underneath us (rebuild race): rebuild on this frame
        // if it is a keyframe, otherwise wait.
        std::lock_guard lock(rebuildMutex_);
        if (s.isKeyframe && !s.sps.empty() && CreatePipeline(&s)) {
            lastSps_ = s.sps;
            lastPps_ = s.pps;
            rebuilds_++;
            return;
        }
        RequestKeyframe();
    }
}

void H264Decoder::ReaderLoop() {
    for (;;) {
        if (!running_) return;

        IMFSample* sample = nullptr;
        DWORD streamIndex = 0;
        DWORD flags = 0;
        const HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               0, &streamIndex, &flags,
                                               nullptr, &sample);
        if (!running_) {
            if (sample) sample->Release();
            return;
        }

        if (FAILED(hr)) {
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                // Expected when we set EOS during a rebuild.
                return;
            }
            decoderErrors_++;
            pipelineReady_ = false;
            LOG_ERROR("decoder: ReadSample failed: 0x%lX (flags=0x%X), "
                      "waiting for keyframe",
                      static_cast<unsigned long>(hr), flags);
            RequestKeyframe();
            return; // thread exits; next keyframe rebuilds
        }

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) sample->Release();
            return;
        }
        if (!sample) continue;

        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            sample->Release();
            continue;
        }

        PublishSample(sample);
        sample->Release();
        framesDecoded_++;
    }
}

void H264Decoder::PublishSample(IMFSample* sample) {
    auto frame = std::make_shared<DecodedFrame>();

    // Dimensions + crop from the current output type.
    if (currentOutputType_) {
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(currentOutputType_.Get(),
                                         MF_MT_FRAME_SIZE, &w, &h))) {
            frame->width = static_cast<int>(w);
            frame->height = static_cast<int>(h);
        }
        BYTE crop[16] = {0};
        DWORD sz = 0;
        if (SUCCEEDED(currentOutputType_->GetItem(
                MF_MT_DEFAULT_CROP, MF_ATTRIBUTE_VALUE_TYPE_INT32, crop,
                sizeof(crop), &sz)) &&
            sz >= 16) {
            const int* c = reinterpret_cast<const int*>(crop);
            frame->cropLeft = c[0];
            frame->cropTop = c[1];
            frame->cropRight = c[2];
            frame->cropBottom = c[3];
        }
    }

    IMFMediaBuffer* buf = nullptr;
    if (FAILED(sample->GetBufferByIndex(0, &buf)) || !buf) return;

    bool done = false;

    // Hardware path: the sample wraps a DXGI surface on our device.
    Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuf;
    if (SUCCEEDED(buf->QueryInterface(__uuidof(IMFDXGIBuffer),
                                      reinterpret_cast<void**>(dxgiBuf.ReleaseAndGetAddressOf())))) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(dxgiBuf->GetResource(0, reinterpret_cast<void**>(&tex))) &&
            tex) {
            D3D11_TEXTURE2D_DESC desc{};
            tex->QueryDesc(&desc);
            frame->texture.Attach(tex);
            frame->format = desc.Format;
            if (!frame->width) frame->width = static_cast<int>(desc.Width);
            if (!frame->height) frame->height = static_cast<int>(desc.Height);
            done = true;
        }
    }

    // Software fallback: read the buffer into memory and upload.
    if (!done) {
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) {
            buf->Release();
            return;
        }
        GUID sub = GUID_EMPTY;
        if (currentOutputType_)
            currentOutputType_->GetGUID(MF_MT_SUBTYPE, &sub);

        BYTE* scan = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (SUCCEEDED(buf->Lock(&scan, &maxLen, &curLen))) {
            Microsoft::WRL::ComPtr<ID3D11Device> dev;
            LONGLONG token = 0;
            if (cfg_.dxgiManager &&
                SUCCEEDED(cfg_.dxgiManager->GetDevice(__uuidof(ID3D11Device),
                                                      reinterpret_cast<void**>(dev.ReleaseAndGetAddressOf()),
                                                      &token)) &&
                dev) {
                if (sub == MFVideoFormat_NV12) {
                    const UINT w = frame->width, h = frame->height;
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = w;
                    td.Height = h;
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_NV12;
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    td.MiscFlags = 0;
                    ID3D11Texture2D* tex = nullptr;
                    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex))) {
                        Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
                        dev->GetImmediateContext(ctx.GetAddressOf());
                        D3D11_MAPPED_SUBRESOURCE yMap, uvMap;
                        if (SUCCEEDED(ctx->Map(tex, 0, D3D11_MAP_WRITE, 0, &yMap)) &&
                            SUCCEEDED(ctx->Map(tex, 1, D3D11_MAP_WRITE, 0, &uvMap))) {
                            const size_t ySize = static_cast<size_t>(w) * h;
                            const size_t uvSize = static_cast<size_t>(w) * (h / 2);
                            std::memcpy(yMap.pData, scan, ySize);
                            std::memcpy(uvMap.pData, scan + ySize, uvSize);
                            ctx->Unmap(tex, 0);
                            ctx->Unmap(tex, 1);
                            frame->texture.Attach(tex);
                            frame->format = DXGI_FORMAT_NV12;
                            done = true;
                        }
                    }
                } else if (sub == MFVideoFormat_RGB32) {
                    const UINT w = frame->width, h = frame->height;
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = w;
                    td.Height = h;
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    td.MiscFlags = 0;
                    ID3D11Texture2D* tex = nullptr;
                    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex))) {
                        Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
                        dev->GetImmediateContext(ctx.GetAddressOf());
                        D3D11_MAPPED_SUBRESOURCE map;
                        if (SUCCEEDED(ctx->Map(tex, 0, D3D11_MAP_WRITE, 0, &map))) {
                            // Row-by-row copy: the source buffer may pad its
                            // rows, so use its real pitch (maxLen / h) rather
                            // than the tightly-packed w*4.
                            const size_t srcPitch =
                                (h > 0) ? maxLen / h : 0;
                            const size_t rowBytes = static_cast<size_t>(w) * 4;
                            if (srcPitch >= rowBytes) {
                                for (UINT row = 0; row < h; ++row) {
                                    std::memcpy(
                                        static_cast<uint8_t*>(map.pData) +
                                            row * map.RowPitch,
                                        scan + row * srcPitch, rowBytes);
                                }
                            }
                            ctx->Unmap(tex, 0);
                            frame->texture.Attach(tex);
                            frame->format = DXGI_FORMAT_B8G8R8A8_UNORM;
                            done = true;
                        }
                    }
                } else {
                    LOG_WARN("decoder: unsupported CPU output format %08lX",
                             static_cast<unsigned long>(sub.Data1));
                }
            }
            buf->Unlock();
        }
    }

    buf->Release();
    if (done && cfg_.onFrame) cfg_.onFrame(frame);
}

} // namespace od::video

#endif  // _WIN32
