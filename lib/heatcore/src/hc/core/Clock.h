#pragma once
// Time abstraction. The core never calls millis()/time() directly so that
// every module can be driven deterministically from host tests.
#include <cstdint>

namespace hc {

// Monotonic milliseconds; wraps after ~49 days. Always compare with elapsed().
using Millis = uint32_t;

inline uint32_t elapsed(Millis now, Millis since) { return now - since; }
inline bool reached(Millis now, Millis deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

struct LocalTime {
  bool valid = false;  // false until RTC or NTP provided a plausible time
  int year = 0, month = 0, day = 0;
  int weekday = 0;  // 0 = Monday ... 6 = Sunday
  int hour = 0, minute = 0, second = 0;
  int minuteOfDay() const { return hour * 60 + minute; }
};

class IClock {
 public:
  virtual ~IClock() = default;
  virtual Millis nowMs() const = 0;
  virtual int64_t epoch() const = 0;  // 0 when unknown
  virtual LocalTime local() const = 0;
};

// Deterministic clock for tests and simulation.
class ManualClock : public IClock {
 public:
  Millis ms = 0;
  int64_t epochBase = 0;  // epoch at ms == 0, 0 = unknown
  int64_t tzOffsetSec = 0;
  Millis nowMs() const override { return ms; }
  int64_t epoch() const override { return epochBase ? epochBase + ms / 1000 : 0; }
  LocalTime local() const override;
  void advance(Millis d) { ms += d; }
};

// Converts an epoch (seconds, already shifted to local time) to calendar fields.
LocalTime localTimeFromEpoch(int64_t localEpoch);

}  // namespace hc
