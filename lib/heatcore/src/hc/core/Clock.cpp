#include "hc/core/Clock.h"

namespace hc {

LocalTime localTimeFromEpoch(int64_t t) {
  LocalTime lt;
  if (t <= 0) return lt;
  int64_t days = t / 86400;
  int64_t rem = t % 86400;
  lt.hour = static_cast<int>(rem / 3600);
  lt.minute = static_cast<int>((rem % 3600) / 60);
  lt.second = static_cast<int>(rem % 60);
  // 1970-01-01 was a Thursday (weekday 3 with Monday = 0)
  lt.weekday = static_cast<int>((days + 3) % 7);
  // Civil-from-days (Howard Hinnant)
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int64_t d = doy - (153 * mp + 2) / 5 + 1;
  int64_t m = mp < 10 ? mp + 3 : mp - 9;
  lt.year = static_cast<int>(y + (m <= 2));
  lt.month = static_cast<int>(m);
  lt.day = static_cast<int>(d);
  lt.valid = lt.year >= 2024;
  return lt;
}

LocalTime ManualClock::local() const {
  int64_t e = epoch();
  if (!e) return LocalTime{};
  return localTimeFromEpoch(e + tzOffsetSec);
}

}  // namespace hc
