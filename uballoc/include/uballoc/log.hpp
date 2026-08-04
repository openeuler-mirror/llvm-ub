// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdio>
#include <sstream>
#include <atomic>
#include <cstdlib>
#include <string>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <unistd.h>

namespace uballoc {

// Quantized log levels. Lower number = more severe. Setting a level enables
// all levels at or below that severity (e.g., set_level(Info) enables
// Error+Warn+Info, silences Debug+Trace+Ubse).
enum class LogLevel : int {
    Off   = 0,  // Silence all non-fatal logs (LOG_ERROR still fires)
    Error = 1,
    Warn  = 2,
    Info  = 3,
    Debug = 4,
    Trace = 5,
    Ubse  = 6,  // Most verbose — also enables UBSE runtime logs via provider_init()
};

namespace log {

// Parse UBALLOC_LOG_LEVEL env var (case-insensitive). Returns the matching
// LogLevel, or Error if the env var is unset or unrecognized. Called once at
// static init time so the level is set before main() — no per-call overhead.
//
// Default is Error (not Info) because the constructor(102) runs before
// std::cout is initialized. Using LOG_INFO during the constructor would
// crash in std::ostream::sentry::sentry(). Users who want verbose output
// must explicitly set UBALLOC_LOG_LEVEL=info.
inline LogLevel parse_env_level() {
    const char* env = std::getenv("UBALLOC_LOG_LEVEL");
    if (!env || !*env) return LogLevel::Error;

    std::string s(env);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (s == "off")    return LogLevel::Off;
    if (s == "error")  return LogLevel::Error;
    if (s == "warn")   return LogLevel::Warn;
    if (s == "info")   return LogLevel::Info;
    if (s == "debug")  return LogLevel::Debug;
    if (s == "trace")  return LogLevel::Trace;
    if (s == "ubse")   return LogLevel::Ubse;

    // Numeric form (e.g., "3" for Info)
    if (s.find_first_not_of("0123456789") == std::string::npos) {
        int v = std::atoi(s.c_str());
        if (v >= static_cast<int>(LogLevel::Off) &&
            v <= static_cast<int>(LogLevel::Ubse)) {
            return static_cast<LogLevel>(v);
        }
    }

    return LogLevel::Error;  // unknown value -> default
}

inline std::atomic<int> g_log_level{static_cast<int>(parse_env_level())};

inline void set_level(LogLevel level) {
    g_log_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

inline LogLevel get_level() {
    return static_cast<LogLevel>(g_log_level.load(std::memory_order_relaxed));
}

inline bool should_log(LogLevel level) {
    return static_cast<int>(level) <= g_log_level.load(std::memory_order_relaxed);
}

// Emit a log line to the given C stdio stream. Uses fputs/fputc (C stdio)
// instead of std::cerr/std::cout because __attribute__((constructor(102)))
// runs before the C++ iostream globals (std::cerr, std::cout) are
// initialized. C stdio (stderr, stdout) is available from process start —
// the FILE* objects are statically initialized by libc before any
// user constructor runs.
//
// Includes [pid=N] after the level prefix so multi-process tests (fork)
// can distinguish which process produced each log line.
inline void emit(const char* prefix, const std::string& msg, std::FILE* stream) {
    std::fputs(prefix, stream);
    std::fprintf(stream, "[pid=%d] ", static_cast<int>(::getpid()));
    std::fputs(msg.c_str(), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}

// --- Deferred log mechanism ---
//
// During early constructors (priority 101/102), std::ostringstream (used by
// LOG_* macros) is not yet constructed — calling LOG_* would crash. These
// constructors still need to report diagnostics (VA reservation results,
// sigaction failures). They call defer_log() which buffers the message using
// C stdio (vsnprintf — always safe). flush_deferred_logs() replays the buffer
// through emit() once the log system is ready (called from init()).
//
// After flush, defer_log() emits directly via emit() + should_log(), so
// runtime calls (e.g. fh_register_segment table-full warning) respect
// UBALLOC_LOG_LEVEL without buffering.

constexpr int MAX_DEFERRED_LOGS = 8;
constexpr int DEFERRED_MSG_SIZE = 256;

struct DeferredEntry {
    LogLevel level;
    char msg[DEFERRED_MSG_SIZE];
};

inline DeferredEntry g_deferred[MAX_DEFERRED_LOGS];
inline std::atomic<int> g_deferred_count{0};
inline std::atomic<bool> g_deferred_flushed{false};

inline const char* level_prefix(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "[E] ";
        case LogLevel::Warn:  return "[W] ";
        case LogLevel::Info:  return "[I] ";
        case LogLevel::Debug: return "[D] ";
        case LogLevel::Trace: return "[T] ";
        case LogLevel::Ubse:  return "[U] ";
        default: return "[?] ";
    }
}

inline std::FILE* level_stream(LogLevel level) {
    return (level == LogLevel::Error) ? stderr : stdout;
}

// Buffer a log message (or emit directly if the log system is already flushed).
// Uses vsnprintf (C stdio) — safe at any constructor priority.
inline void defer_log(LogLevel level, const char* fmt, ...) {
    char buf[DEFERRED_MSG_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_deferred_flushed.load(std::memory_order_acquire)) {
        // Log system ready — emit now (respects UBALLOC_LOG_LEVEL).
        if (should_log(level)) {
            emit(level_prefix(level), buf, level_stream(level));
        }
        return;
    }

    // Buffer for later flush.
    int idx = g_deferred_count.fetch_add(1, std::memory_order_acq_rel);
    if (idx < MAX_DEFERRED_LOGS) {
        g_deferred[idx].level = level;
        // vsnprintf already null-terminated buf; memcpy the full buffer.
        std::memcpy(g_deferred[idx].msg, buf, DEFERRED_MSG_SIZE);
    }
    // If idx >= MAX_DEFERRED_LOGS, the message is dropped (buffer full).
}

// Replay all buffered log entries through emit() (respects UBALLOC_LOG_LEVEL).
// Called from init() — by then g_log_level and std::string are ready.
// Sets g_deferred_flushed so subsequent defer_log() calls emit directly.
inline void flush_deferred_logs() {
    g_deferred_flushed.store(true, std::memory_order_release);

    int count = g_deferred_count.load(std::memory_order_acquire);
    for (int i = 0; i < count && i < MAX_DEFERRED_LOGS; ++i) {
        if (should_log(g_deferred[i].level)) {
            emit(level_prefix(g_deferred[i].level),
                 g_deferred[i].msg,
                 level_stream(g_deferred[i].level));
        }
    }
    g_deferred_count.store(0, std::memory_order_release);
}

}  // namespace log

}  // namespace uballoc

// LOG_ERROR is always on and fatal (aborts). Cannot be silenced — used for
// unrecoverable invariant violations where continuing would corrupt state.
// Uses C stdio (fputs to stderr) because constructor(102) runs before
// std::cerr is initialized.
#define LOG_ERROR(msg) do { \
    std::ostringstream _uballoc_log_oss; \
    _uballoc_log_oss << msg; \
    ::uballoc::log::emit("[E] ", _uballoc_log_oss.str(), stderr); \
    std::abort(); \
} while(0)

#ifdef UBALLOC_LOG

  // Runtime filtering enabled. Per-call cost: one relaxed atomic load + branch.
  // Master switch UBALLOC_LOG=OFF compiles all non-fatal logging to no-ops
  // (zero overhead for production builds).
  #define LOG_WARN(msg)  do { if (::uballoc::log::should_log(::uballoc::LogLevel::Warn))  { std::ostringstream _uballoc_log_oss; _uballoc_log_oss << msg; ::uballoc::log::emit("[W] ", _uballoc_log_oss.str(), stdout); } } while(0)
  #define LOG_INFO(msg)  do { if (::uballoc::log::should_log(::uballoc::LogLevel::Info))  { std::ostringstream _uballoc_log_oss; _uballoc_log_oss << msg; ::uballoc::log::emit("[I] ", _uballoc_log_oss.str(), stdout); } } while(0)
  #define LOG_DEBUG(msg) do { if (::uballoc::log::should_log(::uballoc::LogLevel::Debug)) { std::ostringstream _uballoc_log_oss; _uballoc_log_oss << msg; ::uballoc::log::emit("[D] ", _uballoc_log_oss.str(), stdout); } } while(0)
  #define LOG_TRACE(msg) do { if (::uballoc::log::should_log(::uballoc::LogLevel::Trace)) { std::ostringstream _uballoc_log_oss; _uballoc_log_oss << msg; ::uballoc::log::emit("[T] ", _uballoc_log_oss.str(), stdout); } } while(0)
  #define LOG_UBSE(msg)  do { if (::uballoc::log::should_log(::uballoc::LogLevel::Ubse))  { std::ostringstream _uballoc_log_oss; _uballoc_log_oss << msg; ::uballoc::log::emit("[U] ", _uballoc_log_oss.str(), stdout); } } while(0)

#else

  // Compile-time no-op (zero overhead in production builds without UBALLOC_LOG)
  #define LOG_WARN(msg)  ((void)0)
  #define LOG_INFO(msg)  ((void)0)
  #define LOG_DEBUG(msg) ((void)0)
  #define LOG_TRACE(msg) ((void)0)
  #define LOG_UBSE(msg)  ((void)0)

#endif
