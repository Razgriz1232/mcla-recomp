// The online server's API, as the game uses it: every request carries the
// account's game key, every call blocks for at most a few seconds and reports
// failure instead of throwing. The server is server/mcla-server.js, whose
// README documents the endpoints.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "online_http.h"
#include "online_json.h"

namespace mcla::online {

class ServerApi {
 public:
  void Configure(const HttpUrl& url, const std::string& key);
  const HttpUrl& url() const { return url_; }
  bool has_key() const { return !key_.empty(); }

  // Players. RegisterPlayer returns the HTTP status: 0 when the server was not
  // reached, 401 when the key was refused.
  int RegisterPlayer(const Json& player, Json* reply);
  int Heartbeat(const Json& player, Json* reply);
  void Unregister(uint64_t xuid);
  bool GetPlayer(uint64_t xuid, Json* out);

  // Sessions.
  bool CreateSession(const Json& session);
  bool UpdateSession(uint64_t id, const Json& patch);
  bool DeleteSession(uint64_t id);
  bool GetSession(uint64_t id, Json* out);
  bool SearchSessions(const Json& query, Json* results);
  bool JoinSession(uint64_t id, const Json& members);
  bool LeaveSession(uint64_t id, const Json& xuids);
  bool MigrateSession(uint64_t old_id, const Json& new_session);

  // QoS data a host publishes, bytes the searchers read back.
  bool SetQos(uint64_t id, const uint8_t* data, size_t size);
  bool GetQos(uint64_t id, std::vector<uint8_t>* out);

  // Leaderboard writes (kept by the server; nothing reads them back yet).
  bool WriteStats(const Json& stats);

  // Title storage: the files the server keeps for the game to download
  // (GET /storage/title lists those whose names match `pattern`, with a * as
  // a wildcard; GET /storage/title/<name> is one of them). GetTitleStorage
  // returns the HTTP status (404: no such file; 0: the server was not reached).
  bool ListTitleStorage(const std::string& pattern, Json* out);
  int GetTitleStorage(const std::string& name, std::vector<uint8_t>* out, uint64_t* modified_ms);

  // Rate My Ride (the routes under /rides, used by online_rmr.cpp). Each
  // returns the HTTP status (0: the server was not reached). `query` is what
  // follows "/rides?"; `which` names a ride's file: 0 the car data, 1 and 2
  // the photos.
  int ListRides(const std::string& query, Json* out);
  int UploadRide(const Json& ride, Json* out);
  int ReplaceRide(uint32_t id, const Json& ride, Json* out);
  int DownloadRideFile(uint32_t id, uint32_t which, std::vector<uint8_t>* out);
  int RateRide(uint32_t id, uint32_t stars, Json* out);
  int SetRideUserData(uint32_t id, const std::string& user_data_base64, Json* out);

 private:
  // Returns the HTTP status (0 when the server was not reached); the body is
  // parsed into `reply` when it is JSON.
  // `trace_body` false keeps a large body (a ride's files) out of the log.
  int Call(const char* method, const std::string& path, const Json* body, Json* reply,
           uint32_t timeout_ms = 4000, bool trace_body = true);

  HttpUrl url_;
  std::string key_;
};

}  // namespace mcla::online
