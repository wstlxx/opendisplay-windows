#include "log.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace od::log {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
std::atomic<bool> g_enabled{true};
} // namespace

void Init(const char* filePath) {
    std::lock_guard lock(g_mutex);
    if (filePath) g_file = std::fopen(filePath, "a");
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_enabled = false;
}

void Write(Level level, const char* fmt, ...) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    // Unix epoch seconds + milliseconds, for correlating with sender logs.
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const long long sec = ms / 1000;

    char timeBuf[32];
    std::snprintf(timeBuf, sizeof(timeBuf), "%lld.%03lld", sec, ms % 1000);

    char body[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    static const char* kNames[] = {"DBG", "INF", "WRN", "ERR"};
    // Newlines in the message would break the one-line-per-message format.
    for (char* p = body; *p; ++p)
        if (*p == '\n' || *p == '\r') *p = 0;
    const char* line = body;

    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "[%s] [%s] ", timeBuf,
                  kNames[static_cast<int>(level)]);

    std::lock_guard lock(g_mutex);
    const std::string full = std::string(prefix) + line;
    std::fprintf(stderr, "%s\n", full.c_str());
    if (g_file) {
        std::fputs(full.c_str(), g_file);
        std::fputc('\n', g_file);
        std::fflush(g_file);
    }
}

} // namespace od::log
