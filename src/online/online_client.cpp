#include "online_client.h"

#include <cctype>
#include <cstdio>

#include <rex/logging.h>

#include "online.h"
#include "online_http.h"
#include "online_util.h"

namespace mcla::online {

void ServerApi::Configure(const HttpUrl& url, const std::string& key) {
  url_ = url;
  key_ = Trim(key);
}

int ServerApi::Call(const char* method, const std::string& path, const Json* body, Json* reply,
                    uint32_t timeout_ms) {
  if (url_.host.empty()) {
    return 0;
  }
  const std::string body_text = body ? body->dump() : std::string();
  ONLINE_TRACE("{} {}{}", method, path, body ? " " + body_text : "");
  std::vector<std::string> headers;
  if (!key_.empty()) {
    headers.push_back("Authorization: Bearer " + key_);
  }
  HttpResponse response = HttpRequest(method, url_, path, body_text, headers, timeout_ms);
  if (response.status == 0) {
    ONLINE_WARN("{} {}: {}", method, path, response.error);
    return 0;
  }
  if (reply) {
    if (!response.body.empty() && !Json::Parse(response.body, reply)) {
      ONLINE_WARN("{} {}: the server's answer is not JSON ({} bytes)", method, path,
                  response.body.size());
      *reply = Json();
    }
  }
  if (!response.ok()) {
    ONLINE_WARN("{} {}: HTTP {} {}", method, path, response.status,
                response.body.substr(0, 200));
  } else {
    ONLINE_TRACE("{} {} -> {} ({} bytes)", method, path, response.status, response.body.size());
  }
  return response.status;
}

static bool Ok(int status) {
  return status >= 200 && status < 300;
}

int ServerApi::RegisterPlayer(const Json& player, Json* reply) {
  return Call("POST", "/players", &player, reply);
}

int ServerApi::Heartbeat(const Json& player, Json* reply) {
  const std::string xuid = player["xuid"].as_string("0");
  return Call("POST", "/players/" + xuid + "/heartbeat", &player, reply, 3000);
}

void ServerApi::Unregister(uint64_t xuid) {
  Call("DELETE", "/players/" + Hex64(xuid), nullptr, nullptr, 1500);
}

bool ServerApi::GetPlayer(uint64_t xuid, Json* out) {
  return Ok(Call("GET", "/players/" + Hex64(xuid), nullptr, out));
}

bool ServerApi::CreateSession(const Json& session) {
  return Ok(Call("POST", "/sessions", &session, nullptr));
}

bool ServerApi::UpdateSession(uint64_t id, const Json& patch) {
  return Ok(Call("PUT", "/sessions/" + Hex64(id), &patch, nullptr));
}

bool ServerApi::DeleteSession(uint64_t id) {
  return Ok(Call("DELETE", "/sessions/" + Hex64(id), nullptr, nullptr));
}

bool ServerApi::GetSession(uint64_t id, Json* out) {
  return Ok(Call("GET", "/sessions/" + Hex64(id), nullptr, out));
}

bool ServerApi::SearchSessions(const Json& query, Json* results) {
  return Ok(Call("POST", "/sessions/search", &query, results, 6000));
}

bool ServerApi::JoinSession(uint64_t id, const Json& members) {
  return Ok(Call("POST", "/sessions/" + Hex64(id) + "/join", &members, nullptr));
}

bool ServerApi::LeaveSession(uint64_t id, const Json& xuids) {
  return Ok(Call("POST", "/sessions/" + Hex64(id) + "/leave", &xuids, nullptr));
}

bool ServerApi::MigrateSession(uint64_t old_id, const Json& new_session) {
  return Ok(Call("POST", "/sessions/" + Hex64(old_id) + "/migrate", &new_session, nullptr));
}

bool ServerApi::SetQos(uint64_t id, const uint8_t* data, size_t size) {
  Json body = Json::Object();
  body.set("qos", Base64Encode(data, size));
  return Ok(Call("PUT", "/sessions/" + Hex64(id) + "/qos", &body, nullptr));
}

bool ServerApi::GetQos(uint64_t id, std::vector<uint8_t>* out) {
  Json reply;
  if (!Ok(Call("GET", "/sessions/" + Hex64(id) + "/qos", nullptr, &reply))) {
    return false;
  }
  *out = Base64Decode(reply["qos"].as_string());
  return true;
}

bool ServerApi::WriteStats(const Json& stats) {
  return Ok(Call("POST", "/stats", &stats, nullptr));
}

// The characters of a storage file name that a URL path cannot carry as they are.
static std::string PathEncode(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    const bool plain = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '*';
    if (plain) {
      out += char(c);
    } else {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

bool ServerApi::ListTitleStorage(const std::string& pattern, Json* out) {
  return Ok(Call("GET", "/storage/title?name=" + PathEncode(pattern), nullptr, out));
}

int ServerApi::GetTitleStorage(const std::string& name, std::vector<uint8_t>* out,
                               uint64_t* modified_ms) {
  if (url_.host.empty()) {
    return 0;
  }
  const std::string path = "/storage/title/" + PathEncode(name);
  std::vector<std::string> headers;
  if (!key_.empty()) {
    headers.push_back("Authorization: Bearer " + key_);
  }
  HttpResponse response = HttpRequest("GET", url_, path, "", headers, 15000);
  if (response.status == 0) {
    ONLINE_WARN("GET {}: {}", path, response.error);
    return 0;
  }
  if (response.ok()) {
    out->assign(response.body.begin(), response.body.end());
    if (modified_ms) {
      *modified_ms = NowMs();
    }
    ONLINE_TRACE("GET {} -> {} ({} bytes)", path, response.status, response.body.size());
  } else if (response.status != 404) {
    ONLINE_WARN("GET {}: HTTP {} {}", path, response.status, response.body.substr(0, 200));
  }
  return response.status;
}

}  // namespace mcla::online
