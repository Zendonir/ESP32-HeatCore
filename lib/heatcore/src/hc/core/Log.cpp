#include "hc/core/Log.h"

#include <cstdio>

namespace hc {

namespace {
LogSink g_sink = nullptr;
LogLevel g_min = LogLevel::Info;
}  // namespace

void setLogSink(LogSink sink) { g_sink = sink; }
void setLogLevel(LogLevel minLevel) { g_min = minLevel; }

const char* logLevelName(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "D";
    case LogLevel::Info: return "I";
    case LogLevel::Warn: return "W";
    case LogLevel::Error: return "E";
  }
  return "?";
}

void logf(LogLevel level, const char* tag, const char* fmt, ...) {
  if (!g_sink || level < g_min) return;
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_sink(level, tag, buf);
}

}  // namespace hc
