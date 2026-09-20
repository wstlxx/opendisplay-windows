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
    bool shutdown_ = false;
    // Cumulative bytes ever written; GetLength reports this so the parser
    // sees the live stream as growing (Read consumes `data`, so size() alone
    // would shrink and look like an ending stream).
    std::atomic<long long> totalBytes_{0};

    bool Append(const uint8_t* p, size_t n) {
        {
            std::lock_guard lock(mutex);
            if (eos || shutdown_) return false;
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

    // Unblock a thread parked in Read() so the reader can be torn down.
    void SignalShutdown() {
        {
            std::lock_guard lock(mutex);
            shutdown_ = true;
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
    STDMETHODIMP GetCapabilities(DWORD* pdwCapabilities) override {
        if (!pdwCapabilities) return E_POINTER;
        *pdwCapabilities = 0; // no seeking, no async
        return S_OK;
    }
    STDMETHODIMP GetLength(QWORD* length) override {
        if (!length) return E_POINTER;
        *length = static_cast<QWORD>(
            totalBytes_.load(std::memory_order_relaxed));
        return S_OK;
    }
    STDMETHODIMP GetCurrentPosition(QWORD* pqwPosition) override {
        if (!pqwPosition) return E_POINTER;
        *pqwPosition = 0;
        return S_OK;
    }
    STDMETHODIMP SetCurrentPosition(QWORD) override {
        return S_OK; // position is always "now"
    }
    STDMETHODIMP IsEndOfStream(BOOL* value) override {
        if (!value) return E_POINTER;
        std::lock_guard lock(mutex);
        *value = (eos && data.empty()) ? TRUE : FALSE;
        return S_OK;
    }
    STDMETHODIMP Read(BYTE* pBuffer, ULONG cBuffer, ULONG* pbRead) override {
        if (!pBuffer || !pbRead) return E_POINTER;
        *pbRead = 0;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock,
                    [this] { return shutdown_ || eos || !data.empty(); });
            if (shutdown_) return MF_E_SHUTDOWN;
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
    STDMETHODIMP SetLength(QWORD) override { return E_NOTIMPL; }
    STDMETHODIMP BeginRead(BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP EndRead(IMFAsyncResult*, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP Write(const BYTE*, ULONG, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN, LONGLONG, DWORD, QWORD*) override {
        return E_NOTIMPL; // live stream: no seeking
    }
    STDMETHODIMP Flush() override { return S_OK; }
    STDMETHODIMP Close() override {
        SignalShutdown();
        return S_OK;
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
    if (stream_) stream_->SignalShutdown();
    if (readerThread_.joinable()) {
        readerThread_.join();
    }
    readerThread_ = std::thread();
    // The read thread has exited; release the source reader.
    reader_.Reset();
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
    const bool haveData = firstKeyframe && !firstKeyframe->annexb.empty();

    if (haveData && !memProbed_) {
        memProbed_ = true;
        // Diagnostic: can MF build a source reader from an IN-MEMORY copy of
        // this keyframe? Same bytes, but a standard (seekable) stream instead
        // of our custom live byte stream -> cleanly separates "H.264
        // source/codec missing" from "our live byte stream is the bug".
        Microsoft::WRL::ComPtr<IMFByteStream> mem;
        if (SUCCEEDED(MFCreateMemoryStream(
                firstKeyframe->annexb.data(),
                (DWORD)firstKeyframe->annexb.size(),
                mem.ReleaseAndGetAddressOf()))) {
            Microsoft::WRL::ComPtr<IMFAttributes> memAttrs;
            if (SUCCEEDED(MFCreateAttributes(memAttrs.ReleaseAndGetAddressOf(), 1))) {
                Microsoft::WRL::ComPtr<IMFSourceReader> memReader;
                const HRESULT mhr = MFCreateSourceReaderFromByteStream(
                    mem.Get(), memAttrs.Get(),
                    memReader.ReleaseAndGetAddressOf());
                LOG_INFO("decoder: in-memory keyframe probe: %s (0x%lX)",
                         SUCCEEDED(mhr)
                             ? "OK -> codec present, live byte stream is the bug"
                             : "FAILED -> H.264 source/codec missing (or raw "
                               "Annex-B unsupported by the source reader)",
                         static_cast<unsigned long>(mhr));
            }
        } else {
            LOG_WARN("decoder: MFCreateMemoryStream failed (diagnostic skipped)");
        }
    }

    // Two attempts: (0) hardware decode (D3D manager -> texture output),
    // (1) software decode (system memory). Each uses a FRESH byte stream
    // (the first attempt's stream may have been consumed during probing).
    // Log each attempt's HRESULT so we can tell, from the log alone, whether
    // the failure is the D3D path or the codec/byte stream in general.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool useD3D = (attempt == 0);
        if (!useD3D && !cfg_.device) break; // can't upload software frames

        Microsoft::WRL::ComPtr<ByteStreamSource> fresh = new ByteStreamSource();
        if (haveData)
            fresh->Append(firstKeyframe->annexb.data(), firstKeyframe->annexb.size());

        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(attrs.ReleaseAndGetAddressOf(), 4)))
            return false;
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        if (useD3D && cfg_.dxgiManager)
            attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, cfg_.dxgiManager.Get());

        Microsoft::WRL::ComPtr<IMFSourceReader> sr;
        const HRESULT hr = MFCreateSourceReaderFromByteStream(
            fresh.Get(), attrs.Get(), sr.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            LOG_WARN("decoder: source reader (%s) failed: 0x%lX",
                     useD3D ? "D3D/hardware" : "software",
                     static_cast<unsigned long>(hr));
            continue;
        }
        LOG_INFO("decoder: source reader up (%s path)",
                 useD3D ? "D3D/hardware" : "software");

        // Ask for NV12 output; if rejected, accept the decoder's native output.
        Microsoft::WRL::ComPtr<IMFMediaType> nv12;
        if (SUCCEEDED(MFCreateMediaType(nv12.ReleaseAndGetAddressOf()))) {
            nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            if (FAILED(sr->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get()))) {
                LOG_DEBUG("decoder: NV12 output type rejected, using native");
            }
        }
        sr->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                currentOutputType_.ReleaseAndGetAddressOf());

        stream_ = fresh;
        reader_ = sr;
        generation_++;
        running_ = true;
        readerThread_ = std::thread([this] { ReaderLoop(); });
        pipelineReady_ = true;
        return true;
    }
    return false;
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
    if (!stream_->Append(s.annexb.data(), s.annexb.size())) {
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
        // (MF_MT_DEFAULT_CROP / MF_ATTRIBUTE_VALUE_TYPE are not present in the
        //  SDK on the CI runner, so no cropping is applied.)
    }

    IMFMediaBuffer* buf = nullptr;
    if (FAILED(sample->GetBufferByIndex(0, &buf)) || !buf) return;

    bool done = false;

    // Hardware path: the sample wraps a DXGI surface on our device.
    Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuf;
    if (SUCCEEDED(buf->QueryInterface(__uuidof(IMFDXGIBuffer),
                                      reinterpret_cast<void**>(dxgiBuf.ReleaseAndGetAddressOf())))) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(dxgiBuf->GetResource(__uuidof(ID3D11Texture2D),
                                          reinterpret_cast<void**>(&tex))) &&
            tex) {
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);  // this SDK exposes GetDesc, not QueryDesc
            frame->texture.Attach(tex);
            frame->format = desc.Format;
            if (!frame->width) frame->width = static_cast<int>(desc.Width);
            if (!frame->height) frame->height = static_cast<int>(desc.Height);
            done = true;
        }
    }

    // Software-decode path: no DXGI surface, so the buffer holds NV12 in
    // system memory. Upload it to a D3D11 texture on our device.
    if (!done && cfg_.device && cfg_.deviceCtx &&
        frame->width > 0 && frame->height > 0) {
        GUID subtype = GUID_NULL;
        if (currentOutputType_)
            currentOutputType_->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12)
            done = UploadNV12ToTexture(buf, frame);
        else
            LOG_WARN("decoder: software output is not NV12 (0x%08lX), skipping",
                     subtype.Data1);
    }

    buf->Release();
    if (done && cfg_.onFrame) cfg_.onFrame(frame);
}

bool H264Decoder::UploadNV12ToTexture(IMFMediaBuffer* buf,
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

} // namespace od::video

#endif  // _WIN32
