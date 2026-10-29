#include "hc/mcp/McpServer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hc/ai/AiChanges.h"
#include "hc/system/HeatCoreSystem.h"

namespace hc {

const char* const McpServer::kSupportedVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
const char* McpServer::latestVersion() { return kSupportedVersions[0]; }

namespace {

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kServerDisabled = -32001;

const char* const kInstructions =
    "HeatCore ist eine lokale Heizungssteuerung (Einzelraumregelung mit Ventilen, Hysterese-Regler und lernendem "
    "Raummodell). Ueber diese Schnittstelle koennen Messwerte, Statistik (15-Minuten-Werte der letzten 14 Tage), "
    "Einstellungen und das Lernmodell gelesen werden. Ziel einer Optimierung: Komfort in den Komfortzeiten bei "
    "moeglichst wenig Heizzeit und Ventilschaltungen. Aenderungen (change_settings) sind - je nach Freigabe - "
    "Vorschlaege, die ein Mensch im Webinterface bestaetigt, oder werden direkt angewendet. Sicherheitsgrenzen sind "
    "nicht veraenderbar. Kleine Schritte, immer mit Begruendung; zuerst dry_run verwenden. Perioden mit offenem "
    "Fenster, Handbetrieb oder Stoerung sind in den Kennzahlen bereits ausgeklammert.";

void rpcError(JsonObject resp, int code, const std::string& msg) {
  JsonObject e = resp["error"].to<JsonObject>();
  e["code"] = code;
  e["message"] = msg;
}

float r2(float v) { return std::round(v * 100.f) / 100.f; }

void addTool(JsonArray tools, const char* name, const char* title, const char* desc, const char* schema, bool readOnly,
             bool destructive = false) {
  JsonObject t = tools.add<JsonObject>();
  t["name"] = name;
  t["title"] = title;
  t["description"] = desc;
  t["inputSchema"] = serialized(schema);
  JsonObject a = t["annotations"].to<JsonObject>();
  a["title"] = title;
  a["readOnlyHint"] = readOnly;
  a["destructiveHint"] = destructive;
  a["idempotentHint"] = readOnly;
  a["openWorldHint"] = false;
}

const char* const kChangeSchema = R"JSON({
  "type":"object",
  "properties":{
    "changes":{"type":"array","minItems":1,"maxItems":20,"description":"Liste von Aenderungen","items":{"type":"object","oneOf":[
      {"properties":{"type":{"const":"room"},"room":{"type":"string","description":"Raum-ID"},
        "comfort":{"type":"number","description":"Komforttemperatur in C"},"eco":{"type":"number","description":"Absenktemperatur in C"},
        "frost":{"type":"number","description":"Frostschutz in C"},"hysteresis":{"type":"number","description":"Schalthysterese in K"},
        "modelAssist":{"type":"boolean","description":"Lernmodell darf die Schaltschwellen begrenzt verschieben"}},
       "required":["type","room"],"additionalProperties":false},
      {"properties":{"type":{"const":"schedule"},"room":{"type":"string"},
        "days":{"type":"array","items":{"type":"integer","minimum":0,"maximum":6},"description":"Wochentage, 0 = Montag"},
        "periods":{"type":"array","maxItems":6,"items":{"type":"string","pattern":"^[0-2][0-9]:[0-5][0-9]-[0-2][0-9]:[0-5][0-9]$"},"description":"Komfortzeiten HH:MM-HH:MM, ersetzt die bisherigen dieser Tage"}},
       "required":["type","room","days","periods"],"additionalProperties":false},
      {"properties":{"type":{"const":"circuit"},"circuit":{"type":"integer","minimum":1,"maximum":16},
        "minOnSec":{"type":"integer"},"minOffSec":{"type":"integer"}},
       "required":["type","circuit"],"additionalProperties":false},
      {"properties":{"type":{"const":"model"},"maxAdviceK":{"type":"number","description":"max. Verschiebung der Schaltschwellen durch das Lernmodell in K"},
        "enabled":{"type":"boolean"}},
       "required":["type"],"additionalProperties":false}]}},
    "reason":{"type":"string","description":"Begruendung mit den ausgewerteten Kennzahlen (wird dem Nutzer angezeigt)"},
    "dry_run":{"type":"boolean","default":false,"description":"nur pruefen, nichts aendern"}
  },
  "required":["changes","reason"],"additionalProperties":false})JSON";

}  // namespace

int McpServer::handle(const std::string& body, std::string& out) {
  out.clear();
  JsonDocument in;
  JsonDocument res;
  if (deserializeJson(in, body)) {
    JsonObject r = res.to<JsonObject>();
    r["jsonrpc"] = "2.0";
    r["id"] = nullptr;
    rpcError(r, kParseError, "Parse error");
    serializeJson(res, out);
    return 400;
  }
  if (in.is<JsonArrayConst>()) {  // batch (2025-03-26)
    JsonArray arr = res.to<JsonArray>();
    for (JsonVariantConst v : in.as<JsonArrayConst>()) {
      JsonObject r = arr.add<JsonObject>();
      if (!v.is<JsonObjectConst>()) {
        r["jsonrpc"] = "2.0";
        r["id"] = nullptr;
        rpcError(r, kInvalidRequest, "Invalid Request");
        continue;
      }
      dispatch(v.as<JsonObjectConst>(), r);
      if (r.size() == 0) arr.remove(arr.size() - 1);  // notification
    }
    if (arr.size() == 0) return 202;
    serializeJson(res, out);
    return 200;
  }
  if (!in.is<JsonObjectConst>()) {
    JsonObject r = res.to<JsonObject>();
    r["jsonrpc"] = "2.0";
    r["id"] = nullptr;
    rpcError(r, kInvalidRequest, "Invalid Request");
    serializeJson(res, out);
    return 400;
  }
  JsonObject r = res.to<JsonObject>();
  dispatch(in.as<JsonObjectConst>(), r);
  if (r.size() == 0) return 202;  // notification or response from the client
  serializeJson(res, out);
  return 200;
}

void McpServer::dispatch(JsonObjectConst req, JsonObject resp) {
  JsonVariantConst id = req["id"];
  const char* method = req["method"] | "";
  bool isRequest = !id.isNull() && *method;
  if (!isRequest) {
    if (!*method && id.isNull()) {  // neither request nor notification
      resp["jsonrpc"] = "2.0";
      resp["id"] = nullptr;
      rpcError(resp, kInvalidRequest, "Invalid Request");
    }
    return;  // notifications (notifications/initialized, ...) and client responses
  }
  resp["jsonrpc"] = "2.0";
  resp["id"] = id;
  if (sys_.ai.cfg.access == AiAccess::Off) {
    rpcError(resp, kServerDisabled, "MCP-Zugang ist im Webinterface deaktiviert");
    return;
  }
  std::string m = method;
  JsonObjectConst params = req["params"];
  if (m == "initialize") {
    initialize(params, resp["result"].to<JsonObject>());
  } else if (m == "ping") {
    resp["result"].to<JsonObject>();
  } else if (m == "tools/list") {
    toolsList(resp["result"].to<JsonObject>());
  } else if (m == "tools/call") {
    JsonObject result = resp["result"].to<JsonObject>();
    JsonDocument err;
    toolsCall(params, result, err.to<JsonObject>());
    if (err.size()) {
      resp.remove("result");
      rpcError(resp, err["code"] | kInvalidParams, err["message"] | "invalid params");
    }
  } else if (m == "prompts/list") {
    promptsList(resp["result"].to<JsonObject>());
  } else if (m == "prompts/get") {
    std::string e;
    JsonObject result = resp["result"].to<JsonObject>();
    if (!promptsGet(params, result, e)) {
      resp.remove("result");
      rpcError(resp, kInvalidParams, e);
    }
  } else if (m == "resources/list") {
    resp["result"]["resources"].to<JsonArray>();
  } else if (m == "resources/templates/list") {
    resp["result"]["resourceTemplates"].to<JsonArray>();
  } else {
    rpcError(resp, kMethodNotFound, "Method not found: " + m);  // incl. server/discover -> client falls back
  }
}

void McpServer::initialize(JsonObjectConst params, JsonObject result) {
  const char* requested = params["protocolVersion"] | "";
  const char* chosen = latestVersion();
  for (const char* v : kSupportedVersions)
    if (!std::strcmp(v, requested)) chosen = v;
  std::string client = params["clientInfo"]["name"] | "";
  std::string cver = params["clientInfo"]["version"] | "";
  sys_.ai.lastClient = (client + (cver.empty() ? "" : " " + cver)).substr(0, 48);
  result["protocolVersion"] = chosen;
  JsonObject caps = result["capabilities"].to<JsonObject>();
  caps["tools"]["listChanged"] = false;
  caps["prompts"]["listChanged"] = false;
  JsonObject info = result["serverInfo"].to<JsonObject>();
  info["name"] = "heatcore";
  info["title"] = "HeatCore Heizungssteuerung";
  info["version"] = kFirmwareVersion;
  result["instructions"] = kInstructions;
}

void McpServer::toolsList(JsonObject result) {
  JsonArray tools = result["tools"].to<JsonArray>();
  addTool(tools, "get_overview", "Aktueller Zustand",
          "Aktueller Zustand: Uhrzeit, Betriebsart, je Raum Ist-/Solltemperatur, Heizanforderung, Grund, Fenster, "
          "Lernmodell-Status; Aussen- und Vorlauftemperatur, Alarme, Zugriffsstufe.",
          R"({"type":"object","properties":{},"additionalProperties":false})", true);
  addTool(tools, "get_settings", "Einstellungen",
          "Einstellbare Parameter je Raum (Komfort/Absenkung/Frostschutz, Hysterese, Zeitplan, Modellberatung), "
          "Heizkreise (Mindestlauf-/pausenzeit), Lernmodell-Optionen, Sicherheitsgrenzen (nur lesbar) und die "
          "erlaubten Bereiche fuer KI-Aenderungen.",
          R"({"type":"object","properties":{},"additionalProperties":false})", true);
  addTool(tools, "get_statistics", "Statistik je Raum",
          "Kennzahlen je Raum und Tag: Mittelwerte, Abweichung vom Sollwert in Komfortzeiten, Stunden unter/ueber "
          "Sollwert, Ueberschwingen, Aufheizdauer nach Sollwertsprung, Heizstunden, Ventilschaltungen, Fenster-, "
          "Hand- und Stoerungsstunden. Fenster/Hand/Stoerung sind aus den Komfortkennzahlen ausgeklammert.",
          R"({"type":"object","properties":{"days":{"type":"integer","minimum":1,"maximum":14,"default":7}},"additionalProperties":false})",
          true);
  addTool(tools, "get_room_history", "Verlauf eines Raums",
          "Zeitreihe eines Raums (oder '_system' fuer Vorlauf/Aussen): Temperatur, Sollwert, Aussentemperatur, "
          "Ventil-Einschaltdauer in %, Fensterminuten, Schaltungen und Flags. Lange Zeitraeume werden automatisch "
          "auf max. ca. 600 Punkte verdichtet.",
          R"({"type":"object","properties":{"room":{"type":"string","description":"Raum-ID oder _system"},"hours":{"type":"integer","minimum":1,"maximum":336,"default":48},"resolution_min":{"type":"integer","minimum":15,"maximum":240,"default":15}},"required":["room"],"additionalProperties":false})",
          true);
  addTool(tools, "get_model", "Lernmodell",
          "Gelerntes Raummodell: Heizleistung (K/h bei offenem Ventil), Verlustkoeffizient, Totzeit, Unsicherheiten, "
          "Anzahl gelernter/ausgeschlossener Perioden, Stoerungsverdacht, letzte Ereignisse.",
          R"({"type":"object","properties":{"room":{"type":"string","description":"Raum-ID, leer = alle"}},"additionalProperties":false})",
          true);
  addTool(tools, "get_events", "Ereignisprotokoll", "Letzte Ereignisse (Sollwert, Fenster, Ventile, Alarme, ...).",
          R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":100,"default":40}},"additionalProperties":false})",
          true);
  addTool(tools, "get_change_log", "KI-Aenderungsjournal",
          "Bisherige KI-Vorschlaege und -Aenderungen mit Status (pending, applied, rejected, reverted, failed), "
          "Begruendung und vorherigen Werten.",
          R"({"type":"object","properties":{},"additionalProperties":false})", true);
  if (sys_.ai.cfg.access == AiAccess::Approve || sys_.ai.cfg.access == AiAccess::Direct) {
    bool direct = sys_.ai.cfg.access == AiAccess::Direct;
    addTool(tools, "change_settings", "Einstellungen aendern",
            direct ? "Aendert Einstellungen direkt (Zugriffsstufe 'direkt'). Nur room, schedule, circuit, model in "
                     "engen Grenzen (siehe get_settings.aiLimits); alles oder nichts; Journal mit Ruecknahme. Zuerst "
                     "mit dry_run pruefen."
                   : "Legt einen Aenderungsvorschlag an, den ein Mensch im Webinterface freigibt (Zugriffsstufe "
                     "'Vorschlaege'). Nur room, schedule, circuit, model in engen Grenzen (siehe "
                     "get_settings.aiLimits). Zuerst mit dry_run pruefen.",
            kChangeSchema, false, false);
    addTool(tools, "revert_change", "Aenderung zuruecknehmen",
            "Setzt eine angewendete KI-Aenderung auf die vorherigen Werte zurueck (nur wenn die Werte seitdem nicht "
            "anders geaendert wurden).",
            R"({"type":"object","properties":{"id":{"type":"integer"},"reason":{"type":"string"}},"required":["id"],"additionalProperties":false})",
            false, false);
  }
}

void McpServer::toolsCall(JsonObjectConst params, JsonObject result, JsonObject error) {
  std::string name = params["name"] | "";
  JsonObjectConst args = params["arguments"];
  static const char* const known[] = {"get_overview", "get_settings", "get_statistics", "get_room_history", "get_model",
                                      "get_events",   "get_change_log", "change_settings", "revert_change"};
  bool ok = false;
  for (const char* k : known)
    if (name == k) ok = true;
  bool write = name == "change_settings" || name == "revert_change";
  if (!ok || (write && sys_.ai.cfg.access != AiAccess::Approve && sys_.ai.cfg.access != AiAccess::Direct)) {
    error["code"] = kInvalidParams;
    error["message"] = "Unknown tool: " + name;
    return;
  }
  JsonDocument data;
  std::string err = tool(name, args, data.to<JsonObject>());
  JsonArray content = result["content"].to<JsonArray>();
  JsonObject t = content.add<JsonObject>();
  t["type"] = "text";
  if (err.empty()) {
    std::string text;
    serializeJson(data, text);
    t["text"] = text;
    result["isError"] = false;
  } else {
    t["text"] = "Fehler: " + err;
    result["isError"] = true;
  }
}

std::string McpServer::tool(const std::string& name, JsonObjectConst args, JsonObject o) {
  if (name == "get_overview") {
    LocalTime lt = sys_.clock().local();
    if (lt.valid) {
      char b[32];
      std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d", lt.year, lt.month, lt.day, lt.hour, lt.minute);
      o["localTime"] = b;
      o["weekday"] = lt.weekday;
    } else {
      o["localTime"] = "unbekannt (keine Zeitsynchronisation)";
    }
    o["firmware"] = kFirmwareVersion;
    o["globalMode"] = globalModeName(sys_.heating.globalMode);
    float out = 0.f;
    if (sys_.sensors.firstOfRole(SensorRole::Outdoor, out)) o["outdoorTemp"] = r2(out);
    if (sys_.safety.flowValid()) o["flowTemp"] = r2(sys_.safety.flowTemp());
    JsonArray rooms = o["rooms"].to<JsonArray>();
    for (const Room& r : sys_.heating.rooms) {
      JsonObject x = rooms.add<JsonObject>();
      x["id"] = r.id;
      x["name"] = r.name;
      x["mode"] = roomModeName(r.mode);
      if (r.hasTemp) x["temp"] = r2(r.temperature);
      x["setpoint"] = r2(r.setpoint);
      x["heating"] = r.demand;
      x["reason"] = r.reason;
      x["windowOpen"] = r.windowOpen();
      x["manualSetpoint"] = r.manualActive;
      if (r.sensorFail) x["sensorFail"] = true;
      if (const ThermalModel* m = sys_.model.find(r.id)) x["model"] = modelStatusName(m->status());
    }
    sys_.alarms.toJson(o["alarms"].to<JsonArray>());
    o["aiAccess"] = aiAccessName(sys_.ai.cfg.access);
    return std::string();
  }
  if (name == "get_settings") {
    JsonDocument h;
    sys_.sectionToJson(ConfigSection::Heating, h.to<JsonObject>(), false);
    JsonArray rooms = o["rooms"].to<JsonArray>();
    for (JsonObject r : h["rooms"].as<JsonArray>()) {
      JsonObject x = rooms.add<JsonObject>();
      for (const char* k : {"id", "name", "comfort", "eco", "frost", "mode", "hysteresis", "modelAssist", "manualOverrideMin"})
        x[k] = r[k];
      x["schedule"] = r["schedule"];
    }
    o["scheduleFormat"] = "7 Listen Montag..Sonntag mit Komfortzeiten HH:MM-HH:MM, ausserhalb gilt die Absenktemperatur";
    JsonArray circ = o["circuits"].to<JsonArray>();
    for (JsonObject c : h["circuits"].as<JsonArray>()) {
      if (std::string(c["room"] | "").empty()) continue;
      JsonObject x = circ.add<JsonObject>();
      for (const char* k : {"n", "name", "room", "enabled", "valveNormallyOpen", "minOnSec", "minOffSec"}) x[k] = c[k];
    }
    JsonObject m = o["model"].to<JsonObject>();
    m["enabled"] = sys_.model.enabled;
    m["maxAdviceK"] = sys_.model.cfg.maxAdviceK;
    o["controlIntervalSec"] = sys_.sysCfg.controlIntervalSec;
    JsonObject s = o["safetyReadOnly"].to<JsonObject>();
    s["minSetpoint"] = sys_.safety.cfg.minSetpoint;
    s["maxSetpoint"] = sys_.safety.cfg.maxSetpoint;
    s["maxRoomTemp"] = sys_.safety.cfg.maxRoomTemp;
    s["maxFlowTemp"] = sys_.safety.cfg.maxFlowTemp;
    aiLimitsToJson(sys_, o["aiLimits"].to<JsonObject>());
    return std::string();
  }
  if (name == "get_statistics") {
    int days = args["days"] | 7;
    if (days < 1 || days > kHistDays) return "days 1..14";
    sys_.statsSummaryJson(static_cast<uint16_t>(days), o);
    o["kpiHelp"] =
        "comfortMeanDevK: mittlere Abweichung Ist-Soll in Komfortzeiten; belowSetpointHours: >0,5 K unter Soll; "
        "aboveSetpointHours: >0,5 K ueber Soll bei geschlossenem Ventil; maxOvershootK: Ueberschwingen nach "
        "Heizende; warmupMeanMin: Zeit von Sollwertsprung (>=1 K) bis 0,3 K unter Soll; heatingHours: "
        "Ventil-Offenzeit; switches: Ventil-Einschaltungen.";
    return std::string();
  }
  if (name == "get_room_history") {
    std::string room = args["room"] | "";
    int hours = args["hours"] | 48;
    int res = args["resolution_min"] | 15;
    if (hours < 1 || hours > 336) return "hours 1..336";
    if (res < 15 || res > 240) return "resolution_min 15..240";
    return sys_.historyJson(room, static_cast<uint32_t>(hours), static_cast<uint16_t>(res), o);
  }
  if (name == "get_model") {
    std::string only = args["room"] | "";
    if (!only.empty() && !sys_.heating.room(only)) return "Raum unbekannt: " + only;
    o["enabled"] = sys_.model.enabled;
    o["help"] = "dT/dt = heatingGainKh*Ventil(t-Totzeit) - lossCoeff*(T-Aussen) + offsetKh; status ready = vertrauenswuerdig";
    JsonArray rooms = o["rooms"].to<JsonArray>();
    for (const Room& r : sys_.heating.rooms) {
      if (!only.empty() && r.id != only) continue;
      const ThermalModel* m = sys_.model.find(r.id);
      JsonObject x = rooms.add<JsonObject>();
      x["room"] = r.id;
      if (!m) {
        x["status"] = "keine Daten";
        continue;
      }
      x["status"] = modelStatusName(m->status());
      x["heatingGainKh"] = r2(m->heatingGain());
      x["heatingGainStd"] = r2(m->paramStd(0));
      x["lossCoeff"] = std::round(m->lossCoeff() * 1000.f) / 1000.f;
      x["lossCoeffStd"] = std::round(m->paramStd(1) * 1000.f) / 1000.f;
      x["offsetKh"] = r2(m->offset());
      x["residualStdKh"] = r2(m->residualStd());
      x["trendKh"] = r2(m->trendKh());
      x["deadTimeMin"] = r2(m->delayMin());
      x["deadTimeStdMin"] = r2(m->delayStdMin());
      x["deadTimeSamples"] = m->delaySamples();
      x["faultSuspected"] = m->faultSuspected();
      JsonObject pc = x["samples"].to<JsonObject>();
      for (int c = 0; c < static_cast<int>(PeriodClass::Count); ++c)
        pc[periodClassName(static_cast<PeriodClass>(c))] = m->samples(static_cast<PeriodClass>(c));
      JsonArray ctx = x["recentContext"].to<JsonArray>();
      size_t n = m->context().size();
      for (size_t i = n > 8 ? n - 8 : 0; i < n; ++i) ctx.add(m->context()[i].what);
    }
    return std::string();
  }
  if (name == "get_events") {
    int limit = args["limit"] | 40;
    if (limit < 1 || limit > 100) return "limit 1..100";
    std::vector<Event> ev = sys_.bus.history(0, 200);
    size_t start = ev.size() > static_cast<size_t>(limit) ? ev.size() - static_cast<size_t>(limit) : 0;
    JsonArray a = o["events"].to<JsonArray>();
    Millis now = sys_.clock().nowMs();
    for (size_t i = ev.size(); i-- > start;) {  // newest first
      const Event& e = ev[i];
      if (e.type == EventType::TimeMinute || e.type == EventType::SensorValue) continue;
      JsonObject x = a.add<JsonObject>();
      x["ageMin"] = elapsed(now, e.ts) / 60000u;
      x["type"] = eventTypeName(e.type);
      x["source"] = eventSourceName(e.source);
      if (e.ref[0]) x["ref"] = e.ref;
      if (e.text[0]) x["text"] = e.text;
      if (e.value != 0.f) x["value"] = r2(e.value);
    }
    return std::string();
  }
  if (name == "get_change_log") {
    o["access"] = aiAccessName(sys_.ai.cfg.access);
    sys_.ai.journalToJson(o["changes"].to<JsonArray>(), 20);
    return std::string();
  }
  if (name == "change_settings") {
    std::string reason = args["reason"] | "";
    bool dry = args["dry_run"] | false;
    return aiSubmit(sys_, args["changes"], reason, dry, o);
  }
  if (name == "revert_change") {
    int id = args["id"] | 0;
    if (id <= 0) return "id fehlt";
    return aiRevert(sys_, static_cast<uint16_t>(id), args["reason"] | "", o);
  }
  return "unbekanntes Werkzeug";
}

void McpServer::promptsList(JsonObject result) {
  JsonArray a = result["prompts"].to<JsonArray>();
  JsonObject p = a.add<JsonObject>();
  p["name"] = "optimize_heating";
  p["title"] = "Heizung optimieren";
  p["description"] = "Statistik und Lernmodell auswerten und begruendete, kleine Einstellungsaenderungen vorschlagen.";
  JsonArray args = p["arguments"].to<JsonArray>();
  JsonObject r = args.add<JsonObject>();
  r["name"] = "room";
  r["description"] = "Raum-ID (leer = alle Raeume)";
  r["required"] = false;
  JsonObject f = args.add<JsonObject>();
  f["name"] = "focus";
  f["description"] = "Schwerpunkt, z. B. 'Komfort', 'Energie sparen', 'Ventile schonen'";
  f["required"] = false;
}

bool McpServer::promptsGet(JsonObjectConst params, JsonObject result, std::string& err) {
  std::string name = params["name"] | "";
  if (name != "optimize_heating") {
    err = "Unknown prompt: " + name;
    return false;
  }
  std::string room = params["arguments"]["room"] | "";
  std::string focus = params["arguments"]["focus"] | "ausgewogen: Komfort und Energie";
  std::string scope = room.empty() ? "alle Raeume" : "den Raum '" + room + "'";
  std::string text =
      "Optimiere die Heizungseinstellungen fuer " + scope + " (Schwerpunkt: " + focus +
      ").\n\nVorgehen:\n"
      "1. get_settings und get_overview lesen (Grenzen in aiLimits beachten).\n"
      "2. get_statistics (7 Tage) auswerten: Aufheizdauer (warmupMeanMin) vs. Komfortbeginn, Stunden unter/ueber "
      "Soll, Ueberschwingen, Heizstunden, Schaltungen, Fensterzeiten.\n"
      "3. Bei Auffaelligkeiten get_room_history (48 h) und get_model ansehen; Perioden mit Fenster, Handbetrieb oder "
      "Stoerung nicht als Regelproblem werten.\n"
      "4. Typische Massnahmen: Komfortbeginn um die gemessene Aufheizdauer vorziehen; Hysterese bei Ueberschwingen "
      "bzw. vielen Schaltungen anpassen; Absenktemperatur so waehlen, dass das Aufheizen rechtzeitig gelingt; "
      "Ventil-Mindestzeiten bei sehr vielen Schaltungen erhoehen; Komfortzeiten an tatsaechliche Nutzung anpassen.\n"
      "5. Nur kleine Schritte, je Raum hoechstens 2-3 Aenderungen. Zuerst change_settings mit dry_run, dann ohne. "
      "Begruendung mit Kennzahlen angeben.\n"
      "6. Ergebnis fuer den Nutzer zusammenfassen: was geaendert/vorgeschlagen wurde, warum, erwartete Wirkung, wann "
      "erneut pruefen (fruehestens nach 3-7 Tagen).\n"
      "Sicherheitsgrenzen, Ausgaenge und Regeln sind nicht aenderbar und sollen auch nicht empfohlen werden, ohne den "
      "Nutzer ausdruecklich darauf hinzuweisen.";
  result["description"] = "Heizung optimieren";
  JsonArray msgs = result["messages"].to<JsonArray>();
  JsonObject m = msgs.add<JsonObject>();
  m["role"] = "user";
  m["content"]["type"] = "text";
  m["content"]["text"] = text;
  return true;
}

}  // namespace hc
