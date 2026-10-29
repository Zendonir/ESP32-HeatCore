#include <random>

#include "TestFramework.h"
#include "hc/rf/RfReceiver.h"
#include "hc/rf/RfTransmitter.h"

using namespace hc;
using namespace hc::rf;

namespace {

struct Rig {
  ManualClock clock;
  EventBus bus{128, 64};
  RfRegistry reg;
  RfReceiver rx{reg, bus, clock};

  // Plays a transmission with realistic timing: frames arrive while the
  // clock advances, then the line goes idle.
  void press(uint32_t code, int frames, uint16_t T = 350, Millis holdExtraMs = 0) {
    auto tx = encodeP1Transmission(code, 24, T, static_cast<uint8_t>(frames));
    Millis frameMs = (32 + 96) * T / 1000;
    size_t perFrame = 50;
    for (size_t i = 0; i < tx.size(); i += perFrame) {
      size_t n = std::min(perFrame, tx.size() - i);
      rx.feed(&tx[i], n);
      clock.advance(frameMs);
      rx.tick();
    }
    rx.idle();
    clock.advance(holdExtraMs);
    rx.tick();
  }
  void wait(Millis ms) {
    for (Millis t = 0; t < ms; t += 50) {
      clock.advance(50);
      rx.tick();
    }
  }
  std::vector<Event> drain() {
    std::vector<Event> v;
    Event e;
    while (bus.pop(e)) v.push_back(e);
    return v;
  }
  uint16_t addRemote(uint32_t base) {
    RxDevice d;
    d.name = "Fernbedienung";
    for (uint8_t i = 0; i < 4; ++i) {
      RxButton b;
      b.index = i + 1;
      b.name = "T" + std::to_string(i + 1);
      b.code = (base << 4) | (1u << i);
      d.buttons.push_back(b);
    }
    uint16_t id = 0;
    std::string err = reg.upsertRx(d, &id);
    CHECK_STR(err, "");
    return id;
  }
};

int countGesture(const std::vector<Event>& ev, Gesture g, int button = -2) {
  int n = 0;
  for (auto& e : ev)
    if (e.type == EventType::InputGesture && e.gesture == g && (button == -2 || e.b == button)) ++n;
  return n;
}

}  // namespace

TEST(rf_rx_short_press_known_device) {
  Rig r;
  uint16_t id = r.addRemote(0xABCDE);
  r.press((0xABCDEu << 4) | 0x2, 6);
  r.wait(400);
  auto ev = r.drain();
  CHECK_EQ(countGesture(ev, Gesture::Press, 2), 1);
  CHECK_EQ(countGesture(ev, Gesture::Short, 2), 1);
  CHECK_EQ(countGesture(ev, Gesture::Release, 2), 1);
  CHECK_EQ(countGesture(ev, Gesture::Long), 0);
  const RxDevice* d = r.reg.rxDevice(id);
  CHECK_EQ(d->stats.presses, 1u);
  CHECK(d->stats.frames >= 5);
  CHECK(d->button(2)->count == 1);
  CHECK(d->reliability() > 50);
}

TEST(rf_rx_long_press) {
  Rig r;
  r.addRemote(0x11111);
  // ~40 frames x 45 ms = 1.8 s hold
  r.press((0x11111u << 4) | 0x1, 40);
  r.wait(400);
  auto ev = r.drain();
  CHECK_EQ(countGesture(ev, Gesture::Long, 1), 1);
  CHECK_EQ(countGesture(ev, Gesture::Short, 1), 0);
  CHECK_EQ(countGesture(ev, Gesture::Release, 1), 1);
}

TEST(rf_rx_single_frame_is_rejected) {
  Rig r;
  r.addRemote(0x22222);
  r.press((0x22222u << 4) | 0x4, 1);  // one frame only: below minRepeats=2
  r.wait(400);
  auto ev = r.drain();
  CHECK_EQ(countGesture(ev, Gesture::Press), 0);
  CHECK_EQ(r.rx.stats().rejected, 1u);
}

TEST(rf_rx_unknown_code_never_triggers_input) {
  Rig r;
  r.addRemote(0x33333);
  r.press(0x5A5A5A, 6);
  r.wait(400);
  auto ev = r.drain();
  int inputs = 0, unknown = 0;
  for (auto& e : ev) {
    if (e.type == EventType::InputGesture) ++inputs;
    if (e.type == EventType::RfUnknownCode) ++unknown;
  }
  CHECK_EQ(inputs, 0);
  CHECK_EQ(unknown, 1);
  CHECK_EQ(r.rx.unknownCodes().size(), 1u);
  CHECK_EQ(r.rx.unknownCodes()[0].code, 0x5A5A5Au);
  // after ignoring, the code is silent
  CHECK_STR(r.reg.addIgnored(0x5A5A5A, 24, "Nachbar"), "");
  r.press(0x5A5A5A, 6);
  r.wait(400);
  ev = r.drain();
  CHECK_EQ(ev.size(), 0u);
  CHECK(r.rx.stats().ignored >= 5);
}

TEST(rf_rx_disabled_device_and_timing_mismatch) {
  Rig r;
  uint16_t id = r.addRemote(0x44444);
  r.reg.rxDevice(id)->enabled = false;
  r.press((0x44444u << 4) | 0x1, 6);
  r.wait(400);
  CHECK_EQ(r.drain().size(), 0u);
  r.reg.rxDevice(id)->enabled = true;
  // same code, but sent with T = 150 us instead of 350 -> rejected
  r.press((0x44444u << 4) | 0x1, 6, 150);
  r.wait(400);
  CHECK_EQ(countGesture(r.drain(), Gesture::Press), 0);
  CHECK(r.rx.stats().timingMismatch >= 5);
}

TEST(rf_rx_debounce_retrigger) {
  Rig r;
  r.addRemote(0x55555);
  uint32_t code = (0x55555u << 4) | 0x8;
  r.press(code, 5);
  r.wait(260);  // release after 250 ms
  // second burst inside retrigger window (300 ms after release) -> ignored
  r.press(code, 3);
  r.wait(400);
  auto ev = r.drain();
  CHECK_EQ(countGesture(ev, Gesture::Press, 4), 1);
  CHECK(r.rx.stats().debounced > 0);
}

TEST(rf_rx_contact_state_and_supervision) {
  Rig r;
  r.clock.epochBase = 1767225600;
  RxDevice d;
  d.name = "Fenster Bad";
  d.function = InputFunction::Window;
  d.room = "bad";
  d.supervisionSec = 600;
  d.buttons.push_back({1, "offen", 0xC0FFEA, ButtonRole::StateOn, {}});
  d.buttons.push_back({2, "zu", 0xC0FFE6, ButtonRole::StateOff, {}});
  uint16_t id = 0;
  CHECK_STR(r.reg.upsertRx(d, &id), "");
  r.press(0xC0FFEA, 8);
  r.wait(300);
  r.press(0xC0FFEA, 8);  // repeated "open" -> no second change event
  r.wait(300);
  auto ev = r.drain();
  CHECK_EQ(countGesture(ev, Gesture::Open), 1);
  CHECK_EQ(r.reg.rxDevice(id)->state, 1);
  r.press(0xC0FFE6, 8);
  r.wait(300);
  CHECK_EQ(countGesture(r.drain(), Gesture::Closed), 1);
  // no telegram for > 10 min -> unavailable once
  r.clock.advance(601 * 1000);
  r.rx.tick();
  r.rx.tick();
  ev = r.drain();
  CHECK_EQ(countGesture(ev, Gesture::Unavailable), 1);
  CHECK(!r.reg.rxDevice(id)->available);
  r.press(0xC0FFE6, 4);
  r.wait(300);
  CHECK_EQ(countGesture(r.drain(), Gesture::Available), 1);
}

TEST(rf_rx_learn_mode_candidates) {
  Rig r;
  r.rx.startLearn(60);
  r.press(0x0A0B0C, 6);
  r.wait(400);
  r.press(0x0A0B0C, 6);
  r.wait(400);
  r.press(0x0D0E0F, 1);  // noise-like single frame
  r.wait(400);
  CHECK(r.rx.learning());
  auto& c = r.rx.candidates();
  CHECK_EQ(c.size(), 2u);
  const LearnCandidate* good = nullptr;
  for (auto& x : c)
    if (x.code == 0x0A0B0C) good = &x;
  CHECK(good != nullptr);
  if (good) {
    CHECK_EQ(good->presses, 2u);
    CHECK(good->reliable());
    CHECK_NEAR(good->pulseUs, 350, 10);
  }
  for (auto& x : c)
    if (x.code == 0x0D0E0F) CHECK(!x.reliable());
  r.wait(61000);
  CHECK(!r.rx.learning());
}

TEST(rf_rx_suppression_counts_echo) {
  Rig r;
  r.addRemote(0x66666);
  uint32_t code = (0x66666u << 4) | 1;
  r.rx.suppressFor(2000, code, 24);
  r.press(code, 4);
  auto ev = r.drain();
  CHECK_EQ(countGesture(ev, Gesture::Press), 0);
  CHECK(r.rx.stats().echoes >= 3);
}

TEST(rf_registry_validation) {
  RfRegistry reg;
  RxDevice d;
  d.name = "A";
  d.buttons.push_back({1, "x", 0x123456, ButtonRole::Button, {}});
  CHECK_STR(reg.upsertRx(d), "");
  RxDevice e;
  e.name = "B";
  e.buttons.push_back({1, "y", 0x123456, ButtonRole::Button, {}});
  CHECK(!reg.upsertRx(e).empty());  // code already used
  RxDevice f;
  f.name = "C";
  f.function = InputFunction::Window;
  f.buttons.push_back({1, "z", 0x654321, ButtonRole::StateOn, {}});
  CHECK(!reg.upsertRx(f).empty());  // window function without room
  f.room = "kueche";
  CHECK_STR(reg.upsertRx(f), "");
  // JSON round trip keeps devices and bindings
  JsonDocument doc;
  reg.rx[0].buttons[0].bindings.push_back({Gesture::Short, {Action{ActionType::AdjustSetpoint, "wohnen", "", "", 0.5f}}});
  reg.toJson(doc.to<JsonObject>());
  RfRegistry reg2;
  CHECK_STR(reg2.fromJson(doc.as<JsonObjectConst>()), "");
  CHECK_EQ(reg2.rx.size(), 2u);
  CHECK_EQ(reg2.rx[0].buttons[0].bindings.size(), 1u);
  CHECK(reg2.rx[0].buttons[0].bindings[0].actions[0].type == ActionType::AdjustSetpoint);
  CHECK_NEAR(reg2.rx[0].buttons[0].bindings[0].actions[0].value, 0.5, 1e-6);
  CHECK(reg2.findByCode(0x654321, 24).device != nullptr);
}

// ------------------------------------------------------------ transmitter

namespace {
struct FakeBackend : ITxBackend {
  std::vector<std::vector<Pulse>> sent;
  bool fail = false;
  bool ready() const override { return true; }
  bool transmit(const std::vector<Pulse>& p, uint32_t) override {
    if (fail) return false;
    sent.push_back(p);
    return true;
  }
};

void runTx(RfTransmitter& tx, FakeBackend& be, ManualClock& clock, int steps = 50) {
  for (int i = 0; i < steps; ++i) {
    TxJob j;
    std::vector<Pulse> p;
    if (tx.takeJob(j, p)) {
      bool ok = be.transmit(p, 1000);
      tx.complete(j, ok, transmissionDurationUs(p) / 1000, ok ? nullptr : "driver");
    }
    clock.advance(100);
  }
}

TxDevice makeTx() {
  TxDevice d;
  d.name = "Steckdose";
  d.group = "licht";
  d.repeats = 6;
  TxCommand on{0, "an", 0xF0F0F1, 24, 0, true, true};
  TxCommand off{0, "aus", 0xF0F0F2, 24, 0, false, false};
  d.commands = {on, off};
  return d;
}
}  // namespace

TEST(rf_tx_sends_and_decodes) {
  ManualClock clock;
  EventBus bus;
  RfRegistry reg;
  RfTransmitter tx(reg, bus, clock);
  FakeBackend be;
  uint16_t id = 0;
  CHECK_STR(reg.upsertTx(makeTx(), &id), "");
  CHECK_STR(tx.enqueue(std::to_string(id) + "/1", EventSource::Web, 0, 0, false), "");
  runTx(tx, be, clock);
  CHECK_EQ(be.sent.size(), 1u);
  // the generated waveform decodes to the configured code
  CodecConfig cfg;
  struct Ctx {
    CodecConfig* cfg;
    int ok = 0;
  } ctx{&cfg};
  Segmenter seg(cfg);
  seg.setConsumer(
      [](void* c, const Pulse* p, size_t n, uint32_t lead, uint32_t) {
        auto* x = static_cast<Ctx*>(c);
        DecodedFrame f;
        if (decodeP1Segment(*x->cfg, p, n, lead, f) && f.code == 0xF0F0F1) ++x->ok;
      },
      &ctx);
  seg.feed(be.sent[0].data(), be.sent[0].size());
  seg.flushIdle();
  CHECK_EQ(ctx.ok, 6);
  CHECK_EQ(reg.txDevice(id)->command(1)->sent, 1u);
  Event e;
  bool sentEvt = false;
  while (bus.pop(e))
    if (e.type == EventType::RfSent) sentEvt = true;
  CHECK(sentEvt);
}

TEST(rf_tx_automation_permission_and_groups) {
  ManualClock clock;
  EventBus bus;
  RfRegistry reg;
  RfTransmitter tx(reg, bus, clock);
  uint16_t id = 0;
  reg.upsertTx(makeTx(), &id);
  // "aus" is not allowed for automations
  CHECK(!tx.enqueue(std::to_string(id) + "/2", EventSource::Automation, 1, 7, true).empty());
  CHECK_STR(tx.enqueue(std::to_string(id) + "/2", EventSource::Web, 0, 0, false), "");
  CHECK_STR(tx.enqueue("group:licht/an", EventSource::Automation, 1, 7, true), "");
  CHECK_EQ(tx.queueLength(), 2u);
  CHECK(!tx.enqueue("group:fehlt/an", EventSource::Web, 0, 0, false).empty());
}

TEST(rf_tx_duty_cycle_and_timeout) {
  ManualClock clock;
  EventBus bus;
  RfRegistry reg;
  RfTransmitter tx(reg, bus, clock);
  FakeBackend be;
  tx.config().dutyCyclePct = 0.01f;  // 360 ms per hour
  uint16_t id = 0;
  TxDevice d = makeTx();
  d.repeats = 20;
  reg.upsertTx(d, &id);
  // each job ~ 20 frames * 45 ms = 0.9 s > budget -> rejected
  CHECK_STR(tx.enqueue(std::to_string(id) + "/1", EventSource::Web, 0, 0, false), "");
  runTx(tx, be, clock);
  CHECK_EQ(be.sent.size(), 0u);
  CHECK_EQ(tx.stats().rejectedDuty, 1u);
  // timeout in queue
  tx.config().dutyCyclePct = 10.f;
  tx.config().defaultTimeoutMs = 200;
  tx.enqueue(std::to_string(id) + "/1", EventSource::Web, 0, 0, false);
  clock.advance(1000);
  runTx(tx, be, clock, 2);
  CHECK_EQ(tx.stats().rejectedTimeout, 1u);
  // queue limit
  tx.config().defaultTimeoutMs = 5000;
  for (int i = 0; i < 8; ++i) tx.enqueue(std::to_string(id) + "/1", EventSource::Web, 0, 0, false);
  CHECK(!tx.enqueue(std::to_string(id) + "/1", EventSource::Web, 0, 0, false).empty());
}

TEST(rf_tx_min_interval_spacing) {
  ManualClock clock;
  EventBus bus;
  RfRegistry reg;
  RfTransmitter tx(reg, bus, clock);
  FakeBackend be;
  tx.config().dutyCyclePct = 10.f;
  tx.config().minIntervalMs = 1000;
  uint16_t id = 0;
  reg.upsertTx(makeTx(), &id);
  tx.enqueue(std::to_string(id) + "/1", EventSource::Web, 0, 0, false);
  tx.enqueue(std::to_string(id) + "/1", EventSource::Web, 0, 0, false);
  runTx(tx, be, clock, 5);  // 500 ms
  CHECK_EQ(be.sent.size(), 1u);
  runTx(tx, be, clock, 10);
  CHECK_EQ(be.sent.size(), 2u);
}

// --------------------------------------------------------------- sensors

TEST(rf_nexus_decoder_with_repeat_confirmation) {
  RfSensorHub hub;
  NexusThDecoder nexus;
  hub.addDecoder(&nexus, true);
  std::vector<SensorReading> got;
  hub.setListener([](void* c, const SensorReading& r) { static_cast<std::vector<SensorReading>*>(c)->push_back(r); },
                  &got);
  CodecConfig cfg;
  Segmenter seg(cfg);
  struct Ctx {
    RfSensorHub* hub;
    Millis now;
  } ctx{&hub, 1000};
  seg.setConsumer([](void* c, const Pulse* p, size_t n, uint32_t, uint32_t) {
    auto* x = static_cast<Ctx*>(c);
    x->hub->onSegment(p, n, x->now);
  }, &ctx);
  auto row = NexusThDecoder::encodeRow(0x5C, true, 2, -7.3f, 64);
  // one row alone is not enough
  seg.feed(row.data(), row.size());
  CHECK_EQ(got.size(), 0u);
  for (int i = 0; i < 10; ++i) seg.feed(row.data(), row.size());
  CHECK_EQ(got.size(), 1u);  // deduplicated burst
  if (!got.empty()) {
    CHECK_EQ(got[0].id, 0x5Cu);
    CHECK_EQ(got[0].channel, 2);
    CHECK_NEAR(got[0].temperatureC, -7.3, 0.051);
    CHECK(got[0].hasHumidity);
    CHECK_NEAR(got[0].humidity, 64, 0.1);
    CHECK_EQ(got[0].batteryOk, 1);
    CHECK_STR(got[0].key(), "rf:nexus_th:92:2");
  }
  // clear bit 25 (inside the constant nibble 1111 at bits 24..27) -> rejected
  auto bad = NexusThDecoder::encodeRow(0x5C, true, 2, 21.0f, 40);
  bad[2 * 25 + 1].us = 1000;
  SensorReading r;
  uint64_t bits;
  uint8_t len;
  std::vector<Pulse> rowOnly(bad.begin(), bad.end() - 1);
  CHECK(!nexus.decodeRow(rowOnly.data(), rowOnly.size(), r, bits, len));
  auto good = NexusThDecoder::encodeRow(0x5C, false, 1, 21.0f, 0);
  std::vector<Pulse> goodRow(good.begin(), good.end() - 1);
  CHECK(nexus.decodeRow(goodRow.data(), goodRow.size(), r, bits, len));
  CHECK(!r.hasHumidity);
  CHECK_EQ(r.batteryOk, 0);
}

TEST(rf_nexus_disabled_by_default_flag) {
  NexusThDecoder n;
  CHECK(!n.hardwareVerified());
}

TEST(rf_rtl433_json_ingest) {
  SensorReading r;
  std::string err;
  CHECK(parseRtl433Json(R"({"time":"2026-10-29 10:00:00","model":"Nexus-TH","id":92,"channel":2,"battery_ok":1,"temperature_C":21.4,"humidity":48})",
                        r, err));
  CHECK_STR(r.key(), "rf:rtl433_Nexus-TH:92:2");
  CHECK_NEAR(r.temperatureC, 21.4, 1e-4);
  CHECK(r.hasHumidity);
  CHECK(!parseRtl433Json(R"({"model":"Acurite-Tower"})", r, err));
  CHECK(!parseRtl433Json("kaputt", r, err));
  CHECK(parseRtl433Json(R"({"model":"X","id":1,"temperature_F":68})", r, err));
  CHECK_NEAR(r.temperatureC, 20.0, 1e-3);
}
