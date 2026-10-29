#pragma once
// HTTP server: REST API (forwarded to hc::ApiRouter), WebSocket event
// stream, OTA upload and the embedded single-page web interface.
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include <atomic>
#include <string>

#include "hc/api/ApiRouter.h"
#include "platform/Ota.h"
#include "platform/Queues.h"

namespace platform {

class WebUi {
 public:
  WebUi(hc::HeatCoreSystem& sys, Ota& ota);
  void begin();
  void loop();  // called from the network task
  void onEvent(const hc::Event& e);  // system lock held
  bool rebootRequested() const { return reboot_; }
  void requestReboot() { reboot_ = true; }
  bool otaRunning() const { return ota_.busy(); }

 private:
  bool authorized(AsyncWebServerRequest* r);
  void setupOta();

  hc::HeatCoreSystem& sys_;
  hc::ApiRouter api_;
  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  BoundedQueue<std::string> wsOut_{64};
  std::atomic<bool> reboot_{false};
  Ota& ota_;
  std::atomic<AsyncWebServerRequest*> otaOwner_{nullptr};
  uint32_t lastCleanup_ = 0;
  friend class ApiHandler;
};

}  // namespace platform
