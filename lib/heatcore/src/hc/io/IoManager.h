#pragma once
// Logical model of the 16 outputs, 16 digital inputs and 4 analog inputs of
// the KC868-A16 V3. Hardware access happens in the platform layer; this
// module only holds configuration, debouncing and gesture detection.
#include <ArduinoJson.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "hc/automation/Action.h"
#include "hc/core/Clock.h"
#include "hc/core/Event.h"
#include "hc/rf/RfDevices.h"

namespace hc {

constexpr int kNumOutputs = 16;
constexpr int kNumInputs = 16;
constexpr int kNumAnalog = 4;

enum class OutputFunction : uint8_t { Circuit = 0, General, Disabled };
const char* outputFunctionName(OutputFunction f);
OutputFunction outputFunctionFromName(const char* s);

struct OutputConfig {
  std::string name;
  OutputFunction function = OutputFunction::Circuit;
  bool automationAllowed = false;  // General outputs only
  bool haAllowed = false;          // General outputs only
  uint32_t maxOnSec = 0;           // General outputs: auto-off (0 = none)
};

struct OutputState {
  bool request = false;       // from owner (controller / automation / web)
  Millis requestOnMs = 0;
  bool manualActive = false;  // time-limited manual override
  bool manualState = false;
  Millis manualUntil = 0;
  EventSource manualSource = EventSource::Web;
  bool level = false;         // final logical state written to hardware
  Millis lastChangeMs = 0;
  bool everChanged = false;
  uint32_t switches = 0;
  uint16_t switchesThisHour = 0;
  Millis hourStart = 0;
  bool hwOk = true;
  std::string blocked;        // reason the request was not applied
};

enum class InputKind : uint8_t { Button = 0, Contact, Disabled };
const char* inputKindName(InputKind k);
InputKind inputKindFromName(const char* s);

struct InputConfig {
  std::string name;
  InputKind kind = InputKind::Button;
  bool invert = false;
  rf::InputFunction function = rf::InputFunction::None;
  std::string room;
  uint16_t debounceMs = 50;
  uint16_t longPressMs = 1000;
  std::vector<InputBinding> bindings;
};

struct InputState {
  bool raw = false;
  bool stable = false;
  bool known = false;
  Millis rawChangeMs = 0;
  Millis pressStartMs = 0;
  bool longSent = false;
  uint32_t edges = 0;
};

struct AnalogConfig {
  std::string name;
  bool enabled = false;
  bool asSensor = false;   // publish as sensor "ai:<n>" in the sensor manager
  float dividerGain = 1.f; // board divider: input voltage = pin voltage * gain
  float scale = 1.f;       // value = volts * scale + offset
  float offset = 0.f;
  std::string unit = "V";
};

struct AnalogState {
  float pinVolts = 0.f;
  float volts = 0.f;
  float value = 0.f;
  bool valid = false;
  Millis lastMs = 0;
};

class IoManager {
 public:
  IoManager(EventBus& bus, const IClock& clock);

  std::array<OutputConfig, kNumOutputs> outCfg;
  std::array<OutputState, kNumOutputs> out;
  std::array<InputConfig, kNumInputs> inCfg;
  std::array<InputState, kNumInputs> in;
  std::array<AnalogConfig, kNumAnalog> aiCfg;
  std::array<AnalogState, kNumAnalog> ai;

  // Input sampling from the platform (bit n = input n+1 electrically active).
  void onInputs(uint16_t activeMask, Millis now);
  void tickInputs(Millis now);  // long-press detection
  bool inputLogical(int idx) const;  // active XOR invert
  void onAnalog(int idx, float pinVolts, Millis now);

  // Outputs: owner request and manual override (1-based helpers in system)
  void setRequest(int idx, bool on, Millis now);
  void setManual(int idx, bool on, Millis until, EventSource src);
  void clearManual(int idx);
  uint16_t levelMask() const;
  // Apply a final level; returns true if it changed.
  bool applyLevel(int idx, bool level, Millis now);

  void toJson(JsonObject o, bool runtime) const;
  std::string fromJson(JsonObjectConst o);

 private:
  void emitInput(int idx, Gesture g, Millis now);
  EventBus& bus_;
  const IClock& clock_;
};

}  // namespace hc
