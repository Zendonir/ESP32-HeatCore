#include "hc/display/Display.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace hc {

namespace {
enum Key {
  kTitleSystem, kTitleRooms, kTitleCircuits, kTitleDiag, kTitleModel, kNetwork, kCircuitsActive, kSensors, kNoFaults,
  kFaults, kSetpoint, kHeating, kIdle, kValveOpen, kValveClosed, kWindow, kNoRooms, kUptime, kNoModel, kStatus,
  kTrend, kDelay, kForecast, kNoTime, kOffline, kGain, kKeyCount
};
const char* const kDe[kKeyCount] = {
    "System", "R\xC3\xA4ume", "Heizkreise", "Diagnose", "Lernmodell", "Netz", "Heizkreise aktiv", "Sensoren",
    "Keine Fehler", "Fehler", "Soll", "heizt", "aus", "Ventil auf", "Ventil zu", "Fenster offen", "Keine R\xC3\xA4ume",
    "Laufzeit", "Kein Modell", "Status", "Trend", "Verz\xC3\xB6g.", "Prognose", "keine Uhrzeit", "offline", "Leistung"};
const char* const kEn[kKeyCount] = {
    "System", "Rooms", "Circuits", "Diagnostics", "Learning model", "Network", "Active circuits", "Sensors",
    "No faults", "Faults", "Set", "heating", "idle", "valve open", "valve closed", "window open", "No rooms",
    "Uptime", "No model", "Status", "Trend", "Delay", "Forecast", "no time", "offline", "Gain"};

std::string f1(float v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%.1f", v);
  return b;
}
std::string deg(float v) { return f1(v) + "\xC2\xB0"; }

std::string uptimeStr(uint32_t s) {
  char b[24];
  uint32_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  if (d) std::snprintf(b, sizeof(b), "%lud %02luh %02lum", (unsigned long)d, (unsigned long)h, (unsigned long)m);
  else std::snprintf(b, sizeof(b), "%luh %02lum", (unsigned long)h, (unsigned long)m);
  return b;
}
std::string kb(uint32_t bytes) { return std::to_string(bytes / 1024) + "k"; }

// Cuts UTF-8 text to at most maxChars glyphs.
std::string truncateGlyphs(const std::string& s, int maxChars) {
  std::string out;
  int n = 0;
  const char* p = s.c_str();
  while (*p && n < maxChars) {
    uint8_t g;
    int len = nextGlyph(p, g);
    out.append(p, static_cast<size_t>(len));
    p += len;
    ++n;
  }
  return out;
}
}  // namespace

const char* displayModeName(DisplayMode m) {
  switch (m) {
    case DisplayMode::Large: return "large";
    case DisplayMode::Compact: return "compact";
    default: return "standard";
  }
}
DisplayMode displayModeFromName(const char* s) {
  if (s && !std::strcmp(s, "large")) return DisplayMode::Large;
  if (s && !std::strcmp(s, "compact")) return DisplayMode::Compact;
  return DisplayMode::Standard;
}

const char* DisplayManager::tr(int key) const { return cfg.language == Language::En ? kEn[key] : kDe[key]; }

void DisplayManager::activity() {
  lastActivity_ = clock_.nowMs();
  on_ = true;
}

void DisplayManager::setPage(DisplayPage p, bool fromUser) {
  if (p != page_) {
    page_ = p;
    Event e(EventType::DisplayPageChanged, EventSource::Display);
    e.ts = clock_.nowMs();
    e.a = static_cast<int>(p) + 1;
    bus_.post(e);
  }
  lastCycle_ = clock_.nowMs();
  if (fromUser) activity();
}

void DisplayManager::next() {
  if (!on_) {
    activity();  // first key press only wakes the screen
    return;
  }
  int p = static_cast<int>(page_);
  for (int i = 1; i <= kNumPages; ++i) {
    int c = (p + i) % kNumPages;
    if (pageEnabled(c)) {
      setPage(static_cast<DisplayPage>(c), true);
      return;
    }
  }
  activity();
}

void DisplayManager::prev() {
  if (!on_) {
    activity();
    return;
  }
  int p = static_cast<int>(page_);
  for (int i = 1; i <= kNumPages; ++i) {
    int c = (p - i + kNumPages) % kNumPages;
    if (pageEnabled(c)) {
      setPage(static_cast<DisplayPage>(c), true);
      return;
    }
  }
  activity();
}

bool DisplayManager::select(int page1Based) {
  int p = page1Based - 1;
  if (p < 0 || p >= kNumPages || !pageEnabled(p)) return false;
  setPage(static_cast<DisplayPage>(p), true);
  return true;
}

void DisplayManager::roomNext() {
  if (!on_) {
    activity();
    return;
  }
  if (roomCount_ > 0) roomIndex_ = (roomIndex_ + 1) % roomCount_;
  if (page_ != DisplayPage::Rooms && page_ != DisplayPage::Model && pageEnabled(1)) setPage(DisplayPage::Rooms, true);
  activity();
}

void DisplayManager::wake() { activity(); }

std::string DisplayManager::currentRoom(const DisplaySnapshot& s) const {
  if (s.rooms.empty()) return std::string();
  return s.rooms[static_cast<size_t>(roomIndex_) % s.rooms.size()].id;
}

void DisplayManager::tick(Millis now, AlarmLevel highest, size_t alarmCount) {
  if (lastActivity_ == 0) lastActivity_ = now ? now : 1;
  if (roomCount_ > 0 && roomIndex_ >= roomCount_) roomIndex_ = 0;
  if (cfg.alarmWakes && alarmCount > lastAlarmCount_ && static_cast<int>(highest) >= cfg.alarmPriority) activity();
  lastAlarmCount_ = alarmCount;
  if (!pageEnabled(static_cast<int>(page_))) next();
  if (cfg.timeoutSec && on_ && elapsed(now, lastActivity_) > cfg.timeoutSec * 1000u) on_ = false;
  // auto cycling pauses for 30 s after a user interaction
  if (on_ && cfg.autoCycle && cfg.cycleSec && elapsed(now, lastCycle_) >= cfg.cycleSec * 1000u &&
      elapsed(now, lastActivity_) >= 30000u) {
    int p = static_cast<int>(page_);
    for (int i = 1; i <= kNumPages; ++i) {
      int c = (p + i) % kNumPages;
      if (pageEnabled(c)) {
        setPage(static_cast<DisplayPage>(c), false);
        break;
      }
    }
    lastCycle_ = now;
  }
}

// ----------------------------------------------------------------- render

void DisplayManager::header(Framebuffer& fb, const char* title, int pageNo) const {
  fb.fillRect(0, 0, 128, 9);
  fb.text(2, 1, title, 1, false);
  char b[8];
  std::snprintf(b, sizeof(b), "%d/%d", pageNo, kNumPages);
  int w = Framebuffer::textWidth(b);
  fb.text(126 - w, 1, b, 1, false);
}

void DisplayManager::alarmBanner(Framebuffer& fb, const DisplaySnapshot& s) const {
  if (cfg.alarmPriority > 2) return;
  const Alarm* top = nullptr;
  for (auto& a : s.alarms)
    if (static_cast<int>(a.level) >= cfg.alarmPriority && (!top || a.level > top->level)) top = &a;
  if (!top) return;
  // light text on a filled bar in the last text row
  fb.fillRect(0, 55, 128, 9);
  fb.text(1, 56, truncateGlyphs("! " + top->message, 21), 1, false);
}

void DisplayManager::render(Framebuffer& fb, const DisplaySnapshot& s) const {
  fb.clear();
  if (!screenOn()) return;
  renderPage(page_, fb, s);
}

void DisplayManager::renderPage(DisplayPage p, Framebuffer& fb, const DisplaySnapshot& s) const {
  fb.clear();
  switch (p) {
    case DisplayPage::System: pageSystem(fb, s); break;
    case DisplayPage::Rooms: pageRooms(fb, s); break;
    case DisplayPage::Circuits: pageCircuits(fb, s); break;
    case DisplayPage::Diagnostics: pageDiag(fb, s); break;
    case DisplayPage::Model: pageModel(fb, s); break;
    default: break;
  }
  alarmBanner(fb, s);
}

void DisplayManager::pageSystem(Framebuffer& fb, const DisplaySnapshot& s) const {
  fb.fillRect(0, 0, 128, 9);
  std::string t = s.timeValid ? s.time : "--:--";
  int w = Framebuffer::textWidth(t);
  fb.text(126 - w, 1, t, 1, false);
  fb.text(2, 1, truncateGlyphs(s.deviceName, (122 - w) / 6 - 1), 1, false);
  int y = 11;
  fb.textFit(0, y, "IP " + (s.ip.empty() ? std::string("-") : s.ip), 128);
  y += 9;
  fb.textFit(0, y, std::string(tr(kNetwork)) + ": " + (s.network.empty() ? tr(kOffline) : s.network), 128);
  y += 9;
  fb.textFit(0, y, s.timeValid ? s.date : tr(kNoTime), 128);
  y += 9;
  fb.textFit(0, y, std::string(tr(kCircuitsActive)) + ": " + std::to_string(s.activeCircuits), 128);
  y += 9;
  fb.textFit(0, y,
             std::string(tr(kSensors)) + ": " + std::to_string(s.sensorsOk) + "/" + std::to_string(s.sensorsTotal), 128);
  if (s.alarms.empty()) fb.textFit(0, 56, tr(kNoFaults), 128);
  else fb.textFit(0, 56, std::string(tr(kFaults)) + ": " + std::to_string(s.alarms.size()), 128);
}

void DisplayManager::pageRooms(Framebuffer& fb, const DisplaySnapshot& s) const {
  if (s.rooms.empty()) {
    header(fb, tr(kTitleRooms), 2);
    fb.text(0, 24, tr(kNoRooms));
    return;
  }
  size_t idx = static_cast<size_t>(roomIndex_) % s.rooms.size();
  if (cfg.mode == DisplayMode::Large) {
    const auto& r = s.rooms[idx];
    fb.fillRect(0, 0, 128, 9);
    char cnt[12];
    std::snprintf(cnt, sizeof(cnt), "%u/%u", static_cast<unsigned>(idx + 1), static_cast<unsigned>(s.rooms.size()));
    int w = Framebuffer::textWidth(cnt);
    fb.text(2, 1, truncateGlyphs(r.name, (120 - w) / 6), 1, false);
    fb.text(126 - w, 1, cnt, 1, false);
    fb.text(0, 14, r.hasTemp ? deg(r.temp) : "--.-", 3);
    fb.textFit(0, 40, std::string(tr(kSetpoint)) + " " + deg(r.setpoint), 128);
    std::string st = r.window ? tr(kWindow) : (r.demand ? tr(kHeating) : tr(kIdle));
    if (r.fault) st = "FEHLER";
    fb.textFit(0, 49, st + (r.valveOpen ? " \x02" : ""), 128);
    return;
  }
  bool compact = cfg.mode == DisplayMode::Compact;
  int y0 = compact ? 0 : 11;
  if (!compact) header(fb, tr(kTitleRooms), 2);
  int rows = compact ? 7 : 5;
  size_t n = s.rooms.size();
  size_t first = 0;
  if (n > static_cast<size_t>(rows) && idx >= static_cast<size_t>(rows)) first = idx - rows + 1;
  for (int row = 0; row < rows && first + row < n; ++row) {
    const auto& r = s.rooms[first + row];
    int y = y0 + row * 9;
    fb.textFit(0, y, r.name, 54);
    std::string t = r.hasTemp ? f1(r.temp) : "--.-";
    fb.textRight(84, y, t);
    fb.textRight(110, y, f1(r.setpoint));
    std::string flags;
    if (r.fault) flags = "!";
    else if (r.window) flags = "F";
    else if (r.valveOpen) flags = "\x02";
    else if (r.demand) flags = "+";
    fb.text(116, y, flags);
    if (first + row == idx) fb.invertRect(0, y - 1, 128, 9);
  }
}

void DisplayManager::pageCircuits(Framebuffer& fb, const DisplaySnapshot& s) const {
  header(fb, tr(kTitleCircuits), 3);
  for (int i = 0; i < 16; ++i) {
    int col = i % 8, row = i / 8;
    int x = col * 16, y = 12 + row * 21;
    const auto& c = s.circuits[static_cast<size_t>(i)];
    std::string num = std::to_string(i + 1);
    if (!c.used) {
      fb.text(x + 8 - Framebuffer::textWidth(num) / 2, y + 6, num);
      fb.hline(x + 3, y + 16, 10);
      continue;
    }
    fb.rect(x + 1, y, 14, 19);
    if (c.open) fb.fillRect(x + 2, y + 1, 12, 17);
    fb.text(x + 8 - Framebuffer::textWidth(num) / 2, y + 2, num, 1, !c.open);
    if (c.fault) fb.text(x + 5, y + 11, "X", 1, !c.open);
    else if (c.demand) fb.text(x + 5, y + 11, "+", 1, !c.open);
  }
  fb.textFit(0, 56, std::string("\x01=") + tr(kValveOpen) + " +=" + tr(kHeating), 128);
}

void DisplayManager::pageDiag(Framebuffer& fb, const DisplaySnapshot& s) const {
  header(fb, tr(kTitleDiag), 4);
  int y = 11;
  std::string cpu = s.cpuPct >= 0 ? std::to_string(s.cpuPct) + "%" : "n/a";
  fb.textFit(0, y, "CPU " + cpu + "  RAM " + kb(s.freeHeap), 128);
  y += 9;
  fb.textFit(0, y, "PSRAM " + kb(s.freePsram), 128);
  y += 9;
  fb.textFit(0, y, std::string(tr(kUptime)) + " " + uptimeStr(s.uptimeSec), 128);
  y += 9;
  fb.textFit(0, y, "FW " + s.firmware, 128);
  y += 9;
  fb.textFit(0, y, "SD " + s.sd + "  MQTT " + s.mqtt, 128);
  fb.textFit(0, 56, std::string(tr(kNetwork)) + " " + (s.netDetail.empty() ? s.network : s.netDetail), 128);
}

void DisplayManager::pageModel(Framebuffer& fb, const DisplaySnapshot& s) const {
  header(fb, tr(kTitleModel), 5);
  if (s.models.empty()) {
    fb.text(0, 24, tr(kNoModel));
    return;
  }
  const auto& m = s.models[static_cast<size_t>(roomIndex_) % s.models.size()];
  int y = 11;
  fb.textFit(0, y, m.room, 128);
  y += 9;
  fb.textFit(0, y, std::string(tr(kStatus)) + ": " + m.status, 128);
  y += 9;
  char b[40];
  std::snprintf(b, sizeof(b), "%s %+.2f K/h", tr(kTrend), m.trendKh);
  fb.textFit(0, y, b, 128);
  y += 9;
  std::snprintf(b, sizeof(b), "%s %.0f+-%.0f min", tr(kDelay), m.delayMin, m.delayStd);
  fb.textFit(0, y, b, 128);
  y += 9;
  if (m.predValid) std::snprintf(b, sizeof(b), "%s1h %.1f+-%.1f", tr(kForecast), m.predMean, m.predStd);
  else std::snprintf(b, sizeof(b), "%s1h ---", tr(kForecast));
  fb.textFit(0, y, b, 128);
  std::snprintf(b, sizeof(b), "%s %.2f K/h", tr(kGain), m.gain);
  fb.textFit(0, 56, b, 128);
}

// ------------------------------------------------------------------- JSON

void DisplayManager::toJson(JsonObject o, bool runtime) const {
  o["enabled"] = cfg.enabled;
  o["contrast"] = cfg.contrast;
  o["rotation"] = cfg.rotation;
  o["autoCycle"] = cfg.autoCycle;
  o["cycleSec"] = cfg.cycleSec;
  JsonArray p = o["pages"].to<JsonArray>();
  for (bool b : cfg.pages) p.add(b);
  JsonArray r = o["rooms"].to<JsonArray>();
  for (auto& x : cfg.rooms) r.add(x);
  o["timeoutSec"] = cfg.timeoutSec;
  o["mode"] = displayModeName(cfg.mode);
  o["alarmPriority"] = cfg.alarmPriority;
  o["alarmWakes"] = cfg.alarmWakes;
  o["language"] = cfg.language == Language::En ? "en" : "de";
  if (!runtime) return;
  o["page"] = page() + 1;
  o["roomIndex"] = roomIndex_;
  o["screenOn"] = screenOn();
  JsonObject h = o["hw"].to<JsonObject>();
  h["present"] = hw.present;
  h["initialized"] = hw.initialized;
  h["flushes"] = hw.flushes;
  h["errors"] = hw.errors;
  h["reinits"] = hw.reinits;
  h["lastError"] = hw.lastError;
  h["lastFlushUs"] = hw.lastFlushUs;
}

std::string DisplayManager::fromJson(JsonObjectConst o) {
  DisplayConfig c = cfg;
  c.enabled = o["enabled"] | c.enabled;
  int contrast = o["contrast"] | static_cast<int>(c.contrast);
  if (contrast < 0 || contrast > 255) return "Kontrast 0..255";
  c.contrast = static_cast<uint8_t>(contrast);
  c.rotation = o["rotation"] | c.rotation;
  if (c.rotation != 0 && c.rotation != 180) return "Rotation 0 oder 180 Grad (SSD1306 128x64)";
  c.autoCycle = o["autoCycle"] | c.autoCycle;
  c.cycleSec = o["cycleSec"] | c.cycleSec;
  if (c.cycleSec < 2 || c.cycleSec > 600) return "Wechselintervall 2..600 s";
  JsonArrayConst p = o["pages"];
  if (!p.isNull()) {
    int any = 0;
    for (size_t i = 0; i < c.pages.size(); ++i) {
      c.pages[i] = i < p.size() ? (p[i] | true) : true;
      any += c.pages[i];
    }
    if (!any) return "Mindestens eine Seite muss aktiv sein";
  }
  if (!o["rooms"].isNull()) {
    c.rooms.clear();
    for (const char* r : o["rooms"].as<JsonArrayConst>())
      if (r && *r) c.rooms.push_back(r);
  }
  c.timeoutSec = o["timeoutSec"] | c.timeoutSec;
  if (c.timeoutSec != 0 && c.timeoutSec < 10) return "Timeout 0 oder >= 10 s";
  c.mode = displayModeFromName(o["mode"] | displayModeName(c.mode));
  c.alarmPriority = o["alarmPriority"] | c.alarmPriority;
  if (c.alarmPriority > 3) return "Alarmprioritaet 0..3";
  c.alarmWakes = o["alarmWakes"] | c.alarmWakes;
  c.language = std::strcmp(o["language"] | (c.language == Language::En ? "en" : "de"), "en") == 0 ? Language::En
                                                                                                    : Language::De;
  cfg = c;
  ++revision_;
  return std::string();
}

}  // namespace hc
