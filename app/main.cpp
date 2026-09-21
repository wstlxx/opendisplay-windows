// OpenDisplay protocol 3 (pv3) native receiver for Windows 10/11.
//
// Wiring (see docs/RESEARCH.md for the architecture):
//
//   net read thread (per session)
//     -> VideoQueue (<=4, drop-oldest)
//   decode thread
//     -> H264Decoder (MF byte stream + source reader)
//   MF reader thread
//     -> onFrame: publish latest DecodedFrame (D3D texture)
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

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "../net/log.h"
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

std::string MakeHelloId() {
    static std::mt19937_64 rng(
        std::random_device{}() ^ static_cast<uint64_t>(
                                     reinterpret_cast<uintptr_t>(&rng)));
    char buf[33];
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        const uint64_t r = rng() & 0xF;
        buf[i] = hex[r];
    }
    buf[32] = 0;
    return buf;
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
    HANDLE frameEvent = nullptr;

    // Network
    net::TcpListener listener;
    std::shared_ptr<net::Session> session;  // guarded by sessionMutex
    std::mutex sessionMutex;
    // The session whose read loop just ended. onClosed (running on the read
    // thread) records it here; the main loop reaps it so that ~Session (which
    // joins the read thread) never runs on the read thread itself.
    net::Session* endedSession = nullptr;   // guarded by sessionMutex
    std::string helloId = MakeHelloId();

    // hello resend after user resize (debounced, PROTOCOL.md 6.1)
    int64_t resizeAtMs = 0;

    // Stats
    uint64_t lastDecodedForFps = 0;
    int64_t lastStatsAtMs = 0;
    int lastRttMs = -1;

    // Fullscreen
    bool fullscreen = false;
    RECT restoreRect{};

    // Run control
    bool running = false;

    bool Setup(int port);
    void Run();
    void TearDown();

    void OnAccept(SOCKET sock, const std::string& peer);
    void ReapEndedSession();
    void HandleControl(net::Session* s, const ControlMessage& msg);
    void SendStats(net::Session* s);
    void MaybeSendHello();
    void ToggleFullscreen();

    // WndProc trampoline (whnd is the window being messaged; it is valid even
    // during CreateWindowEx, when the App::hwnd member is still null)
    LRESULT WndProcThunk(HWND whnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProcHandle(HWND whnd, UINT msg, WPARAM wp, LPARAM lp);

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
                    resizeAtMs = SteadyNowMs(); // debounced hello
                }
            }
            return 0;
        case WM_KEYDOWN:
            switch (wp) {
                case VK_F1: ToggleFullscreen(); return 0;
                case VK_ESCAPE:
                    if (fullscreen) ToggleFullscreen();
                    return 0;
            }
            break;
        case WM_CLOSE:
            {
                std::lock_guard lock(sessionMutex);
                if (session) session->SendClosing();
                running = false;
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
    }
    return DefWindowProc(whnd, msg, wp, lp);
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

    // --- Window (client 1280x720; letterbox the stream inside) ---
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

    const int initW = 1280, initH = 720;
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
        if (queue.Push(std::move(v))) {
            // Drop-oldest happened; log at most every 128 drops.
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
    RECT cr{};
    GetClientRect(hwnd, &cr);
    s->SendHello(cr.right - cr.left, cr.bottom - cr.top, uiScale, helloId);
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
            break;
        }
        case ControlType::Pong: {
            if (msg.hasT) {
                lastRttMs = static_cast<int>(UnixNowMs() - msg.t);
                LOG_DEBUG("pong: rtt=%d ms", lastRttMs);
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
    int fps = 0;
    if (dt > 100) {
        fps = static_cast<int>((decoded - lastDecodedForFps) * 1000 / dt);
    }
    lastStatsAtMs = now;
    lastDecodedForFps = decoded;
    s->SendControl(od::BuildStats(fps, 0, lastRttMs, -1, -1));
}

void App::MaybeSendHello() {
    if (resizeAtMs == 0) return;
    if (SteadyNowMs() - resizeAtMs < 300) return; // debounce
    resizeAtMs = 0;
    std::lock_guard lock(sessionMutex);
    if (!session) return;
    RECT cr{};
    GetClientRect(hwnd, &cr);
    session->SendHello(cr.right - cr.left, cr.bottom - cr.top, uiScale,
                       helloId);
    LOG_INFO("resent hello with window size %dx%d", cr.right - cr.left,
             cr.bottom - cr.top);
}

void App::ToggleFullscreen() {
    if (!fullscreen) {
        fullscreen = true;
        GetWindowRect(hwnd, &restoreRect);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfo(mon, &mi);
        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
        ShowCursor(FALSE);
    } else {
        fullscreen = false;
        SetWindowLong(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, nullptr, restoreRect.left, restoreRect.top,
                     restoreRect.right - restoreRect.left,
                     restoreRect.bottom - restoreRect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        ShowCursor(TRUE);
    }
}

void App::Run() {
    LOG_INFO("main loop running (F=fullscreen, Esc=exit fullscreen)");
    MSG msg;
    while (running) {
        const HANDLE waitables[1] = {frameEvent};
        MsgWaitForMultipleObjectsEx(1, waitables, 16, QS_ALLINPUT,
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
        MaybeSendHello();
        std::shared_ptr<video::DecodedFrame> latest;
        {
            std::lock_guard lock(frameMutex);
            latest = latestFrame;
        }
        renderer.Present(latest.get());
    }
}

void App::TearDown() {
    running = false;

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
    return pos == std::string::npos ? std::string() : s.substr(0, pos);
}

// ---------------------------------------------------------------------------

} // namespace

int main(int argc, char** argv) {
    int port = 9000;
    const char* logFile = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::string(argv[i]) == "--log" && i + 1 < argc) {
            logFile = argv[++i];
        } else {
            std::printf("usage: opendisplay_receiver [--port N] [--log file]\n");
            return 1;
        }
    }

    // Log to a file by default (next to the exe) so a failure is visible even
    // when the exe is double-clicked and there is no console to read from.
    const std::string logPath =
        logFile ? std::string(logFile) : ExeDir() + "opendisplay_receiver.log";
    log::Init(logPath.c_str());
    LOG_INFO("opendisplay_windows receiver starting (log: %s)", logPath.c_str());

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
