#pragma once
// Minimal logging facade. The platform installs a sink (Serial, SD card,
// web log ring); host tests leave it empty or capture it.
#include <cstdarg>
#include <cstdint>

namespace hc {

enum class LogLevel : uint8_t { Debug, Info, Warn, Error };

using LogSink = void (*)(LogLevel level, const char* tag, const char* msg);

void setLogSink(LogSink sink);
void setLogLevel(LogLevel minLevel);
void logf(LogLevel level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

const char* logLevelName(LogLevel l);

}  // namespace hc

#define HC_LOGD(tag, ...) ::hc::logf(::hc::LogLevel::Debug, tag, __VA_ARGS__)
#define HC_LOGI(tag, ...) ::hc::logf(::hc::LogLevel::Info, tag, __VA_ARGS__)
#define HC_LOGW(tag, ...) ::hc::logf(::hc::LogLevel::Warn, tag, __VA_ARGS__)
#define HC_LOGE(tag, ...) ::hc::logf(::hc::LogLevel::Error, tag, __VA_ARGS__)
