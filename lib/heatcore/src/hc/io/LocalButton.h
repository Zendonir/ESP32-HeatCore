#pragma once
// Debounced push button on the board (e.g. BOOT/"DOWNLOAD", GPIO0).
// Short press: reported on release. Long press: reported once while still
// held, after kLongMs; the release that follows reports nothing.
#include <cstdint>

#include "hc/core/Clock.h"

namespace hc {

class LocalButton {
 public:
  enum class Press : uint8_t { None = 0, Short, Long };
  static constexpr Millis kDebounceMs = 40;
  static constexpr Millis kLongMs = 2000;

  // `down`: raw level (true = pressed). Call every 10..50 ms.
  Press update(bool down, Millis now) {
    if (down != raw_) {
      raw_ = down;
      rawSince_ = now;
    }
    if (raw_ != stable_ && elapsed(now, rawSince_) >= kDebounceMs) {
      stable_ = raw_;
      if (stable_) {
        pressedAt_ = now;
        longSent_ = false;
      } else if (!longSent_ && armed_) {
        return Press::Short;
      }
      armed_ = true;
    }
    if (stable_ && armed_ && !longSent_ && elapsed(now, pressedAt_) >= kLongMs) {
      longSent_ = true;
      return Press::Long;
    }
    return Press::None;
  }
  // A button held down at start (e.g. right after a download-mode reset) is
  // ignored until it has been released once.
  void begin(bool down) {
    raw_ = stable_ = down;
    armed_ = !down;
  }

 private:
  bool raw_ = false, stable_ = false, longSent_ = false, armed_ = true;
  Millis rawSince_ = 0, pressedAt_ = 0;
};

}  // namespace hc
