// Timestamped diagnostic logging to stderr (and optionally a file).
// Console app: stderr is the primary sink; `--log file` adds a file sink.

#pragma once

#include <cstdarg>
#include <cstdio>

namespace od::log {

enum class Level { Debug, Info, Warn, Error };

void Init(const char* filePath);  // pass nullptr for stderr only
void Shutdown();
#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define PRINTF_FMT(a, b)
#endif

void Write(Level level, const char* fmt, ...) PRINTF_FMT(2, 3);

} // namespace od::log

#define LOG_DEBUG(...) ::od::log::Write(::od::log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...)  ::od::log::Write(::od::log::Level::Info,  __VA_ARGS__)
#define LOG_WARN(...)  ::od::log::Write(::od::log::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) ::od::log::Write(::od::log::Level::Error, __VA_ARGS__)
