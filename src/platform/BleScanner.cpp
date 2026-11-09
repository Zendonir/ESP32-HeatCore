#include "platform/BleScanner.h"

#include <sdkconfig.h>

#if defined(CONFIG_BT_NIMBLE_ENABLED)

#include <BLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "hc/ble/BleDecoders.h"

namespace platform {

namespace {

QueueHandle_t g_queue = nullptr;
std::atomic<uint32_t> g_dropped{0};

void push(const ble_addr_t& addr, int8_t rssi, const uint8_t* data, uint8_t len) {
  if (!g_queue || !data || len == 0 || len > sizeof(BleAdvert::data)) return;
  if (!hc::ble::isCandidate(data, len)) return;  // phones, beacons, ... are ignored right here
  BleAdvert a;
  std::memcpy(a.mac, addr.val, 6);
  a.rssi = rssi;
  a.len = len;
  std::memcpy(a.data, data, len);
  if (xQueueSend(g_queue, &a, 0) != pdTRUE) g_dropped.fetch_add(1);
}

// runs in the NimBLE host task: keep it short
int onGap(struct ble_gap_event* ev, void*) {
  switch (ev->type) {
    case BLE_GAP_EVENT_DISC: push(ev->disc.addr, ev->disc.rssi, ev->disc.data, ev->disc.length_data); break;
#if MYNEWT_VAL(BLE_EXT_ADV)
    case BLE_GAP_EVENT_EXT_DISC:
      push(ev->ext_disc.addr, ev->ext_disc.rssi, ev->ext_disc.data, ev->ext_disc.length_data);
      break;
#endif
    default: break;
  }
  return 0;
}

}  // namespace

bool BleScanner::supported() { return true; }

bool BleScanner::begin() {
  if (started_) return true;
  if (!g_queue) g_queue = xQueueCreate(32, sizeof(BleAdvert));
  if (!g_queue) {
    error_ = "kein Speicher fuer die Empfangswarteschlange";
    return false;
  }
  if (!BLEDevice::init("")) {
    error_ = "Bluetooth-Stack startet nicht";
    return false;
  }
  started_ = true;
  error_.clear();
  return true;
}

bool BleScanner::scanning() const { return started_ && ble_gap_disc_active(); }

void BleScanner::setScanning(bool on) {
  if (!started_) return;
  bool active = ble_gap_disc_active();
  if (on && !active) {
    if (!ble_hs_synced()) return;  // host reset in progress: retried on the next call
    struct ble_gap_disc_params p;
    std::memset(&p, 0, sizeof(p));
    p.itvl = 160;    // 100 ms
    p.window = 48;   // 30 ms listening: leaves the shared antenna mostly to Wi-Fi
    p.filter_policy = 0;  // accept all
    p.limited = 0;
    p.passive = 1;
    p.filter_duplicates = 0;  // thermometers repeat the same address with new values
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, onGap, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
      char b[64];
      std::snprintf(b, sizeof(b), "Suche startet nicht (Fehler %d)", rc);
      error_ = b;
    } else {
      error_.clear();
    }
  } else if (!on && active) {
    ble_gap_disc_cancel();
  }
}

bool BleScanner::pop(BleAdvert& a) { return g_queue && xQueueReceive(g_queue, &a, 0) == pdTRUE; }

uint32_t BleScanner::dropped() const { return g_dropped.load(); }

}  // namespace platform

#else  // firmware without Bluetooth host

namespace platform {
bool BleScanner::supported() { return false; }
bool BleScanner::begin() {
  error_ = "Firmware ohne Bluetooth";
  return false;
}
bool BleScanner::scanning() const { return false; }
void BleScanner::setScanning(bool) {}
bool BleScanner::pop(BleAdvert&) { return false; }
uint32_t BleScanner::dropped() const { return 0; }
}  // namespace platform

#endif
