#include "hc/stats/Balance.h"

#include <algorithm>
#include <cstdio>

namespace hc {

// ------------------------------------------------------------ config

void BalanceConfig::toJson(JsonObject o) const {
  o["days"] = days;
  JsonArray a = o["notes"].to<JsonArray>();
  for (const BalanceNote& n : notes) {
    JsonObject x = a.add<JsonObject>();
    x["t"] = n.tMin;
    x["room"] = n.room;
    x["text"] = n.text;
  }
}

std::string BalanceConfig::fromJson(JsonObjectConst o) {
  if (o.isNull()) return "";
  int d = o["days"] | static_cast<int>(days);
  if (d < 3 || d > kHistDays) return "Abgleich: Auswertezeitraum 3..14 Tage";
  std::vector<BalanceNote> list = notes;
  if (!o["notes"].isNull()) {
    list.clear();
    for (JsonObjectConst x : o["notes"].as<JsonArrayConst>()) {
      BalanceNote n;
      n.tMin = x["t"] | 0u;
      n.room = x["room"] | "";
      n.text = x["text"] | "";
      if (!n.tMin) continue;
      if (n.text.size() > kMaxText) n.text.resize(kMaxText);
      list.push_back(n);
    }
    std::stable_sort(list.begin(), list.end(), [](const BalanceNote& a, const BalanceNote& b) { return a.tMin < b.tMin; });
    if (list.size() > kMaxNotes) list.erase(list.begin(), list.end() - kMaxNotes);
  }
  days = static_cast<uint8_t>(d);
  notes = std::move(list);
  return "";
}

void BalanceConfig::addNote(uint32_t tMin, const std::string& room, const std::string& text) {
  BalanceNote n;
  n.tMin = tMin;
  n.room = room;
  n.text = text.size() > kMaxText ? text.substr(0, kMaxText) : text;
  auto it = std::upper_bound(notes.begin(), notes.end(), tMin,
                             [](uint32_t t, const BalanceNote& x) { return t < x.tMin; });
  notes.insert(it, n);
  if (notes.size() > kMaxNotes) notes.erase(notes.begin());
}

// ------------------------------------------------------------ names

const char* balanceVerdictName(BalanceVerdict v) {
  switch (v) {
    case BalanceVerdict::Ok: return "ok";
    case BalanceVerdict::IncreaseSlight: return "increase_slight";
    case BalanceVerdict::Increase: return "increase";
    case BalanceVerdict::Reduce: return "reduce";
    default: return "nodata";
  }
}

const char* balanceVerdictLabel(BalanceVerdict v) {
  switch (v) {
    case BalanceVerdict::Ok: return "passt";
    case BalanceVerdict::IncreaseSlight: return "etwas erhöhen";
    case BalanceVerdict::Increase: return "Durchfluss erhöhen";
    case BalanceVerdict::Reduce: return "Durchfluss reduzieren";
    default: return "zu wenig Daten";
  }
}

const char* balanceSystemName(BalanceSystem s) {
  switch (s) {
    case BalanceSystem::Ok: return "ok";
    case BalanceSystem::FlowLow: return "flow_low";
    case BalanceSystem::FlowHigh: return "flow_high";
    default: return "none";
  }
}

// ------------------------------------------------------------ analysis

RoomBalance BalanceAnalyzer::analyzeRoom(const std::string& room, const std::vector<HistPoint>& pts,
                                         uint32_t sinceMin) {
  RoomBalance rb;
  rb.room = room;
  rb.sinceMin = sinceMin;
  constexpr uint8_t kExclude =
      kHistManualSetpoint | kHistManualOutput | kHistFault | kHistSensorFallback | kHistNoSupply;
  float minutes = 0.f, dutySum = 0.f, devSum = 0.f, below = 0.f, above = 0.f, outSum = 0.f, outMin = 0.f;
  float prevSp = NAN;
  uint32_t stepMin = 0;
  for (const HistPoint& p : pts) {
    if (!p.tMin || p.setpoint == kHistNoValue) {
      prevSp = NAN;
      continue;
    }
    float sp = p.setpoint / 100.f;
    if (!std::isnan(prevSp) && sp - prevSp >= 0.5f) stepMin = p.tMin;  // warm-up after a setpoint step
    prevSp = sp;
    if (p.tMin < sinceMin || p.minutes < 10 || p.temp == kHistNoValue) continue;
    if (!(p.flags & kHistComfortSlot) || (p.flags & kExclude) || p.windowMin) continue;
    if (sp < 15.f) continue;
    if (p.outdoor != kHistNoValue && p.outdoor / 100.f > kMaxOutdoor) continue;
    if (stepMin && p.tMin - stepMin < kWarmupSkipMin) continue;
    float m = p.minutes, t = p.temp / 100.f;
    minutes += m;
    dutySum += p.duty * m;
    devSum += (t - sp) * m;
    if (t < sp - 0.5f) below += m;
    if (t > sp + 0.5f) above += m;
    if (p.outdoor != kHistNoValue) {
      outSum += p.outdoor / 100.f * m;
      outMin += m;
    }
  }
  rb.hours = minutes / 60.f;
  if (minutes > 0.f) {
    rb.duty = dutySum / minutes;
    rb.dev = devSum / minutes;
    rb.belowPct = below / minutes * 100.f;
    rb.abovePct = above / minutes * 100.f;
  }
  if (outMin > 0.f) rb.outdoor = outSum / outMin;
  rb.externalHeat = minutes > 0.f && rb.abovePct >= 25.f && rb.duty <= 15.f;
  return rb;
}

void BalanceAnalyzer::classify(BalanceResult& r) {
  std::vector<float> duties;
  for (const RoomBalance& rb : r.rooms)
    if (rb.hours >= kMinHours) duties.push_back(rb.duty);
  r.assessed = static_cast<int>(duties.size());
  r.medianDuty = NAN;
  r.system = BalanceSystem::None;
  if (!duties.empty()) {
    std::sort(duties.begin(), duties.end());
    size_t n = duties.size();
    r.medianDuty = n % 2 ? duties[n / 2] : (duties[n / 2 - 1] + duties[n / 2]) / 2.f;
  }
  int increase = 0, reduce = 0;
  bool allLow = r.assessed >= 2;
  for (RoomBalance& rb : r.rooms) {
    if (rb.hours < kMinHours) {
      rb.verdict = BalanceVerdict::NoData;
      continue;
    }
    if (rb.duty >= 80.f && rb.dev <= -0.4f)
      rb.verdict = BalanceVerdict::Increase;  // valve open nearly all the time and still too cold
    else if (rb.duty >= 80.f)
      rb.verdict = BalanceVerdict::IncreaseSlight;  // holds the setpoint only just
    else if (r.assessed >= 2 && rb.duty >= 60.f && rb.duty >= r.medianDuty + 30.f)
      rb.verdict = BalanceVerdict::IncreaseSlight;  // works much harder than the others
    else if (rb.dev >= -0.2f && (rb.duty <= 20.f || (rb.abovePct >= 25.f && rb.duty <= 45.f)))
      rb.verdict = BalanceVerdict::Reduce;  // needs little valve time or overshoots
    else
      rb.verdict = BalanceVerdict::Ok;
    if (rb.verdict == BalanceVerdict::Increase) ++increase;
    if (rb.verdict == BalanceVerdict::Reduce) ++reduce;
    if (rb.duty > 35.f || rb.dev < -0.3f) allLow = false;
  }
  if (!r.assessed) return;
  if (increase && increase * 2 >= r.assessed && !reduce) {
    // most rooms are short of heat and none has heat to spare: more flow at one
    // circuit only takes it from another - the flow temperature is too low
    r.system = BalanceSystem::FlowLow;
  } else if (allLow) {
    // every room needs little valve time: the flow temperature could be lower;
    // relative to each other only clear outliers are worth reducing
    r.system = BalanceSystem::FlowHigh;
    for (RoomBalance& rb : r.rooms)
      if (rb.verdict == BalanceVerdict::Reduce && rb.duty > r.medianDuty * 0.5f)
        rb.verdict = BalanceVerdict::Ok;
  } else {
    r.system = BalanceSystem::Ok;
  }
}

std::string BalanceAnalyzer::advice(const RoomBalance& rb, BalanceSystem sys, bool multi) {
  std::string s;
  switch (rb.verdict) {
    case BalanceVerdict::Increase:
      s = "Der Raum erreicht den Sollwert nicht, obwohl das Ventil fast immer offen ist. Durchfluss am Verteiler "
          "erhöhen (etwa +0,5 l/min bzw. eine halbe Umdrehung weiter öffnen).";
      if (sys == BalanceSystem::FlowLow) s += " Da mehrere Räume zu wenig Wärme bekommen: zuerst die Vorlauftemperatur anheben.";
      break;
    case BalanceVerdict::IncreaseSlight:
      s = rb.duty >= 80.f ? "Der Sollwert wird nur knapp gehalten, das Ventil ist fast immer offen. Durchfluss etwas "
                            "erhöhen (etwa +0,25 l/min)."
                          : "Das Ventil ist deutlich länger offen als in den anderen Räumen. Durchfluss etwas erhöhen "
                            "(etwa +0,25 l/min).";
      break;
    case BalanceVerdict::Reduce:
      s = rb.externalHeat
              ? "Der Raum wird wärmer als gewünscht, obwohl das Ventil selten offen ist – vermutlich Fremdwärme "
                "(Sonne, Herd, Ofen). Der Durchfluss kann reduziert werden (etwa −0,25 l/min); die Bewertung ist hier "
                "weniger aussagekräftig."
              : "Der Raum hält den Sollwert mit wenig Ventilzeit. Durchfluss etwas reduzieren (etwa −0,25 l/min) – "
                "die anderen Kreise bekommen dadurch mehr.";
      break;
    case BalanceVerdict::Ok: s = "Passt – nichts ändern."; break;
    default: {
      char b[160];
      std::snprintf(b, sizeof(b),
                    rb.afterNote ? "Nach der letzten Änderung wird noch gesammelt (%.0f von %.0f h geeigneter Heizzeit)."
                                 : "Noch zu wenig geeignete Heizzeit (%.0f von %.0f h in Komfortzeiten ohne Fenster, "
                                   "Handbetrieb oder Störung).",
                    static_cast<double>(std::floor(rb.hours)), static_cast<double>(kMinHours));
      s = b;
    }
  }
  if (multi && (rb.verdict == BalanceVerdict::Increase || rb.verdict == BalanceVerdict::IncreaseSlight ||
                rb.verdict == BalanceVerdict::Reduce))
    s += " Gilt für alle Kreise des Raums (gleichmäßig verstellen).";
  return s;
}

std::string BalanceAnalyzer::systemAdvice(BalanceSystem sys) {
  switch (sys) {
    case BalanceSystem::FlowLow:
      return "Mehrere Räume bleiben trotz offener Ventile zu kalt und keiner hat Wärme übrig: der Abgleich allein "
             "hilft hier nicht. Zuerst die Vorlauftemperatur (Heizkurve) anheben, danach erneut auswerten.";
    case BalanceSystem::FlowHigh:
      return "Alle Räume halten den Sollwert mit wenig Ventilzeit. Die Vorlauftemperatur könnte gesenkt werden "
             "(spart Energie, gleichmäßigere Raumtemperatur).";
    case BalanceSystem::Ok:
      return "Bei Empfehlungen jeweils nur einen Schritt verstellen, im Änderungsprotokoll notieren und die nächste "
             "Auswertung abwarten (ein Tag Einschwingzeit, dann mindestens 12 Stunden Daten).";
    default: return "Noch keine Auswertung möglich – es fehlen geeignete Heiztage.";
  }
}

}  // namespace hc
