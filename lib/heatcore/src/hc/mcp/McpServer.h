#pragma once
// Model Context Protocol server (JSON-RPC 2.0, "Streamable HTTP" transport
// without server-sent events): an AI assistant such as Claude reads the
// recorded statistics, the settings and the learning model and may propose
// or apply tuning changes within the limits of hc::AiState.
//
// Protocol: initialize handshake, revisions 2024-11-05 .. 2025-11-25.
// Clients that first probe the stateless revision (server/discover) receive
// "method not found" and fall back to initialize, as the specification
// intends. The platform layer handles HTTP, the bearer token and the Origin
// check; this class only turns a request body into a response body.
#include <ArduinoJson.h>

#include <string>

namespace hc {

class HeatCoreSystem;

class McpServer {
 public:
  explicit McpServer(HeatCoreSystem& sys) : sys_(sys) {}

  // Processes one POSTed JSON-RPC message (or batch). Returns the HTTP status;
  // `out` is the JSON response, empty for notifications (HTTP 202).
  int handle(const std::string& body, std::string& out);

  static const char* const kSupportedVersions[];
  static const char* latestVersion();

 private:
  void dispatch(JsonObjectConst req, JsonObject resp);
  void initialize(JsonObjectConst params, JsonObject result);
  void toolsList(JsonObject result);
  void toolsCall(JsonObjectConst params, JsonObject result, JsonObject error);
  void promptsList(JsonObject result);
  bool promptsGet(JsonObjectConst params, JsonObject result, std::string& err);
  // tools: return "" or an error text; fill `data` with the answer
  std::string tool(const std::string& name, JsonObjectConst args, JsonObject data);

  HeatCoreSystem& sys_;
};

}  // namespace hc
