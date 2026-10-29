#pragma once
// Simulation rig: a complete HeatCoreSystem plus a simple thermal plant per
// room, RF transmission helper and JSON helpers.
#include <cmath>
#include <map>
#include <string>

#include "TestFramework.h"
#include "hc/rf/RfCodec.h"
#include "hc/system/HeatCoreSystem.h"

namespace sim {

using namespace hc;

struct Plant {
  float T = 18.f;
  float gain = 4.0f;      // K/h at full valve
  float loss = 0.12f;     // 1/h towards outdoor
  float outdoor = 2.f;
  float windowLoss = 2.0f;  // additional 1/h while window is open
  int delayMin = 20;
  bool windowOpen = false;
  bool valveBroken = false;
  std::deque<float> u;  // valve history, one per minute
  void step(float valve, float minutes = 1.f) {
    u.push_back(valveBroken ? 0.f : valve);
    float ud = u.size() > static_cast<size_t>(delayMin) ? u[u.size() - 1 - delayMin] : 0.f;
    if (u.size() > 300) u.pop_front();
    float l = loss + (windowOpen ? windowLoss : 0.f);
    T += minutes / 60.f * (gain * ud - l * (T - outdoor));
  }
};

class Rig {
 public:
  ManualClock clock;
  HeatCoreSystem sys{clock};
  std::map<std::string, Plant> plants;  // room id -> plant
  std::map<std::string, std::string> sensorOf;

  Rig() {
    clock.epochBase = 1767571200;  // 2026-01-05 00:00 UTC, Monday
    clock.ms = 1000;
  }

  void load(ConfigSection s, const char* json) {
    JsonDocument d;
    auto e = deserializeJson(d, json);
    CHECK(!e);
    std::string err = sys.sectionFromJson(s, d.as<JsonObjectConst>());
    CHECK_STR(err, "");
  }

  // one room "wohnen" on circuit 1 with sensor ow1:01
  void basicRoom(const char* id = "wohnen", int circuit = 1, const char* sensor = "ow1:01") {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  R"({"rooms":[{"id":"%s","name":"Wohnen","comfort":21,"eco":18,"frost":7,"mode":"comfort","hysteresis":0.4,"groups":["eg"]}],
                    "circuits":[{"n":%d,"room":"%s","minOnSec":60,"minOffSec":60}]})",
                  id, circuit, id);
    load(ConfigSection::Heating, buf);
    std::snprintf(buf, sizeof(buf), R"({"sensors":[{"id":"%s","name":"Fuehler","room":"%s","timeoutSec":300}]})", sensor, id);
    load(ConfigSection::Sensors, buf);
    plants[id] = Plant{};
    sensorOf[id] = sensor;
  }

  bool output(int n) const { return (sys.outputMask() >> (n - 1)) & 1u; }

  // advances simulated time, feeding sensors every minute
  void run(uint32_t ms, bool feedSensors = true) {
    const uint32_t step = 100;
    for (uint32_t t = 0; t < ms; t += step) {
      clock.advance(step);
      if (feedSensors && clock.ms % 60000 < step) {
        for (auto& [room, p] : plants) {
          float valve = 0.f;
          for (int i = 0; i < 16; ++i)
            if (sys.heating.circuits[i].room == room && output(i + 1)) valve = 1.f;
          p.step(valve);
          sys.sensors.update(sensorOf[room], std::round(p.T * 16.f) / 16.f, clock.ms);
        }
      }
      sys.loop();
    }
  }

  void sendRf(uint32_t code, int frames = 6, uint16_t T = 350) {
    auto tx = rf::encodeP1Transmission(code, 24, T, static_cast<uint8_t>(frames));
    for (size_t i = 0; i < tx.size(); i += 50) {
      size_t n = std::min<size_t>(50, tx.size() - i);
      sys.rfRx.feed(&tx[i], n);
      run(45, false);
    }
    sys.rfRx.idle();
    run(400, false);
  }

  void setInputs(uint16_t mask, uint32_t holdMs = 300) {
    sys.reportInputs(mask, true);
    for (uint32_t t = 0; t < holdMs; t += 20) {
      clock.advance(20);
      sys.reportInputs(mask, true);
      sys.loop();
    }
  }
};

}  // namespace sim
