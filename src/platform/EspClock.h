#pragma once
// IClock backed by millis() and the system time (RTC/NTP).
#include <Arduino.h>
#include <sys/time.h>
#include <time.h>

#include "hc/core/Clock.h"

namespace platform {

class EspClock : public hc::IClock {
 public:
  hc::Millis nowMs() const override { return millis(); }
  int64_t epoch() const override {
    time_t t = time(nullptr);
    return t > 1704067200 ? static_cast<int64_t>(t) : 0;  // valid after 2024-01-01
  }
  hc::LocalTime local() const override {
    hc::LocalTime lt;
    time_t t = time(nullptr);
    if (t <= 1704067200) return lt;
    struct tm tmv;
    localtime_r(&t, &tmv);
    lt.valid = true;
    lt.year = tmv.tm_year + 1900;
    lt.month = tmv.tm_mon + 1;
    lt.day = tmv.tm_mday;
    lt.weekday = (tmv.tm_wday + 6) % 7;  // Monday = 0
    lt.hour = tmv.tm_hour;
    lt.minute = tmv.tm_min;
    lt.second = tmv.tm_sec;
    return lt;
  }
  static void setTimezone(const char* tz) {
    setenv("TZ", tz, 1);
    tzset();
  }
  static void setEpoch(time_t t) {
    struct timeval tv{t, 0};
    settimeofday(&tv, nullptr);
  }
};

}  // namespace platform
