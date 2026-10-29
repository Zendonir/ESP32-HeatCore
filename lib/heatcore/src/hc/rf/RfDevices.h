#pragma once
// Persistent registry of learned RF433 receive devices, ignored codes and
// transmitter devices/commands.
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hc/automation/Action.h"
#include "hc/core/Clock.h"
#include "hc/rf/RfCodec.h"

namespace hc {
namespace rf {

// What a button code means.
//  Button   - momentary key: press / short / long / release gestures
//  StateOn  - code that switches the device state on  (gesture "open")
//  StateOff - code that switches the device state off (gesture "closed")
enum class ButtonRole : uint8_t { Button = 0, StateOn, StateOff };
const char* buttonRoleName(ButtonRole r);
ButtonRole buttonRoleFromName(const char* s);

// Meaning of the on/off state of a device for the heating logic. The same
// functions are available for wired digital inputs.
enum class InputFunction : uint8_t { None = 0, Window, Presence, HeatDemand };
const char* inputFunctionName(InputFunction f);
InputFunction inputFunctionFromName(const char* s);

struct RxButton {
  uint8_t index = 0;
  std::string name;
  uint32_t code = 0;
  ButtonRole role = ButtonRole::Button;
  std::vector<InputBinding> bindings;
  // statistics (persisted)
  uint32_t count = 0;
  int64_t lastEpoch = 0;
  // runtime
  Millis lastReleaseMs = 0;
  bool everReleased = false;
};

struct RxDeviceStats {
  uint32_t frames = 0;
  uint32_t presses = 0;
  uint32_t rejected = 0;        // presses with fewer frames than minRepeats
  uint32_t timingMismatch = 0;  // code matched but pulse length implausible
  int64_t lastEpoch = 0;
  Millis lastSeenMs = 0;
  bool seen = false;
  float qualityAvg = 0.f;       // EMA of frame timing quality
  float framesPerPress = 0.f;   // EMA
  float pulseUsAvg = 0.f;
};

struct RxDevice {
  uint16_t id = 0;
  std::string name;
  Protocol protocol = Protocol::Ev1527;
  uint8_t bits = 24;
  uint16_t pulseUs = 350;
  uint32_t address = 0;
  bool enabled = true;
  InputFunction function = InputFunction::None;
  std::string room;
  uint8_t minRepeats = 2;
  uint16_t longPressMs = 1000;
  uint16_t releaseMs = 250;
  uint16_t retriggerMs = 300;
  uint32_t supervisionSec = 0;  // 0 = device sends no heartbeat, no supervision
  bool failsafeOn = false;      // state assumed when supervision times out
  uint32_t autoOffSec = 0;      // single-code sensors: state returns to off
  std::vector<RxButton> buttons;
  RxDeviceStats stats;
  // runtime state
  int8_t state = -1;            // -1 unknown, 0 off, 1 on
  bool available = true;
  Millis stateOnMs = 0;

  RxButton* button(uint8_t idx);
  const RxButton* button(uint8_t idx) const;
  // 0..100 combined reliability (timing quality and repeat count)
  uint8_t reliability() const;
};

struct IgnoredCode {
  uint32_t code = 0;
  uint8_t bits = 24;
  std::string note;
};

struct TxCommand {
  uint16_t id = 0;
  std::string name;
  uint32_t code = 0;
  uint8_t bits = 24;
  uint8_t repeats = 0;  // 0 = device default
  bool automationAllowed = true;
  bool haExposed = false;
  // stats
  uint32_t sent = 0;
  uint32_t failed = 0;
  int64_t lastEpoch = 0;
};

struct TxDevice {
  uint16_t id = 0;
  std::string name;
  std::string group;
  Protocol protocol = Protocol::Ev1527;
  uint16_t pulseUs = 350;
  uint8_t repeats = 8;
  bool enabled = true;
  std::vector<TxCommand> commands;
  TxCommand* command(uint16_t cid);
  const TxCommand* command(uint16_t cid) const;
};

struct ButtonRef {
  RxDevice* device = nullptr;
  RxButton* button = nullptr;
};

class RfRegistry {
 public:
  static constexpr size_t kMaxRxDevices = 48;
  static constexpr size_t kMaxButtonsPerDevice = 16;
  static constexpr size_t kMaxTxDevices = 32;
  static constexpr size_t kMaxIgnored = 64;
  static constexpr uint8_t kMaxRepeats = 20;

  std::vector<RxDevice> rx;
  std::vector<TxDevice> tx;
  std::vector<IgnoredCode> ignored;

  RxDevice* rxDevice(uint16_t id);
  const RxDevice* rxDevice(uint16_t id) const;
  TxDevice* txDevice(uint16_t id);
  const TxDevice* txDevice(uint16_t id) const;
  ButtonRef findByCode(uint32_t code, uint8_t bits);
  bool isIgnored(uint32_t code, uint8_t bits) const;
  // Owner of a code other than (exceptDevice); empty name if free.
  bool codeInUse(uint32_t code, uint8_t bits, uint16_t exceptDevice, std::string* owner = nullptr) const;

  // Validates and inserts/replaces. Returns empty string or error message.
  std::string upsertRx(RxDevice d, uint16_t* assignedId = nullptr);
  bool removeRx(uint16_t id);
  std::string upsertTx(TxDevice d, uint16_t* assignedId = nullptr);
  bool removeTx(uint16_t id);
  std::string addIgnored(uint32_t code, uint8_t bits, const std::string& note);
  bool removeIgnored(uint32_t code, uint8_t bits);

  // Resolves "<dev>/<cmd>" or "group:<name>/<commandName>" to commands.
  struct TxTarget {
    const TxDevice* device;
    const TxCommand* command;
  };
  std::vector<TxTarget> resolveTx(const std::string& ref, bool forAutomation, std::string* err = nullptr) const;
  std::vector<std::string> groups() const;

  // Persistence
  void toJson(JsonObject o, bool includeStats = true) const;
  std::string fromJson(JsonObjectConst o);
  static void rxDeviceToJson(const RxDevice& d, JsonObject o, bool includeStats);
  static std::string rxDeviceFromJson(RxDevice& d, JsonObjectConst o);
  static void txDeviceToJson(const TxDevice& d, JsonObject o);
  static std::string txDeviceFromJson(TxDevice& d, JsonObjectConst o);

  uint32_t revision() const { return revision_; }
  void touch() { ++revision_; }

 private:
  uint16_t nextRxId() const;
  uint16_t nextTxId() const;
  uint32_t revision_ = 0;
};

std::string formatCode(uint32_t code, uint8_t bits);  // "0xA5C3E1"
bool parseCode(const char* s, uint32_t& out);

}  // namespace rf
}  // namespace hc
