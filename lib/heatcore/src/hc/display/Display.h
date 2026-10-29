#pragma once
// OLED display logic: configuration, page navigation, timeout, alarm
// priority and rendering of the five pages into a Framebuffer. The display is
// optional: nothing in the heating control depends on it.
#include <ArduinoJson.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "hc/core/Alarms.h"
#include "hc/core/Clock.h"
#include "hc/core/Event.h"
#include "hc/display/Framebuffer.h"

namespace hc {

enum class DisplayPage : uint8_t { System = 0, Rooms, Circuits, Diagnostics, Model, Count };
constexpr int kNumPages = static_cast<int>(DisplayPage::Count);
enum class DisplayMode : uint8_t { Standard = 0, Large, Compact };
enum class Language : uint8_t { De = 0, En };

const char* displayModeName(DisplayMode m);
DisplayMode displayModeFromName(const char* s);

struct DisplayConfig {
  bool enabled = true;
  uint8_t contrast = 128;
  uint16_t rotation = 0;  // 0 or 180 (the SSD1306 supports these natively)
  bool autoCycle = true;
  uint16_t cycleSec = 8;
  std::array<bool, kNumPages> pages{{true, true, true, true, true}};
  std::vector<std::string> rooms;  // empty = all rooms with showOnDisplay
  uint16_t timeoutSec = 300;       // 0 = always on
  DisplayMode mode = DisplayMode::Standard;
  uint8_t alarmPriority = 1;       // min. alarm level shown (0 info, 1 warning, 2 critical, 3 never)
  bool alarmWakes = true;
  Language language = Language::De;
};

struct DisplaySnapshot {
  // page 1
  std::string deviceName = "HeatCore";
  std::string ip;
  std::string network;  // "ETH", "WLAN", "AP", "offline"
  bool timeValid = false;
  std::string time, date;
  int activeCircuits = 0;
  int sensorsOk = 0, sensorsTotal = 0;
  std::vector<Alarm> alarms;
  // page 2
  struct RoomLine {
    std::string id, name;
    bool hasTemp = false;
    float temp = 0.f;
    float setpoint = 0.f;
    bool demand = false;
    bool valveOpen = false;
    bool window = false;
    bool fault = false;
    std::string reason;
  };
  std::vector<RoomLine> rooms;
  // page 3
  struct CircuitCell {
    bool used = false;
    bool open = false;
    bool demand = false;
    bool fault = false;
  };
  std::array<CircuitCell, 16> circuits{};
  // page 4
  int cpuPct = -1;
  uint32_t freeHeap = 0, freePsram = 0;
  uint32_t uptimeSec = 0;
  std::string firmware;
  std::string sd = "-";
  std::string mqtt = "aus";
  std::string netDetail;
  // page 5
  struct ModelInfo {
    std::string room;
    std::string status;
    float trendKh = 0.f;
    float delayMin = 0.f, delayStd = 0.f;
    bool predValid = false;
    float predMean = 0.f, predStd = 0.f;
    float gain = 0.f;
  };
  std::vector<ModelInfo> models;
};

struct DisplayHwStatus {
  bool present = false;
  bool initialized = false;
  uint32_t flushes = 0;
  uint32_t errors = 0;
  uint32_t reinits = 0;
  std::string lastError;
  uint32_t lastFlushUs = 0;
};

class DisplayManager {
 public:
  DisplayManager(EventBus& bus, const IClock& clock) : bus_(bus), clock_(clock) {}

  DisplayConfig cfg;
  DisplayHwStatus hw;

  // navigation (any source: DI, RF, web, MQTT, automation)
  void next();
  void prev();
  bool select(int page1Based);
  void roomNext();
  void wake();
  void tick(Millis now, AlarmLevel highestAlarm, size_t alarmCount);

  int page() const { return static_cast<int>(page_); }
  int roomIndex() const { return roomIndex_; }
  bool screenOn() const { return cfg.enabled && on_; }
  // id of the room currently highlighted on the room page (for "displayed room" actions)
  std::string currentRoom(const DisplaySnapshot& s) const;
  void setRoomCount(int n) { roomCount_ = n; }

  void render(Framebuffer& fb, const DisplaySnapshot& s) const;
  void renderPage(DisplayPage p, Framebuffer& fb, const DisplaySnapshot& s) const;

  void toJson(JsonObject o, bool runtime) const;
  std::string fromJson(JsonObjectConst o);
  uint32_t revision() const { return revision_; }

 private:
  void activity();
  void setPage(DisplayPage p, bool fromUser);
  bool pageEnabled(int p) const { return cfg.pages[static_cast<size_t>(p)]; }
  const char* tr(int key) const;
  void header(Framebuffer& fb, const char* title, int pageNo) const;
  void alarmBanner(Framebuffer& fb, const DisplaySnapshot& s) const;
  void pageSystem(Framebuffer& fb, const DisplaySnapshot& s) const;
  void pageRooms(Framebuffer& fb, const DisplaySnapshot& s) const;
  void pageCircuits(Framebuffer& fb, const DisplaySnapshot& s) const;
  void pageDiag(Framebuffer& fb, const DisplaySnapshot& s) const;
  void pageModel(Framebuffer& fb, const DisplaySnapshot& s) const;

  EventBus& bus_;
  const IClock& clock_;
  DisplayPage page_ = DisplayPage::System;
  int roomIndex_ = 0;
  int roomCount_ = 0;
  bool on_ = true;
  Millis lastActivity_ = 0;
  Millis lastCycle_ = 0;
  size_t lastAlarmCount_ = 0;
  uint32_t revision_ = 0;
};

}  // namespace hc
