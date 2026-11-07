#include "platform/Peripherals.h"

#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
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
  g_edges = g_edges + 1;
  if (next == g_tail) {
    g_dropped = g_dropped + 1;
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

void GpioOneWire::begin(int pin) {
  pin_ = pin;
  gpio_num_t p = static_cast<gpio_num_t>(pin);
  gpio_reset_pin(p);
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = GPIO_MODE_INPUT_OUTPUT_OD;  // drive low or release, always readable
  c.pull_up_en = GPIO_PULLUP_ENABLE;   // weak, in addition to the board pull-up
  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  c.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&c);
  gpio_ll_set_level(&GPIO, pin_, 1);
}

bool GpioOneWire::lineHigh() const { return pin_ >= 0 && gpio_ll_get_level(&GPIO, pin_); }

bool GpioOneWire::reset() {
  if (pin_ < 0) return false;
  // the released line must return high (otherwise short to GND / no pull-up)
  for (int i = 0; i < 125 && !gpio_ll_get_level(&GPIO, pin_); ++i) esp_rom_delay_us(2);
  if (!gpio_ll_get_level(&GPIO, pin_)) return false;
  gpio_ll_set_level(&GPIO, pin_, 0);
  esp_rom_delay_us(480);
  portENTER_CRITICAL(&mux_);
  gpio_ll_set_level(&GPIO, pin_, 1);
  esp_rom_delay_us(70);
  bool presence = !gpio_ll_get_level(&GPIO, pin_);
  portEXIT_CRITICAL(&mux_);
  esp_rom_delay_us(410);
  return presence;
}

void GpioOneWire::writeBit(bool bit) {
  portENTER_CRITICAL(&mux_);
  gpio_ll_set_level(&GPIO, pin_, 0);
  if (bit) {
    esp_rom_delay_us(6);
    gpio_ll_set_level(&GPIO, pin_, 1);
    portEXIT_CRITICAL(&mux_);
    esp_rom_delay_us(64);
  } else {
    esp_rom_delay_us(60);
    gpio_ll_set_level(&GPIO, pin_, 1);
    portEXIT_CRITICAL(&mux_);
    esp_rom_delay_us(10);
  }
}

bool GpioOneWire::readBit() {
  portENTER_CRITICAL(&mux_);
  gpio_ll_set_level(&GPIO, pin_, 0);
  esp_rom_delay_us(3);
  gpio_ll_set_level(&GPIO, pin_, 1);
  esp_rom_delay_us(10);
  bool v = gpio_ll_get_level(&GPIO, pin_);
  portEXIT_CRITICAL(&mux_);
  esp_rom_delay_us(53);
  return v;
}

void OneWireBuses::begin(const int* pins, size_t n) {
  n_ = n < kMaxBuses ? n : kMaxBuses;
  for (size_t i = 0; i < n_; ++i) links_[i].begin(pins[i]);
  rescan();
}

void OneWireBuses::rescan() {
  for (size_t b = 0; b < n_; ++b) {
    GpioOneWire& l = links_[b];
    std::vector<hc::ow::Rom> found;
    std::string info;
    if (!l.lineHigh()) {
      info = "Datenleitung liegt dauerhaft auf GND (Kurzschluss oder falsch angeschlossen)";
    } else {
      hc::ow::SearchResult r = hc::ow::search(l, 16);
      if (!r.presence) {
        info = "Kein Geraet antwortet (Anschluss pruefen: GND, Daten, VDD an 3,3 V)";
      } else {
        size_t other = 0;
        for (auto& rom : r.roms) {
          if (hc::ow::isTemperatureFamily(rom[0])) found.push_back(rom);
          else ++other;
        }
        if (!r.error.empty()) info = r.error;
        if (other) info += (info.empty() ? "" : "; ") + std::to_string(other) + " andere(s) 1-Wire-Geraet(e) ignoriert";
      }
    }
    // keep "converted" flags of sensors that are still present
    std::vector<bool> conv(found.size(), false);
    for (size_t i = 0; i < found.size(); ++i)
      for (size_t j = 0; j < addrs_[b].size() && j < converted_[b].size(); ++j)
        if (addrs_[b][j] == found[i]) conv[i] = converted_[b][j];
    addrs_[b] = std::move(found);
    converted_[b] = std::move(conv);
    counts_[b] = static_cast<uint8_t>(addrs_[b].size());
    info_[b] = info;
  }
}

void OneWireBuses::request() {
  for (size_t b = 0; b < n_; ++b)
    if (!addrs_[b].empty()) hc::ow::startConversion(links_[b]);
}

void OneWireBuses::collect(std::vector<Reading>& out) {
  for (size_t b = 0; b < n_; ++b) {
    for (size_t i = 0; i < addrs_[b].size(); ++i) {
      const hc::ow::Rom& a = addrs_[b][i];
      std::string id = "ow" + std::to_string(b + 1) + ":" + hc::ow::romHex(a);
      uint8_t sp[9];
      float t = 0.f;
      bool ok = hc::ow::readScratchpad(links_[b], a, sp);
      if (!ok) {  // one retry: a single disturbed bit fails the CRC
        ok = hc::ow::readScratchpad(links_[b], a, sp);
      }
      ok = ok && hc::ow::decodeTemperature(a[0], sp, t);
      // 85 C right after power-up means "no conversion yet", not a reading
      if (ok && !converted_[b][i] && hc::ow::isPowerOnValue(a[0], sp)) continue;
      if (ok) converted_[b][i] = true;
      out.push_back({id, t, ok});
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
