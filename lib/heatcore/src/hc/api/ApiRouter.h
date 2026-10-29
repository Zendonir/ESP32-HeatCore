#pragma once
// REST API (JSON) of the HeatCore web interface. Pure request/response logic:
// the ESP32 web server forwards requests here with the system mutex held.
#include <map>
#include <string>

#include "hc/mqtt/MqttBridge.h"
#include "hc/system/HeatCoreSystem.h"

namespace hc {

struct ApiRequest {
  std::string method;  // GET POST PUT DELETE
  std::string path;    // "/api/..."
  std::map<std::string, std::string> query;
  std::string body;
};

struct ApiResponse {
  int status = 200;
  std::string body;
  const char* contentType = "application/json";
};

class ApiRouter {
 public:
  explicit ApiRouter(HeatCoreSystem& sys) : sys_(sys) {}
  ApiResponse handle(const ApiRequest& req);
  static std::string base64(const uint8_t* data, size_t len);

 private:
  HeatCoreSystem& sys_;
};

}  // namespace hc
