#include "platform/WebUi.h"

#include <cstdio>
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

WebUi::WebUi(hc::HeatCoreSystem& sys, Ota& ota) : sys_(sys), api_(sys), ota_(ota) {}

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

namespace {
// Per-request upload state (freed by the web server with free()).
struct OtaUpload {
  bool started;
  bool failed;
  bool done;
  char error[160];
};

void setError(OtaUpload* u, const std::string& e) {
  u->failed = true;
  std::snprintf(u->error, sizeof(u->error), "%s", e.c_str());
}

std::string jsonEscape(const char* s) {
  std::string o;
  for (; *s; ++s) {
    if (*s == '"' || *s == '\\') o += '\\';
    if (static_cast<unsigned char>(*s) >= 0x20) o += *s;
  }
  return o;
}
}  // namespace

// POST /api/ota?size=<bytes>&sha256=<hex>&source=<text>  (multipart file)
// size and sha256 are optional; the online update passes the values of the
// release so that a damaged download is never installed.
void WebUi::setupOta() {
  server_.on(
      "/api/ota", HTTP_POST,
      [this](AsyncWebServerRequest* r) {
        if (!authorized(r)) return r->requestAuthentication();
        auto* u = static_cast<OtaUpload*>(r->_tempObject);
        if (!u || !u->done || u->failed) {
          std::string msg = u && u->error[0] ? u->error : "keine Firmware-Datei empfangen";
          r->send(u && u->started ? 422 : 409, "application/json", ("{\"error\":\"" + jsonEscape(msg.c_str()) + "\"}").c_str());
          return;
        }
        r->send(200, "application/json", "{\"ok\":true,\"message\":\"Firmware geprueft und installiert - Neustart\"}");
        reboot_ = true;
      },
      [this](AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          if (!r->_tempObject) r->_tempObject = calloc(1, sizeof(OtaUpload));
          auto* u = static_cast<OtaUpload*>(r->_tempObject);
          if (!u || u->started || u->failed) return;
          if (!authorized(r)) return setError(u, "nicht angemeldet");
          size_t size = r->hasParam("size") ? static_cast<size_t>(r->getParam("size")->value().toInt()) : 0;
          std::string sha = r->hasParam("sha256") ? r->getParam("sha256")->value().c_str() : "";
          std::string src = r->hasParam("source") ? r->getParam("source")->value().c_str() : "Upload";
          if (src.size() > 24) src.resize(24);
          std::string err = ota_.start(size, sha, src.c_str());
          if (!err.empty()) return setError(u, err);
          u->started = true;
          otaOwner_ = r;
          HC_LOGI(TAG, "OTA upload %s", filename.c_str());
          // connection lost mid-transfer: discard the partial image
          r->onDisconnect([this, r]() {
            AsyncWebServerRequest* owner = r;
            if (otaOwner_.compare_exchange_strong(owner, nullptr)) ota_.abort("Verbindung abgebrochen");
          });
        }
        auto* u = static_cast<OtaUpload*>(r->_tempObject);
        if (!u || !u->started || u->failed || u->done) return;
        if (len && !ota_.write(data, len)) {
          setError(u, ota_.lastError());
          AsyncWebServerRequest* owner = r;
          if (otaOwner_.compare_exchange_strong(owner, nullptr)) ota_.abort(u->error);
          return;
        }
        if (final) {
          AsyncWebServerRequest* owner = r;
          if (!otaOwner_.compare_exchange_strong(owner, nullptr)) return setError(u, "Update abgebrochen");
          std::string err = ota_.finish();
          if (!err.empty()) return setError(u, err);
          u->done = true;
        }
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
