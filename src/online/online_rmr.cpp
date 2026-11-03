// Rate My Ride: the RAGE rlUpload commands the game's menu issues, answered
// against the online server's /rides routes.
//
// In the game, Rate My Ride (pause menu, Modes, Xbox Live) is the RAGE
// library's upload service: the screen (mcRateMyRide) calls
// rlUpload::GetTableSize, ReadRecordsByRank, ReadRecordsByGamer,
// ReadRandomRecord, UploadFile, OverwriteFile, DownloadFile, RateRecord and
// OverwriteUserData, each of which queued a command for a worker thread that
// spoke Rockstar's own protocol to the title server and set a netStatus when
// done; without that server the worker failed every command before any
// network call and the screen said the Midnight Club: Los Angeles server is
// not available. Those nine entry points are replaced here: each one asks the
// online server at once (on the calling thread; the exchanges take a fraction
// of a second), fills the game's own structures (the rlUploadRecord array,
// the table size, the downloaded car data) and marks the status succeeded,
// or failed with one of the GameSpy Sake result codes the screen's error
// table names. The worker and the title server are never involved.
//
// The layouts (the 152-byte rlUploadRecord, the 8-byte netStatus, the
// argument lists, the stack slots of the arguments past the eighth) were
// read from the recompiled code; docs/online.md says what works.
//
// A ride on the server holds the car data (file 0), up to two photos (files
// 1 and 2) and the game's own 64 bytes of "user data" (the vehicle type,
// the price, how many times it was bought, the money earned), which travel
// unchanged. A file id is ride * 4 + 1 + file.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "online.h"
#include "online_client.h"
#include "online_util.h"

// The game's own functions the hooks call: the vehicle table's index of a
// vehicle type (-1 when unknown), and the text of a string hash from the UI's
// text manager.
REX_EXTERN(sub_82396F58);
REX_EXTERN(sub_822183A8);

namespace mcla::online {

namespace {

using rex::be;

// rage::netStatus as the game keeps it: the state, then the result code.
constexpr uint32_t kStatusFailed = 2;
constexpr uint32_t kStatusSucceeded = 3;

// The result codes of a failed command: GameSpy Sake's, which the screen's
// error table names ("Operation Timed Out", "Requested file could not be
// found on server", ...) and shows as its server or file error message.
constexpr int32_t kResultOk = 0;
constexpr int32_t kResultInvalidArgs = 1;
constexpr int32_t kResultHttp = 4;
constexpr int32_t kResultAtLimit = 5;
constexpr int32_t kResultBufferOverflow = 6;
constexpr int32_t kResultNotAllowed = 7;
constexpr int32_t kResultAuth = 9;
constexpr int32_t kResultFileNotFound = 10;
constexpr int32_t kResultUnknown = 12;
constexpr int32_t kResultTimedOut = -1;

constexpr uint32_t kRecordSize = 152;
constexpr uint32_t kMaxRecords = 32;  // what one read fills at most: the game's array is small
constexpr uint32_t kMaxFiles = 3;
constexpr uint32_t kUserDataSize = 64;
constexpr uint32_t kMaxFileBytes = 2u << 20;
constexpr uint32_t kFilesPerRide = 4;  // a file id is ride * 4 + 1 + file
constexpr uint32_t kTableNameChars = 31;

// The game's globals: the vehicle table (104 bytes an entry, the name's string
// hash at +8) and the UI's text manager.
constexpr uint32_t kVehicleTablePtr = 0x828CD0A8;
constexpr uint32_t kTextManagerPtr = 0x8286D7FC;
constexpr uint32_t kVehicleEntrySize = 104;
constexpr uint32_t kVehicleNameHashOffset = 8;

struct NetStatus {
  be<uint32_t> status;
  be<int32_t> result;
};
static_assert(sizeof(NetStatus) == 8);

// rage::rlGamerHandle: the service (1 to 3 are valid) and the XUID.
struct GamerHandle {
  uint8_t service;
  uint8_t pad[7];
  be<uint64_t> xuid;
};
static_assert(sizeof(GamerHandle) == 16);

// rage::rlUploadRecord as the game lays it out and its reply parser fills it.
struct UploadRecord {
  GamerHandle owner;
  char owner_name[16];
  be<int32_t> record_id;
  be<uint32_t> num_files;
  be<int32_t> file_id[3];
  be<uint32_t> file_size[3];
  be<int32_t> times_rated;
  be<float> average_rating;
  be<int32_t> rank;  // zeroed by the rating reply, copied beside the rating figures: the rank
  uint8_t user_data[kUserDataSize];  // the game's own: vehicle type, price, times bought, money earned
  be<uint32_t> user_data_size;
  uint8_t category;
  uint8_t pad[7];
};
static_assert(sizeof(UploadRecord) == kRecordSize);

template <typename T>
T* Guest(uint8_t* base, uint32_t addr) {
  return addr ? reinterpret_cast<T*>(base + addr) : nullptr;
}

// A guest pointer that can be read: not null (the game's buffers sit anywhere
// in its 4 GB, the photos above 0xE0000000).
bool GuestOk(uint32_t addr) {
  return addr >= 0x10000;
}

// The n-th argument (n of 9 and up) of the call being answered, which the
// caller put on its stack: 8-byte slots from 84 above its stack pointer.
uint32_t StackArg(uint8_t* base, const PPCContext& ctx, uint32_t n) {
  return *Guest<be<uint32_t>>(base, ctx.r1.u32 + 84 + (n - 9) * 8);
}

std::string GuestString(uint8_t* base, uint32_t addr, size_t max_chars) {
  if (!GuestOk(addr)) {
    return "";
  }
  const char* p = Guest<char>(base, addr);
  size_t n = 0;
  while (n < max_chars && p[n]) {
    ++n;
  }
  return std::string(p, n);
}

bool HandleOk(const GamerHandle* h) {
  return h && h->service >= 1 && h->service <= 3 && uint64_t(h->xuid) != 0;
}

// As rlUploadRecord::IsValid: an owner, a name, an id, and files with ids and sizes.
bool RecordOk(const UploadRecord* r) {
  if (!r || !HandleOk(&r->owner) || !r->owner_name[0] || int32_t(r->record_id) == 0) {
    return false;
  }
  const uint32_t files = r->num_files;
  if (files == 0 || files > kMaxFiles) {
    return false;
  }
  for (uint32_t i = 0; i < files; ++i) {
    if (int32_t(r->file_id[i]) == 0 || uint32_t(r->file_size[i]) == 0) {
      return false;
    }
  }
  return true;
}

// The service of the handles the game uses, remembered from the ones it
// passes, so that the records written here compare equal to the local gamer's.
uint8_t g_service = 1;

void Finish(NetStatus* status, int32_t code) {
  if (!status) {
    return;
  }
  status->result = code;
  status->status = code == kResultOk ? kStatusSucceeded : kStatusFailed;
}

bool HttpOk(int status) {
  return status >= 200 && status < 300;
}

// A failed HTTP exchange as a result code.
int32_t CodeOfHttp(int status) {
  if (status == 0) return kResultTimedOut;
  if (status == 401 || status == 403) return kResultNotAllowed;
  if (status == 404) return kResultFileNotFound;
  if (status == 429) return kResultAtLimit;
  if (status == 400) return kResultInvalidArgs;
  if (status >= 500) return kResultUnknown;
  return kResultHttp;
}

// True when the server can be asked: online, signed in, with a key. Otherwise
// the status fails as "not authenticated" and the screen says so.
bool Ready(NetStatus* status, const char* what) {
  auto& state = OnlineState::Get();
  if (!state.enabled() || !state.connected() || !state.server().has_key()) {
    ONLINE_WARN("Rate My Ride: {}: not signed in to the online server", what);
    Finish(status, kResultAuth);
    return false;
  }
  return true;
}

// The game keeps other things in upload tables too ("ProfileStatsFull" and
// "Rewards" are read when the Xbox Live menu opens). Only the rides table is
// served; a command on another table fails as every command did without a
// title server, which the game takes in its stride.
constexpr const char* kRidesTable = "RateMyRide";

bool TableOk(uint8_t* base, uint32_t table_addr, NetStatus* status, const char* what) {
  const std::string table = GuestString(base, table_addr, kTableNameChars);
  if (table == kRidesTable) {
    return true;
  }
  ONLINE_TRACE("Rate My Ride: {} on the table \"{}\": not served (only {} is)", what, table,
               kRidesTable);
  Finish(status, kResultTimedOut);
  return false;
}

uint32_t ClampRecords(uint32_t max) {
  return std::max(UINT32_C(1), std::min(max, kMaxRecords));
}

std::string Plural(size_t n, const char* word) {
  return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

// A record from the server's ride JSON.
void FillRecord(UploadRecord* r, const Json& ride) {
  std::memset(r, 0, sizeof(*r));
  r->owner.service = g_service;
  r->owner.xuid = ParseHex64(ride["ownerXuid"].as_string());
  const std::string owner = ride["owner"].as_string();
  std::strncpy(r->owner_name, owner.c_str(), sizeof(r->owner_name) - 1);
  const uint32_t id = ride["id"].as_u32();
  r->record_id = int32_t(id);
  const Json& sizes = ride["fileSizes"];
  uint32_t files = 0;
  for (uint32_t i = 0; i < kMaxFiles; ++i) {
    const uint32_t size = i == 0 ? ride["size"].as_u32() : sizes[i].as_u32();
    if (i > 0 && size == 0) {
      break;
    }
    r->file_id[i] = int32_t(id * kFilesPerRide + 1 + i);
    r->file_size[i] = size;
    files = i + 1;
  }
  r->num_files = files;
  r->times_rated = int32_t(ride["ratings"].as_u32());
  r->average_rating = float(ride["rating"].as_number());
  r->rank = int32_t(ride["rank"].as_u32());
  const std::vector<uint8_t> user_data = Base64Decode(ride["userData"].as_string());
  const size_t n = std::min(user_data.size(), size_t(kUserDataSize));
  if (n) {
    std::memcpy(r->user_data, user_data.data(), n);
  }
  r->user_data_size = uint32_t(n);
  r->category = uint8_t(ride["category"].as_u32() & 0x3F);
}

// The rides of a listing into the game's array: how many were written.
uint32_t FillRecords(uint8_t* base, const Json& rides, uint32_t records_addr, uint32_t max) {
  uint32_t n = 0;
  for (; n < rides.size() && n < max; ++n) {
    FillRecord(Guest<UploadRecord>(base, records_addr + n * kRecordSize), rides[n]);
  }
  return n;
}

// Reads rides from the server into the game's array, its count and its status.
void ReadRecords(uint8_t* base, const std::string& what, const std::string& query,
                 uint32_t records_addr, uint32_t count_addr, uint32_t max, NetStatus* status) {
  Json reply;
  const int http = OnlineState::Get().server().ListRides(query, &reply);
  if (!HttpOk(http)) {
    ONLINE_WARN("Rate My Ride: {}: the server answered {}", what, http);
    Finish(status, CodeOfHttp(http));
    return;
  }
  const uint32_t n = FillRecords(base, reply["rides"], records_addr, max);
  *Guest<be<uint32_t>>(base, count_addr) = n;
  ONLINE_LOG("Rate My Ride: {} -> {} of {}", what, n, Plural(reply["total"].as_u32(), "ride"));
  Finish(status, kResultOk);
}

// The picture in a buffer the game hands over: its length when the buffer
// starts with a complete JPEG or PNG, else 0. The sizes the game gives for its
// photos are the buffers', not the pictures'.
size_t ImageLength(const uint8_t* p, size_t n) {
  if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) {
    size_t i = 2;
    while (i + 4 <= n) {
      if (p[i] != 0xFF) {
        return 0;
      }
      const uint8_t marker = p[i + 1];
      if (marker == 0xFF) {  // a fill byte
        ++i;
        continue;
      }
      i += 2;
      if (marker == 0xD9) {  // the end of the image
        return i;
      }
      if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
        continue;  // a marker without a segment
      }
      if (i + 2 > n) {
        return 0;
      }
      const size_t length = (size_t(p[i]) << 8) | p[i + 1];
      if (length < 2) {
        return 0;
      }
      i += length;
      if (marker == 0xDA) {  // a scan: entropy-coded data up to the next marker
        while (i + 1 < n) {
          if (p[i] == 0xFF && p[i + 1] != 0 && !(p[i + 1] >= 0xD0 && p[i + 1] <= 0xD7)) {
            break;
          }
          ++i;
        }
      }
    }
    return 0;
  }
  static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (n >= 8 && std::memcmp(p, kPng, 8) == 0) {
    size_t i = 8;
    while (i + 12 <= n) {
      const size_t length =
          (size_t(p[i]) << 24) | (size_t(p[i + 1]) << 16) | (size_t(p[i + 2]) << 8) | p[i + 3];
      const bool end = std::memcmp(p + i + 4, "IEND", 4) == 0;
      i += 12 + length;
      if (end) {
        return i <= n ? i : 0;
      }
    }
  }
  return 0;
}

// The name a ride gets on the website: "<owner>'s <vehicle>", in the
// characters the server accepts.
std::string RideName(const std::string& owner, const std::string& vehicle) {
  std::string text = (owner.empty() ? std::string("Someone") : owner) + "'s " +
                     (vehicle.empty() ? std::string("ride") : vehicle);
  std::string out;
  for (char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == ' ' || c == '.' || c == ',' || c == '\'' || c == '!' || c == '_' ||
        c == '-') {
      out += c;
    }
  }
  while (!out.empty() && !std::isalnum(static_cast<unsigned char>(out[0]))) {
    out.erase(0, 1);
  }
  if (out.empty()) {
    out = "A ride";
  }
  if (out.size() > 40) {
    out.resize(40);
  }
  return out;
}

// The vehicle's name as the game shows it, from its type: the vehicle table's
// entry names a string hash the UI's text manager resolves. Empty when unknown.
std::string VehicleName(PPCContext& ctx, uint8_t* base, uint32_t type_hash) {
  if (!type_hash) {
    return "";
  }
  rex::CallFrame frame(ctx);
  PPCContext& call = frame;
  const uint32_t sp = (ctx.r1.u32 - 256) & ~UINT32_C(15);
  *Guest<be<uint32_t>>(base, sp) = ctx.r1.u32;  // the back chain
  call.r1.u64 = sp;
  call.r3.u64 = type_hash;
  sub_82396F58(call, base);
  const int32_t index = call.r3.s32;
  const uint32_t table = *Guest<be<uint32_t>>(base, kVehicleTablePtr);
  const uint32_t manager = *Guest<be<uint32_t>>(base, kTextManagerPtr);
  if (index < 0 || index > 4096 || !GuestOk(table) || !GuestOk(manager)) {
    return "";
  }
  const uint32_t name_hash =
      *Guest<be<uint32_t>>(base, table + uint32_t(index) * kVehicleEntrySize + kVehicleNameHashOffset);
  call.r1.u64 = sp;
  call.r3.u64 = manager;
  call.r4.u64 = name_hash;
  sub_822183A8(call, base);
  const uint32_t text = call.r3.u32;
  return GuestOk(text) ? GuestString(base, text, 40) : "";
}

std::string Hex8(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", v);
  return buf;
}

uint32_t Be32(const std::vector<uint8_t>& v, size_t at) {
  return (uint32_t(v[at]) << 24) | (uint32_t(v[at + 1]) << 16) | (uint32_t(v[at + 2]) << 8) |
         v[at + 3];
}

// What an upload or an overwrite hands over.
struct UploadArgs {
  std::string owner_name;
  uint32_t num_files = 0;
  uint32_t ptrs_addr = 0;   // unsigned char*[num_files]
  uint32_t sizes_addr = 0;  // unsigned[num_files]
  uint32_t user_data_addr = 0;
  uint32_t user_data_size = 0;
  uint8_t category = 0;
};

// The files the game hands over, as the server's upload body: the car data,
// the photos that are pictures, the user data, and a name and price for the
// website. False when a file cannot be read.
bool UploadBody(PPCContext& ctx, uint8_t* base, const UploadArgs& a, Json* body,
                std::string* summary) {
  const auto* ptrs = Guest<be<uint32_t>>(base, a.ptrs_addr);
  const auto* sizes = Guest<be<uint32_t>>(base, a.sizes_addr);
  const uint32_t files = std::min(a.num_files, kMaxFiles);
  std::vector<uint8_t> user_data;
  if (GuestOk(a.user_data_addr) && a.user_data_size) {
    const uint32_t n = std::min(a.user_data_size, kUserDataSize);
    const auto* p = Guest<uint8_t>(base, a.user_data_addr);
    user_data.assign(p, p + n);
  }
  uint32_t type_hash = 0;
  uint32_t price = 0;
  if (user_data.size() >= 8) {
    type_hash = Be32(user_data, 0);
    price = Be32(user_data, 4);
  }
  const std::string vehicle = VehicleName(ctx, base, type_hash);
  *body = Json::Object();
  body->set("name", RideName(a.owner_name, vehicle));
  body->set("vehicle", vehicle);
  body->set("price", Json(unsigned(price)));
  body->set("category", Json(unsigned(a.category)));
  if (!user_data.empty()) {
    body->set("userData", Base64Encode(user_data.data(), user_data.size()));
  }
  *summary = vehicle.empty() ? "vehicle type " + Hex8(type_hash) : vehicle;
  for (uint32_t i = 0; i < files; ++i) {
    const uint32_t addr = ptrs[i];
    const uint32_t size = sizes[i];
    if (!GuestOk(addr) || !size || size > kMaxFileBytes) {
      ONLINE_WARN("Rate My Ride: file {} ({} bytes at {:08X}) cannot be sent", i, size, addr);
      return false;
    }
    const uint8_t* data = Guest<uint8_t>(base, addr);
    if (i == 0) {
      body->set("data", Base64Encode(data, size));
      *summary += ", " + Plural(size, "byte") + " of car data";
      continue;
    }
    const size_t length = ImageLength(data, size);
    if (!length) {
      ONLINE_WARN("Rate My Ride: photo {} ({} bytes at {:08X}) is not a JPEG or a PNG (starts {:02x} "
                  "{:02x} {:02x} {:02x}); not sent",
                  i, size, addr, data[0], data[1], data[2], data[3]);
      continue;
    }
    body->set(i == 1 ? "photo" : "photo2", Base64Encode(data, length));
    *summary += ", a photo of " + Plural(length, "byte");
  }
  return body->has("data");
}

// The reply of an upload or an overwrite: the ride as a record, where the
// game asked for it.
void FinishUpload(uint8_t* base, const char* what, int http, const Json& reply,
                  uint32_t records_addr, uint32_t progress_addr, NetStatus* status,
                  const std::string& summary) {
  if (!HttpOk(http)) {
    ONLINE_WARN("Rate My Ride: {}: the server answered {} {}", what, http,
                reply["error"].as_string());
    Finish(status, CodeOfHttp(http));
    return;
  }
  const Json& ride = reply["ride"];
  if (GuestOk(records_addr)) {
    FillRecord(Guest<UploadRecord>(base, records_addr), ride);
  }
  if (GuestOk(progress_addr)) {
    *Guest<be<float>>(base, progress_addr) = 1.0f;
  }
  ONLINE_LOG("Rate My Ride: {} -> ride {} \"{}\" ({})", what, ride["id"].as_u32(),
             ride["name"].as_string(), summary);
  Finish(status, kResultOk);
}

}  // namespace

// --- The entry points --------------------------------------------------------
// Each returns 1 when the command was taken (its status then tells the outcome),
// 0 when the arguments were not usable, as the originals did.

// rlUpload::GetTableSize(table, 3, 0, unsigned* size, netStatus*)
extern "C" REX_FUNC(sub_824F6DE8) {
  const uint32_t table = ctx.r4.u32;
  const uint32_t size_addr = ctx.r7.u32;
  auto* status = Guest<NetStatus>(base, ctx.r8.u32);
  if (!table || !size_addr) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = 1;
  ONLINE_TRACE("Rate My Ride: GetTableSize(\"{}\", {}, {})", GuestString(base, table, kTableNameChars),
               ctx.r5.u32, ctx.r6.u32);
  if (!TableOk(base, table, status, "GetTableSize") || !Ready(status, "GetTableSize")) {
    return;
  }
  Json reply;
  const int http = OnlineState::Get().server().ListRides("by=rank&count=0", &reply);
  if (!HttpOk(http)) {
    ONLINE_WARN("Rate My Ride: GetTableSize: the server answered {}", http);
    Finish(status, CodeOfHttp(http));
    return;
  }
  const uint32_t total = reply["total"].as_u32();
  *Guest<be<uint32_t>>(base, size_addr) = total;
  ONLINE_LOG("Rate My Ride: the table holds {}", Plural(total, "ride"));
  Finish(status, kResultOk);
}

// rlUpload::ReadRecordsByGamer(table, const rlGamerHandle*, unsigned maxRecords,
//                              rlUploadRecord* records, unsigned* numRecords, netStatus*)
extern "C" REX_FUNC(sub_824F6EA0) {
  const uint32_t table = ctx.r4.u32;
  const auto* handle = Guest<GamerHandle>(base, ctx.r5.u32);
  const uint32_t max = ctx.r6.u32;
  const uint32_t records = ctx.r7.u32;
  const uint32_t count = ctx.r8.u32;
  auto* status = Guest<NetStatus>(base, ctx.r9.u32);
  if (!table || !HandleOk(handle) || !max || !records || !count) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = 1;
  g_service = handle->service;
  const uint64_t xuid = handle->xuid;
  ONLINE_TRACE("Rate My Ride: ReadRecordsByGamer(\"{}\", {}, up to {})",
               GuestString(base, table, kTableNameChars), Hex64(xuid), max);
  if (!TableOk(base, table, status, "ReadRecordsByGamer") || !Ready(status, "ReadRecordsByGamer")) {
    return;
  }
  const uint32_t n = ClampRecords(max);
  ReadRecords(base, "the rides of " + Hex64(xuid),
              "by=gamer&xuid=" + Hex64(xuid) + "&count=" + std::to_string(n), records, count, n,
              status);
}

// rlUpload::ReadRandomRecord(table, const rlGamerHandle* self, 0, unsigned maxRecords,
//                            unsigned categories, rlUploadRecord* records, unsigned* numRecords,
//                            netStatus*): rides other than the player's own, at random.
extern "C" REX_FUNC(sub_824F6FA0) {
  const uint32_t table = ctx.r4.u32;
  const auto* handle = Guest<GamerHandle>(base, ctx.r5.u32);
  const uint32_t max = ctx.r7.u32;
  const uint32_t records = ctx.r9.u32;
  const uint32_t count = ctx.r10.u32;
  auto* status = Guest<NetStatus>(base, StackArg(base, ctx, 9));
  if (!table || !HandleOk(handle) || !records || !count) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = 1;
  g_service = handle->service;
  const uint64_t xuid = handle->xuid;
  ONLINE_TRACE("Rate My Ride: ReadRandomRecord(\"{}\", {}, {}, {}, {:#x})",
               GuestString(base, table, kTableNameChars), Hex64(xuid), ctx.r6.u32, max, ctx.r8.u32);
  if (!TableOk(base, table, status, "ReadRandomRecord") || !Ready(status, "ReadRandomRecord")) {
    return;
  }
  const uint32_t n = ClampRecords(max);
  ReadRecords(base, "random rides", "by=random&exclude=" + Hex64(xuid) + "&count=" + std::to_string(n),
              records, count, n, status);
}

// rlUpload::ReadRecordsByRank(table, unsigned startRank, 3, unsigned maxRecords,
//                             rlUploadRecord* records, unsigned* numRecords, netStatus*)
extern "C" REX_FUNC(sub_824F70A8) {
  const uint32_t table = ctx.r4.u32;
  const uint32_t start_rank = ctx.r5.u32;
  const uint32_t max = ctx.r7.u32;
  const uint32_t records = ctx.r8.u32;
  const uint32_t count = ctx.r9.u32;
  auto* status = Guest<NetStatus>(base, ctx.r10.u32);
  if (!table || !start_rank || !max || !records || !count) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = 1;
  ONLINE_TRACE("Rate My Ride: ReadRecordsByRank(\"{}\", from rank {}, {}, up to {})",
               GuestString(base, table, kTableNameChars), start_rank, ctx.r6.u32, max);
  if (!TableOk(base, table, status, "ReadRecordsByRank") || !Ready(status, "ReadRecordsByRank")) {
    return;
  }
  const uint32_t n = ClampRecords(max);
  ReadRecords(base, "rides from rank " + std::to_string(start_rank),
              "by=rank&from=" + std::to_string(start_rank - 1) + "&count=" + std::to_string(n),
              records, count, n, status);
}

// rlUpload::RateRecord(table, const rlUploadRecord*, unsigned char rating,
//                      rlUploadRecord* updated, netStatus*): 1 to 5 stars.
extern "C" REX_FUNC(sub_824F7188) {
  const uint32_t table = ctx.r4.u32;
  const auto* record = Guest<UploadRecord>(base, ctx.r5.u32);
  const uint32_t stars = ctx.r6.u8;
  const uint32_t updated_addr = ctx.r7.u32;
  auto* status = Guest<NetStatus>(base, ctx.r8.u32);
  if (!table || !RecordOk(record)) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = 1;
  const uint32_t id = uint32_t(int32_t(record->record_id));
  ONLINE_TRACE("Rate My Ride: RateRecord(\"{}\", ride {} of {}, {} star{})",
               GuestString(base, table, kTableNameChars), id, record->owner_name, stars,
               stars == 1 ? "" : "s");
  if (!TableOk(base, table, status, "RateRecord") || !Ready(status, "RateRecord")) {
    return;
  }
  Json reply;
  const int http = OnlineState::Get().server().RateRide(id, stars, &reply);
  if (!HttpOk(http)) {
    ONLINE_WARN("Rate My Ride: rating ride {}: the server answered {} {}", id, http,
                reply["error"].as_string());
    Finish(status, CodeOfHttp(http));
    return;
  }
  auto* updated = Guest<UploadRecord>(base, updated_addr);
  if (updated) {
    if (updated != record) {
      std::memcpy(updated, record, kRecordSize);
    }
    updated->times_rated = int32_t(reply["ratings"].as_u32());
    updated->average_rating = float(reply["rating"].as_number());
    updated->rank = 0;
  }
  ONLINE_LOG("Rate My Ride: rated ride {} of {} {} star{} -> {:.2f} over {}", id, record->owner_name,
             stars, stars == 1 ? "" : "s", reply["rating"].as_number(),
             Plural(reply["ratings"].as_u32(), "rating"));
  Finish(status, kResultOk);
}

// rlUpload::UploadFile(table, 1, const rlGamerHandle* owner, const char* name, unsigned numFiles,
//                      const void* files[], const unsigned sizes[], const void* userData,
//                      unsigned userDataSize, unsigned char category, rlUploadRecord* record,
//                      float* progress, netStatus*): the player's ride, registered.
extern "C" REX_FUNC(sub_824F7308) {
  const uint32_t table = ctx.r4.u32;
  const auto* handle = Guest<GamerHandle>(base, ctx.r6.u32);
  const uint32_t name = ctx.r7.u32;
  UploadArgs a;
  a.num_files = ctx.r8.u32;
  a.ptrs_addr = ctx.r9.u32;
  a.sizes_addr = ctx.r10.u32;
  a.user_data_addr = StackArg(base, ctx, 9);
  a.user_data_size = StackArg(base, ctx, 10);
  a.category = uint8_t(StackArg(base, ctx, 11) & 0xFF);
  const uint32_t record_addr = StackArg(base, ctx, 12);
  const uint32_t progress_addr = StackArg(base, ctx, 13);
  auto* status = Guest<NetStatus>(base, StackArg(base, ctx, 14));
  if (!table || !HandleOk(handle) || !name || !a.num_files || !a.ptrs_addr || !a.sizes_addr ||
      !a.user_data_addr || !record_addr || a.category >= 32) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = 1;
  g_service = handle->service;
  a.owner_name = GuestString(base, name, 15);
  ONLINE_TRACE("Rate My Ride: UploadFile(\"{}\", {}, {}, \"{}\", {} files, {} bytes of user data, "
               "category {})",
               GuestString(base, table, kTableNameChars), ctx.r5.u32, Hex64(uint64_t(handle->xuid)),
               a.owner_name, a.num_files, a.user_data_size, a.category);
  if (GuestOk(progress_addr)) {
    *Guest<be<float>>(base, progress_addr) = 0.0f;
  }
  if (!TableOk(base, table, status, "UploadFile") || !Ready(status, "UploadFile")) {
    return;
  }
  Json body;
  std::string summary;
  if (!UploadBody(ctx, base, a, &body, &summary)) {
    Finish(status, kResultInvalidArgs);
    return;
  }
  Json reply;
  const int http = OnlineState::Get().server().UploadRide(body, &reply);
  FinishUpload(base, "UploadFile", http, reply, record_addr, progress_addr, status, summary);
}

// rlUpload::OverwriteFile(table, const rlUploadRecord*, unsigned numFiles, const void* files[],
//                         const unsigned sizes[], const void* userData, unsigned userDataSize,
//                         unsigned char category, rlUploadRecord* record, float* progress,
//                         netStatus*): the player's ride, changed.
extern "C" REX_FUNC(sub_824F7468) {
  const uint32_t table = ctx.r4.u32;
  const auto* record = Guest<UploadRecord>(base, ctx.r5.u32);
  UploadArgs a;
  a.num_files = ctx.r6.u32;
  a.ptrs_addr = ctx.r7.u32;
  a.sizes_addr = ctx.r8.u32;
  a.user_data_addr = ctx.r9.u32;
  a.user_data_size = ctx.r10.u32;
  a.category = uint8_t(StackArg(base, ctx, 9) & 0xFF);
  const uint32_t record_addr = StackArg(base, ctx, 10);
  const uint32_t progress_addr = StackArg(base, ctx, 11);
  auto* status = Guest<NetStatus>(base, StackArg(base, ctx, 12));
  if (!table || !RecordOk(record) || !a.num_files || !a.ptrs_addr || !a.sizes_addr || !record_addr ||
      a.category >= 32) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = 1;
  const uint32_t id = uint32_t(int32_t(record->record_id));
  a.owner_name = GuestString(base, ctx.r5.u32 + offsetof(UploadRecord, owner_name), sizeof(record->owner_name));
  ONLINE_TRACE("Rate My Ride: OverwriteFile(\"{}\", ride {}, {} files, {} bytes of user data, category {})",
               GuestString(base, table, kTableNameChars), id, a.num_files, a.user_data_size,
               a.category);
  if (GuestOk(progress_addr)) {
    *Guest<be<float>>(base, progress_addr) = 0.0f;
  }
  if (!TableOk(base, table, status, "OverwriteFile") || !Ready(status, "OverwriteFile")) {
    return;
  }
  Json body;
  std::string summary;
  if (!UploadBody(ctx, base, a, &body, &summary)) {
    Finish(status, kResultInvalidArgs);
    return;
  }
  Json reply;
  const int http = OnlineState::Get().server().ReplaceRide(id, body, &reply);
  FinishUpload(base, "OverwriteFile", http, reply, record_addr, progress_addr, status, summary);
}

// rlUpload::DownloadFile(int fileId, void* buffer, unsigned bufferSize, unsigned* bytesRead,
//                        float* progress, netStatus*): a ride's car data.
extern "C" REX_FUNC(sub_824F7578) {
  const int32_t file_id = ctx.r4.s32;
  const uint32_t buffer_addr = ctx.r5.u32;
  const uint32_t buffer_size = ctx.r6.u32;
  const uint32_t bytes_addr = ctx.r7.u32;
  const uint32_t progress_addr = ctx.r8.u32;
  auto* status = Guest<NetStatus>(base, ctx.r9.u32);
  if (!file_id || !buffer_addr || !buffer_size || !bytes_addr || !progress_addr) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = 1;
  *Guest<be<uint32_t>>(base, bytes_addr) = 0;
  *Guest<be<float>>(base, progress_addr) = 0.0f;
  const uint32_t id = (uint32_t(file_id) - 1) / kFilesPerRide;
  const uint32_t which = (uint32_t(file_id) - 1) % kFilesPerRide;
  ONLINE_TRACE("Rate My Ride: DownloadFile({}: ride {} file {}, into {} bytes)", file_id, id, which,
               buffer_size);
  if (!Ready(status, "DownloadFile")) {
    return;
  }
  std::vector<uint8_t> bytes;
  const int http = OnlineState::Get().server().DownloadRideFile(id, which, &bytes);
  if (!HttpOk(http)) {
    ONLINE_WARN("Rate My Ride: downloading file {} of ride {}: the server answered {}", which, id,
                http);
    Finish(status, CodeOfHttp(http));
    return;
  }
  if (bytes.size() > buffer_size) {
    ONLINE_WARN("Rate My Ride: file {} of ride {} is {} bytes; the game's buffer holds {}", which, id,
                bytes.size(), buffer_size);
    Finish(status, kResultBufferOverflow);
    return;
  }
  std::memcpy(Guest<uint8_t>(base, buffer_addr), bytes.data(), bytes.size());
  *Guest<be<uint32_t>>(base, bytes_addr) = uint32_t(bytes.size());
  *Guest<be<float>>(base, progress_addr) = 1.0f;
  ONLINE_LOG("Rate My Ride: downloaded file {} of ride {}: {}", which, id, Plural(bytes.size(), "byte"));
  Finish(status, kResultOk);
}

// rlUpload::OverwriteUserData(table, const rlUploadRecord*, const void* userData,
//                             unsigned userDataSize, rlUploadRecord* updated, netStatus*):
//                             the game's 64 bytes on a ride (a purchase credits the seller's).
extern "C" REX_FUNC(sub_824F7638) {
  const uint32_t table = ctx.r4.u32;
  const auto* record = Guest<UploadRecord>(base, ctx.r5.u32);
  const uint32_t user_data_addr = ctx.r6.u32;
  const uint32_t user_data_size = ctx.r7.u32;
  const uint32_t updated_addr = ctx.r8.u32;
  auto* status = Guest<NetStatus>(base, ctx.r9.u32);
  if (!table || !record || !user_data_addr || !user_data_size || !updated_addr) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = 1;
  const uint32_t id = uint32_t(int32_t(record->record_id));
  const uint32_t n = std::min(user_data_size, kUserDataSize);
  ONLINE_TRACE("Rate My Ride: OverwriteUserData(\"{}\", ride {}, {} bytes)",
               GuestString(base, table, kTableNameChars), id, user_data_size);
  if (!TableOk(base, table, status, "OverwriteUserData") || !Ready(status, "OverwriteUserData")) {
    return;
  }
  Json reply;
  const int http = OnlineState::Get().server().SetRideUserData(
      id, Base64Encode(Guest<uint8_t>(base, user_data_addr), n), &reply);
  if (!HttpOk(http)) {
    ONLINE_WARN("Rate My Ride: the user data of ride {}: the server answered {} {}", id, http,
                reply["error"].as_string());
    Finish(status, CodeOfHttp(http));
    return;
  }
  if (GuestOk(updated_addr)) {
    FillRecord(Guest<UploadRecord>(base, updated_addr), reply["ride"]);
  }
  ONLINE_LOG("Rate My Ride: the user data of ride {} replaced ({})", id, Plural(n, "byte"));
  Finish(status, kResultOk);
}

}  // namespace mcla::online
