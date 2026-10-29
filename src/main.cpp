// HeatCore firmware for the KinCony KC868-A16 V3 (ESP32-S3).
//
// Task layout (control and I/O on core 1, everything else on core 0):
//   control  core 1 prio 5  system loop every 50 ms (events, control cycle)
//   io       core 1 prio 6  inputs every 20 ms, outputs + soft watchdog
//   rf-rx    core 0 prio 4  drains the RF ISR ring buffer
//   rf-tx    core 0 prio 3  RMT transmission (never holds the system lock)
//   sensors  core 0 prio 2  1-Wire, analog, RS485, RTC temperature
//   display  core 0 prio 1  OLED rendering (optional component)
//   net      core 0 prio 2  Ethernet/Wi-Fi, MQTT, WebSocket
//   storage  core 0 prio 1  debounced configuration writes, SD log
// Only the control and io tasks are needed for heating; a failure of any
// other task, of the network or of the display does not stop control.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>

#include "hc/core/Log.h"
#include "hc/system/HeatCoreSystem.h"
#include "platform/Board.h"
#include "platform/EspClock.h"
#include "platform/I2cBus.h"
#include "platform/MqttLink.h"
#include "platform/Net.h"
#include "platform/Ota.h"
#include "platform/Peripherals.h"
#include "platform/Queues.h"
#include "platform/Storage.h"
#include "platform/WebUi.h"

using namespace platform;

namespace {

EspClock g_clock;
hc::HeatCoreSystem* g_sys = nullptr;
I2cBus g_i2c;
IoExpanders g_io(g_i2c);
Ds3231 g_rtc(g_i2c);
Ssd1306 g_oled(g_i2c, board::kOledAddr);
RmtTxBackend g_tx;
OneWireBuses g_ow;
Rs485Lines g_rs485;
Storage g_storage;
NetManager g_net;
MqttLink* g_mqtt = nullptr;
WebUi* g_web = nullptr;
Ota g_ota;
BoundedQueue<std::string> g_sdLog(64);

std::atomic<bool> g_selfTestRunning{false};
std::atomic<bool> g_applyNetwork{false};
std::atomic<bool> g_wifiScan{false};  // Wi-Fi scan requested via web (net task owns Wi-Fi)
std::atomic<bool> g_applySystem{false};
std::atomic<bool> g_factoryReset{false};
std::atomic<bool> g_shutdown{false};   // reboot pending: io task keeps outputs safe
std::atomic<bool> g_owRescan{false};   // 1-Wire rescan requested via web
std::atomic<uint32_t> g_busyUs{0};
std::atomic<uint32_t> g_historySavedRev{0};
std::atomic<uint32_t> g_controlBeatMs{0};  // last completed control cycle (OTA probation)
std::atomic<uint32_t> g_netBeatMs{0};      // last network task cycle
std::atomic<uint32_t> g_rfEdgesPerSec{0};
bool g_rtcWritten = false;

using Lock = std::lock_guard<std::recursive_mutex>;

struct Busy {
  uint32_t t0 = micros();
  ~Busy() { g_busyUs += micros() - t0; }
};

void logSink(hc::LogLevel level, const char* tag, const char* msg) {
  Serial.printf("[%lu][%s][%s] %s\n", static_cast<unsigned long>(millis()), hc::logLevelName(level), tag, msg);
  if (level >= hc::LogLevel::Warn) {
    char line[240];
    std::snprintf(line, sizeof(line), "%lld;log;%s;%s;%s", static_cast<long long>(g_clock.epoch()),
                  hc::logLevelName(level), tag, msg);
    g_sdLog.push(line);
  }
}

std::string deviceIdFromMac() {
  uint64_t mac = ESP.getEfuseMac();
  char b[16];
  std::snprintf(b, sizeof(b), "%02x%02x%02x", static_cast<unsigned>((mac >> 24) & 0xFF),
                static_cast<unsigned>((mac >> 32) & 0xFF), static_cast<unsigned>((mac >> 40) & 0xFF));
  return b;
}

void writeSafeOutputs() {
  uint16_t safe;
  {
    Lock lk(g_sys->mutex());
    safe = g_sys->safeOutputMask();
  }
  for (int i = 0; i < 3 && !g_io.writeOutputs(safe, false); ++i) delay(5);
}

// ------------------------------------------------------------------ tasks

void controlTask(void*) {
  esp_task_wdt_add(nullptr);  // hard watchdog: a stalled control loop resets the ESP
  uint32_t maxUs = 0, lastStat = 0;
  for (;;) {
    uint32_t t0 = micros();
    {
      Busy b;
      Lock lk(g_sys->mutex());
      g_sys->loop();
      uint32_t now = millis();
      if (now - lastStat >= 1000) {
        lastStat = now;
        auto& p = g_sys->platform;
        p.uptimeSec = now / 1000;
        p.freeHeap = ESP.getFreeHeap();
        p.minFreeHeap = ESP.getMinFreeHeap();
        p.freePsram = ESP.getFreePsram();
        p.totalPsram = ESP.getPsramSize();
        p.controlLoopMaxUs = maxUs;
        p.i2cErrors = g_i2c.errors();
        p.i2cRecoveries = g_i2c.recoveries();
        g_sys->rfRx.stats().edgesPerSec = g_rfEdgesPerSec;
        g_sys->rfRx.stats().isrEdges = rfhw::edges();
        g_sys->rfRx.stats().droppedPulses = rfhw::dropped();
        uint32_t busy = g_busyUs.exchange(0);
        p.cpuPct = static_cast<int>(busy / 20000u);  // busy time of HeatCore tasks over 2 cores
        maxUs = 0;
      }
    }
    uint32_t d = micros() - t0;
    if (d > maxUs) maxUs = d;
    g_controlBeatMs = millis();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void ioTask(void*) {
  uint32_t lastWrite = 0, lastVerify = 0;
  bool watchdogActive = false;
  for (;;) {
    {
      Busy b;
      uint16_t in = 0;
      bool inOk = g_io.readInputs(in);
      uint16_t desired, safe;
      bool stale;
      {
        Lock lk(g_sys->mutex());
        g_sys->reportInputs(in, inOk);
        desired = g_sys->outputMask();
        safe = g_sys->safeOutputMask();
        stale = hc::elapsed(millis(), g_sys->lastLoopMs()) > g_sys->safety.cfg.watchdogSec * 1000u;
      }
      if (stale != watchdogActive) {
        watchdogActive = stale;
        if (stale) HC_LOGE("io", "control loop stalled - outputs to safe state");
      }
      uint16_t mask = (stale || g_shutdown) ? safe : desired;
      uint32_t now = millis();
      if (mask != g_io.lastWritten() || now - lastWrite > 1000) {
        bool verify = now - lastVerify > 5000;
        if (verify) lastVerify = now;
        bool ok = g_io.writeOutputs(mask, verify);
        lastWrite = now;
        Lock lk(g_sys->mutex());
        g_sys->reportOutputWrite(ok);
        g_sys->platform.ioExpandersOk = ok && inOk;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void rfRxTask(void*) {
  static hc::rf::Pulse buf[512];
  bool pendingIdle = false;
  uint32_t lastEdges = rfhw::edges(), lastRate = millis();
  for (;;) {
    size_t n = rfhw::drain(buf, 512);
    if (n) {
      Busy b;
      Lock lk(g_sys->mutex());
      g_sys->rfRx.feed(buf, n);
      pendingIdle = true;
    } else if (pendingIdle && rfhw::usSinceLastEdge() > 25000) {
      Lock lk(g_sys->mutex());
      g_sys->rfRx.idle();
      pendingIdle = false;
    }
    uint32_t now = millis();
    if (now - lastRate >= 1000) {
      uint32_t e = rfhw::edges();
      g_rfEdgesPerSec = (e - lastEdges) * 1000u / (now - lastRate);
      lastEdges = e;
      lastRate = now;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void rfTxTask(void*) {
  for (;;) {
    hc::rf::TxJob job;
    std::vector<hc::rf::Pulse> pulses;
    bool got;
    {
      Lock lk(g_sys->mutex());
      got = g_tx.ready() && g_sys->takeTxJob(job, pulses);
    }
    if (got) {
      uint32_t air = hc::rf::transmissionDurationUs(pulses) / 1000u;
      uint32_t t0 = millis();
      bool ok = g_tx.transmit(pulses, air + 500);
      uint32_t took = millis() - t0;
      Lock lk(g_sys->mutex());
      g_sys->completeTxJob(job, ok, ok ? took : 0, ok ? nullptr : "RMT-Fehler oder Timeout");
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void sensorTask(void*) {
  uint32_t lastOw = 0, lastRescan = millis(), lastRtcTemp = 0;
  bool owPending = false;
  uint32_t owRequested = 0;
  std::string line;
  for (;;) {
    uint32_t now = millis();
    {
      Busy b;
      // analog inputs (averaged)
      float volts[4];
      for (int i = 0; i < 4; ++i) {
        uint32_t sum = 0;
        for (int k = 0; k < 8; ++k) sum += analogReadMilliVolts(board::kAnalog[i]);
        volts[i] = sum / 8000.f;
      }
      // 1-Wire: request, collect 800 ms later
      std::vector<OneWireBuses::Reading> readings;
      if (!owPending && now - lastOw >= 10000) {
        g_ow.request();
        owPending = true;
        owRequested = now;
        lastOw = now;
      } else if (owPending && now - owRequested >= 800) {
        g_ow.collect(readings);
        owPending = false;
      }
      if ((g_owRescan.exchange(false) || now - lastRescan > 600000) && !owPending) {
        g_ow.rescan();
        lastRescan = now;
      }
      float rtcT = 0;
      bool rtcTemp = false;
      if (now - lastRtcTemp > 60000) {
        lastRtcTemp = now;
        rtcTemp = g_rtc.temperature(rtcT);
      }
      Lock lk(g_sys->mutex());
      for (int i = 0; i < 4; ++i) {
        g_sys->io.onAnalog(i, volts[i], now);
        const hc::AnalogConfig& ac = g_sys->io.aiCfg[static_cast<size_t>(i)];
        const hc::AnalogState& as = g_sys->io.ai[static_cast<size_t>(i)];
        if (!ac.enabled || !ac.asSensor) continue;
        if (as.loopFault)
          g_sys->sensors.reportError("ai:" + std::to_string(i + 1), "4-20 mA ausserhalb 3,6..21 mA (Drahtbruch?)", now);
        else
          g_sys->sensors.update("ai:" + std::to_string(i + 1), as.value, now);
      }
      for (auto& r : readings) {
        if (r.ok) g_sys->sensors.update(r.id, r.value, now);
        else g_sys->sensors.reportError(r.id, "1-Wire Lesefehler", now);
      }
      for (size_t bus = 0; bus < 3; ++bus) g_sys->platform.oneWireCount[bus] = g_ow.count(bus);
      if (rtcTemp) g_sys->platform.rtcTempC = rtcT;
      while (g_rs485.poll(line)) {
        if (!line.empty() && line[0] == '{') g_sys->reportRtl433Line(line.c_str());
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void displayTask(void*) {
  hc::Framebuffer fb;
  hc::DisplaySnapshot snap;
  uint32_t lastInit = 0;
  uint8_t contrast = 0;
  uint16_t rotation = 0;
  bool first = true;
  for (;;) {
    bool enabled, on;
    hc::DisplayConfig cfg;
    {
      Lock lk(g_sys->mutex());
      cfg = g_sys->display.cfg;
      enabled = cfg.enabled;
      on = g_sys->display.screenOn();
      if (enabled && on) {
        g_sys->buildDisplaySnapshot(snap);
        g_sys->display.render(fb, snap);
      }
    }
    uint32_t now = millis();
    bool ok = true;
    std::string err;
    {
      Busy b;
      if (!enabled) {
        if (g_oled.initialized()) g_oled.power(false);
      } else if (!g_oled.initialized()) {
        if (first || now - lastInit > 30000) {
          first = false;
          lastInit = now;
          ok = g_oled.init(cfg.contrast, cfg.rotation == 180);
          contrast = cfg.contrast;
          rotation = cfg.rotation;
          if (!ok) err = "SSD1306 antwortet nicht (0x3C)";
        }
      } else {
        if (cfg.contrast != contrast) {
          g_oled.setContrast(cfg.contrast);
          contrast = cfg.contrast;
        }
        if (cfg.rotation != rotation) {
          g_oled.setRotation(cfg.rotation == 180);
          rotation = cfg.rotation;
        }
        g_oled.power(on);
        if (on) {
          ok = g_oled.flush(fb, false);
          if (!ok) err = "I2C-Schreibfehler";
        }
      }
    }
    {
      Lock lk(g_sys->mutex());
      auto& hw = g_sys->display.hw;
      bool wasInit = hw.initialized;
      hw.initialized = g_oled.initialized();
      hw.present = hw.present || hw.initialized;
      if (!err.empty()) {
        ++hw.errors;
        hw.lastError = err;
      }
      if (hw.initialized && !wasInit && hw.flushes) ++hw.reinits;
      if (hw.initialized && on && ok) ++hw.flushes;
      hw.lastFlushUs = g_oled.lastFlushUs();
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void netTask(void*) {
  uint32_t lastStat = 0;
  for (;;) {
    {
      Busy b;
      if (g_applyNetwork.exchange(false)) {
        hc::NetworkConfig nc;
        {
          Lock lk(g_sys->mutex());
          nc = g_sys->netCfg;
        }
        g_net.reconfigure(nc);
      }
      if (g_applySystem.exchange(false)) {
        Lock lk(g_sys->mutex());
        EspClock::setTimezone(g_sys->sysCfg.timezone.c_str());
      }
      if (g_wifiScan.exchange(false)) g_net.startScan();
      g_net.loop();
      g_mqtt->loop(g_net.online());
      g_web->loop();
      if (g_net.timeSynced() && !g_rtcWritten) {
        g_rtcWritten = g_rtc.write(time(nullptr)) || !g_rtc.present();
      }
      uint32_t now = millis();
      g_netBeatMs = now;
      if (now - lastStat > 1000) {
        lastStat = now;
        hc::FirmwareStatus fw;
        g_ota.fillStatus(fw);
        Lock lk(g_sys->mutex());
        auto& p = g_sys->platform;
        p.firmware = fw;
        p.ip = g_net.ip();
        p.network = g_net.kind();
        p.netDetail = g_net.detail();
        p.netState = g_net.stateText();
        p.apSsid = g_net.apSsid();
        p.wifiScanRunning = g_net.scanRunning();
        p.wifiScan = g_net.scanResults();
        p.ethLink = g_net.ethLink();
        p.wifiConnected = g_net.wifiConnected();
        p.wifiRssi = g_net.rssi();
        p.mqtt = g_mqtt->stateText();
        p.mqttConnected = g_mqtt->connected();
        p.timeSynced = g_net.timeSynced();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void saveHistory() {
  std::string blob;
  {
    Lock lk(g_sys->mutex());
    if (g_sys->history.revision() == g_historySavedRev.load()) return;
    g_historySavedRev = g_sys->history.revision();
    blob = g_sys->history.serialize();
  }
  if (!g_storage.saveHistory(blob)) HC_LOGE("storage", "saving statistics failed");
}

void storageTask(void*) {
  for (;;) {
    std::vector<std::pair<hc::ConfigSection, std::string>> todo;
    {
      Lock lk(g_sys->mutex());
      for (auto s : g_sys->takeDirty()) {
        JsonDocument doc;
        g_sys->sectionToJson(s, doc.to<JsonObject>(), true);
        std::string out;
        serializeJson(doc, out);
        todo.emplace_back(s, std::move(out));
      }
      auto& p = g_sys->platform;
      p.sd = g_storage.sdStatus();
      p.sdTotalBytes = g_storage.sdTotal();
      p.sdUsedBytes = g_storage.sdUsed();
    }
    for (auto& t : todo) {
      if (!g_storage.save(t.first, t.second)) HC_LOGE("storage", "saving %s failed", hc::configSectionName(t.first));
    }
    // statistics: hourly snapshot (4 new buckets per room and hour)
    static uint32_t lastHistorySave = 0;
    if (millis() - lastHistorySave > 3600000u) {
      lastHistorySave = millis();
      saveHistory();
    }
    std::string line;
    hc::LocalTime lt = g_clock.local();
    for (int i = 0; i < 32 && g_sdLog.pop(line); ++i) g_storage.sdLog(line, lt);
    if (g_factoryReset.exchange(false)) {
      {
        Lock lk(g_sys->mutex());
        g_sys->history.clear();
      }
      g_storage.factoryReset();
      g_web->requestReboot();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// -------------------------------------------------------------- self-test

void addResult(std::vector<hc::SelfTestItem>& r, const char* c, const char* st, const std::string& d) {
  r.push_back({c, st, d});
}

void selfTestTask(void*) {
  std::vector<hc::SelfTestItem> res;
  std::vector<uint8_t> found;
  {
    I2cLock lk(g_i2c, 500);
    if (lk.ok()) found = g_i2c.scan();
  }
  auto has = [&](uint8_t a) { return std::find(found.begin(), found.end(), a) != found.end(); };
  auto addr = [](uint8_t a) {
    char b[8];
    std::snprintf(b, sizeof(b), "0x%02X", a);
    return std::string(b);
  };
  addResult(res, "Ausgaenge 1-8 (PCF8574)", has(board::kPcfOut1) ? "ok" : "fehlt", addr(board::kPcfOut1));
  addResult(res, "Ausgaenge 9-16 (PCF8574)", has(board::kPcfOut2) ? "ok" : "fehlt", addr(board::kPcfOut2));
  bool swapIn = false;
  {
    Lock lk(g_sys->mutex());
    swapIn = g_sys->io.swapInputBanks;
  }
  addResult(res, swapIn ? "Eingaenge 9-16 (PCF8574)" : "Eingaenge 1-8 (PCF8574)", has(board::kPcfIn1) ? "ok" : "fehlt",
            addr(board::kPcfIn1));
  addResult(res, swapIn ? "Eingaenge 1-8 (PCF8574)" : "Eingaenge 9-16 (PCF8574)", has(board::kPcfIn2) ? "ok" : "fehlt",
            addr(board::kPcfIn2));
  addResult(res, "RTC DS3231", has(board::kRtcAddr) ? "ok" : "fehlt", addr(board::kRtcAddr));
  addResult(res, "EEPROM 24C02", has(board::kEepromAddr) ? "ok" : "fehlt", addr(board::kEepromAddr));
  addResult(res, "OLED SSD1306", has(board::kOledAddr) ? (g_oled.initialized() ? "ok" : "fehler") : "fehlt",
            has(board::kOledAddr) ? addr(board::kOledAddr) : "optional - nicht angeschlossen");
  std::string other;
  for (uint8_t a : found)
    if (a != board::kPcfOut1 && a != board::kPcfOut2 && a != board::kPcfIn1 && a != board::kPcfIn2 &&
        a != board::kRtcAddr && a != board::kEepromAddr && a != board::kOledAddr)
      other += addr(a) + " ";
  if (!other.empty()) addResult(res, "I2C weitere Geraete", "ok", other);
  time_t t;
  bool rtcValid = g_rtc.read(t);
  addResult(res, "RTC Uhrzeit", rtcValid ? "ok" : (g_rtc.present() ? "fehler" : "fehlt"),
            rtcValid ? "gueltig" : "Oszillator-Stopp-Flag / keine Zeit (NTP stellt sie)");
  // RF receiver activity over 2 s
  uint32_t e0 = rfhw::edges();
  vTaskDelay(pdMS_TO_TICKS(2000));
  uint32_t edges = rfhw::edges() - e0;
  addResult(res, "433-MHz-Empfaenger (GPIO8)", edges > 20 ? "ok" : "unbestaetigt",
            edges > 20 ? std::to_string(edges / 2) + " Flanken/s (Empfaenger liefert Signal)"
                       : "keine Flanken - Modul fehlt oder sehr ruhiger Superhet-Empfaenger");
  // RF loopback
  std::string err;
  {
    Lock lk(g_sys->mutex());
    err = g_tx.ready() ? g_sys->startRfLoopback() : std::string("RMT nicht initialisiert");
  }
  if (err.empty()) {
    for (int i = 0; i < 60; ++i) {
      vTaskDelay(pdMS_TO_TICKS(250));
      Lock lk(g_sys->mutex());
      if (g_sys->loopback.done) break;
    }
    Lock lk(g_sys->mutex());
    addResult(res, "433-MHz-Sender (GPIO18)", g_sys->loopback.ok ? "ok" : "unbestaetigt",
              g_sys->loopback.ok ? "Loopback bestaetigt: " + g_sys->loopback.detail
                                 : "RMT bereit, Abstrahlung nicht bestaetigt: " + g_sys->loopback.detail);
  } else {
    addResult(res, "433-MHz-Sender (GPIO18)", "fehler", err);
  }
  for (size_t b = 0; b < 3; ++b) {
    char name[32];
    std::snprintf(name, sizeof(name), "1-Wire Bus %u (GPIO%d)", static_cast<unsigned>(b + 1), board::kOneWire[b]);
    uint8_t n = g_ow.count(b);
    addResult(res, name, n ? "ok" : "unbestaetigt", std::to_string(n) + " Sensor(en)");
  }
  addResult(res, "SD-Karte", g_storage.sdMounted() ? "ok" : "fehlt", g_storage.sdStatus());
  addResult(res, "Ethernet W5500", g_net.ethLink() ? "ok" : "unbestaetigt", g_net.ethLink() ? "Link" : "kein Link");
  addResult(res, "WLAN", g_net.wifiConnected() ? "ok" : "deaktiviert",
            g_net.wifiConnected() ? "verbunden" : "aus (LAN hat Vorrang oder nicht eingerichtet)");
  addResult(res, "MQTT", g_mqtt->connected() ? "ok" : "deaktiviert", g_mqtt->stateText());
  addResult(res, "RS485 (GPIO16/17)", g_rs485.active() ? "unbestaetigt" : "deaktiviert",
            "rtl_433-JSON-Eingang, 115200 Baud; Funktion erst mit Gateway pruefbar");
  addResult(res, "PSRAM", ESP.getPsramSize() > 0 ? "ok" : "fehlt", std::to_string(ESP.getPsramSize() / 1024) + " KB");
  addResult(res, "Dateisystem", g_storage.ready() ? "ok" : "fehler",
            std::to_string(g_storage.usedBytes() / 1024) + " / " + std::to_string(g_storage.totalBytes() / 1024) + " KB");
  {
    Lock lk(g_sys->mutex());
    g_sys->selfTest = std::move(res);
    g_sys->platform.i2cDevices = found;
    g_sys->platform.rtcPresent = g_rtc.present();
    g_sys->platform.rtcValid = rtcValid;
  }
  g_selfTestRunning = false;
  vTaskDelete(nullptr);
}

std::string startSelfTest() {
  if (g_selfTestRunning.exchange(true)) return "Selbsttest laeuft bereits";
  {
    Lock lk(g_sys->mutex());
    g_sys->selfTest = {{"Selbsttest", "laeuft", "bitte ca. 15 s warten"}};
  }
  xTaskCreatePinnedToCore(selfTestTask, "selftest", 8192, nullptr, 1, nullptr, 0);
  return "";
}

// Called from web handlers with the system lock held: never block here.
std::string platformCommand(const std::string& cmd, JsonObjectConst) {
  if (cmd == "selftest") return startSelfTest();
  if (cmd == "reboot") {
    g_web->requestReboot();
    return "";
  }
  if (cmd == "factory_reset") {
    g_factoryReset = true;
    return "";
  }
  if (cmd == "wifi_scan") {
    g_wifiScan = true;
    return "";
  }
  if (cmd == "apply_network") {
    g_applyNetwork = true;
    return "";
  }
  if (cmd == "apply_mqtt") {
    g_mqtt->requestReconnect();
    return "";
  }
  if (cmd == "apply_system") {
    g_applySystem = true;
    return "";
  }
  if (cmd == "ota_rollback") {
    std::string err = g_ota.rollback();
    if (err.empty()) g_web->requestReboot();
    return err;
  }
  if (cmd == "onewire_rescan") {
    g_owRescan = true;  // executed by the sensor task (owns the 1-Wire buses)
    return "";
  }
  return "unbekannter Befehl";
}

void loadConfiguration() {
  const hc::ConfigSection order[] = {hc::ConfigSection::System, hc::ConfigSection::Network, hc::ConfigSection::Mqtt,
                                     hc::ConfigSection::Safety, hc::ConfigSection::Heating, hc::ConfigSection::Sensors,
                                     hc::ConfigSection::Io,     hc::ConfigSection::Rf,      hc::ConfigSection::Display,
                                     hc::ConfigSection::Model,  hc::ConfigSection::Rules,   hc::ConfigSection::Ai};
  for (auto s : order) {
    std::string json;
    if (!g_storage.load(s, json)) continue;
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
      HC_LOGE("config", "%s.json corrupt - defaults used", hc::configSectionName(s));
      continue;
    }
    std::string err = g_sys->sectionFromJson(s, doc.as<JsonObjectConst>());
    if (!err.empty()) HC_LOGE("config", "%s: %s - defaults used", hc::configSectionName(s), err.c_str());
  }
  g_sys->takeDirty();  // freshly loaded: nothing to write back
}

}  // namespace

void setup() {
  // 1) outputs to a defined state before anything else (PCF8574 keeps its
  //    latch across an ESP reset: an OTA restart must not leave valves open)
  g_i2c.begin(board::kI2cSda, board::kI2cScl, board::kI2cHz);
  g_io.begin(0x0000);

  Serial.begin(115200);
  hc::setLogSink(logSink);
  HC_LOGI("main", "HeatCore %s booting, reset reason %d", hc::kFirmwareVersion, static_cast<int>(esp_reset_reason()));

  void* mem = heap_caps_malloc(sizeof(hc::HeatCoreSystem), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_sys = mem ? new (mem) hc::HeatCoreSystem(g_clock) : new hc::HeatCoreSystem(g_clock);
  g_sys->platform.deviceId = deviceIdFromMac();
  g_sys->platform.resetReason = std::to_string(static_cast<int>(esp_reset_reason()));

  g_ota.begin();  // slot state; a new firmware starts on probation
  g_storage.begin();
  loadConfiguration();
  {
    std::string blob;
    if (g_storage.loadHistory(blob) && g_sys->history.deserialize(blob)) {
      g_historySavedRev = g_sys->history.revision();
      HC_LOGI("main", "statistics restored (%u bytes)", static_cast<unsigned>(blob.size()));
    }
  }
  EspClock::setTimezone(g_sys->sysCfg.timezone.c_str());
  if (g_rtc.begin()) {
    time_t t;
    if (g_rtc.read(t)) EspClock::setEpoch(t);
  }
  {
    Lock lk(g_sys->mutex());
    g_sys->begin();
  }
  writeSafeOutputs();  // configuration known now (normally-open valves)

  // 2) optional peripherals - failures are reported, never fatal
  {
    Lock lk(g_sys->mutex());
    g_sys->platform.rfRxHw = rfhw::beginRx(board::kRf433Rx);
    g_sys->platform.rfTxHw = g_tx.begin(board::kRf433Tx);
  }
  g_ow.begin(board::kOneWire, 3);
  for (int pin : board::kAnalog) analogSetPinAttenuation(pin, ADC_11db);
  g_rs485.begin(board::kRs485Rx, board::kRs485Tx, 115200);
  g_storage.sdBegin();

  // 3) network services
  g_mqtt = new MqttLink(*g_sys);
  g_web = new WebUi(*g_sys, g_ota);
  {
    Lock lk(g_sys->mutex());
    g_sys->onEvent = [](const hc::Event& e) {
      g_mqtt->onEvent(e);
      g_web->onEvent(e);
      if (e.type != hc::EventType::TimeMinute && e.type != hc::EventType::SensorValue) {
        char line[200];
        std::snprintf(line, sizeof(line), "%lld;%s;%s;%s;%s;%ld;%ld;%.2f;%s", static_cast<long long>(g_clock.epoch()),
                      hc::eventTypeName(e.type), hc::eventSourceName(e.source), hc::gestureName(e.gesture), e.ref,
                      static_cast<long>(e.a), static_cast<long>(e.b), e.value, e.text);
        g_sdLog.push(line);
      }
    };
    g_sys->mqttPublish = [](const std::string& suffix, const std::string& payload, bool retain) {
      g_mqtt->enqueue({g_sys->baseTopic() + "/" + suffix, payload, retain});
    };
    g_sys->platformCommand = platformCommand;
  }
  hc::NetworkConfig nc;
  hc::SystemConfig sc;
  {
    Lock lk(g_sys->mutex());
    nc = g_sys->netCfg;
    sc = g_sys->sysCfg;
  }
  g_net.begin(nc, sc, g_sys->platform.deviceId);
  g_web->begin();

  // 4) tasks
  xTaskCreatePinnedToCore(controlTask, "control", 12288, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(ioTask, "io", 6144, nullptr, 6, nullptr, 1);
  xTaskCreatePinnedToCore(rfRxTask, "rf-rx", 6144, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(rfTxTask, "rf-tx", 6144, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(sensorTask, "sensors", 8192, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(storageTask, "storage", 10240, nullptr, 1, nullptr, 0);

  delay(5000);
  startSelfTest();
}

namespace {
void safeShutdown() {
  g_shutdown = true;
  {
    Lock lk(g_sys->mutex());
    g_sys->display.cfg.enabled = false;
  }
  writeSafeOutputs();
  saveHistory();  // keep the statistics across the restart
}
}  // namespace

// Arduino core hook: do not confirm a freshly installed firmware during
// startup; HeatCore confirms it after its own probation (see platform::Ota).
extern "C" bool verifyRollbackLater() { return true; }

void loop() {
  // OTA probation: confirm the new firmware once control and network tasks
  // run healthy; otherwise return to the previous firmware.
  uint32_t now = millis();
  bool healthy = now - g_controlBeatMs.load() < 5000 && now - g_netBeatMs.load() < 10000;
  if (g_ota.probation(healthy, now)) {
    safeShutdown();
    HC_LOGE("main", "OTA probation failed - rollback");
    delay(200);
    g_ota.rejectAndRestart();
  }
  // Reboot (OTA, user request): close all valves first, then restart.
  if (g_web && g_web->rebootRequested()) {
    g_shutdown = true;
    delay(800);  // let the HTTP response go out
    safeShutdown();
    HC_LOGW("main", "restart");
    delay(200);
    ESP.restart();
  }
  delay(200);
}
