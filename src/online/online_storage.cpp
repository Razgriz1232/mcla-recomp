// Title-managed storage (XStorage) and XStringVerify: the XLiveBase (0xFC)
// messages the game sends for files its publisher put on LIVE.
//
// The game reads per-title storage: at the city load it asks for a file by
// name (XStorageBuildServerPath with facility 2, then XStorageEnumerate for
// that one name and XStorageDownloadToMemory of what the enumeration found),
// and the online tab verifies text with XStringVerify. Those four are
// answered here against the online server, which serves the files of its
// storage folder (GET /storage/title and /storage/title/<name>).
//
// XStorageBuildServerPath is a plain structure. The other three are
// "marshalled" tasks: the buffer is an XLIVEBASE_ASYNC_MESSAGE whose task
// holds the request as a byte stream (big-endian words and UTF-16 strings) and
// a results buffer the title allocated. The layouts follow the XDK's and
// Xenia's netplay fork.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "online.h"
#include "online_client.h"
#include "online_util.h"

namespace mcla::online {

namespace {

namespace sys = rex::system;

constexpr uint32_t kESuccess = 0;
constexpr uint32_t kEFail = 0x80004005;
constexpr uint32_t kEInvalidArg = 0x80070057;
constexpr uint32_t kEStorageFileNotFound = 0x8015C004;
constexpr uint32_t kEStorageInvalidFacility = 0x8015C009;
constexpr uint32_t kEStorageInvalidPath = 0x8015C008;
constexpr uint32_t kELogonNotLoggedOn = 0x80151802;
constexpr uint32_t kMaxPathChars = 255;        // X_ONLINE_MAX_PATHNAME_LENGTH
constexpr uint32_t kMaxEnumerateItems = 256;   // X_STORAGE_MAX_RESULTS_TO_RETURN
constexpr uint32_t kMaxDownloadBytes = 20u << 20;

enum Facility : uint32_t { kFacilityGameClip = 1, kFacilityPerTitle = 2, kFacilityPerUserTitle = 3 };

template <typename T>
T* Guest(uint32_t addr) {
  return addr ? sys::kernel_state()->memory()->TranslateVirtual<T*>(addr) : nullptr;
}

// XStorageBuildServerPath's argument block (40 bytes, the XUID at 8).
struct BuildServerPathArgs {
  be<uint32_t> user_index;
  be<uint32_t> pad;
  be<uint64_t> xuid;
  be<uint32_t> facility;
  be<uint32_t> facility_info_ptr;
  be<uint32_t> facility_info_size;
  be<uint32_t> file_name_ptr;
  be<uint32_t> server_path_ptr;
  be<uint32_t> server_path_length_ptr;  // in: the buffer's size in bytes; out: the path's length
};
static_assert(sizeof(BuildServerPathArgs) == 40);

struct AsyncMessage {
  be<uint32_t> task_ptr;  // XLIVE_ASYNC_TASK*
  be<uint64_t> current_numerator;
  be<uint64_t> current_denominator;
  be<uint64_t> last_numerator;
  be<uint64_t> last_denominator;
};

#pragma pack(push, 1)
struct AsyncTask {
  be<uint32_t> ordinal;
  be<uint32_t> schema_data_ptr;
  be<uint32_t> schema_index;
  be<uint32_t> task_flags;
  be<uint32_t> internal_ptr;
  be<uint32_t> internal_size;
  be<uint32_t> request_ptr;
  be<uint32_t> request_size;
  be<uint32_t> total_wire_buffer_size;
  be<uint32_t> counter;
  be<uint32_t> logon_id;
  be<uint32_t> results_ptr;
  be<uint32_t> results_size;
  be<uint32_t> wire_buffer[5];
  be<uint32_t> overlapped_ptr;
};
static_assert(sizeof(AsyncTask) == 76);

// X_STORAGE_FILE_INFO as the game lays it out: packed, 65 bytes.
struct StorageFileInfo {
  be<uint32_t> title_id;
  be<uint32_t> title_version;
  be<uint64_t> owner_puid;
  uint8_t country_id;
  be<uint64_t> reserved;
  be<uint32_t> content_type;
  be<uint32_t> storage_size;
  be<uint32_t> installed_size;
  be<uint64_t> created;
  be<uint64_t> last_modified;
  be<uint16_t> attributes_size;
  be<uint16_t> path_name_length;
  be<uint32_t> path_name_ptr;
  be<uint32_t> attributes_ptr;
};
static_assert(sizeof(StorageFileInfo) == 65);

struct EnumerateResults {
  be<uint32_t> total_items;
  be<uint32_t> items_returned;
  be<uint32_t> items_ptr;
};
static_assert(sizeof(EnumerateResults) == 12);

struct DownloadResults {
  be<uint32_t> bytes_total;
  be<uint64_t> owner_xuid;
  be<uint64_t> created;
};
static_assert(sizeof(DownloadResults) == 20);

struct StringVerifyResponse {
  be<uint16_t> num_strings;
  be<uint32_t> results_ptr;  // HRESULT[num_strings], after the response
};
static_assert(sizeof(StringVerifyResponse) == 6);
#pragma pack(pop)

// The marshalled request of a task: big-endian words and UTF-16BE strings in order.
class Request {
 public:
  Request(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool ok() const { return ok_; }
  size_t position() const { return pos_; }
  uint32_t U32() {
    if (pos_ + 4 > size_) {
      ok_ = false;
      return 0;
    }
    const uint32_t v = (uint32_t(data_[pos_]) << 24) | (uint32_t(data_[pos_ + 1]) << 16) |
                       (uint32_t(data_[pos_ + 2]) << 8) | data_[pos_ + 3];
    pos_ += 4;
    return v;
  }
  uint16_t U16() {
    if (pos_ + 2 > size_) {
      ok_ = false;
      return 0;
    }
    const uint16_t v = uint16_t((data_[pos_] << 8) | data_[pos_ + 1]);
    pos_ += 2;
    return v;
  }
  // UTF-16BE, `chars` code units (the game counts the terminator in); as UTF-8 without it.
  std::string Utf16(uint32_t chars) {
    if (chars > size_ / 2 || pos_ + size_t(chars) * 2 > size_) {
      ok_ = false;
      return "";
    }
    std::string out;
    for (uint32_t i = 0; i < chars; ++i) {
      const uint16_t c = uint16_t((data_[pos_ + i * 2] << 8) | data_[pos_ + i * 2 + 1]);
      if (!c) break;
      AppendUtf8(out, c);
    }
    pos_ += size_t(chars) * 2;
    return out;
  }
  std::string Ascii(uint32_t bytes) {
    if (pos_ + bytes > size_) {
      ok_ = false;
      return "";
    }
    std::string out(reinterpret_cast<const char*>(data_ + pos_), bytes);
    pos_ += bytes;
    return out;
  }

 private:
  static void AppendUtf8(std::string& out, uint16_t c) {
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// A guest UTF-16BE string (NUL-terminated) as UTF-8 (ASCII and the BMP).
std::string ReadGuestUtf16(uint32_t addr, size_t max_chars) {
  const auto* p = Guest<uint8_t>(addr);
  std::string out;
  for (size_t i = 0; p && i < max_chars; ++i) {
    const uint16_t c = uint16_t((p[i * 2] << 8) | p[i * 2 + 1]);
    if (!c) break;
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// Writes `text` (ASCII) as UTF-16BE with a terminator; returns the code units written
// without it. Stops at `max_chars` including the terminator.
uint32_t WriteGuestUtf16(uint32_t addr, const std::string& text, size_t max_chars) {
  auto* p = Guest<uint8_t>(addr);
  if (!p || max_chars == 0) {
    return 0;
  }
  const size_t n = std::min(text.size(), max_chars - 1);
  for (size_t i = 0; i < n; ++i) {
    p[i * 2] = 0;
    p[i * 2 + 1] = uint8_t(text[i]);
  }
  p[n * 2] = 0;
  p[n * 2 + 1] = 0;
  return uint32_t(n);
}

std::string Hex8(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", v);
  return buf;
}

std::string Hex16(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

// The item name of a server path: what follows the last slash.
std::string ItemName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The folder of a server path, for the enumeration's item paths.
std::string ItemFolder(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

bool IsPerTitlePath(const std::string& path) {
  return path.rfind("//title.", 0) == 0;
}

// A marshalled task from its message buffer: null when the buffer is not one.
const AsyncTask* TaskOf(uint32_t buffer_ptr, const char* what) {
  const auto* message = Guest<AsyncMessage>(buffer_ptr);
  const auto* task = message ? Guest<AsyncTask>(message->task_ptr) : nullptr;
  if (!task) {
    ONLINE_WARN("{}: no task in the message at {:08X}", what, buffer_ptr);
    return nullptr;
  }
  ONLINE_TRACE("{}: ordinal {:#x} schema index {} request {} bytes results {} bytes", what,
               uint32_t(task->ordinal), uint32_t(task->schema_index), uint32_t(task->request_size),
               uint32_t(task->results_size));
  return task;
}

// Windows FILETIME of a Unix time in milliseconds.
uint64_t FileTimeOf(uint64_t unix_ms) {
  return unix_ms * 10000ull + 116444736000000000ull;
}

}  // namespace

// --- XStorageBuildServerPath (0x58035) -----------------------------------------------

uint32_t StorageBuildServerPath(uint32_t buffer_ptr) {
  const auto* args = Guest<BuildServerPathArgs>(buffer_ptr);
  if (!args) {
    return kEInvalidArg;
  }
  auto& state = OnlineState::Get();
  if (!state.connected()) {
    return kELogonNotLoggedOn;
  }
  const uint32_t title = state.title_id();
  const uint64_t xuid = args->user_index == 0xFF ? uint64_t(args->xuid) : state.identity().xuid_online;
  const std::string name = ReadGuestUtf16(args->file_name_ptr, kMaxPathChars);
  std::string path;
  switch (uint32_t(args->facility)) {
    case kFacilityPerTitle:
      if (name.empty()) {
        return kEInvalidArg;
      }
      path = "//title." + Hex8(title) + "/t:" + Hex8(title) + "/" + name;
      break;
    case kFacilityPerUserTitle:
      if (name.empty()) {
        return kEInvalidArg;
      }
      path = "//tuser." + Hex8(title) + "/u:" + Hex16(xuid) + "/" + Hex8(title) + "/" + name;
      break;
    case kFacilityGameClip: {
      const auto* info = Guest<be<uint32_t>>(args->facility_info_ptr);
      const uint32_t leaderboard = info ? uint32_t(*info) : 0;
      path = "//xestats/u:" + Hex16(xuid) + "/" + Hex8(title) + "/" + Hex8(leaderboard);
      break;
    }
    default:
      ONLINE_WARN("XStorageBuildServerPath: facility {} is not one", uint32_t(args->facility));
      return kEStorageInvalidFacility;
  }
  auto* length = Guest<be<uint32_t>>(args->server_path_length_ptr);
  if (!length) {
    return kEInvalidArg;
  }
  if (args->server_path_ptr) {
    const uint32_t buffer_bytes = *length;
    if (buffer_bytes < (path.size() + 1) * 2) {
      ONLINE_WARN("XStorageBuildServerPath: {} bytes is too small for {}", buffer_bytes, path);
      return kEInvalidArg;
    }
    WriteGuestUtf16(args->server_path_ptr, path, buffer_bytes / 2);
  }
  *length = uint32_t(path.size() + 1);
  ONLINE_LOG("XStorageBuildServerPath(facility {}, \"{}\") -> {}", uint32_t(args->facility), name,
             path);
  return kESuccess;
}

// --- XStorageEnumerate (0x5000A) --------------------------------------------------------

uint32_t StorageEnumerate(uint32_t buffer_ptr) {
  const auto* task = TaskOf(buffer_ptr, "XStorageEnumerate");
  if (!task) {
    return kEInvalidArg;
  }
  const auto* data = Guest<uint8_t>(task->request_ptr);
  Request r(data, data ? size_t(uint32_t(task->request_size)) : size_t(0));
  const uint32_t user_index = r.U32();
  const uint32_t path_chars = r.U32();
  const std::string path = r.Utf16(path_chars);
  const uint32_t starting_index = r.U32();
  const uint32_t max_results = r.U32();
  auto* results = Guest<EnumerateResults>(task->results_ptr);
  const uint32_t results_size = task->results_size;
  if (!r.ok() || !results || results_size < sizeof(EnumerateResults) || path.empty() ||
      max_results > kMaxEnumerateItems) {
    ONLINE_WARN("XStorageEnumerate: a bad request (user {}, path \"{}\", {} results into {} bytes)",
                user_index, path, max_results, results_size);
    return kEInvalidArg;
  }
  std::memset(results, 0, results_size);
  const size_t bytes_per_item = sizeof(StorageFileInfo) + size_t(kMaxPathChars) * 2;
  const uint32_t room = uint32_t((results_size - sizeof(EnumerateResults)) / bytes_per_item);
  if (!room) {
    return kEInvalidArg;
  }
  if (!IsPerTitlePath(path)) {
    ONLINE_WARN("XStorageEnumerate: only per-title storage is served; not {}", path);
    return kEStorageInvalidFacility;
  }
  // The server lists the files whose names match (a * in the name is a wildcard).
  Json listing;
  const std::string pattern = ItemName(path);
  if (!OnlineState::Get().server().ListTitleStorage(pattern, &listing)) {
    ONLINE_WARN("XStorageEnumerate: the server did not list {}", pattern);
    return kEFail;
  }
  const Json& files = listing["files"];
  const uint32_t title = OnlineState::Get().title_id();
  const uint32_t items_addr = task->results_ptr + sizeof(EnumerateResults);
  const uint32_t names_addr = items_addr + room * sizeof(StorageFileInfo);
  auto* items = Guest<StorageFileInfo>(items_addr);
  results->items_ptr = items_addr;
  uint32_t returned = 0;
  for (size_t i = starting_index; i < files.size() && returned < room && returned < max_results;
       ++i) {
    const Json& f = files[i];
    const std::string item_path = ItemFolder(path) + f["name"].as_string();
    const uint32_t name_addr = names_addr + returned * kMaxPathChars * 2;
    const uint32_t written = WriteGuestUtf16(name_addr, item_path, kMaxPathChars);
    StorageFileInfo& item = items[returned];
    std::memset(&item, 0, sizeof(item));
    item.title_id = title;
    item.title_version = 0;
    item.owner_puid = 0;
    item.content_type = 0;
    item.storage_size = f["size"].as_u32();
    item.installed_size = f["size"].as_u32();
    item.created = FileTimeOf(uint64_t(f["modified"].as_number()));
    item.last_modified = item.created;
    item.path_name_length = uint16_t(written);
    item.path_name_ptr = name_addr;
    ++returned;
  }
  results->total_items = uint32_t(files.size());
  results->items_returned = returned;
  ONLINE_LOG("XStorageEnumerate(\"{}\", from {}, up to {}) -> {} of {}", path, starting_index,
             max_results, returned, files.size());
  return kESuccess;
}

// --- XStorageDownloadToMemory (0x50009) --------------------------------------------------

uint32_t StorageDownloadToMemory(uint32_t buffer_ptr) {
  const auto* task = TaskOf(buffer_ptr, "XStorageDownloadToMemory");
  if (!task) {
    return kEInvalidArg;
  }
  const auto* data = Guest<uint8_t>(task->request_ptr);
  Request r(data, data ? size_t(uint32_t(task->request_size)) : size_t(0));
  const uint32_t user_index = r.U32();
  const uint32_t path_chars = r.U32();
  const std::string path = r.Utf16(path_chars);
  const uint32_t buffer_size = r.U32();
  const uint32_t buffer_addr = r.U32();
  auto* results = Guest<DownloadResults>(task->results_ptr);
  auto* out = Guest<uint8_t>(buffer_addr);
  if (!r.ok() || !results || task->results_size < sizeof(DownloadResults) || path.empty() ||
      !out || buffer_size > kMaxDownloadBytes) {
    ONLINE_WARN("XStorageDownloadToMemory: a bad request (user {}, path \"{}\", {} bytes at {:08X})",
                user_index, path, buffer_size, buffer_addr);
    return kEInvalidArg;
  }
  std::memset(results, 0, sizeof(DownloadResults));
  if (!IsPerTitlePath(path)) {
    ONLINE_WARN("XStorageDownloadToMemory: only per-title storage is served; not {}", path);
    return kEStorageInvalidFacility;
  }
  std::vector<uint8_t> bytes;
  uint64_t modified_ms = 0;
  const int status = OnlineState::Get().server().GetTitleStorage(ItemName(path), &bytes, &modified_ms);
  if (status == 404) {
    ONLINE_LOG("XStorageDownloadToMemory(\"{}\"): the server has no such file", path);
    return kEStorageFileNotFound;
  }
  if (status < 200 || status >= 300) {
    ONLINE_WARN("XStorageDownloadToMemory(\"{}\"): the server answered {}", path, status);
    return kEFail;
  }
  if (bytes.size() > buffer_size) {
    ONLINE_WARN("XStorageDownloadToMemory(\"{}\"): {} bytes do not fit the game's {}", path,
                bytes.size(), buffer_size);
    return kEInvalidArg;
  }
  std::memcpy(out, bytes.data(), bytes.size());
  results->bytes_total = uint32_t(bytes.size());
  results->owner_xuid = 0;
  results->created = FileTimeOf(modified_ms);
  ONLINE_LOG("XStorageDownloadToMemory(\"{}\") -> {} bytes into a buffer of {}", path, bytes.size(),
             buffer_size);
  return kESuccess;
}

// --- XStringVerify (0x5000C) ---------------------------------------------------------

// Every string passes: there is no word list on the private server. The
// request's words are little-endian here (the title writes them as it holds
// them, Xenia reads them unswapped); the response holds the count and a
// pointer to one HRESULT per string, placed right after it.
uint32_t StringVerify(uint32_t buffer_ptr) {
  const auto* task = TaskOf(buffer_ptr, "XStringVerify");
  if (!task) {
    return kEInvalidArg;
  }
  const auto* data = Guest<uint8_t>(task->request_ptr);
  const uint32_t size = data ? uint32_t(task->request_size) : 0;
  uint32_t num_strings = 0;
  std::vector<std::string> strings;
  if (size >= 12) {
    const uint16_t locale_size = uint16_t(data[8] | (data[9] << 8));
    num_strings = uint16_t(data[10] | (data[11] << 8));
    size_t pos = 12 + locale_size;
    for (uint32_t i = 0; i < num_strings && pos + 2 <= size; ++i) {
      const uint16_t len = uint16_t(data[pos] | (data[pos + 1] << 8));
      pos += 2;
      if (pos + len > size) break;
      strings.emplace_back(reinterpret_cast<const char*>(data + pos), len);
      pos += len;
    }
  }
  auto* response = Guest<StringVerifyResponse>(task->results_ptr);
  const uint32_t need = uint32_t(sizeof(StringVerifyResponse) + num_strings * 4);
  if (!response || task->results_size < need) {
    ONLINE_WARN("XStringVerify: {} strings do not fit a results buffer of {} bytes", num_strings,
                uint32_t(task->results_size));
    return kEInvalidArg;
  }
  const uint32_t codes_addr = task->results_ptr + sizeof(StringVerifyResponse);
  auto* codes = Guest<be<uint32_t>>(codes_addr);
  for (uint32_t i = 0; i < num_strings; ++i) {
    codes[i] = kESuccess;
  }
  response->num_strings = uint16_t(num_strings);
  response->results_ptr = codes_addr;
  std::string shown;
  for (const auto& s : strings) shown += (shown.empty() ? "" : ", ") + ("\"" + s + "\"");
  ONLINE_LOG("XStringVerify({} string{}: {}) -> all fine", num_strings, num_strings == 1 ? "" : "s",
             shown);
  return kESuccess;
}

// The dispatch: true when the message was one of ours.
bool HandleStorageMessage(uint32_t message, uint32_t buffer_ptr, uint32_t* result) {
  switch (message) {
    case 0x00058035:
      *result = StorageBuildServerPath(buffer_ptr);
      return true;
    case 0x0005000A:
      *result = StorageEnumerate(buffer_ptr);
      return true;
    case 0x00050009:
      *result = StorageDownloadToMemory(buffer_ptr);
      return true;
    case 0x0005000C:
      *result = StringVerify(buffer_ptr);
      return true;
    default:
      return false;
  }
}

}  // namespace mcla::online
