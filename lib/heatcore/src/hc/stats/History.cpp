#include "hc/stats/History.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hc {

namespace {

int16_t centi(float v) {
  float c = std::round(v * 100.f);
  if (c > 32000.f) c = 32000.f;
  if (c < -32000.f) c = -32000.f;
  return static_cast<int16_t>(c);
}

bool has(int16_t v) { return v != kHistNoValue; }
float val(int16_t v) { return v / 100.f; }
float round2(float v) { return std::round(v * 100.f) / 100.f; }

uint8_t sat8(uint32_t v) { return static_cast<uint8_t>(v > 255u ? 255u : v); }

int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

uint32_t fnv1a(const std::string& s, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<uint8_t>(s[i]);
    h *= 16777619u;
  }
  return h;
}

void putU16(std::string& o, uint16_t v) {
  o += static_cast<char>(v & 0xFF);
  o += static_cast<char>(v >> 8);
}
uint16_t getU16(const std::string& s, size_t p) {
  return static_cast<uint16_t>(static_cast<uint8_t>(s[p]) | (static_cast<uint8_t>(s[p + 1]) << 8));
}

}  // namespace

int32_t utcOffsetSec(const IClock& clock) {
  LocalTime lt = clock.local();
  int64_t ep = clock.epoch();
  if (!lt.valid || !ep) return 0;
  int64_t local = daysFromCivil(lt.year, lt.month, lt.day) * 86400 + lt.hour * 3600 + lt.minute * 60 + lt.second;
  int64_t off = local - ep;
  off = (off >= 0 ? off + 450 : off - 450) / 900 * 900;  // whole quarter hours
  return static_cast<int32_t>(off);
}

std::string formatLocalMinute(uint32_t epochMin, int32_t off) {
  if (!epochMin) return std::string();
  LocalTime lt = localTimeFromEpoch(static_cast<int64_t>(epochMin) * 60 + off);
  char b[24];
  std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d", lt.year, lt.month, lt.day, lt.hour, lt.minute);
  return b;
}

// ------------------------------------------------------------ recording

History::Series* History::find(const std::string& name) {
  for (auto& s : series_)
    if (s.name == name) return &s;
  return nullptr;
}

const History::Series* History::find(const std::string& name) const {
  for (auto& s : series_)
    if (s.name == name) return &s;
  return nullptr;
}

void History::push(Series& s, const HistPoint& p) {
  if (s.ring.size() != cap_) s.ring.assign(cap_, HistPoint());
  s.ring[s.head] = p;
  s.head = (s.head + 1) % cap_;
  if (s.size < cap_) ++s.size;
}

void History::flush(Series& s) {
  Acc& a = s.acc;
  if (!a.open || !a.minutes) {
    a = Acc();
    return;
  }
  HistPoint p;
  p.tMin = a.bucket;
  if (a.tN) p.temp = centi(a.tSum / a.tN);
  if (a.spN) p.setpoint = centi(a.spSum / a.spN);
  if (a.outN) p.outdoor = centi(a.outSum / a.outN);
  p.duty = static_cast<uint8_t>(std::lround(std::min(1.f, std::max(0.f, a.dutySum / a.minutes)) * 100.f));
  p.windowMin = a.windowMin;
  p.minutes = a.minutes;
  p.flags = a.flags;
  p.switches = sat8(a.switches);
  push(s, p);
  ++revision_;
  a = Acc();
}

void History::record(const std::string& name, const HistSample& in, uint32_t epochMin) {
  Series* s = find(name);
  if (!s) {
    series_.push_back(Series());
    s = &series_.back();
    s->name = name;
  }
  uint32_t bucket = epochMin ? epochMin - epochMin % kHistBucketMin : 0;
  Acc& a = s->acc;
  if (a.open && (a.bucket != bucket || a.minutes >= kHistBucketMin)) flush(*s);
  if (!a.open) {
    a.open = true;
    a.bucket = bucket;
  }
  ++a.minutes;
  if (in.tempValid) {
    a.tSum += in.temp;
    ++a.tN;
  }
  if (in.setpointValid) {
    a.spSum += in.setpoint;
    ++a.spN;
  }
  if (in.outdoorValid) {
    a.outSum += in.outdoor;
    ++a.outN;
  }
  a.dutySum += std::min(1.f, std::max(0.f, in.duty));
  if (in.windowOpen) ++a.windowMin;
  a.flags |= in.flags;
  a.switches = static_cast<uint16_t>(a.switches + in.switches);
}

void History::keepOnly(const std::vector<std::string>& names) {
  series_.erase(std::remove_if(series_.begin(), series_.end(),
                               [&](const Series& s) {
                                 return s.name != kHistSystemSeries &&
                                        std::find(names.begin(), names.end(), s.name) == names.end();
                               }),
                series_.end());
}

void History::clear() {
  series_.clear();
  ++revision_;
}

std::vector<std::string> History::seriesNames() const {
  std::vector<std::string> n;
  for (auto& s : series_) n.push_back(s.name);
  return n;
}

size_t History::points(const std::string& name) const {
  const Series* s = find(name);
  return s ? s->size : 0;
}

std::vector<HistPoint> History::ordered(const Series& s) const {
  std::vector<HistPoint> out;
  out.reserve(s.size);
  size_t start = (s.head + cap_ - s.size) % cap_;
  for (size_t i = 0; i < s.size; ++i) out.push_back(s.ring[(start + i) % cap_]);
  return out;
}

std::vector<HistPoint> History::query(const std::string& name, uint32_t hours) const {
  const Series* s = find(name);
  if (!s) return {};
  std::vector<HistPoint> all = ordered(*s);
  size_t want = static_cast<size_t>(hours) * 60 / kHistBucketMin;
  if (all.size() > want) all.erase(all.begin(), all.end() - static_cast<long>(want));
  return all;
}

std::vector<HistPoint> History::downsample(const std::vector<HistPoint>& pts, uint16_t res) {
  if (res <= kHistBucketMin) return pts;
  if (res > 240) res = 240;
  size_t k = res / kHistBucketMin;
  std::vector<HistPoint> out;
  size_t i = 0;
  while (i < pts.size()) {
    // group by aligned time when known, otherwise by position
    size_t j = i;
    uint32_t key = pts[i].tMin ? pts[i].tMin / res : 0;
    float tS = 0, tW = 0, sS = 0, sW = 0, oS = 0, oW = 0, dS = 0;
    uint32_t minutes = 0, win = 0, sw = 0;
    uint8_t flags = 0;
    while (j < pts.size() && j - i < k && (pts[i].tMin == 0 || pts[j].tMin / res == key)) {
      const HistPoint& p = pts[j];
      float w = p.minutes;
      if (has(p.temp)) tS += val(p.temp) * w, tW += w;
      if (has(p.setpoint)) sS += val(p.setpoint) * w, sW += w;
      if (has(p.outdoor)) oS += val(p.outdoor) * w, oW += w;
      dS += p.duty * w;
      minutes += p.minutes;
      win += p.windowMin;
      sw += p.switches;
      flags |= p.flags;
      ++j;
    }
    HistPoint m;
    m.tMin = pts[i].tMin ? key * res : 0;
    if (tW > 0) m.temp = centi(tS / tW);
    if (sW > 0) m.setpoint = centi(sS / sW);
    if (oW > 0) m.outdoor = centi(oS / oW);
    m.duty = minutes ? static_cast<uint8_t>(std::lround(dS / minutes)) : 0;
    m.minutes = sat8(minutes);
    m.windowMin = sat8(win);
    m.switches = sat8(sw);
    m.flags = flags;
    out.push_back(m);
    i = j;
  }
  return out;
}

// ------------------------------------------------------------ KPIs

RoomKpis History::computeKpis(const std::vector<HistPoint>& pts) {
  RoomKpis k;
  double tSum = 0, spSum = 0, spH = 0, outSum = 0, outH = 0, devSum = 0, absSum = 0, comfortH = 0;
  double wSum = 0;
  float wMax = NAN;
  int sinceHeat = 1000;  // buckets since the valve was last open
  const uint8_t excludeFlags = kHistFault | kHistManualSetpoint | kHistManualOutput;
  for (size_t i = 0; i < pts.size(); ++i) {
    const HistPoint& p = pts[i];
    if (!p.minutes) continue;
    double h = p.minutes / 60.0;
    if (has(p.temp)) {
      k.hours += static_cast<float>(h);
      tSum += val(p.temp) * h;
    }
    if (has(p.setpoint)) spSum += val(p.setpoint) * h, spH += h;
    if (has(p.outdoor)) outSum += val(p.outdoor) * h, outH += h;
    k.heatingHours += static_cast<float>(p.duty / 100.0 * h);
    k.switches += p.switches;
    k.windowHours += p.windowMin / 60.f;
    if (p.flags & kHistFault) k.faultHours += static_cast<float>(h);
    if (p.flags & (kHistManualSetpoint | kHistManualOutput)) k.manualHours += static_cast<float>(h);
    sinceHeat = p.duty > 0 ? 0 : sinceHeat + 1;
    bool excluded = p.windowMin > 0 || (p.flags & excludeFlags);
    if (!has(p.temp) || !has(p.setpoint) || excluded) continue;
    float dev = val(p.temp) - val(p.setpoint);
    if (p.flags & kHistComfortSlot) {
      devSum += dev * h;
      absSum += std::fabs(dev) * h;
      comfortH += h;
    }
    if (dev < -0.5f) k.belowHours += static_cast<float>(h);
    if (dev > 0.5f && p.duty == 0) k.aboveHours += static_cast<float>(h);
    // overshoot: up to 2 h after the valve closed
    if (p.duty == 0 && sinceHeat <= 8 && dev > 0 && !(dev <= k.maxOvershootK)) k.maxOvershootK = dev;
    // warm-up after an upward setpoint step
    if (i > 0 && has(pts[i - 1].setpoint) && val(p.setpoint) - val(pts[i - 1].setpoint) >= 1.0f) {
      float target = val(p.setpoint) - 0.3f;
      bool done = false;
      for (size_t j = i; j < pts.size() && j - i < 24; ++j) {
        const HistPoint& q = pts[j];
        if (q.windowMin > 0 || (q.flags & excludeFlags) || !has(q.setpoint) ||
            val(q.setpoint) < val(p.setpoint) - 0.05f) {
          done = true;  // disturbed or setpoint lowered again: not counted
          break;
        }
        if (has(q.temp) && val(q.temp) >= target) {
          float minutes = static_cast<float>((j - i) * kHistBucketMin) + kHistBucketMin / 2.f;
          ++k.warmups;
          wSum += minutes;
          if (!(minutes <= wMax)) wMax = minutes;
          done = true;
          break;
        }
        if (j + 1 == pts.size()) done = true;  // still running at the end of the data
      }
      if (!done) ++k.warmupsNotReached;
    }
  }
  if (k.hours > 0) k.meanTemp = static_cast<float>(tSum / k.hours);
  if (spH > 0) k.meanSetpoint = static_cast<float>(spSum / spH);
  if (outH > 0) k.meanOutdoor = static_cast<float>(outSum / outH);
  if (comfortH > 0) {
    k.comfortMeanDev = static_cast<float>(devSum / comfortH);
    k.comfortMeanAbsDev = static_cast<float>(absSum / comfortH);
  }
  if (k.warmups) {
    k.warmupMeanMin = static_cast<float>(wSum / k.warmups);
    k.warmupMaxMin = wMax;
  }
  return k;
}

// ------------------------------------------------------------ JSON

void History::pointsToJson(const std::vector<HistPoint>& pts, JsonObject o, int32_t off) {
  JsonArray t = o["t"].to<JsonArray>();
  JsonArray temp = o["temp"].to<JsonArray>();
  JsonArray sp = o["setpoint"].to<JsonArray>();
  JsonArray out = o["outdoor"].to<JsonArray>();
  JsonArray duty = o["dutyPct"].to<JsonArray>();
  JsonArray win = o["windowMin"].to<JsonArray>();
  JsonArray sw = o["switches"].to<JsonArray>();
  JsonArray fl = o["flags"].to<JsonArray>();
  auto addVal = [](JsonArray a, int16_t v) {
    if (has(v)) a.add(round2(val(v)));
    else a.add<JsonVariant>();  // null
  };
  for (const HistPoint& p : pts) {
    t.add(formatLocalMinute(p.tMin, off));
    addVal(temp, p.temp);
    addVal(sp, p.setpoint);
    addVal(out, p.outdoor);
    duty.add(p.duty);
    win.add(p.windowMin);
    sw.add(p.switches);
    fl.add(p.flags);
  }
  o["flagBits"] = "1=Handsollwert 2=Handbetrieb Ausgang 4=Stoerung 8=Ersatzfuehler 16=Komfortzeit 32=Modellberatung aktiv";
}

void History::kpisToJson(const RoomKpis& k, JsonObject o) {
  auto put = [&](const char* key, float v) {
    if (!std::isnan(v)) o[key] = round2(v);
  };
  put("hoursWithData", k.hours);
  put("meanTemp", k.meanTemp);
  put("meanSetpoint", k.meanSetpoint);
  put("meanOutdoor", k.meanOutdoor);
  put("comfortMeanDevK", k.comfortMeanDev);
  put("comfortMeanAbsDevK", k.comfortMeanAbsDev);
  put("belowSetpointHours", k.belowHours);
  put("aboveSetpointHours", k.aboveHours);
  put("maxOvershootK", k.maxOvershootK);
  put("heatingHours", k.heatingHours);
  o["switches"] = k.switches;
  put("windowHours", k.windowHours);
  put("faultHours", k.faultHours);
  put("manualHours", k.manualHours);
  o["warmups"] = k.warmups;
  put("warmupMeanMin", k.warmupMeanMin);
  put("warmupMaxMin", k.warmupMaxMin);
  o["warmupsNotReachedIn6h"] = k.warmupsNotReached;
}

void History::dailyKpisToJson(const std::string& name, uint32_t nowEpochMin, int32_t off, uint16_t days,
                              JsonObject o) const {
  if (days < 1) days = 1;
  if (days > kHistDays) days = kHistDays;
  std::vector<HistPoint> pts = query(name, static_cast<uint32_t>(days) * 24);
  JsonArray arr = o["days"].to<JsonArray>();
  if (nowEpochMin) {
    int64_t localNow = static_cast<int64_t>(nowEpochMin) * 60 + off;
    int64_t today = localNow / 86400;
    for (int d = days - 1; d >= 0; --d) {
      int64_t day = today - d;
      std::vector<HistPoint> sel;
      for (auto& p : pts)
        if (p.tMin && (static_cast<int64_t>(p.tMin) * 60 + off) / 86400 == day) sel.push_back(p);
      if (sel.empty()) continue;
      JsonObject x = arr.add<JsonObject>();
      LocalTime lt = localTimeFromEpoch(day * 86400);
      char b[12];
      std::snprintf(b, sizeof(b), "%04d-%02d-%02d", lt.year, lt.month, lt.day);
      x["date"] = b;
      kpisToJson(computeKpis(sel), x);
    }
  }
  kpisToJson(computeKpis(pts), o["total"].to<JsonObject>());
}

// ------------------------------------------------------------ persistence

std::string History::serialize() const {
  std::string o = "HCH1";
  putU16(o, 1);
  putU16(o, cap_);
  putU16(o, static_cast<uint16_t>(series_.size()));
  for (auto& s : series_) {
    std::string n = s.name.substr(0, 255);
    o += static_cast<char>(n.size());
    o += n;
    std::vector<HistPoint> pts = ordered(s);
    putU16(o, static_cast<uint16_t>(pts.size()));
    if (!pts.empty()) o.append(reinterpret_cast<const char*>(pts.data()), pts.size() * sizeof(HistPoint));
  }
  uint32_t h = fnv1a(o, o.size());
  for (int i = 0; i < 4; ++i) o += static_cast<char>((h >> (8 * i)) & 0xFF);
  return o;
}

bool History::deserialize(const std::string& b) {
  if (b.size() < 14 || b.compare(0, 4, "HCH1") != 0) return false;
  uint32_t h = 0;
  for (int i = 0; i < 4; ++i) h |= static_cast<uint32_t>(static_cast<uint8_t>(b[b.size() - 4 + i])) << (8 * i);
  if (h != fnv1a(b, b.size() - 4)) return false;
  if (getU16(b, 4) != 1) return false;
  size_t p = 10, end = b.size() - 4;
  uint16_t n = getU16(b, 8);
  std::vector<Series> loaded;
  for (uint16_t i = 0; i < n; ++i) {
    if (p + 1 > end) return false;
    size_t len = static_cast<uint8_t>(b[p++]);
    if (p + len + 2 > end) return false;
    Series s;
    s.name = b.substr(p, len);
    p += len;
    size_t cnt = getU16(b, p);
    p += 2;
    if (p + cnt * sizeof(HistPoint) > end) return false;
    size_t skip = cnt > cap_ ? cnt - cap_ : 0;
    for (size_t k = 0; k < cnt; ++k) {
      if (k >= skip) {
        HistPoint pt;
        std::memcpy(&pt, b.data() + p + k * sizeof(HistPoint), sizeof(HistPoint));
        push(s, pt);
      }
    }
    p += cnt * sizeof(HistPoint);
    loaded.push_back(std::move(s));
  }
  series_ = std::move(loaded);
  return true;
}

}  // namespace hc
