// HeatCore host simulator: runs the complete platform-independent firmware
// core (same code as on the ESP32) against simulated rooms, sensors and an
// RF "air" channel, and serves the real web interface plus REST API.
//
//   ./heatcore_sim [--port 8080] [--speed 1] [--demo] [--web web/index.html]
//
// Simulation endpoints (not part of the firmware):
//   POST /sim/rf      {"code":"0xA1B2C3","frames":6,"pulseUs":350}
//   POST /sim/nexus   {"id":42,"channel":1,"temp":20.5,"humidity":50}
//   POST /sim/input   {"n":3,"active":true}
//   POST /sim/window  {"room":"wohnen","open":true}   (physical window in the plant)
// No WebSocket: the UI falls back to polling.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "hc/api/ApiRouter.h"
#include "hc/system/Firmware.h"
#include "hc/core/Log.h"
#include "hc/rf/RfCodec.h"
#include "hc/system/HeatCoreSystem.h"

using namespace hc;

namespace {

struct Plant {
  float T = 18.5f;
  float gain = 4.f, loss = 0.12f;
  int delay = 20;
  bool window = false;
  std::deque<float> u;
  void step(float valve, float outdoor) {
    u.push_back(valve);
    if (u.size() > 300) u.pop_front();
    float ud = u.size() > static_cast<size_t>(delay) ? u[u.size() - 1 - delay] : 0.f;
    float l = loss + (window ? 2.f : 0.f);
    T += (gain * ud - l * (T - outdoor)) / 60.f;
  }
};

ManualClock g_clock;
HeatCoreSystem* g_sys;
std::map<std::string, Plant> g_plants;
std::atomic<int> g_speed{1};
std::atomic<bool> g_run{true};
uint16_t g_inputs = 0x0002;  // DI2 = reed contact of the bath window: closed
float g_outdoor = 3.f;

void sinkFn(LogLevel l, const char* tag, const char* msg) { std::fprintf(stderr, "[%s][%s] %s\n", logLevelName(l), tag, msg); }

void loadJson(ConfigSection s, const char* json) {
  JsonDocument d;
  deserializeJson(d, json);
  std::string e = g_sys->sectionFromJson(s, d.as<JsonObjectConst>());
  if (!e.empty()) std::fprintf(stderr, "demo config %s: %s\n", configSectionName(s), e.c_str());
}

void demo() {
  loadJson(ConfigSection::Heating, R"({"rooms":[
    {"id":"wohnen","name":"Wohnzimmer","comfort":21,"eco":18,"frost":7,"mode":"auto","groups":["eg"]},
    {"id":"bad","name":"Bad","comfort":23,"eco":19,"frost":7,"mode":"auto","groups":["og"]},
    {"id":"schlafen","name":"Schlafzimmer","comfort":19,"eco":16,"frost":7,"mode":"auto","groups":["og"]}],
    "circuits":[{"n":1,"name":"HK Wohnen 1","room":"wohnen"},{"n":2,"name":"HK Wohnen 2","room":"wohnen"},
                {"n":3,"name":"HK Bad","room":"bad"},{"n":4,"name":"HK Schlafen","room":"schlafen"}]})");
  loadJson(ConfigSection::Sensors, R"({"sensors":[
    {"id":"ow1:28FF000000000001","name":"Wohnen","room":"wohnen"},
    {"id":"ow1:28FF000000000002","name":"Bad","room":"bad"},
    {"id":"ow1:28FF000000000003","name":"Schlafen","room":"schlafen"},
    {"id":"ow2:28FF000000000010","name":"Aussen","role":"outdoor"},
    {"id":"ow2:28FF000000000011","name":"Vorlauf","role":"flow"}]})");
  loadJson(ConfigSection::Io, R"({"outputs":[{"n":8,"name":"Zirkulationspumpe","function":"general","automationAllowed":true,"haAllowed":true}],
    "inputs":[{"n":1,"name":"Display weiter","kind":"button","bindings":[{"gesture":"short","actions":[{"type":"display_next"}]}]},
              {"n":2,"name":"Fenster Bad","kind":"contact","invert":true,"function":"window","room":"bad"}]})");
  loadJson(ConfigSection::Rf, R"({"rx":[{"id":1,"name":"Fernbedienung Wohnen","protocol":"ev1527","pulseUs":350,
      "buttons":[{"index":1,"name":"Plus","code":"0xA5C3E1","bindings":[{"gesture":"short","actions":[{"type":"adjust_setpoint","room":"wohnen","delta":0.5}]}]},
                 {"index":2,"name":"Minus","code":"0xA5C3E2","bindings":[{"gesture":"short","actions":[{"type":"adjust_setpoint","room":"wohnen","delta":-0.5}]}]}]},
      {"id":2,"name":"Fensterkontakt Schlafen","protocol":"ev1527","pulseUs":330,"function":"window","room":"schlafen","supervisionSec":3600,
      "buttons":[{"index":1,"name":"offen","code":"0x3C5A0A","role":"state_on"},{"index":2,"name":"zu","code":"0x3C5A0E","role":"state_off"}]}],
    "tx":[{"id":1,"name":"Funksteckdose Flur","group":"licht","pulseUs":320,"repeats":6,
      "commands":[{"id":1,"name":"an","code":"0x15A5A1","haExposed":true},{"id":2,"name":"aus","code":"0x15A5A2"}]}]})");
  loadJson(ConfigSection::Rules, R"({"rules":[
    {"id":1,"name":"Absenkbetrieb schaltet Flurlicht aus","enabled":true,"priority":40,
     "triggers":[{"type":"global_mode","state":"eco"}],"actions":[{"type":"rf_send","command":"1/2"}]},
    {"id":2,"name":"Urlaub per Webknopf","enabled":true,"priority":60,
     "triggers":[{"type":"manual"}],"actions":[{"type":"set_global_mode","mode":"vacation","durationSec":604800}]}]})");
  for (auto& r : g_sys->heating.rooms) g_plants[r.id] = Plant{};
  g_plants["bad"].T = 21.f;
}

std::string sensorFor(const std::string& room) {
  for (auto& s : g_sys->sensors.sensors())
    if (s.room == room && s.role == SensorRole::Room) return s.id;
  return "";
}

void simLoop() {
  Millis lastMinute = 0;
  while (g_run) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::lock_guard<std::recursive_mutex> lk(g_sys->mutex());
    for (int k = 0; k < g_speed; ++k) {
      g_clock.advance(50);
      if (elapsed(g_clock.ms, lastMinute) >= 60000) {
        lastMinute = g_clock.ms;
        for (auto& [room, p] : g_plants) {
          float valve = 0.f;
          for (int i = 0; i < kNumOutputs; ++i)
            if (g_sys->heating.circuits[i].room == room &&
                g_sys->io.out[i].level != g_sys->heating.circuitSafeLevel(g_sys->heating.circuits[i]))
              valve = 1.f;
          p.step(valve, g_outdoor);
          std::string sid = sensorFor(room);
          if (!sid.empty()) g_sys->sensors.update(sid, std::round(p.T * 16.f) / 16.f, g_clock.ms);
        }
        g_sys->sensors.update("ow2:28FF000000000010", g_outdoor, g_clock.ms);
        g_sys->sensors.update("ow2:28FF000000000011", 34.f + std::sin(g_clock.ms / 3.6e6f) * 3.f, g_clock.ms);
      }
      g_sys->reportInputs(g_inputs, true);
      g_sys->reportOutputWrite(true);
      g_sys->loop();
      // RF "air": transmitted frames are received by the own receiver
      rf::TxJob job;
      std::vector<rf::Pulse> pulses;
      if (g_sys->takeTxJob(job, pulses)) {
        g_sys->rfRx.feed(pulses.data(), pulses.size());
        g_sys->rfRx.idle();
        g_sys->completeTxJob(job, true, rf::transmissionDurationUs(pulses) / 1000, nullptr);
      }
    }
    auto& p = g_sys->platform;
    p.uptimeSec = g_clock.ms / 1000;
    p.ip = "127.0.0.1";
    p.network = "SIM";
    p.netDetail = "Host-Simulator";
    p.freeHeap = 200000;
    p.freePsram = 7000000;
    p.totalPsram = 8 * 1024 * 1024;
    p.rfRxHw = p.rfTxHw = true;
    p.ioExpandersOk = true;
    p.i2cDevices = {0x21, 0x22, 0x24, 0x25, 0x3C, 0x50, 0x68};
    g_sys->display.hw.present = g_sys->display.hw.initialized = true;
  }
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// POST /api/ota: same image checks as the controller (multipart upload).
ApiResponse simOta(const std::string& head, const std::string& body, const std::map<std::string, std::string>& q) {
  auto fail = [](const std::string& e) { return ApiResponse{422, "{\"error\":\"" + e + "\"}"}; };
  size_t b = head.find("boundary=");
  if (b == std::string::npos) return fail("kein multipart");
  std::string boundary = "--" + head.substr(b + 9, head.find("\r\n", b) - b - 9);
  size_t start = body.find("\r\n\r\n");
  size_t end = body.find("\r\n" + boundary, start == std::string::npos ? 0 : start);
  if (start == std::string::npos || end == std::string::npos) return fail("Datei fehlt");
  start += 4;
  FirmwareImageCheck chk;
  chk.feed(reinterpret_cast<const uint8_t*>(body.data() + start), end - start);
  std::string err;
  auto get = [&](const char* k) { auto it = q.find(k); return it == q.end() ? std::string() : it->second; };
  size_t size = get("size").empty() ? 0 : std::stoul(get("size"));
  FirmwareStatus& fw = g_sys->platform.firmware;
  if (!chk.finish(err, get("sha256"), size)) {
    fw.lastResult = "Update abgebrochen: " + err;
    return fail(err);
  }
  fw.previousVersion = kFirmwareVersion;
  fw.rollbackAvailable = true;
  fw.lastResult = "Firmware " + chk.version() + " installiert (Simulator, SHA-256 " + chk.sha256().substr(0, 12) +
                  "...) - kein Neustart im Simulator";
  return ApiResponse{200, "{\"ok\":true}"};
}

void reply(int fd, int status, const char* type, const std::string& body) {
  char head[256];
  int n = std::snprintf(head, sizeof(head),
                        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
                        "Cache-Control: no-store\r\n\r\n",
                        status, status == 200 ? "OK" : "ERR", type, body.size());
  (void)!write(fd, head, static_cast<size_t>(n));
  (void)!write(fd, body.data(), body.size());
}

std::string simCommand(const std::string& path, const std::string& body) {
  JsonDocument d;
  deserializeJson(d, body);
  std::lock_guard<std::recursive_mutex> lk(g_sys->mutex());
  if (path == "/sim/rf") {
    uint32_t code = 0;
    rf::parseCode(d["code"] | "0", code);
    auto tx = rf::encodeP1Transmission(code, 24, d["pulseUs"] | 350, d["frames"] | 6);
    g_sys->rfRx.feed(tx.data(), tx.size());
    g_sys->rfRx.idle();
    return "{\"ok\":true}";
  }
  if (path == "/sim/nexus") {
    auto row = rf::NexusThDecoder::encodeRow(d["id"] | 42, true, d["channel"] | 1, d["temp"] | 20.f, d["humidity"] | 50);
    for (int i = 0; i < 6; ++i) g_sys->rfRx.feed(row.data(), row.size());
    return "{\"ok\":true}";
  }
  if (path == "/sim/input") {
    int n = d["n"] | 1;
    if (d["active"] | false) g_inputs |= static_cast<uint16_t>(1u << (n - 1));
    else g_inputs &= static_cast<uint16_t>(~(1u << (n - 1)));
    return "{\"ok\":true}";
  }
  if (path == "/sim/window") {
    g_plants[d["room"] | ""].window = d["open"] | false;
    return "{\"ok\":true}";
  }
  return "{\"error\":\"unknown\"}";
}

void serve(int port, const std::string& webPath) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) || listen(s, 16)) {
    std::perror("bind");
    return;
  }
  std::fprintf(stderr, "HeatCore simulator on http://127.0.0.1:%d/\n", port);
  ApiRouter api(*g_sys);
  while (g_run) {
    int c = accept(s, nullptr, nullptr);
    if (c < 0) continue;
    std::string req;
    char buf[4096];
    size_t headerEnd = std::string::npos, need = 0;
    while (true) {
      ssize_t n = read(c, buf, sizeof(buf));
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
      if (headerEnd == std::string::npos) {
        headerEnd = req.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
          size_t cl = req.find("Content-Length:");
          if (cl == std::string::npos) cl = req.find("content-length:");
          need = cl != std::string::npos && cl < headerEnd ? std::stoul(req.substr(cl + 15)) : 0;
        }
      }
      if (headerEnd != std::string::npos && req.size() >= headerEnd + 4 + need) break;
    }
    if (headerEnd == std::string::npos) {
      close(c);
      continue;
    }
    std::string line = req.substr(0, req.find("\r\n"));
    std::string method = line.substr(0, line.find(' '));
    std::string target = line.substr(method.size() + 1, line.rfind(' ') - method.size() - 1);
    std::string body = req.substr(headerEnd + 4);
    std::string path = target.substr(0, target.find('?'));
    if (path.rfind("/api/", 0) == 0) {
      ApiRequest r;
      r.method = method;
      r.path = path;
      r.body = body;
      if (target.find('?') != std::string::npos) {
        std::string q = target.substr(target.find('?') + 1);
        std::stringstream qs(q);
        std::string kv;
        while (std::getline(qs, kv, '&')) {
          size_t eq = kv.find('=');
          if (eq != std::string::npos) r.query[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
      }
      ApiResponse res;
      {
        std::lock_guard<std::recursive_mutex> lk(g_sys->mutex());
        if (path == "/api/system/reboot") res = {501, "{\"error\":\"im Simulator nicht verfuegbar\"}"};
        else if (path == "/api/ota" && method == "POST") res = simOta(req.substr(0, headerEnd), body, r.query);
        else res = api.handle(r);
      }
      reply(c, res.status, res.contentType, res.body);
    } else if (path.rfind("/sim/", 0) == 0) {
      reply(c, 200, "application/json", simCommand(path, body));
    } else {
      reply(c, 200, "text/html; charset=utf-8", readFile(webPath));
    }
    close(c);
  }
}

}  // namespace

int main(int argc, char** argv) {
  int port = 8080;
  bool withDemo = false;
  std::string web = "web/index.html";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
    else if (a == "--speed" && i + 1 < argc) g_speed = std::atoi(argv[++i]);
    else if (a == "--demo") withDemo = true;
    else if (a == "--web" && i + 1 < argc) web = argv[++i];
  }
  setLogSink(sinkFn);
  g_clock.epochBase = static_cast<int64_t>(std::time(nullptr));
  g_clock.tzOffsetSec = 3600;
  g_clock.ms = 1000;
  g_sys = new HeatCoreSystem(g_clock);
  g_sys->platform.deviceId = "51a7e5";
  g_sys->platform.firmware.runningPartition = "app0";
  g_sys->platform.firmware.slotSize = 0x400000;
  g_sys->platformCommand = [](const std::string& cmd, JsonObjectConst) -> std::string {
    if (cmd == "selftest") {
      g_sys->selfTest = {{"Simulator", "ok", "alle Komponenten simuliert"},
                         {"433-MHz-Sender (GPIO18)", "unbestaetigt", "nur simulierte Funkstrecke"}};
      return "";
    }
    if (cmd == "ota_rollback") {
      if (!g_sys->platform.firmware.rollbackAvailable) return "keine vorherige Firmware vorhanden";
      g_sys->platform.firmware.lastResult = "Zurueck zur vorherigen Firmware (Simulator)";
      return "";
    }
    return cmd.rfind("apply_", 0) == 0 ? "" : "im Simulator nicht verfuegbar";
  };
  if (withDemo) demo();
  {
    std::lock_guard<std::recursive_mutex> lk(g_sys->mutex());
    g_sys->begin();
  }
  std::thread t(simLoop);
  serve(port, web);
  g_run = false;
  t.join();
}
