#include "platform/WebUi.h"

#include <Update.h>

#include <cstdlib>
#include <mutex>

#include "generated/web_assets.h"
#include "hc/core/Log.h"

namespace platform {

namespace {
constexpr const char* TAG = "web";
constexpr size_t kMaxBody = 48 * 1024;
}  // namespace

// Forwards /api/* to the platform-independent router.
class ApiHandler : public AsyncWebHandler {
 public:
  explicit ApiHandler(WebUi& ui) : ui_(ui) {}
  bool canHandle(AsyncWebServerRequest* r) const override {
    return r->url().startsWith("/api/") && r->url() != "/api/ota";
  }
  void handleBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) override {
    if (total > kMaxBody) return;
    if (index == 0 && !r->_tempObject) r->_tempObject = calloc(total + 1, 1);
    if (r->_tempObject && index + len <= total) memcpy(static_cast<uint8_t*>(r->_tempObject) + index, data, len);
  }
  void handleRequest(AsyncWebServerRequest* r) override {
    if (!ui_.authorized(r)) return r->requestAuthentication();
    if (r->contentLength() > kMaxBody) {
      r->send(413, "application/json", "{\"error\":\"Anfrage zu gross\"}");
      return;
    }
    hc::ApiRequest req;
    req.method = r->methodToString();
    req.path = r->url().c_str();
    for (size_t i = 0; i < r->params(); ++i) {
      const AsyncWebParameter* p = r->getParam(i);
      if (!p->isPost() && !p->isFile()) req.query[p->name().c_str()] = p->value().c_str();
    }
    if (r->_tempObject) req.body = static_cast<const char*>(r->_tempObject);
    hc::ApiResponse res;
    {
      std::lock_guard<std::recursive_mutex> lk(ui_.sys_.mutex());
      res = ui_.api_.handle(req);
    }
    AsyncWebServerResponse* resp = r->beginResponse(res.status, res.contentType, res.body.c_str());
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
  }
  bool isRequestHandlerTrivial() const override { return false; }

 private:
  WebUi& ui_;
};

WebUi::WebUi(hc::HeatCoreSystem& sys) : sys_(sys), api_(sys) {}

bool WebUi::authorized(AsyncWebServerRequest* r) {
  std::string user, pass;
  {
    std::lock_guard<std::recursive_mutex> lk(sys_.mutex());
    user = sys_.sysCfg.webUser;
    pass = sys_.sysCfg.webPassword;
  }
  if (pass.empty()) return true;
  return r->authenticate(user.c_str(), pass.c_str());
}

void WebUi::setupOta() {
  server_.on(
      "/api/ota", HTTP_POST,
      [this](AsyncWebServerRequest* r) {
        if (!authorized(r)) return r->requestAuthentication();
        bool ok = !Update.hasError() && otaError_.empty();
        r->send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true,\"message\":\"Update erfolgreich - Neustart\"}"
                   : (std::string("{\"error\":\"") + otaError_ + "\"}").c_str());
        otaRunning_ = false;
        if (ok) reboot_ = true;
      },
      [this](AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          otaError_.clear();
          if (!authorized(r)) {
            otaError_ = "nicht angemeldet";
            return;
          }
          otaRunning_ = true;
          HC_LOGI(TAG, "OTA start: %s", filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) otaError_ = Update.errorString();
        }
        if (otaError_.empty() && len && Update.write(data, len) != len) otaError_ = Update.errorString();
        if (final && otaError_.empty() && !Update.end(true)) otaError_ = Update.errorString();
      });
}

void WebUi::begin() {
  ws_.onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType type, void*, uint8_t*, size_t) {
    if (type == WS_EVT_CONNECT) c->text("{\"t\":\"hello\",\"fw\":\"" + String(hc::kFirmwareVersion) + "\"}");
  });
  ws_.setFilter([this](AsyncWebServerRequest* r) { return authorized(r); });
  server_.addHandler(&ws_);
  setupOta();
  server_.addHandler(new ApiHandler(*this));
  auto index = [this](AsyncWebServerRequest* r) {
    if (!authorized(r)) return r->requestAuthentication();
    AsyncWebServerResponse* resp = r->beginResponse(200, "text/html", kIndexHtmlGz, kIndexHtmlGzLen);
    resp->addHeader("Content-Encoding", "gzip");
    resp->addHeader("Cache-Control", "no-cache");
    r->send(resp);
  };
  server_.on("/", HTTP_GET, index);
  server_.on("/index.html", HTTP_GET, index);
  server_.onNotFound([index](AsyncWebServerRequest* r) {
    if (r->url().startsWith("/api/")) r->send(404, "application/json", "{\"error\":\"nicht gefunden\"}");
    else index(r);  // single page application
  });
  server_.begin();
}

void WebUi::onEvent(const hc::Event& e) {
  // compact JSON for the live views (event log, RF monitor, display preview)
  char buf[200];
  int n = std::snprintf(buf, sizeof(buf),
                        "{\"t\":\"event\",\"seq\":%lu,\"type\":\"%s\",\"source\":\"%s\",\"gesture\":\"%s\",\"ref\":\"%s\","
                        "\"a\":%ld,\"b\":%ld,\"value\":%.2f}",
                        static_cast<unsigned long>(e.seq), hc::eventTypeName(e.type), hc::eventSourceName(e.source),
                        hc::gestureName(e.gesture), e.ref, static_cast<long>(e.a), static_cast<long>(e.b), e.value);
  if (n > 0 && n < static_cast<int>(sizeof(buf))) wsOut_.push(buf);
}

void WebUi::loop() {
  std::string m;
  for (int i = 0; i < 16 && wsOut_.pop(m); ++i)
    if (ws_.count() && ws_.availableForWriteAll()) ws_.textAll(m.c_str());
  uint32_t now = millis();
  if (now - lastCleanup_ > 2000) {
    lastCleanup_ = now;
    ws_.cleanupClients(4);
  }
}

}  // namespace platform
