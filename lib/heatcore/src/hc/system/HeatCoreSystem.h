#pragma once
// HeatCoreSystem wires all core modules together. It is platform independent:
// the ESP32 layer feeds hardware samples in and writes outputMask() out; the
// host test suite drives the very same object with a ManualClock.
//
// Threading: every public method must be called with mutex() held (the
// platform tasks and web handlers lock it; tests are single threaded).
#include <ArduinoJson.h>

#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "hc/automation/AutomationEngine.h"
#include "hc/core/Alarms.h"
#include "hc/core/Clock.h"
#include "hc/core/Event.h"
#include "hc/display/Display.h"
#include "hc/heating/Heating.h"
#include "hc/heating/Safety.h"
#include "hc/io/IoManager.h"
#include "hc/model/ThermalModel.h"
#include "hc/stats/History.h"
#include "hc/ai/AiChanges.h"
#include "hc/rf/RfDevices.h"
#include "hc/rf/RfReceiver.h"
#include "hc/rf/RfSensors.h"
#include "hc/rf/RfTransmitter.h"
#include "hc/sensors/SensorManager.h"
#include "hc/system/Config.h"
#include "hc/system/Firmware.h"

namespace hc {

enum class ConfigSection : uint8_t { System = 0, Network, Mqtt, Io, Heating, Sensors, Safety, Rf, Display, Rules, Model, Ai, Count };
const char* configSectionName(ConfigSection s);
bool configSectionFromName(const char* s, ConfigSection& out);

// Firmware slot / update state reported by the ESP32 layer.
struct FirmwareStatus {
  std::string runningPartition;   // "app0" / "app1"
  uint32_t slotSize = 0;          // bytes per OTA slot
  bool pendingVerify = false;     // new image on probation (rollback armed)
  bool rollbackAvailable = false; // other slot holds a valid image
  std::string previousVersion;    // version before the last update
  std::string lastResult;         // result of the last update / rollback
  bool busy = false;              // an update is being written
};

struct WifiNetwork {
  std::string ssid;
  int rssi = 0;
  bool open = false;
  int channel = 0;
};

// Hardware / platform facts reported by the ESP32 layer.
struct PlatformStatus {
  std::string deviceId = "000000";
  std::string ip;
  std::string network;     // "ETH", "WLAN", "AP" or ""
  std::string netDetail;
  std::string netState;    // hc::netStateName of the network policy
  std::string apSsid;      // setup access point name while it is active
  bool wifiScanRunning = false;
  std::vector<WifiNetwork> wifiScan;
  bool ethLink = false;
  bool wifiConnected = false;
  int wifiRssi = 0;
  std::string mqtt = "aus";  // aus | getrennt | verbunden
  bool mqttConnected = false;
  std::string sd = "-";
  uint64_t sdTotalBytes = 0, sdUsedBytes = 0;
  uint32_t freeHeap = 0, minFreeHeap = 0, freePsram = 0, totalPsram = 0;
  int cpuPct = -1;
  uint32_t uptimeSec = 0;
  std::string resetReason;
  bool rtcPresent = false;
  bool rtcValid = false;
  float rtcTempC = 0.f;
  bool timeSynced = false;
  std::vector<uint8_t> i2cDevices;
  uint32_t i2cErrors = 0, i2cRecoveries = 0;
  uint32_t outputWriteErrors = 0;
  uint32_t inputReadErrors = 0;
  bool ioExpandersOk = false;
  uint32_t controlLoopMaxUs = 0;
  std::array<uint8_t, 3> oneWireCount{{0, 0, 0}};
  std::string rs485 = "-";
  uint32_t rs485Lines = 0, rs485Errors = 0;
  bool rfRxHw = false, rfTxHw = false;
  FirmwareStatus firmware;
};

struct SelfTestItem {
  std::string component;
  std::string status;  // ok | fehler | fehlt | unbestaetigt | deaktiviert | laeuft
  std::string detail;
};

class HeatCoreSystem : public IAutomationContext {
 public:
  explicit HeatCoreSystem(const IClock& clock);
  ~HeatCoreSystem() override = default;
  HeatCoreSystem(const HeatCoreSystem&) = delete;
  HeatCoreSystem& operator=(const HeatCoreSystem&) = delete;

  // Recorded statistics (REST and MCP). historyJson returns an error text.
  std::string historyJson(const std::string& series, uint32_t hours, uint16_t resolutionMin, JsonObject o) const;
  void statsSummaryJson(uint16_t days, JsonObject o) const;

  std::recursive_mutex& mutex() { return mtx_; }
  const IClock& clock() const { return clock_; }

  // ---- modules (public for the API and platform layers)
  EventBus bus;
  AlarmManager alarms;
  IoManager io;
  SensorManager sensors;
  HeatingManager heating;
  SafetySupervisor safety;
  rf::RfRegistry rfReg;
  rf::RfReceiver rfRx;
  rf::RfTransmitter rfTx;
  rf::RfSensorHub rfSensors;
  rf::NexusThDecoder nexusDecoder;
  ThermalModelManager model;
  History history;  // recorded operating data (statistics, AI analysis)
  AiState ai;       // MCP access level, token and change journal
  DisplayManager display;
  AutomationEngine automation;

  SystemConfig sysCfg;
  NetworkConfig netCfg;
  MqttConfig mqttCfg;
  PlatformStatus platform;
  std::vector<SelfTestItem> selfTest;

  // ---- hooks installed by the platform (never block!)
  std::function<void(const Event&)> onEvent;
  std::function<void(const std::string& suffix, const std::string& payload, bool retain)> mqttPublish;
  std::function<void(const std::string& message, int level)> notify;
  std::function<std::string(const std::string& cmd, JsonObjectConst args)> platformCommand;

  // ---- lifecycle
  void begin();
  void loop();  // call every ~50..100 ms from the control task
  Millis lastLoopMs() const { return lastLoop_; }

  // ---- hardware interface
  uint16_t outputMask() const { return io.levelMask(); }
  uint16_t safeOutputMask() const;
  void reportOutputWrite(bool ok);
  void reportInputs(uint16_t activeMask, bool ok);
  void reportRtl433Line(const char* line);
  void onRfSensorReading(const rf::SensorReading& r);

  // ---- RF transmit task interface: takes the next job and blinds the
  // receiver for the duration of the own transmission.
  bool takeTxJob(rf::TxJob& job, std::vector<rf::Pulse>& pulses);
  void completeTxJob(const rf::TxJob& job, bool ok, uint32_t airtimeMs, const char* reason);

  // ---- RF loopback self-test (needs transmitter and receiver installed)
  std::string startRfLoopback();
  struct LoopbackResult {
    bool running = false;
    bool done = false;
    bool ok = false;
    uint32_t echoes = 0;
    std::string detail;
  } loopback;

  // ---- display
  void buildDisplaySnapshot(DisplaySnapshot& s) const;
  std::vector<std::string> displayRooms() const;

  // ---- MQTT inbound (generic automation topics "in/<suffix>")
  void mqttInbound(const std::string& suffix, const std::string& payload);
  std::string baseTopic() const;

  // ---- commands (API / MQTT); execute() is the single action executor
  std::string execute(const Action& a, const ActionOrigin& origin) override;
  std::string checkAction(const Action& a, const ActionOrigin& origin) const;
  std::string manualOutput(int n, bool on, uint32_t durationSec, EventSource src);
  void clearManualOutput(int n);
  std::string requestOutput(int n, bool on, uint32_t durationSec, EventSource src);
  std::string runRule(uint16_t id, EventSource src);

  // ---- persistence
  void sectionToJson(ConfigSection s, JsonObject o, bool includeSecrets) const;
  std::string sectionFromJson(ConfigSection s, JsonObjectConst o);
  void markDirty(ConfigSection s, bool important = true);
  // Sections that should be written now (debounced). Clears the flags.
  std::vector<ConfigSection> takeDirty();
  std::string validateBindings(const std::vector<InputBinding>& b, EventSource src) const;

  // ---- IAutomationContext
  Millis now() const override { return clock_.nowMs(); }
  int64_t epoch() const override { return clock_.epoch(); }
  LocalTime localTime() const override { return clock_.local(); }
  bool temperature(const std::string& ref, float& v) const override;
  bool setpoint(const std::string& room, float& v) const override;
  std::string globalMode() const override { return globalModeName(heating.globalMode); }
  std::string roomMode(const std::string& room) const override;
  int presence(const std::string& room) const override;
  bool windowOpen(const std::string& room) const override;
  bool sensorAvailable(const std::string& id) const override;
  int inputState(const std::string& ref) const override;
  bool outputOn(int output) const override;
  bool groupState(const std::string& group, const std::string& state) const override;
  bool refExists(const std::string& kind, const std::string& ref) const override;
  std::string validateAction(const Action& a) const override;
  void systemAlarm(const std::string& code, const std::string& message, bool active) override;

  struct Counters {
    uint32_t events = 0;
    uint32_t controlCycles = 0;
    uint32_t bindingsExecuted = 0;
    uint32_t actionsRejected = 0;
  } counters;

 private:
  void processEvent(const Event& e);
  void handleInputGesture(const Event& e);
  void applyFunction(rf::InputFunction fn, const std::string& room, const std::string& source, bool on,
                     uint32_t demandSec, const Event& e);
  void runBindings(const std::vector<InputBinding>& bindings, const Event& e);
  void controlCycle(Millis now);
  void computeOutputs(Millis now);
  void minuteTasks(Millis now);
  void checkCircuitFaults(Millis now);
  void updateLoopback(Millis now);
  bool outputAllowedFor(int n, EventSource src) const;

  const IClock& clock_;
  std::recursive_mutex mtx_;
  Millis lastLoop_ = 0;
  Millis lastControl_ = 0;
  Millis lastSecond_ = 0;
  int lastMinute_ = -1;
  Millis lastMinuteMs_ = 0;
  bool begun_ = false;
  Millis begunMs_ = 0;
  static constexpr uint32_t kSensorStartupGraceMs = 120000;
  bool outputWriteOk_ = true;
  uint32_t consecutiveWriteErrors_ = 0;
  // per room duty accumulation for the thermal model
  struct DutyAcc {
    std::string room;
    float openSec = 0.f;
    float totalSec = 0.f;
  };
  std::vector<DutyAcc> duty_;
  std::array<bool, kNumOutputs> histCircuitOpen_{};  // for counting valve switch-ons
  // automation/HA timed output requests
  std::array<Millis, kNumOutputs> requestUntil_{};
  std::array<bool, kNumOutputs> requestTimed_{};
  // persistence
  std::array<Millis, static_cast<size_t>(ConfigSection::Count)> dirtySince_{};
  std::array<bool, static_cast<size_t>(ConfigSection::Count)> dirty_{};
  std::array<bool, static_cast<size_t>(ConfigSection::Count)> dirtyImportant_{};
  Millis lastStatsSave_ = 0;
  uint32_t loopbackMark_ = 0;
  Millis loopbackStart_ = 0;
  uint32_t loopbackJob_ = 0;
};

}  // namespace hc
