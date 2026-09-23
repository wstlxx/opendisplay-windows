// OpenDisplay protocol 3 (pv3) native receiver for Windows 10/11.
//
// Wiring (see docs/RESEARCH.md for the architecture):
//
//   net read thread (per session)
//     -> VideoQueue (<=2, drop-oldest)
//   decode thread
//     -> H264Decoder (Media Foundation H.264 transform)
//     -> onFrame: publish latest DecodedFrame (CPU NV12)
//   main thread (Win32 message loop)
//     -> Renderer::Present(latest)
//
// Control path: the session read thread handles sender messages (pong,
// welcome, streamConfig, ...) and the decode thread can ask for keyframes.

#include "../win32_compat.h"  // must precede windows.h / MF headers
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "../net/log.h"
#include "../net/mdns_advertise.h"
#include "../net/session.h"
#include "../net/tcp_listener.h"
#include "../protocol/control.h"
#include "../render/renderer.h"
#include "../video/h264_decoder.h"
#include "video_queue.h"

using namespace od;
using namespace od::app;

namespace {

int64_t UnixNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Mouse message coordinates are signed 16-bit values packed into the low /
// high halves of lParam (what GET_X_LPARAM/GET_Y_LPARAM in windowsx.h do; we
// avoid pulling that header in). Sign-extend so points left/above the client
// origin come back negative.
inline int MouseX(LPARAM lp) { return (short)LOWORD(lp); }
inline int MouseY(LPARAM lp) { return (short)HIWORD(lp); }

// Defined below; needed here so the id can be persisted next to the exe.
std::string ExeDir();

// Stable per-install identity (PROTOCOL.md 2.1): the Bonjour TXT `id` MUST
// equal the hello `id`, so a sender recognizes "same device, new name/port".
// A random 32-hex value is generated on first run and persisted to disk.
std::string MakeStableHelloId() {
    const std::string path = ExeDir() + "opendisplay_id";
    std::string existing;
    {
        std::ifstream in(path);
        if (in) std::getline(in, existing);
        if (existing.size() == 32) return existing;
    }
    // Versions before config.ini omitted the separator in ExeDir(). Preserve
    // their per-install identity when moving the file beside the exe.
    const std::string dir = ExeDir();
    if (!dir.empty()) {
        std::ifstream old(dir.substr(0, dir.size() - 1) + "opendisplay_id");
        if (old) std::getline(old, existing);
        if (existing.size() == 32) {
            std::ofstream out(path, std::ios::trunc);
            if (out) out << existing << "\n";
            return existing;
        }
    }
    static std::mt19937_64 rng(
        std::random_device{}() ^ static_cast<uint64_t>(
                                     reinterpret_cast<uintptr_t>(&rng)));
    const char* hex = "0123456789abcdef";
    char buf[33];
    for (int i = 0; i < 32; ++i) {
        const uint64_t r = rng() & 0xF;
        buf[i] = hex[r];
    }
    buf[32] = 0;
    std::string id(buf);
    std::ofstream out(path, std::ios::trunc);
    if (out) out << id << "\n";
    return id;
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

struct App {
    // Window
    HWND hwnd = nullptr;
    double uiScale = 1.0;

    // D3D / rendering
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> deviceCtx;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiManager;
    render::Renderer renderer;

    // Decode
    std::unique_ptr<video::H264Decoder> decoder;
    VideoQueue queue;
    std::thread decodeThread;
    std::atomic<bool> decodeRunning{false};

    // Latest decoded frame (published by the MF reader thread)
    std::mutex frameMutex;
    std::shared_ptr<video::DecodedFrame> latestFrame;
    std::shared_ptr<video::DecodedFrame> lastPresentedFrame;
    HANDLE frameEvent = nullptr;
    bool redrawNeeded = true;  // window size changed or first draw
    std::atomic<uint64_t> receivedVideo_{0};
    std::atomic<int64_t> lastVideoArrivalMs_{0};
    // The current Mac sender stamps `cap` when submitting to VideoToolbox,
    // so this measures encode submission to send, not ScreenCaptureKit age.
    std::atomic<int64_t> lastSenderEncodeMs_{-1};

    // Network
    net::TcpListener listener;
    std::shared_ptr<net::Session> session;  // guarded by sessionMutex
    std::mutex sessionMutex;
    // The session whose read loop just ended. onClosed (running on the read
    // thread) records it here; the main loop reaps it so that ~Session (which
    // joins the read thread) never runs on the read thread itself.
    net::Session* endedSession = nullptr;   // guarded by sessionMutex
    std::string helloId = MakeStableHelloId();
    net::MdnsAdvertiser mdns;      // Bonjour/mDNS advertisement
    std::string mdnsName;          // --name flag; empty => computer name
    bool vsync = false;            // --vsync flag; default off (low latency)
    ReceiverConfig config;
    int64_t resizeAtMs = 0;        // main thread, debounced adaptive hello
    std::atomic<int> lastHelloWidth{0};
    std::atomic<int> lastHelloHeight{0};

    // Stats
    uint64_t lastDecodedForFps = 0;
    uint64_t lastBytesAtStats_ = 0;
    int64_t lastStatsAtMs = 0;
    int lastRttMs = -1;
    int lastE2eMs = -1;   // clock-based e2e (may include Mac/PC clock skew)
    int lastR2pMs = -1;   // recv->present, skew-free (our clock only)
    int64_t lastFramePresentedMs_ = 0;
    int64_t lastPipelineLogMs_ = 0;
    uint64_t lastReceivedLogged_ = 0;
    uint64_t lastSubmittedLogged_ = 0;
    uint64_t lastAcceptedLogged_ = 0;
    uint64_t lastPublishedLogged_ = 0;
    bool skewWarned_ = false;
    long presentN_ = 0;   // frames presented since start (for rate-limited logs)

    // Fullscreen
    bool fullscreen = false;

    // Mouse -> touch input (PROTOCOL.md 6.1): the Mac sender maps touch
    // began/moved/ended onto real left-button mouse down/drag/up, and scroll
    // onto the scroll wheel. (The protocol has no keyboard messages and the
    // Mac app does not inject keys, so keyboard is not forwarded.)
    bool mouseDown = false;
    double lastTouchX = 0.0, lastTouchY = 0.0;
    int64_t lastTouchSentMs_ = 0;  // touch-moved throttle (~120 Hz max)

    // Stream desync recovery: a lost frame means the P-frames after it
    // decode as garbage (ghosting, wrong colors) until an IDR arrives.
    int64_t lastKfSendMs_ = 0;      // rate limit for kf requests
    // Run control
    bool running = false;

    bool Setup(int port);
    void Run();
    void TearDown();

    void OnAccept(SOCKET sock, const std::string& peer);
    void ReapEndedSession();
    void HandleControl(net::Session* s, const ControlMessage& msg);
    void SendStats(net::Session* s);
    void HelloSize(int* width, int* height) const;
    void MaybeSendResizeHello();
    void EnterFullscreen();

    // WndProc trampoline (whnd is the window being messaged; it is valid even
    // during CreateWindowEx, when the App::hwnd member is still null)
    LRESULT WndProcThunk(HWND whnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProcHandle(HWND whnd, UINT msg, WPARAM wp, LPARAM lp);

    // Window client pixel -> normalized video coords (0..1, top-left origin).
    bool WindowToVideo(int px, int py, double* nx, double* ny) const;
    void SendTouch(const char* phase, double x, double y);
    // Rate-limited (500 ms) keyframe request for stream desync recovery.
    void MaybeRequestKeyframe(const char* why, int64_t minIntervalMs);

    static LRESULT CALLBACK WndProcStatic(HWND h, UINT msg, WPARAM wp,
                                          LPARAM lp);
};

// ---------------------------------------------------------------------------

LRESULT CALLBACK App::WndProcStatic(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtr(h, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    App* app = reinterpret_cast<App*>(GetWindowLongPtr(h, GWLP_USERDATA));
    if (app) return app->WndProcThunk(h, msg, wp, lp);
    return DefWindowProc(h, msg, wp, lp);
}

LRESULT App::WndProcThunk(HWND whnd, UINT msg, WPARAM wp, LPARAM lp) {
    return WndProcHandle(whnd, msg, wp, lp);
}

LRESULT App::WndProcHandle(HWND whnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                const int w = LOWORD(lp), h = HIWORD(lp);
                if (w > 0 && h > 0) {
                    renderer.Resize(w, h);
                    redrawNeeded = true;
                    if (config.adaptiveResolution) resizeAtMs = SteadyNowMs();
                }
            }
            return 0;
        case WM_PAINT:
            redrawNeeded = true;
            return DefWindowProc(whnd, msg, wp, lp);
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (fullscreen && wp == 'Q' && !(lp & (1u << 30)) &&
                (GetKeyState(VK_CONTROL) & 0x8000) &&
                (GetKeyState(VK_SHIFT) & 0x8000) &&
                (GetKeyState(VK_MENU) & 0x8000)) {
                SendMessage(whnd, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        case WM_SYSCOMMAND:
            // Alt+F4 must not become another fullscreen exit shortcut.
            if (fullscreen && (wp & 0xFFF0) == SC_CLOSE) return 0;
            break;
        case WM_CLOSE:
            {
                std::lock_guard lock(sessionMutex);
                if (session) session->SendClosing();
                running = false;
            }
            if (fullscreen) {
                fullscreen = false;
                ShowCursor(TRUE);
            }
            DestroyWindow(whnd);
            return 0;
        case WM_GETMINMAXINFO: {
            // Never let the window exceed the work area.
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            HMONITOR mon = MonitorFromWindow(whnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfo(mon, &mi)) {
                RECT work = mi.rcWork;
                mmi->ptMaxTrackSize.x = work.right - work.left;
                mmi->ptMaxTrackSize.y = work.bottom - work.top;
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (mouseDown) return 0;
            mouseDown = true;
            SetCapture(whnd); // keep receiving mouse-up outside the window
            {
                double nx, ny;
                if (WindowToVideo(MouseX(lp), MouseY(lp), &nx, &ny)) {
                    lastTouchX = nx;
                    lastTouchY = ny;
                    SendTouch("began", nx, ny);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (mouseDown) {
                mouseDown = false;
                if (GetCapture() == whnd) ReleaseCapture();
                double nx, ny;
                if (WindowToVideo(MouseX(lp), MouseY(lp), &nx, &ny)) {
                    lastTouchX = nx;
                    lastTouchY = ny;
                }
                SendTouch("ended", lastTouchX, lastTouchY);
            }
            return 0;
        case WM_MOUSEMOVE: {
            double nx, ny;
            if (WindowToVideo(MouseX(lp), MouseY(lp), &nx, &ny)) {
                lastTouchX = nx;
                lastTouchY = ny;
                if (mouseDown) {
                    // Throttle to ~120 Hz. WM_MOUSEMOVE fires at hundreds of
                    // Hz; forwarding every one would flood the Mac's Window
                    // Server with synthetic mouse-moved CGEvents (far more
                    // than a real mouse produces) and can starve its screen
                    // capture -- observed as capFps collapsing to 0.
                    if (SteadyNowMs() - lastTouchSentMs_ >= 8) {
                        SendTouch("moved", nx, ny);
                    }
                }
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = (short)HIWORD(wp); // +/-120 per notch
            if (delta != 0) {
                // dx/dy in video pixels, natural-scrolling sign: wheel up
                // (positive delta) scrolls the content down => positive dy.
                std::lock_guard lock(sessionMutex);
                if (session) session->SendControl(od::BuildScroll(0.0, delta));
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            // Mouse-up happened outside the window: end the drag cleanly.
            if (mouseDown) {
                mouseDown = false;
                SendTouch("cancelled", lastTouchX, lastTouchY);
            }
            return 0;
    }
    return DefWindowProc(whnd, msg, wp, lp);
}

bool App::WindowToVideo(int px, int py, double* nx, double* ny) const {
    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (!renderer.VideoRect(&rx, &ry, &rw, &rh) || rw <= 0 || rh <= 0)
        return false;
    *nx = std::clamp((double)(px - rx) / rw, 0.0, 1.0);
    *ny = std::clamp((double)(py - ry) / rh, 0.0, 1.0);
    return true;
}

void App::SendTouch(const char* phase, double x, double y) {
    lastTouchSentMs_ = SteadyNowMs();
    std::lock_guard lock(sessionMutex);
    if (session) session->SendControl(od::BuildTouch(phase, x, y));
}

void App::MaybeRequestKeyframe(const char* why, int64_t minIntervalMs) {
    const int64_t now = SteadyNowMs();
    if (now - lastKfSendMs_ < minIntervalMs) return;  // rate limit
    lastKfSendMs_ = now;
    LOG_WARN("main: %s -> requesting keyframe (desync recovery)", why);
    std::lock_guard lock(sessionMutex);
    if (session) session->SendKeyframeRequest();
}

bool App::Setup(int port) {
    LOG_INFO("opendisplay receiver starting (hello id %s)", helloId.c_str());

    // --- D3D11 device (shared with MF via the DXGI device manager) ---
    // NOTE: the 7th parameter is D3D11_SDK_VERSION (==7), NOT a flags value.
    // An earlier version passed D3D11_CREATE_DEVICE_BGRA_SUPPORT (16) there,
    // which made the call malformed and D3D11CreateDevice returned E_INVALIDARG
    // (0x80070057) for both hardware and WARP. Flags (4th) is 0 and the feature
    // level list uses the default (nullptr, 0). Fallback: hardware -> WARP.
    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), &obtained,
        deviceCtx.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        LOG_WARN("hardware D3D11 device failed (0x%lX), trying WARP", hr);
        device.Reset();
        deviceCtx.Reset();
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), &obtained,
            deviceCtx.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDevice failed (hw+warp): 0x%lX", hr);
        return false;
    }
    LOG_INFO("D3D11 device up (feature level 0x%04X)", obtained);

    UINT resetToken = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&resetToken,
                                         dxgiManager.ReleaseAndGetAddressOf())) ||
        FAILED(dxgiManager->ResetDevice(device.Get(), resetToken))) {
        LOG_ERROR("DXGI device manager setup failed");
        return false;
    }

    // --- Window (configured client size; letterbox the stream inside) ---
    WNDCLASSEX wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WndProcStatic;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OpenDisplayReceiver";
    const ATOM atom = RegisterClassEx(&wc);
    if (!atom) {
        LOG_ERROR("RegisterClassEx failed: %lu", GetLastError());
        return false;
    }
    LOG_INFO("window class registered (atom=%lu)", (unsigned long)atom);

    const int initW = config.width, initH = config.height;
    RECT rc{0, 0, initW, initH};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    // Center on the primary monitor's work area (fall back to 0,0 if the
    // monitor query fails, e.g. no interactive desktop).
    int x = 0, y = 0;
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi)) {
        x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left -
                              (rc.right - rc.left)) / 2;
        y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top -
                             (rc.bottom - rc.top)) / 2;
    }

    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    LOG_INFO("creating window at (%d,%d) size %dx%d", x, y, ww, wh);

    hwnd = CreateWindowEx(0, L"OpenDisplayReceiver", L"OpenDisplay",
                          WS_OVERLAPPEDWINDOW, x, y, ww, wh, nullptr, nullptr,
                          GetModuleHandle(nullptr), this);
    if (!hwnd) {
        LOG_ERROR("CreateWindowEx failed: %lu", GetLastError());
        return false;
    }
    uiScale = static_cast<double>(GetDpiForWindow(hwnd)) / 96.0;
    ShowWindow(hwnd, SW_SHOW);
    LOG_INFO("window shown; calling renderer.Init");

    if (!renderer.Init(hwnd, device, initW, initH)) return false;
    renderer.SetVsync(vsync);
    LOG_INFO("renderer: vsync %s", vsync ? "on" : "off (lowest latency)");
    if (config.fullscreen) EnterFullscreen();

    frameEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); // auto-reset

    // --- Decoder ---
    video::H264Decoder::Config dcfg;
    dcfg.device = device;
    dcfg.deviceCtx = deviceCtx;
    dcfg.onFrame = [this](std::shared_ptr<video::DecodedFrame> f) {
        {
            std::lock_guard lock(frameMutex);
            latestFrame = std::move(f);
        }
        SetEvent(frameEvent);
    };
    dcfg.onStreamSize = [](int w, int h) {
        LOG_INFO("stream coded size: %dx%d (window letterboxes)", w, h);
    };
    dcfg.onRequestKeyframe = [this]() {
        std::lock_guard lock(sessionMutex);
        if (session) session->SendKeyframeRequest();
    };
    decoder = std::make_unique<video::H264Decoder>(dcfg);
    decoder->Init();

    // --- Decode thread ---
    decodeRunning = true;
    decodeThread = std::thread([this] {
        while (decodeRunning) {
            auto sample = queue.PopWait(200);
            if (!sample) continue;
            decoder->Submit(std::move(*sample));
        }
    });

    // --- Network ---
    if (!listener.Start(port, [this](SOCKET sock, const std::string& peer) {
            OnAccept(sock, peer);
        })) {
        TearDown();
        return false;
    }

    // --- Bonjour/mDNS advertisement (so a Mac sender can discover us) ---
    net::MdnsAdvertiser::Config mc;
    mc.instanceName = mdnsName;  // empty => computer name
    mc.id = helloId;             // MUST equal the hello id (PROTOCOL.md 2.1)
    mc.pv = 3;
    mc.port = static_cast<uint16_t>(port);
    if (!mdns.Start(mc))
        LOG_WARN("mdns: advertisement not started (discovery needs a direct dial)");

    running = true;
    return true;
}

void App::OnAccept(SOCKET sock, const std::string& peer) {
    // Replace any existing session (PROTOCOL.md: new connection wins).
    std::shared_ptr<net::Session> replaced;
    {
        std::lock_guard lock(sessionMutex);
        replaced = session;
    }
    if (replaced) {
        LOG_WARN("new connection from %s, dropping previous session",
                 peer.c_str());
        replaced->Close();  // outside the lock; its onClosed runs on its own thread
    }

    // The callbacks must NOT hold a strong reference to the session itself:
    // that would be a self-referential shared_ptr cycle and the session (and
    // its read thread) would never be destroyed. Capture a heap weak_ptr box
    // instead. The callbacks only ever run on this session's read thread, so
    // resolving the weak_ptr succeeds for their whole lifetime.
    auto self = std::make_shared<std::weak_ptr<net::Session>>();
    net::Session::Callbacks cbs;
    cbs.onControl = [this, self](const ControlMessage& m) {
        if (auto sp = self->lock()) HandleControl(sp.get(), m);
    };
    cbs.onVideo = [this](net::VideoSample&& v) {
        // Sparse captures are normal when the Mac is idle or overloaded.
        // A capture timestamp gap cannot establish H.264 reference loss.
        receivedVideo_.fetch_add(1, std::memory_order_relaxed);
        lastVideoArrivalMs_.store(v.arrivalMs, std::memory_order_relaxed);
        if (v.captureMs > 0 && v.sendMs >= v.captureMs)
            lastSenderEncodeMs_.store(v.sendMs - v.captureMs,
                                      std::memory_order_relaxed);
        if (queue.Push(std::move(v))) {
            // We dropped a frame -> the decoder's reference chain is broken;
            // without an IDR the picture stays corrupted (ghosting, wrong
            // colors). This only happens when the Mac is FAST enough to
            // overflow the queue (i.e. when an IDR is affordable), so it is
            // kept responsive.
            MaybeRequestKeyframe("queue drop", 500);
            // Log at most every 128 drops.
            if ((queue.Dropped() & 127) == 1) {
                LOG_WARN("video queue full, dropped %llu frames so far",
                         (unsigned long long)queue.Dropped());
            }
        }
    };
    cbs.onClosed = [this, self, peer](bool clean) {
        LOG_INFO("session %s closed (%s)", peer.c_str(),
                 clean ? "peer closed" : "error");
        // Do NOT reset the session here: onClosed runs on the read thread, and
        // ~Session joins the read thread (itself) -> deadlock/UB. Record the
        // ended session and let the main loop destroy it.
        std::lock_guard lock(sessionMutex);
        if (auto sp = self->lock()) {
            if (session == sp) endedSession = sp.get();
        }
    };
    auto s = std::make_shared<net::Session>(sock, peer, std::move(cbs));
    *self = s;  // wire up the weak ref (no ownership cycle)
    {
        std::lock_guard lock(sessionMutex);
        session = s;
    }
    s->Start();
    // hello MUST be the first message we send (PROTOCOL.md 6.1).
    int helloWidth = 0, helloHeight = 0;
    HelloSize(&helloWidth, &helloHeight);
    s->SendHello(helloWidth, helloHeight, uiScale, helloId,
                 config.bitrateKbps);
    lastHelloWidth.store(helloWidth);
    lastHelloHeight.store(helloHeight);
    lastStatsAtMs = SteadyNowMs();
}

void App::ReapEndedSession() {
    std::shared_ptr<net::Session> toDestroy;
    {
        std::lock_guard lock(sessionMutex);
        if (endedSession && session && session.get() == endedSession) {
            toDestroy = std::move(session);
            endedSession = nullptr;
        }
    }
    if (toDestroy) toDestroy.reset();  // ~Session joins read thread (main thread)
}

void App::HandleControl(net::Session* s, const ControlMessage& msg) {
    switch (msg.type) {
        case ControlType::Welcome:
            LOG_INFO("sender hello: pv=%d min=%d", msg.welcomePv,
                     msg.welcomeMin);
            if (msg.welcomeMin > 3) {
                LOG_ERROR("sender requires pv >= %d, we are pv 3",
                          msg.welcomeMin);
            }
            break;
        case ControlType::Ping: {
            // Sender-side liveness beat; log health counters at debug.
            LOG_DEBUG(
                "sender ping: drops=%d pending=%d capFps=%.1f",
                msg.drops, msg.pending, msg.capFps);
            // Low capFps alone is ambiguous: ScreenCaptureKit may produce
            // few frames for a static desktop. Use the pipeline counters
            // and the actual on-screen freshness to diagnose a stall.
            break;
        }
        case ControlType::Pong: {
            if (msg.hasT) {
                // t is the Unix-ms we stamped on the ping; the difference is
                // the RTT. Guard against clock jumps / stale pings.
                const int64_t rtt = UnixNowMs() - static_cast<int64_t>(msg.t);
                if (rtt >= 0 && rtt < 5000) {
                    lastRttMs = static_cast<int>(rtt);
                    LOG_DEBUG("pong: rtt=%d ms", lastRttMs);
                } else {
                    LOG_WARN("pong: implausible rtt %lld ms (clock skew?)",
                             static_cast<long long>(rtt));
                }
            }
            break;
        }
        case ControlType::StreamConfig:
            LOG_INFO("streamConfig: codec=%s %dx%d @ %d fps",
                     msg.codec.c_str(), msg.width, msg.height, msg.fps);
            break;
        case ControlType::UpdateRequired:
            LOG_WARN("sender says update required (target=%s): %s",
                     msg.target.c_str(), msg.message.c_str());
            break;
        case ControlType::Cursor:
        case ControlType::CursorImg:
            break; // parsed, not rendered in initial scope
        case ControlType::Unknown:
            LOG_DEBUG("unknown control type: %s", msg.typeString.c_str());
            break;
    }

    // Periodic stats (PROTOCOL.md: every ~5 s).
    if (SteadyNowMs() - lastStatsAtMs >= 5000) {
        SendStats(s);
    }
}

void App::SendStats(net::Session* s) {
    const int64_t now = SteadyNowMs();
    const uint64_t decoded = decoder->FramesDecoded();
    const int64_t dt = now - lastStatsAtMs;
    int fps = 0, mbps = 0;
    if (dt > 100) {
        fps = static_cast<int>((decoded - lastDecodedForFps) * 1000 / dt);
        const uint64_t bytes = s->BytesReceived();
        mbps = static_cast<int>((bytes - lastBytesAtStats_) * 8 / dt / 1000);
        lastBytesAtStats_ = bytes;
    }
    lastStatsAtMs = now;
    lastDecodedForFps = decoded;
    const int rtt = (lastRttMs >= 0 && lastRttMs < 5000) ? lastRttMs : -1;
    s->SendControl(od::BuildStats(fps, mbps, rtt, lastE2eMs, lastE2eMs));
}

void App::HelloSize(int* width, int* height) const {
    *width = config.width;
    *height = config.height;
    if (!config.adaptiveResolution || !hwnd) return;
    RECT client{};
    if (GetClientRect(hwnd, &client)) {
        const int w = (client.right - client.left) & ~1;
        const int h = (client.bottom - client.top) & ~1;
        if (w >= 320 && w <= 8192 && h >= 240 && h <= 8192) {
            *width = w;
            *height = h;
        }
    }
}

void App::MaybeSendResizeHello() {
    if (!config.adaptiveResolution || resizeAtMs == 0 ||
        SteadyNowMs() - resizeAtMs < 500) return;
    resizeAtMs = 0;
    int width = 0, height = 0;
    HelloSize(&width, &height);
    if (width == lastHelloWidth.load() && height == lastHelloHeight.load())
        return;
    std::shared_ptr<net::Session> active;
    {
        std::lock_guard lock(sessionMutex);
        active = session;
    }
    if (!active) return;
    active->SendHello(width, height, uiScale, helloId, config.bitrateKbps);
    lastHelloWidth.store(width);
    lastHelloHeight.store(height);
    LOG_INFO("adaptive hello: display size %dx%d", width, height);
}

void App::EnterFullscreen() {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) {
        LOG_WARN("fullscreen: GetMonitorInfo failed (%lu)", GetLastError());
        return;
    }
    const LONG oldStyle = GetWindowLong(hwnd, GWL_STYLE);
    SetWindowLong(hwnd, GWL_STYLE, WS_POPUP);
    if (!SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                      mi.rcMonitor.right - mi.rcMonitor.left,
                      mi.rcMonitor.bottom - mi.rcMonitor.top,
                      SWP_FRAMECHANGED)) {
        LOG_WARN("fullscreen: SetWindowPos failed (%lu)", GetLastError());
        SetWindowLong(hwnd, GWL_STYLE, oldStyle);
        return;
    }
    fullscreen = true;
    ShowCursor(FALSE);
}

void App::Run() {
    LOG_INFO("main loop running (fullscreen exit: Ctrl+Shift+Alt+Q)");
    MSG msg;
    while (running) {
        const HANDLE waitables[1] = {frameEvent};
        MsgWaitForMultipleObjectsEx(1, waitables, 100, QS_ALLINPUT,
                                   MWMO_ALERTABLE);
        BOOL pm;
        while ((pm = PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) != 0) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;
        ReapEndedSession();
        MaybeSendResizeHello();
        const int64_t now = SteadyNowMs();
        if (now - lastPipelineLogMs_ >= 5000) {
            const uint64_t received = receivedVideo_.load(std::memory_order_relaxed);
            const uint64_t submitted = decoder->SamplesSubmitted();
            const uint64_t accepted = decoder->AccessUnitsAccepted();
            const uint64_t published = decoder->FramesPublished();
            const int64_t arrival = lastVideoArrivalMs_.load(std::memory_order_relaxed);
            const int64_t senderEncode =
                lastSenderEncodeMs_.load(std::memory_order_relaxed);
            LOG_INFO("pipeline/5s: recv=%llu submit=%llu accepted=%llu "
                     "publish=%llu "
                     "queue=%zu dropped=%llu sender-encode-to-send=%lldms "
                     "last-input-age=%lldms "
                     "last-new-frame-age=%lldms last-recv-to-present=%dms",
                     (unsigned long long)(received - lastReceivedLogged_),
                     (unsigned long long)(submitted - lastSubmittedLogged_),
                     (unsigned long long)(accepted - lastAcceptedLogged_),
                     (unsigned long long)(published - lastPublishedLogged_),
                     queue.Size(), (unsigned long long)queue.Dropped(),
                     (long long)senderEncode,
                     (long long)(arrival > 0 ? now - arrival : -1),
                     (long long)(lastFramePresentedMs_ > 0
                                     ? now - lastFramePresentedMs_ : -1),
                     lastR2pMs);
            lastPipelineLogMs_ = now;
            lastReceivedLogged_ = received;
            lastSubmittedLogged_ = submitted;
            lastAcceptedLogged_ = accepted;
            lastPublishedLogged_ = published;
        }
        std::shared_ptr<video::DecodedFrame> latest;
        {
            std::lock_guard lock(frameMutex);
            latest = latestFrame;
        }
        // The loop also wakes for Win32 messages and the housekeeping
        // timeout. Neither requires uploading and presenting the same NV12
        // picture again. In particular, an idle sender may send no frames.
        const bool newFrame = latest != lastPresentedFrame;
        if (!redrawNeeded && !newFrame) continue;
        redrawNeeded = false;
        renderer.Present(latest.get());
        lastPresentedFrame = latest;
        if (latest && newFrame) lastFramePresentedMs_ = SteadyNowMs();

        // Latency, two measurements:
        //  - recv->present (skew-free, OUR clock only): socket arrival to
        //    window present. Bounded by queue + decode + upload + present.
        //  - clock e2e (crosses machines): sender capture time -> our
        //    present. Meaningful only when Mac and PC clocks are synced;
        //    otherwise it carries a constant clock-skew offset.
        if (latest && newFrame) {
            if (latest->arrivalMs > 0) {
                const int64_t r2p = SteadyNowMs() - latest->arrivalMs;
                if (r2p >= 0 && r2p < 600000)
                    lastR2pMs = static_cast<int>(r2p);
            }
            if (latest->captureMs > 0 && lastRttMs >= 0) {
                const int64_t e2e =
                    UnixNowMs() - latest->captureMs - lastRttMs / 2;
                if (e2e > 0 && e2e < 60000) {
                    lastE2eMs = static_cast<int>(e2e);
                    // Our side is fast but the clocks disagree by seconds:
                    // that offset is clock skew, not latency.
                    if (lastR2pMs > 0 && lastR2pMs < 1000 &&
                        lastE2eMs > 5000 && !skewWarned_) {
                        skewWarned_ = true;
                        LOG_WARN(
                            "latency: clock e2e %d ms but recv->present %d "
                            "ms -- Mac/PC clock skew ~%d ms suspected. "
                            "Sync both clocks (PC: Settings > Time & date > "
                            "Sync now) for a meaningful e2e; the real "
                            "latency is close to the recv->present number",
                            lastE2eMs, lastR2pMs, lastE2eMs - lastR2pMs);
                    }
                    if (++presentN_ % 250 == 0)
                        LOG_INFO(
                            "latency: recv->present ~ %d ms | clock e2e ~ %d "
                            "ms (rtt=%d ms)%s",
                            lastR2pMs, lastE2eMs, lastRttMs,
                            skewWarned_ ? " [clocks skewed]" : "");
                }
            }
        }
    }
}

void App::TearDown() {
    running = false;
    if (fullscreen) {
        fullscreen = false;
        ShowCursor(TRUE);
    }

    // Take the session out under the lock, then close/destroy it OUTSIDE the
    // lock: ~Session joins the read thread, whose onClosed re-locks
    // sessionMutex, so holding it here would deadlock.
    std::shared_ptr<net::Session> s;
    {
        std::lock_guard lock(sessionMutex);
        s = std::move(session);
    }
    if (s) {
        s->Close();
        s.reset();  // ~Session joins the read thread here
    }
    queue.Shutdown();
    decodeRunning = false;
    if (decodeThread.joinable()) decodeThread.join();
    if (decoder) decoder->Shutdown();
    listener.Stop();
    mdns.Stop();

    if (frameEvent) CloseHandle(frameEvent);
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    renderer = render::Renderer{}; // release COM objects
}

// ---------------------------------------------------------------------------

// Logs an unhandled SEH (access violation, ...) so a crash is not silently
// swallowed when the exe is double-clicked and its console closes on exit.
LONG WINAPI UnhandledCrashHandler(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "UNHANDLED EXCEPTION 0x%08lX at %p",
                      ep->ExceptionRecord->ExceptionCode,
                      ep->ExceptionRecord->ExceptionAddress);
        LOG_ERROR("%s", msg);
        std::fprintf(stderr, "\n%s\n", msg);
        std::fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Directory containing the running exe ("" if it can't be determined).
std::string ExeDir() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string s(path, n);
    const auto pos = s.find_last_of("\\/");
    return pos == std::string::npos ? std::string() : s.substr(0, pos + 1);
}

// ---------------------------------------------------------------------------

} // namespace

int main(int argc, char** argv) {
    int port = 9000;
    const char* logFile = nullptr;
    std::string mdnsName;
    bool vsync = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::string(argv[i]) == "--log" && i + 1 < argc) {
            logFile = argv[++i];
        } else if (std::string(argv[i]) == "--name" && i + 1 < argc) {
            mdnsName = argv[++i];
        } else if (std::string(argv[i]) == "--vsync") {
            vsync = true;
        } else {
            std::printf(
                "usage: opendisplay_receiver [--port N] [--log file] [--name "
                "S] [--vsync]\n");
            return 1;
        }
    }

    // Log to a file by default (next to the exe) so a failure is visible even
    // when the exe is double-clicked and there is no console to read from.
    const std::string logPath =
        logFile ? std::string(logFile) : ExeDir() + "opendisplay_receiver.log";
    log::Init(logPath.c_str());
    LOG_INFO("opendisplay_windows receiver starting (log: %s)", logPath.c_str());
    ReceiverConfig config;
    const std::string configPath = ExeDir() + "config.ini";
    std::ifstream configFile(configPath);
    if (configFile) {
        std::vector<std::string> warnings;
        config = ParseConfig(configFile, warnings);
        for (const auto& warning : warnings)
            LOG_WARN("config.ini: %s", warning.c_str());
    } else {
        LOG_WARN("config.ini not found at %s; using defaults", configPath.c_str());
    }
    LOG_INFO("config: %dx%d fullscreen=%d adaptive_resolution=%d "
             "requested_bitrate=%d kbps", config.width, config.height,
             config.fullscreen ? 1 : 0, config.adaptiveResolution ? 1 : 0,
             config.bitrateKbps);

    SetUnhandledExceptionFilter(UnhandledCrashHandler);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("WSAStartup failed");
        return 1;
    }
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        LOG_ERROR("MFStartup failed: 0x%lX", hr);
        WSACleanup();
        return 1;
    }

    App app;
    app.mdnsName = std::move(mdnsName);
    app.vsync = vsync;
    app.config = config;
    int rc = 1;
    if (app.Setup(port)) {
        app.Run();
        rc = 0;
    }
    app.TearDown();

    MFShutdown();
    WSACleanup();
    LOG_INFO("opendisplay_windows receiver exiting (rc=%d)", rc);
    log::Shutdown();
    if (rc != 0) {
        // Keep a double-clicked console open so the last log line is readable.
        std::fprintf(stderr, "\n*** receiver exited with code %d (log: %s) ***\n",
                     rc, logPath.c_str());
        std::fflush(stderr);
        std::system("pause");
    }
    return rc;
}
