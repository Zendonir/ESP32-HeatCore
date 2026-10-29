#include "hc/ai/AiChanges.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "hc/system/Config.h"
#include "hc/system/HeatCoreSystem.h"

namespace hc {

namespace {

const char* const kAccessNames[] = {"off", "read", "approve", "direct"};
const char* const kDayNames[] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};

float round1(float v) { return std::round(v * 10.f) / 10.f; }
float round2(float v) { return std::round(v * 100.f) / 100.f; }

std::string fmt1(float v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%.1f", static_cast<double>(v));
  for (char* p = b; *p; ++p)
    if (*p == '.') *p = ',';
  return b;
}

std::string fmt2(float v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%.2f", static_cast<double>(v));
  for (char* p = b; *p; ++p)
    if (*p == '.') *p = ',';
  return b;
}

JsonObject findById(JsonArray a, const char* key, const std::string& id) {
  for (JsonObject x : a)
    if (id == (x[key] | "")) return x;
  return JsonObject();
}

JsonObject findCircuit(JsonArray a, int n) {
  for (JsonObject x : a)
    if ((x["n"] | 0) == n) return x;
  return JsonObject();
}

// "HH:MM-HH:MM" -> minutes; false on error
bool parsePeriod(const std::string& s, uint16_t& a, uint16_t& b) {
  size_t dash = s.find('-');
  return dash != std::string::npos && parseHHMM(s.substr(0, dash).c_str(), a) &&
         parseHHMM(s.substr(dash + 1).c_str(), b) && b > a;
}

int periodsMinutes(JsonArrayConst periods) {
  int sum = 0;
  for (const char* p : periods) {
    uint16_t a, b;
    if (p && parsePeriod(p, a, b)) sum += b - a;
  }
  return sum;
}

std::string periodsText(JsonArrayConst periods) {
  std::string t;
  for (const char* p : periods) {
    if (!t.empty()) t += ", ";
    t += p ? p : "";
  }
  return t.empty() ? "keine" : t;
}

std::string daysText(JsonArrayConst days) {
  std::string t;
  for (int d : days) {
    if (!t.empty()) t += ",";
    t += (d >= 0 && d < 7) ? kDayNames[d] : "?";
  }
  return t;
}

bool sameSchedule(JsonArrayConst a, JsonArrayConst b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::strcmp(a[i] | "", b[i] | "") != 0) return false;
  return true;
}

// Applies normalized items to an exported heating section (and checks that
// the referenced rooms/circuits exist).
std::string applyToHeatingJson(JsonObject heating, JsonArrayConst items) {
  JsonArray rooms = heating["rooms"];
  JsonArray circuits = heating["circuits"];
  for (JsonObjectConst it : items) {
    std::string type = it["type"] | "";
    if (type == "room") {
      JsonObject r = findById(rooms, "id", it["room"] | "");
      if (r.isNull()) return std::string("Raum unbekannt: ") + (it["room"] | "");
      for (const char* k : {"comfort", "eco", "frost", "hysteresis"})
        if (!it[k].isNull()) r[k] = it[k].as<float>();
      if (!it["modelAssist"].isNull()) r["modelAssist"] = it["modelAssist"].as<bool>();
    } else if (type == "schedule") {
      JsonObject r = findById(rooms, "id", it["room"] | "");
      if (r.isNull()) return std::string("Raum unbekannt: ") + (it["room"] | "");
      JsonArray sch = r["schedule"];
      for (int d : it["days"].as<JsonArrayConst>()) {
        if (d < 0 || d > 6 || static_cast<size_t>(d) >= sch.size()) return "Wochentag 0..6";
        JsonArray day = sch[static_cast<size_t>(d)].as<JsonArray>();
        day.clear();
        for (const char* p : it["periods"].as<JsonArrayConst>()) day.add(p);
      }
    } else if (type == "circuit") {
      JsonObject c = findCircuit(circuits, it["circuit"] | 0);
      if (c.isNull()) return "Heizkreis unbekannt";
      if (!it["minOnSec"].isNull()) c["minOnSec"] = it["minOnSec"].as<int>();
      if (!it["minOffSec"].isNull()) c["minOffSec"] = it["minOffSec"].as<int>();
    }
  }
  return std::string();
}

}  // namespace

const char* aiAccessName(AiAccess a) { return kAccessNames[static_cast<int>(a)]; }

bool aiAccessFromName(const char* s, AiAccess& out) {
  for (int i = 0; i < 4; ++i)
    if (s && !std::strcmp(s, kAccessNames[i])) {
      out = static_cast<AiAccess>(i);
      return true;
    }
  return false;
}

// ------------------------------------------------------------ state

void AiState::changeToJson(const AiChange& c, JsonObject o) {
  o["id"] = c.id;
  o["epoch"] = c.epoch;
  o["status"] = c.status;
  o["client"] = c.client;
  o["reason"] = c.reason;
  o["summary"] = c.summary;
  o["changes"] = serialized(c.changes.empty() ? std::string("[]") : c.changes);
  o["before"] = serialized(c.before.empty() ? std::string("[]") : c.before);
  if (!c.note.empty()) o["note"] = c.note;
  if (c.decidedEpoch) o["decidedEpoch"] = c.decidedEpoch;
}

void AiState::journalToJson(JsonArray a, size_t max) const {
  size_t start = journal.size() > max ? journal.size() - max : 0;
  for (size_t i = journal.size(); i-- > start;) changeToJson(journal[i], a.add<JsonObject>());  // newest first
}

void AiState::toJson(JsonObject o, bool includeSecrets) const {
  o["access"] = aiAccessName(cfg.access);
  if (includeSecrets) o["token"] = cfg.token;
  else o["token"] = cfg.token.empty() ? "" : kSecretMask;
  o["maxDirectPerDay"] = cfg.maxDirectPerDay;
  JsonArray j = o["journal"].to<JsonArray>();
  for (auto& c : journal) changeToJson(c, j.add<JsonObject>());  // oldest first (storage order)
}

std::string AiState::fromJson(JsonObjectConst o) {
  AiConfig n = cfg;
  if (!o["access"].isNull() && !aiAccessFromName(o["access"] | "", n.access)) return "Zugriff: off|read|approve|direct";
  if (!o["token"].isNull()) {
    const char* t = o["token"] | "";
    if (std::strcmp(t, kSecretMask) != 0) n.token = t;
  }
  if (!n.token.empty() && (n.token.size() < 16 || n.token.size() > 64)) return "Token 16..64 Zeichen";
  for (char ch : n.token)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '_'))
      return "Token: nur Buchstaben, Ziffern, - und _";
  if (n.access != AiAccess::Off && n.token.empty()) return "Fuer den MCP-Zugang ist ein Token erforderlich";
  n.maxDirectPerDay = static_cast<uint8_t>(std::min(50, std::max(1, o["maxDirectPerDay"] | static_cast<int>(n.maxDirectPerDay))));
  if (!o["journal"].isNull()) {
    std::vector<AiChange> j;
    for (JsonObjectConst x : o["journal"].as<JsonArrayConst>()) {
      AiChange c;
      c.id = x["id"] | 0;
      c.epoch = x["epoch"] | static_cast<int64_t>(0);
      c.status = x["status"] | "";
      c.client = x["client"] | "";
      c.reason = x["reason"] | "";
      c.summary = x["summary"] | "";
      serializeJson(x["changes"], c.changes);
      serializeJson(x["before"], c.before);
      c.note = x["note"] | "";
      c.decidedEpoch = x["decidedEpoch"] | static_cast<int64_t>(0);
      if (c.id) j.push_back(std::move(c));
    }
    if (j.size() > kMaxJournal) j.erase(j.begin(), j.end() - static_cast<long>(kMaxJournal));
    journal = std::move(j);
  }
  cfg = n;
  return std::string();
}

AiChange* AiState::find(uint16_t id) {
  for (auto& c : journal)
    if (c.id == id) return &c;
  return nullptr;
}

uint16_t AiState::nextId() const {
  uint16_t m = 0;
  for (auto& c : journal) m = std::max(m, c.id);
  return static_cast<uint16_t>(m + 1);
}

void AiState::add(AiChange c) {
  journal.push_back(std::move(c));
  if (journal.size() > kMaxJournal) journal.erase(journal.begin());
}

// ------------------------------------------------------------ validation

std::string aiValidate(HeatCoreSystem& sys, JsonArrayConst changes, JsonArray normalized, JsonArray before,
                       std::string& summary) {
  const AiLimits& L = sys.ai.limits;
  const SafetyConfig& sc = sys.safety.cfg;
  if (changes.isNull() || changes.size() == 0) return "Keine Aenderungen angegeben";
  if (changes.size() > 20) return "Max. 20 Aenderungen pro Aufruf";
  JsonDocument cur;
  sys.sectionToJson(ConfigSection::Heating, cur.to<JsonObject>(), false);
  JsonArray rooms = cur["rooms"];
  JsonArray circuits = cur["circuits"];
  summary.clear();
  auto line = [&](const std::string& s) { summary += (summary.empty() ? "" : "\n") + s; };
  int idx = 0;
  for (JsonObjectConst it : changes) {
    ++idx;
    std::string pre = "Aenderung " + std::to_string(idx) + ": ";
    std::string type = it["type"] | "";
    auto unknownKey = [&](std::initializer_list<const char*> allowed) -> std::string {
      for (JsonPairConst kv : it) {
        bool ok = false;
        for (const char* a : allowed)
          if (!std::strcmp(kv.key().c_str(), a)) ok = true;
        if (!ok) return pre + "Feld '" + kv.key().c_str() + "' ist nicht erlaubt";
      }
      return std::string();
    };
    if (type == "room") {
      std::string e = unknownKey({"type", "room", "comfort", "eco", "frost", "hysteresis", "modelAssist"});
      if (!e.empty()) return e;
      std::string id = it["room"] | "";
      JsonObject r = findById(rooms, "id", id);
      if (r.isNull()) return pre + "Raum unbekannt: " + id;
      std::string name = r["name"] | id.c_str();
      JsonObject n = normalized.add<JsonObject>();
      JsonObject b = before.add<JsonObject>();
      n["type"] = "room";
      n["room"] = id;
      b["type"] = "room";
      b["room"] = id;
      bool any = false;
      struct Num {
        const char* key;
        const char* label;
        float lo, hi, step;
      };
      const Num nums[] = {{"comfort", "Komfort", std::max(L.comfortMin, sc.minSetpoint), std::min(L.comfortMax, sc.maxSetpoint), L.comfortStepK},
                          {"eco", "Absenkung", std::max(L.ecoMin, sc.minSetpoint), std::min(L.ecoMax, sc.maxSetpoint), L.ecoStepK},
                          {"frost", "Frostschutz", L.frostMin, L.frostMax, 0.f},
                          {"hysteresis", "Hysterese", L.hystMin, L.hystMax, 0.f}};
      for (const Num& x : nums) {
        if (it[x.key].isNull()) continue;
        if (!it[x.key].is<float>()) return pre + x.key + " muss eine Zahl sein";
        float v = it[x.key].as<float>();
        v = std::strcmp(x.key, "hysteresis") == 0 ? round2(v) : round1(v);
        float old = r[x.key] | 0.f;
        if (v < x.lo - 1e-4f || v > x.hi + 1e-4f)
          return pre + x.label + " " + fmt1(v) + " ausserhalb " + fmt1(x.lo) + ".." + fmt1(x.hi);
        if (x.step > 0.f && std::fabs(v - old) > x.step + 1e-4f)
          return pre + x.label + " max. " + fmt1(x.step) + " K Aenderung pro Schritt (aktuell " + fmt1(old) + ")";
        n[x.key] = v;
        b[x.key] = old;
        line(name + ": " + x.label + " " + (std::strcmp(x.key, "hysteresis") == 0 ? fmt2(old) + " -> " + fmt2(v) + " K"
                                                                             : fmt1(old) + " -> " + fmt1(v) + " C"));
        any = true;
      }
      if (!it["modelAssist"].isNull()) {
        if (!it["modelAssist"].is<bool>()) return pre + "modelAssist muss true/false sein";
        bool v = it["modelAssist"].as<bool>();
        n["modelAssist"] = v;
        b["modelAssist"] = r["modelAssist"] | true;
        line(name + ": Modellberatung " + (v ? "ein" : "aus"));
        any = true;
      }
      if (!any) return pre + "keine Werte angegeben";
    } else if (type == "schedule") {
      std::string e = unknownKey({"type", "room", "days", "periods"});
      if (!e.empty()) return e;
      std::string id = it["room"] | "";
      JsonObject r = findById(rooms, "id", id);
      if (r.isNull()) return pre + "Raum unbekannt: " + id;
      std::string name = r["name"] | id.c_str();
      JsonArrayConst days = it["days"];
      JsonArrayConst periods = it["periods"];
      if (days.isNull() || days.size() == 0 || days.size() > 7) return pre + "days: Liste von Wochentagen 0..6 (0 = Montag)";
      if (periods.isNull()) return pre + "periods fehlt (leere Liste = keine Komfortzeit)";
      if (periods.size() > 6) return pre + "max. 6 Komfortzeiten pro Tag";
      std::vector<std::pair<uint16_t, uint16_t>> ps;
      JsonObject n = normalized.add<JsonObject>();
      n["type"] = "schedule";
      n["room"] = id;
      JsonArray nd = n["days"].to<JsonArray>();
      JsonArray np = n["periods"].to<JsonArray>();
      for (JsonVariantConst p : periods) {
        uint16_t a, b2;
        std::string s = p | "";
        if (!parsePeriod(s, a, b2)) return pre + "ungueltiger Zeitraum '" + s + "' (Format HH:MM-HH:MM)";
        ps.push_back({a, b2});
      }
      std::sort(ps.begin(), ps.end());
      for (size_t i = 1; i < ps.size(); ++i)
        if (ps[i].first < ps[i - 1].second) return pre + "Zeitraeume ueberschneiden sich";
      for (auto& pr : ps) np.add(formatHHMM(pr.first) + "-" + formatHHMM(pr.second));
      int newMin = periodsMinutes(np);
      bool seen[7] = {};
      for (JsonVariantConst dv : days) {
        if (!dv.is<int>()) return pre + "days: Zahlen 0..6";
        int d = dv.as<int>();
        if (d < 0 || d > 6 || seen[d]) return pre + "days: Zahlen 0..6 ohne Wiederholung";
        seen[d] = true;
        nd.add(d);
        JsonArrayConst old = r["schedule"][d];
        int diff = std::abs(newMin - periodsMinutes(old));
        if (diff > L.maxComfortShiftMin)
          return pre + "Komfortzeit am " + kDayNames[d] + " wuerde sich um " + std::to_string(diff) +
                 " min aendern (max. " + std::to_string(L.maxComfortShiftMin) + " min pro Schritt)";
        JsonObject b = before.add<JsonObject>();
        b["type"] = "schedule";
        b["room"] = id;
        b["days"].to<JsonArray>().add(d);
        JsonArray bp = b["periods"].to<JsonArray>();
        for (const char* p : old) bp.add(p);
      }
      line(name + " " + daysText(nd) + ": Komfortzeiten " + periodsText(r["schedule"][nd[0].as<int>()]) + " -> " +
           periodsText(np));
    } else if (type == "circuit") {
      std::string e = unknownKey({"type", "circuit", "minOnSec", "minOffSec"});
      if (!e.empty()) return e;
      int cn = it["circuit"] | 0;
      JsonObject c = findCircuit(circuits, cn);
      if (c.isNull() || std::string(c["room"] | "").empty()) return pre + "Heizkreis " + std::to_string(cn) + " ist keinem Raum zugeordnet";
      JsonObject n = normalized.add<JsonObject>();
      JsonObject b = before.add<JsonObject>();
      n["type"] = "circuit";
      n["circuit"] = cn;
      b["type"] = "circuit";
      b["circuit"] = cn;
      bool any = false;
      for (const char* k : {"minOnSec", "minOffSec"}) {
        if (it[k].isNull()) continue;
        if (!it[k].is<int>()) return pre + k + " muss eine ganze Zahl sein";
        int v = it[k].as<int>();
        if (v < L.minTimeSec || v > L.maxTimeSec)
          return pre + k + " " + std::to_string(L.minTimeSec) + ".." + std::to_string(L.maxTimeSec) + " s";
        n[k] = v;
        b[k] = c[k] | 0;
        line("Heizkreis " + std::to_string(cn) + ": " + (std::strcmp(k, "minOnSec") == 0 ? "Mindestlaufzeit " : "Mindestpause ") +
             std::to_string(c[k] | 0) + " -> " + std::to_string(v) + " s");
        any = true;
      }
      if (!any) return pre + "keine Werte angegeben";
    } else if (type == "model") {
      std::string e = unknownKey({"type", "maxAdviceK", "enabled"});
      if (!e.empty()) return e;
      JsonObject n = normalized.add<JsonObject>();
      JsonObject b = before.add<JsonObject>();
      n["type"] = "model";
      b["type"] = "model";
      bool any = false;
      if (!it["maxAdviceK"].isNull()) {
        float v = round2(it["maxAdviceK"] | -1.f);
        if (v < 0.f || v > L.adviceMaxK) return pre + "maxAdviceK 0.." + fmt2(L.adviceMaxK) + " K";
        n["maxAdviceK"] = v;
        b["maxAdviceK"] = sys.model.cfg.maxAdviceK;
        line("Lernmodell: max. Beratung " + fmt2(sys.model.cfg.maxAdviceK) + " -> " + fmt2(v) + " K");
        any = true;
      }
      if (!it["enabled"].isNull()) {
        if (!it["enabled"].is<bool>()) return pre + "enabled muss true/false sein";
        n["enabled"] = it["enabled"].as<bool>();
        b["enabled"] = sys.model.enabled;
        line(std::string("Lernmodell ") + (it["enabled"].as<bool>() ? "ein" : "aus"));
        any = true;
      }
      if (!any) return pre + "keine Werte angegeben";
    } else {
      return pre + "unbekannter Typ '" + type + "' (erlaubt: room, schedule, circuit, model)";
    }
  }
  // complete check with the regular configuration validation (probe)
  std::string err = applyToHeatingJson(cur.as<JsonObject>(), normalized);
  if (!err.empty()) return err;
  HeatingManager probe(sys.bus, sys.clock());
  err = probe.fromJson(cur.as<JsonObjectConst>());
  if (!err.empty()) return "Ungueltig: " + err;
  return std::string();
}

std::string aiApply(HeatCoreSystem& sys, JsonArrayConst items) {
  bool heatingTouched = false;
  for (JsonObjectConst it : items) {
    std::string t = it["type"] | "";
    if (t == "room" || t == "schedule" || t == "circuit") heatingTouched = true;
  }
  if (heatingTouched) {
    JsonDocument cur;
    sys.sectionToJson(ConfigSection::Heating, cur.to<JsonObject>(), false);
    std::string err = applyToHeatingJson(cur.as<JsonObject>(), items);
    if (!err.empty()) return err;
    err = sys.sectionFromJson(ConfigSection::Heating, cur.as<JsonObjectConst>());
    if (!err.empty()) return err;
  }
  for (JsonObjectConst it : items) {
    if (std::string(it["type"] | "") != "model") continue;
    if (!it["maxAdviceK"].isNull()) sys.model.cfg.maxAdviceK = std::min(0.5f, std::max(0.f, it["maxAdviceK"].as<float>()));
    if (!it["enabled"].isNull()) sys.model.enabled = it["enabled"].as<bool>();
    sys.markDirty(ConfigSection::Model);
  }
  return std::string();
}

// True when the current configuration still holds the values `items` set.
static std::string stillCurrent(HeatCoreSystem& sys, JsonArrayConst items) {
  JsonDocument cur;
  sys.sectionToJson(ConfigSection::Heating, cur.to<JsonObject>(), false);
  JsonArray rooms = cur["rooms"];
  JsonArray circuits = cur["circuits"];
  for (JsonObjectConst it : items) {
    std::string t = it["type"] | "";
    if (t == "room") {
      JsonObject r = findById(rooms, "id", it["room"] | "");
      if (r.isNull()) return "Raum existiert nicht mehr";
      for (const char* k : {"comfort", "eco", "frost", "hysteresis"})
        if (!it[k].isNull() && std::fabs((r[k] | 0.f) - it[k].as<float>()) > 0.01f) return "inzwischen geaendert";
      if (!it["modelAssist"].isNull() && (r["modelAssist"] | true) != it["modelAssist"].as<bool>()) return "inzwischen geaendert";
    } else if (t == "schedule") {
      JsonObject r = findById(rooms, "id", it["room"] | "");
      if (r.isNull()) return "Raum existiert nicht mehr";
      for (int d : it["days"].as<JsonArrayConst>())
        if (!sameSchedule(r["schedule"][d], it["periods"])) return "Zeitplan inzwischen geaendert";
    } else if (t == "circuit") {
      JsonObject c = findCircuit(circuits, it["circuit"] | 0);
      for (const char* k : {"minOnSec", "minOffSec"})
        if (!it[k].isNull() && (c[k] | 0) != it[k].as<int>()) return "inzwischen geaendert";
    } else if (t == "model") {
      if (!it["maxAdviceK"].isNull() && std::fabs(sys.model.cfg.maxAdviceK - it["maxAdviceK"].as<float>()) > 0.001f)
        return "inzwischen geaendert";
      if (!it["enabled"].isNull() && sys.model.enabled != it["enabled"].as<bool>()) return "inzwischen geaendert";
    }
  }
  return std::string();
}

namespace {
void announce(HeatCoreSystem& sys, const AiChange& c, const char* what) {
  Event e(EventType::ConfigChanged, EventSource::Ai);
  e.ts = sys.clock().nowMs();
  e.a = c.id;
  std::snprintf(e.ref, sizeof(e.ref), "ai/%u", static_cast<unsigned>(c.id));
  std::snprintf(e.text, sizeof(e.text), "KI-Aenderung %u %s", static_cast<unsigned>(c.id), what);
  sys.bus.post(e);
}

uint32_t appliedLastDay(const HeatCoreSystem& sys) {
  int64_t now = sys.clock().epoch();
  uint32_t n = 0;
  for (auto& c : sys.ai.journal)
    if (c.status == "applied" || c.status == "reverted")
      if (!now || !c.epoch || now - c.epoch < 86400) ++n;
  return n;
}
}  // namespace

std::string aiSubmit(HeatCoreSystem& sys, JsonArrayConst changes, const std::string& reason, bool dryRun,
                     JsonObject result) {
  AiAccess acc = sys.ai.cfg.access;
  if (acc == AiAccess::Off || acc == AiAccess::Read) return "Aenderungen sind nicht freigegeben (Zugriff: nur lesen)";
  if (reason.empty() || reason.size() > 600) return "reason: kurze Begruendung erforderlich (max. 600 Zeichen)";
  JsonDocument nd, bd;
  std::string summary;
  std::string err = aiValidate(sys, changes, nd.to<JsonArray>(), bd.to<JsonArray>(), summary);
  if (!err.empty()) return err;
  result["summary"] = summary;
  if (dryRun) {
    result["status"] = "dry_run";
    result["message"] = "Gueltig - es wurde nichts geaendert";
    result["changes"] = nd.as<JsonArrayConst>();
    return std::string();
  }
  AiChange c;
  c.id = sys.ai.nextId();
  c.epoch = sys.clock().epoch();
  c.client = sys.ai.lastClient;
  c.reason = reason;
  c.summary = summary;
  serializeJson(nd, c.changes);
  serializeJson(bd, c.before);
  if (acc == AiAccess::Approve) {
    size_t pending = 0;
    for (auto& j : sys.ai.journal)
      if (j.status == "pending") ++pending;
    if (pending >= 10) return "Bereits 10 offene Vorschlaege - erst im Webinterface entscheiden";
    c.status = "pending";
    result["message"] = "Vorschlag gespeichert - wartet auf Freigabe im Webinterface (System -> KI / MCP)";
  } else {
    if (appliedLastDay(sys) >= sys.ai.cfg.maxDirectPerDay)
      return "Tageslimit fuer direkte Aenderungen erreicht (" + std::to_string(sys.ai.cfg.maxDirectPerDay) + ")";
    err = aiApply(sys, nd.as<JsonArrayConst>());
    c.status = err.empty() ? "applied" : "failed";
    c.note = err;
    c.decidedEpoch = c.epoch;
    result["message"] = err.empty() ? "Angewendet" : "Fehlgeschlagen: " + err;
  }
  result["id"] = c.id;
  result["status"] = c.status;
  announce(sys, c, c.status == "pending" ? "vorgeschlagen" : c.status == "applied" ? "angewendet" : "fehlgeschlagen");
  sys.ai.add(c);
  sys.markDirty(ConfigSection::Ai);
  return err;
}

std::string aiDecide(HeatCoreSystem& sys, uint16_t id, bool accept, JsonObject result) {
  AiChange* c = sys.ai.find(id);
  if (!c) return "Vorschlag nicht gefunden";
  if (c->status != "pending") return "Vorschlag ist nicht offen (" + c->status + ")";
  c->decidedEpoch = sys.clock().epoch();
  if (!accept) {
    c->status = "rejected";
    announce(sys, *c, "abgelehnt");
  } else {
    // re-validate against the current state: limits refer to the values now
    JsonDocument in, nd, bd;
    deserializeJson(in, c->changes);
    std::string summary;
    std::string err = aiValidate(sys, in.as<JsonArrayConst>(), nd.to<JsonArray>(), bd.to<JsonArray>(), summary);
    if (err.empty()) err = aiApply(sys, nd.as<JsonArrayConst>());
    if (!err.empty()) {
      c->status = "failed";
      c->note = err;
    } else {
      c->status = "applied";
      c->summary = summary;
      c->before.clear();
      serializeJson(bd, c->before);
    }
    announce(sys, *c, c->status == "applied" ? "freigegeben und angewendet" : "fehlgeschlagen");
  }
  sys.markDirty(ConfigSection::Ai);
  AiState::changeToJson(*c, result);
  return c->status == "failed" ? c->note : std::string();
}

std::string aiRevert(HeatCoreSystem& sys, uint16_t id, const std::string& reason, JsonObject result) {
  AiChange* c = sys.ai.find(id);
  if (!c) return "Aenderung nicht gefunden";
  if (c->status != "applied") return "Nur angewendete Aenderungen koennen zurueckgenommen werden (" + c->status + ")";
  JsonDocument changes, before;
  deserializeJson(changes, c->changes);
  deserializeJson(before, c->before);
  std::string err = stillCurrent(sys, changes.as<JsonArrayConst>());
  if (!err.empty()) return "Ruecknahme nicht moeglich: " + err + " - bitte manuell pruefen";
  JsonDocument cur;
  sys.sectionToJson(ConfigSection::Heating, cur.to<JsonObject>(), false);
  err = applyToHeatingJson(cur.as<JsonObject>(), before.as<JsonArrayConst>());
  HeatingManager probe(sys.bus, sys.clock());
  if (err.empty()) err = probe.fromJson(cur.as<JsonObjectConst>());
  if (err.empty()) err = aiApply(sys, before.as<JsonArrayConst>());
  if (!err.empty()) return "Ruecknahme fehlgeschlagen: " + err;
  c->status = "reverted";
  c->note = reason.empty() ? "zurueckgenommen" : "zurueckgenommen: " + reason.substr(0, 200);
  c->decidedEpoch = sys.clock().epoch();
  announce(sys, *c, "zurueckgenommen");
  sys.markDirty(ConfigSection::Ai);
  AiState::changeToJson(*c, result);
  return std::string();
}

void aiLimitsToJson(const HeatCoreSystem& sys, JsonObject o) {
  const AiLimits& L = sys.ai.limits;
  const SafetyConfig& sc = sys.safety.cfg;
  o["access"] = aiAccessName(sys.ai.cfg.access);
  JsonObject r = o["room"].to<JsonObject>();
  r["comfort"] = fmt1(std::max(L.comfortMin, sc.minSetpoint)) + ".." + fmt1(std::min(L.comfortMax, sc.maxSetpoint)) +
                 " C, max. +-" + fmt1(L.comfortStepK) + " K pro Schritt";
  r["eco"] = fmt1(L.ecoMin) + ".." + fmt1(L.ecoMax) + " C, max. +-" + fmt1(L.ecoStepK) + " K pro Schritt";
  r["frost"] = fmt1(L.frostMin) + ".." + fmt1(L.frostMax) + " C";
  r["hysteresis"] = fmt2(L.hystMin) + ".." + fmt2(L.hystMax) + " K";
  r["modelAssist"] = "true/false";
  r["order"] = "frost <= eco <= comfort";
  o["schedule"] = "Komfortzeiten je Wochentag (0 = Montag), max. 6 Zeitraeume HH:MM-HH:MM, Tageskomfortzeit max. +-" +
                  std::to_string(L.maxComfortShiftMin) + " min pro Schritt";
  o["circuit"] = "minOnSec/minOffSec " + std::to_string(L.minTimeSec) + ".." + std::to_string(L.maxTimeSec) + " s";
  o["model"] = "maxAdviceK 0.." + fmt2(L.adviceMaxK) + " K, enabled true/false";
  o["directChangesPerDay"] = sys.ai.cfg.maxDirectPerDay;
  o["notReachable"] = "Sicherheitsgrenzen, Ausgaenge, Eingaenge, Regeln, Funk, Netzwerk, MQTT, Passwoerter";
}

}  // namespace hc
