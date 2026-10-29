#include "platform/Peripherals.h"

#include <DallasTemperature.h>
#include <OneWire.h>
#include <esp32-hal-rmt.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>

#include <cstring>

#include "platform/Board.h"

namespace platform {

// ===================================================================== PCF8574

bool IoExpanders::writePort(uint8_t addr, uint8_t logical8) {
  // quasi-bidirectional port: HIGH = released. Outputs are active LOW.
  uint8_t v = board::kOutputsActiveLow ? static_cast<uint8_t>(~logical8) : logical8;
  return bus_.writeByte(addr, v);
}

bool IoExpanders::begin(uint16_t safeMask) {
  I2cLock lk(bus_, 200);
  if (!lk.ok()) return false;
  // Write the safe state first, before probing anything else.
  out1_ = writePort(board::kPcfOut1, static_cast<uint8_t>(safeMask & 0xFF));
  out2_ = writePort(board::kPcfOut2, static_cast<uint8_t>(safeMask >> 8));
  in1_ = bus_.writeByte(board::kPcfIn1, 0xFF);  // all pins released = inputs
  in2_ = bus_.writeByte(board::kPcfIn2, 0xFF);
  last_ = safeMask;
  return out1_ && out2_;
}

bool IoExpanders::writeOutputs(uint16_t mask, bool verify) {
  I2cLock lk(bus_, 30);
  if (!lk.ok()) return false;
  bool ok = writePort(board::kPcfOut1, static_cast<uint8_t>(mask & 0xFF)) &&
            writePort(board::kPcfOut2, static_cast<uint8_t>(mask >> 8));
  if (ok && verify) {
    uint8_t a = 0, b = 0;
    ok = bus_.read(board::kPcfOut1, &a, 1) && bus_.read(board::kPcfOut2, &b, 1);
    uint16_t raw = static_cast<uint16_t>(a | (b << 8));
    uint16_t expect = board::kOutputsActiveLow ? static_cast<uint16_t>(~mask) : mask;
    ok = ok && raw == expect;
  }
  if (ok) last_ = mask;
  return ok;
}

bool IoExpanders::readInputs(uint16_t& active) {
  I2cLock lk(bus_, 30);
  if (!lk.ok()) return false;
  uint8_t a = 0xFF, b = 0xFF;
  bool ok = bus_.read(board::kPcfIn1, &a, 1) && bus_.read(board::kPcfIn2, &b, 1);
  if (!ok) return false;
  uint16_t raw = static_cast<uint16_t>(a | (b << 8));
  active = board::kInputsActiveLow ? static_cast<uint16_t>(~raw) : raw;
  return true;
}

// ===================================================================== DS3231

namespace {
uint8_t bcd2bin(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
uint8_t bin2bcd(uint8_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

// days since 1970-01-01 for a civil date (Howard Hinnant)
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}
}  // namespace

bool Ds3231::begin() {
  I2cLock lk(bus_, 100);
  present_ = lk.ok() && bus_.probe(board::kRtcAddr);
  return present_;
}

bool Ds3231::read(time_t& utc) {
  if (!present_) return false;
  I2cLock lk(bus_, 100);
  if (!lk.ok()) return false;
  uint8_t reg = 0x00, r[7];
  if (!bus_.writeRead(board::kRtcAddr, &reg, 1, r, 7)) return false;
  uint8_t sreg = 0x0F, status = 0;
  if (!bus_.writeRead(board::kRtcAddr, &sreg, 1, &status, 1)) return false;
  if (status & 0x80) return false;  // oscillator stopped: time not valid
  int sec = bcd2bin(r[0] & 0x7F), min = bcd2bin(r[1] & 0x7F), hour = bcd2bin(r[2] & 0x3F);
  int day = bcd2bin(r[4] & 0x3F), month = bcd2bin(r[5] & 0x1F), year = 2000 + bcd2bin(r[6]);
  if (month < 1 || month > 12 || day < 1 || day > 31 || year < 2024) return false;
  utc = static_cast<time_t>(daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
                            hour * 3600 + min * 60 + sec);
  return true;
}

bool Ds3231::write(time_t utc) {
  if (!present_) return false;
  struct tm t;
  gmtime_r(&utc, &t);
  uint8_t buf[8] = {0x00,
                    bin2bcd(static_cast<uint8_t>(t.tm_sec)),
                    bin2bcd(static_cast<uint8_t>(t.tm_min)),
                    bin2bcd(static_cast<uint8_t>(t.tm_hour)),
                    static_cast<uint8_t>(t.tm_wday + 1),
                    bin2bcd(static_cast<uint8_t>(t.tm_mday)),
                    bin2bcd(static_cast<uint8_t>(t.tm_mon + 1)),
                    bin2bcd(static_cast<uint8_t>(t.tm_year - 100))};
  I2cLock lk(bus_, 100);
  if (!lk.ok() || !bus_.write(board::kRtcAddr, buf, sizeof(buf))) return false;
  uint8_t clr[2] = {0x0F, 0x00};  // clear oscillator-stop flag
  return bus_.write(board::kRtcAddr, clr, 2);
}

bool Ds3231::temperature(float& c) {
  if (!present_) return false;
  I2cLock lk(bus_, 100);
  if (!lk.ok()) return false;
  uint8_t reg = 0x11, r[2];
  if (!bus_.writeRead(board::kRtcAddr, &reg, 1, r, 2)) return false;
  c = static_cast<int8_t>(r[0]) + (r[1] >> 6) * 0.25f;
  return true;
}

// ===================================================================== SSD1306

bool Ssd1306::cmd(std::initializer_list<uint8_t> c) {
  uint8_t buf[24];
  buf[0] = 0x00;  // command stream
  size_t n = 1;
  for (uint8_t b : c)
    if (n < sizeof(buf)) buf[n++] = b;
  I2cLock lk(bus_, 30);
  return lk.ok() && bus_.write(addr_, buf, n);
}

bool Ssd1306::init(uint8_t contrast, bool rotate180) {
  init_ = false;
  haveShadow_ = false;
  {
    I2cLock lk(bus_, 50);
    if (!lk.ok() || !bus_.probe(addr_)) return false;
  }
  bool ok = cmd({0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00}) &&
            cmd({static_cast<uint8_t>(rotate180 ? 0xA0 : 0xA1), static_cast<uint8_t>(rotate180 ? 0xC0 : 0xC8), 0xDA,
                 0x12, 0x81, contrast, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0x2E, 0xAF});
  init_ = ok;
  on_ = true;
  return ok;
}

bool Ssd1306::setContrast(uint8_t c) { return init_ && cmd({0x81, c}); }

bool Ssd1306::setRotation(bool r) {
  if (!init_) return false;
  haveShadow_ = false;
  return cmd({static_cast<uint8_t>(r ? 0xA0 : 0xA1), static_cast<uint8_t>(r ? 0xC0 : 0xC8)});
}

bool Ssd1306::power(bool on) {
  if (!init_ || on == on_) return init_;
  on_ = on;
  return cmd({static_cast<uint8_t>(on ? 0xAF : 0xAE)});
}

bool Ssd1306::flush(const hc::Framebuffer& fb, bool force) {
  if (!init_) return false;
  uint32_t t0 = micros();
  const uint8_t* d = fb.data();
  for (int page = 0; page < 8; ++page) {
    const uint8_t* src = d + page * 128;
    if (!force && haveShadow_ && std::memcmp(src, shadow_ + page * 128, 128) == 0) continue;
    if (!cmd({0x21, 0, 127, 0x22, static_cast<uint8_t>(page), static_cast<uint8_t>(page)})) {
      init_ = false;
      return false;
    }
    // 16 data bytes per transaction keeps every bus lock short
    for (int off = 0; off < 128; off += 16) {
      uint8_t buf[17];
      buf[0] = 0x40;
      std::memcpy(buf + 1, src + off, 16);
      I2cLock lk(bus_, 30);
      if (!lk.ok() || !bus_.write(addr_, buf, sizeof(buf))) {
        init_ = false;
        return false;
      }
    }
    std::memcpy(shadow_ + page * 128, src, 128);
  }
  haveShadow_ = true;
  lastFlushUs_ = micros() - t0;
  return true;
}

// ===================================================================== RF RX

namespace rfhw {
namespace {
constexpr uint32_t kRing = 2048;  // power of two
uint32_t g_ring[kRing];
volatile uint32_t g_head = 0;
volatile uint32_t g_tail = 0;
volatile uint32_t g_edges = 0;
volatile uint32_t g_dropped = 0;
volatile int64_t g_lastUs = 0;
int g_pin = -1;

void IRAM_ATTR rxIsr() {
  int64_t now = esp_timer_get_time();
  int64_t dur = now - g_lastUs;
  g_lastUs = now;
  // level after the edge; the pulse that just ended had the opposite level
  uint32_t level = (REG_READ(GPIO_IN_REG) >> g_pin) & 1u;
  uint32_t ended = level ^ 1u;
  uint32_t us = dur > 65535 ? 65535u : static_cast<uint32_t>(dur);
  uint32_t head = g_head;
  uint32_t next = (head + 1) & (kRing - 1);
  ++g_edges;
  if (next == g_tail) {
    ++g_dropped;
    return;
  }
  g_ring[head] = us | (ended << 16);
  g_head = next;
}
}  // namespace

bool beginRx(int pin) {
  if (pin < 0 || pin >= 32) return false;  // GPIO_IN_REG covers GPIO0..31
  g_pin = pin;
  pinMode(pin, INPUT);
  g_lastUs = esp_timer_get_time();
  attachInterrupt(digitalPinToInterrupt(pin), rxIsr, CHANGE);
  return true;
}

size_t drain(hc::rf::Pulse* out, size_t max) {
  size_t n = 0;
  while (n < max && g_tail != g_head) {
    uint32_t v = g_ring[g_tail];
    g_tail = (g_tail + 1) & (kRing - 1);
    out[n].us = static_cast<uint16_t>(v & 0xFFFF);
    out[n].level = static_cast<uint8_t>((v >> 16) & 1u);
    ++n;
  }
  return n;
}

uint32_t edges() { return g_edges; }
uint32_t dropped() { return g_dropped; }
uint32_t usSinceLastEdge() {
  int64_t d = esp_timer_get_time() - g_lastUs;
  return d > 0x7FFFFFFF ? 0x7FFFFFFFu : static_cast<uint32_t>(d);
}
}  // namespace rfhw

// ===================================================================== RF TX

bool RmtTxBackend::begin(int pin) {
  pin_ = pin;
  // keep the transmitter keyed off before the RMT channel takes the pin
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  ready_ = rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000);  // 1 us ticks
  return ready_;
}

bool RmtTxBackend::transmit(const std::vector<hc::rf::Pulse>& pulses, uint32_t timeoutMs) {
  if (!ready_ || pulses.empty()) return false;
  std::vector<rmt_data_t> sym((pulses.size() + 1) / 2);
  for (size_t i = 0; i < pulses.size(); i += 2) {
    rmt_data_t& s = sym[i / 2];
    uint16_t d0 = pulses[i].us > 32767 ? 32767 : pulses[i].us;
    s.duration0 = d0;
    s.level0 = pulses[i].level;
    if (i + 1 < pulses.size()) {
      uint16_t d1 = pulses[i + 1].us > 32767 ? 32767 : pulses[i + 1].us;
      s.duration1 = d1;
      s.level1 = pulses[i + 1].level;
    } else {
      s.duration1 = 1;
      s.level1 = 0;
    }
  }
  bool ok = rmtWrite(pin_, sym.data(), sym.size(), timeoutMs);
  if (!ok) {
    // make sure the carrier is not left on after a timeout
    rmtDeinit(pin_);
    ready_ = begin(pin_);
  }
  return ok;
}

// ===================================================================== 1-Wire

void OneWireBuses::begin(const int* pins, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    auto* w = new OneWire(pins[i]);
    auto* d = new DallasTemperature(w);
    d->begin();
    d->setWaitForConversion(false);
    d->setResolution(12);
    wires_.push_back(w);
    dallas_.push_back(d);
    addrs_.emplace_back();
  }
  rescan();
}

void OneWireBuses::rescan() {
  for (size_t b = 0; b < dallas_.size(); ++b) {
    addrs_[b].clear();
    dallas_[b]->begin();
    uint8_t n = dallas_[b]->getDeviceCount();
    for (uint8_t i = 0; i < n && i < 16; ++i) {
      DeviceAddress a;
      if (dallas_[b]->getAddress(a, i)) {
        std::array<uint8_t, 8> x;
        std::memcpy(x.data(), a, 8);
        addrs_[b].push_back(x);
        dallas_[b]->setResolution(a, 12);
      }
    }
    if (b < counts_.size()) counts_[b] = static_cast<uint8_t>(addrs_[b].size());
  }
}

void OneWireBuses::request() {
  for (auto* d : dallas_) d->requestTemperatures();
}

void OneWireBuses::collect(std::vector<Reading>& out) {
  for (size_t b = 0; b < dallas_.size(); ++b) {
    for (auto& a : addrs_[b]) {
      char id[24];
      std::snprintf(id, sizeof(id), "ow%u:%02X%02X%02X%02X%02X%02X%02X%02X", static_cast<unsigned>(b + 1), a[0], a[1],
                    a[2], a[3], a[4], a[5], a[6], a[7]);
      float t = dallas_[b]->getTempC(a.data());
      out.push_back({id, t, t != DEVICE_DISCONNECTED_C});
    }
  }
}

// ====================================================================== RS485

void Rs485Lines::begin(int rx, int tx, uint32_t baud) {
  Serial1.begin(baud, SERIAL_8N1, rx, tx);
  active_ = true;
}

bool Rs485Lines::poll(std::string& line) {
  if (!active_) return false;
  while (Serial1.available()) {
    char c = static_cast<char>(Serial1.read());
    if (c == '\r') continue;
    if (c == '\n') {
      bool ok = !overflow_ && !buf_.empty();
      line.swap(buf_);
      buf_.clear();
      overflow_ = false;
      if (ok) return true;
      continue;
    }
    if (buf_.size() < 512) buf_ += c;
    else overflow_ = true;
  }
  return false;
}

}  // namespace platform
