//  Copyright (c) 2024 Exle
//
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//  DEALINGS IN THE SOFTWARE.

#include "cover_publisher.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include "utils.h"

namespace {

using CoverPublisher::Result;

// --- Transport constants ----------------------------------------------------

constexpr DWORD kTotalTimeoutMs = 10000;
constexpr DWORD kMaxResponseBytes = 64 * 1024;
constexpr size_t kMaxCacheBytes = 1024 * 1024;
constexpr long long kNegativeCacheTtlSeconds = 60;
// 29 bytes keep the closing delimiter ("\r\n--<boundary>--\r\n") at 37 bytes,
// matching the live measurement of the host's request framing.
constexpr size_t kBoundaryLength = 29;

constexpr wchar_t kAgentName[] = L"AIMP-Discord-Presence/2.0";
constexpr wchar_t kUploadHost[] = L"litterbox.catbox.moe";
constexpr wchar_t kUploadPath[] = L"/resources/internals/api.php";

// The host answers a request it cannot parse with HTTP 200 and an empty body,
// so this exact URL shape is the only trustworthy success response.
constexpr char kUploadUrlPattern[] = R"(^https://litter\.catbox\.moe/[a-z0-9]{6}\.(png|jpg|jpeg)$)";
constexpr char kMimePattern[] = R"(^[A-Za-z0-9][A-Za-z0-9.+-]*/[A-Za-z0-9][A-Za-z0-9.+-]*$)";

Result Failure(const std::string& reason) {
  Result result;
  result.reason = reason;
  return result;
}

std::string TrimWhitespace(const std::string& text) {
  const char* kWhitespace = " \t\r\n";
  const size_t begin = text.find_first_not_of(kWhitespace);
  if (begin == std::string::npos) {
    return std::string();
  }
  const size_t end = text.find_last_not_of(kWhitespace);
  return text.substr(begin, end - begin + 1);
}

bool LooksLikeUploadUrl(const std::string& text) {
  static const std::regex pattern(kUploadUrlPattern);
  return std::regex_match(text, pattern);
}

std::string SanitizeMime(const char* mime) {
  const std::string candidate = mime != nullptr ? std::string(mime) : std::string();
  static const std::regex pattern(kMimePattern);
  if (std::regex_match(candidate, pattern)) {
    return candidate;
  }
  return "application/octet-stream";
}

const char* FilenameForMime(const std::string& mime) {
  if (mime == "image/png") {
    return "cover.png";
  }
  if (mime == "image/jpeg") {
    return "cover.jpg";
  }
  if (mime == "image/gif") {
    return "cover.gif";
  }
  if (mime == "image/webp") {
    return "cover.webp";
  }
  if (mime == "image/bmp") {
    return "cover.bmp";
  }
  return "cover.bin";
}

// --- Multipart body ---------------------------------------------------------

std::string MultipartPrefix(const std::string& boundary, const std::string& mime) {
  const std::string delimiter = "--" + boundary + "\r\n";
  std::string prefix;
  prefix += delimiter;
  prefix += "Content-Disposition: form-data; name=\"reqtype\"\r\n\r\n";
  prefix += "fileupload\r\n";
  prefix += delimiter;
  prefix += "Content-Disposition: form-data; name=\"time\"\r\n\r\n";
  prefix += "72h\r\n";
  prefix += delimiter;
  prefix += "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"";
  prefix += FilenameForMime(mime);
  prefix += "\"\r\nContent-Type: ";
  prefix += mime;
  prefix += "\r\n\r\n";
  return prefix;
}

std::string MultipartSuffix(const std::string& boundary) {
  return "\r\n--" + boundary + "--\r\n";
}

std::string BuildMultipartBody(const std::string& boundary, const std::string& mime,
                               const std::vector<unsigned char>& image) {
  std::string body = MultipartPrefix(boundary, mime);
  if (!image.empty()) {
    body.append(reinterpret_cast<const char*>(image.data()), image.size());
  }
  body += MultipartSuffix(boundary);
  return body;
}

std::string MakeBoundary() {
  static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  const uint64_t seed = static_cast<uint64_t>(std::random_device()()) ^ static_cast<uint64_t>(GetTickCount64()) ^
                        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  std::mt19937_64 generator(seed);
  std::uniform_int_distribution<size_t> pick(0, sizeof(kAlphabet) - 2);
  std::string boundary = "----";
  while (boundary.size() < kBoundaryLength) {
    boundary.push_back(kAlphabet[pick(generator)]);
  }
  return boundary;
}

// --- Response classification ------------------------------------------------

Result ClassifyUploadResponse(DWORD status, const std::string& body) {
  const std::string text = TrimWhitespace(body);
  if (status == 200) {
    if (text.empty()) {
      return Failure("empty-body");
    }
    if (LooksLikeUploadUrl(text)) {
      Result result;
      result.ok = true;
      result.url = text;
      return result;
    }
    return Failure("http-200-unexpected-body");
  }
  if (status == 412) {
    return Failure(text.empty() ? std::string("http-412") : text);
  }
  return Failure("http-" + std::to_string(status));
}

// --- SHA-256 (cache keys) ---------------------------------------------------

constexpr uint32_t kSha256RoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr uint32_t kSha256InitialState[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

uint32_t RotateRight(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32 - bits));
}

void Sha256Transform(uint32_t state[8], const unsigned char block[64]) {
  uint32_t schedule[64];
  for (size_t i = 0; i < 16; ++i) {
    schedule[i] = static_cast<uint32_t>(block[i * 4]) << 24 | static_cast<uint32_t>(block[i * 4 + 1]) << 16 |
                  static_cast<uint32_t>(block[i * 4 + 2]) << 8 | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 =
        RotateRight(schedule[i - 15], 7) ^ RotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
    const uint32_t s1 =
        RotateRight(schedule[i - 2], 17) ^ RotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }
  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  uint32_t f = state[5];
  uint32_t g = state[6];
  uint32_t h = state[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t sigma1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const uint32_t choice = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + sigma1 + choice + kSha256RoundConstants[i] + schedule[i];
    const uint32_t sigma0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = sigma0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

class Sha256 {
 public:
  Sha256() { std::copy(kSha256InitialState, kSha256InitialState + 8, state_); }

  void Update(const unsigned char* data, size_t length) {
    if (data == nullptr || length == 0) {
      return;
    }
    total_bytes_ += length;
    size_t offset = 0;
    if (buffer_size_ > 0) {
      const size_t needed = sizeof(buffer_) - buffer_size_;
      const size_t take = length < needed ? length : needed;
      std::memcpy(buffer_ + buffer_size_, data, take);
      buffer_size_ += take;
      offset = take;
      if (buffer_size_ == sizeof(buffer_)) {
        Sha256Transform(state_, buffer_);
        buffer_size_ = 0;
      }
    }
    while (length - offset >= sizeof(buffer_)) {
      Sha256Transform(state_, data + offset);
      offset += sizeof(buffer_);
    }
    if (offset < length) {
      std::memcpy(buffer_, data + offset, length - offset);
      buffer_size_ = length - offset;
    }
  }

  void Final(unsigned char digest[32]) {
    const uint64_t total_bits = total_bytes_ * 8;
    unsigned char padding[72] = {};
    padding[0] = 0x80;
    const size_t pad_length = buffer_size_ < 56 ? 56 - buffer_size_ : 120 - buffer_size_;
    Update(padding, pad_length);
    unsigned char length_bytes[8];
    for (size_t i = 0; i < 8; ++i) {
      length_bytes[i] = static_cast<unsigned char>(total_bits >> (56 - i * 8));
    }
    Update(length_bytes, 8);
    for (size_t i = 0; i < 8; ++i) {
      digest[i * 4] = static_cast<unsigned char>(state_[i] >> 24);
      digest[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16);
      digest[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8);
      digest[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
    }
  }

 private:
  uint32_t state_[8] = {};
  unsigned char buffer_[64] = {};
  size_t buffer_size_ = 0;
  uint64_t total_bytes_ = 0;
};

std::string Sha256Hex(const std::vector<unsigned char>& bytes) {
  unsigned char digest[32] = {};
  Sha256 hasher;
  hasher.Update(bytes.empty() ? nullptr : bytes.data(), bytes.size());
  hasher.Final(digest);
  static const char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (unsigned char byte : digest) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 0x0f]);
  }
  return hex;
}

// --- Cache ------------------------------------------------------------------

struct CacheEntry {
  bool ok = false;
  std::string payload;  // URL when ok, failure reason otherwise
  long long expires_at = 0;
};

std::mutex g_cache_mutex;
std::string g_cache_path;
std::map<std::string, CacheEntry> g_cache;

bool IsCacheKey(const std::string& key) {
  if (key.size() != 64) {
    return false;
  }
  for (char c : key) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool ParseLongLong(const std::string& text, long long* value) {
  if (text.empty() || text.size() > 18) {
    return false;
  }
  long long parsed = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + (c - '0');
  }
  *value = parsed;
  return true;
}

std::string SanitizeCacheText(const std::string& text) {
  std::string clean;
  clean.reserve(std::min<size_t>(text.size(), 200));
  for (char c : text) {
    if (clean.size() >= 200) {
      break;
    }
    clean.push_back(c == '\t' || c == '\r' || c == '\n' ? ' ' : c);
  }
  return clean;
}

bool ParseCacheLine(const std::string& line, std::string* key, CacheEntry* entry) {
  if (line.empty() || line[0] == '#') {
    return false;
  }
  const size_t first = line.find('\t');
  if (first == std::string::npos || !IsCacheKey(line.substr(0, first))) {
    return false;
  }
  const size_t second = line.find('\t', first + 1);
  if (second == std::string::npos) {
    return false;
  }
  const std::string state = line.substr(first + 1, second - first - 1);
  const std::string payload = line.substr(second + 1);
  CacheEntry parsed;
  if (state == "ok") {
    if (!LooksLikeUploadUrl(payload)) {
      return false;
    }
    parsed.ok = true;
    parsed.payload = payload;
  } else if (state == "fail") {
    const size_t third = payload.find('\t');
    if (third == std::string::npos || !ParseLongLong(payload.substr(0, third), &parsed.expires_at) ||
        parsed.expires_at <= 0) {
      return false;
    }
    parsed.ok = false;
    parsed.payload = payload.substr(third + 1);
  } else {
    return false;
  }
  *key = line.substr(0, first);
  *entry = parsed;
  return true;
}

std::string FormatCacheLine(const std::string& key, const CacheEntry& entry) {
  if (entry.ok) {
    return key + "\tok\t" + entry.payload;
  }
  return key + "\tfail\t" + std::to_string(entry.expires_at) + "\t" + entry.payload;
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  out->clear();
  const std::wstring wide = Utils::ToWString(path);
  if (wide.empty()) {
    return false;
  }
  HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  char buffer[4096];
  bool ok = true;
  for (;;) {
    DWORD read = 0;
    if (ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) == FALSE) {
      ok = false;
      break;
    }
    if (read == 0) {
      break;
    }
    out->append(buffer, read);
    if (out->size() > kMaxCacheBytes) {
      ok = false;
      break;
    }
  }
  CloseHandle(file);
  if (!ok) {
    out->clear();
  }
  return ok;
}

bool WriteWholeFile(const std::string& path, const std::string& content) {
  const std::wstring wide = Utils::ToWString(path);
  if (wide.empty()) {
    return false;
  }
  const std::wstring temp = wide + L".tmp";
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool ok = true;
  const char* data = content.data();
  size_t remaining = content.size();
  while (remaining > 0) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1 << 20));
    DWORD written = 0;
    if (WriteFile(file, data, chunk, &written, nullptr) == FALSE || written != chunk) {
      ok = false;
      break;
    }
    data += chunk;
    remaining -= chunk;
  }
  CloseHandle(file);
  if (!ok || MoveFileExW(temp.c_str(), wide.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
    DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

void LoadCacheLocked() {
  g_cache.clear();
  std::string content;
  if (!ReadWholeFile(g_cache_path, &content)) {
    return;
  }
  size_t start = 0;
  for (;;) {
    const size_t end = content.find('\n', start);
    const std::string line =
        end == std::string::npos ? content.substr(start) : content.substr(start, end - start);
    std::string key;
    CacheEntry entry;
    if (ParseCacheLine(line, &key, &entry)) {
      g_cache[key] = entry;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
}

void SaveCacheLocked() {
  std::string content = "# cover publisher cache v1\n";
  for (const auto& item : g_cache) {
    content += FormatCacheLine(item.first, item.second);
    content += "\n";
  }
  WriteWholeFile(g_cache_path, content);  // best effort: unwritable paths degrade to memory only
}

bool CacheLookup(const std::string& key, long long now, Result* out) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_cache_path.empty()) {
    return false;
  }
  const auto found = g_cache.find(key);
  if (found == g_cache.end()) {
    return false;
  }
  if (!found->second.ok && found->second.expires_at <= now) {
    g_cache.erase(found);
    SaveCacheLocked();
    return false;
  }
  Result result;
  if (found->second.ok) {
    result.ok = true;
    result.url = found->second.payload;
  } else {
    result.reason = found->second.payload;
  }
  *out = result;
  return true;
}

void CacheStore(const std::string& key, const Result& result, long long now) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_cache_path.empty()) {
    return;
  }
  CacheEntry entry;
  if (result.ok && LooksLikeUploadUrl(result.url)) {
    entry.ok = true;
    entry.payload = result.url;
  } else {
    entry.ok = false;
    entry.expires_at = now + kNegativeCacheTtlSeconds;
    entry.payload = SanitizeCacheText(result.ok ? std::string("unexpected-body") : result.reason);
  }
  g_cache[key] = entry;
  SaveCacheLocked();
}

// --- WinHTTP plumbing -------------------------------------------------------

class WinHttpHandle {
 public:
  explicit WinHttpHandle(HINTERNET handle = nullptr) : handle_(handle) {}
  ~WinHttpHandle() {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
    }
  }
  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;
  WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        WinHttpCloseHandle(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  HINTERNET get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }

 private:
  HINTERNET handle_;
};

class Deadline {
 public:
  explicit Deadline(DWORD budget_ms)
      : end_(std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms)) {}

  bool Expired() const { return std::chrono::steady_clock::now() >= end_; }

  DWORD RemainingMs() const {
    const auto now = std::chrono::steady_clock::now();
    if (now >= end_) {
      return 1;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end_ - now).count();
    if (remaining < 1) {
      return 1;
    }
    if (remaining > static_cast<long long>(kTotalTimeoutMs)) {
      return kTotalTimeoutMs;
    }
    return static_cast<DWORD>(remaining);
  }

 private:
  std::chrono::steady_clock::time_point end_;
};

WinHttpHandle OpenSession(const Deadline& deadline) {
  WinHttpHandle session(WinHttpOpen(kAgentName, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    return session;
  }
  const int timeout = static_cast<int>(deadline.RemainingMs());
  WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);
  return session;
}

bool QueryStatusCode(HINTERNET request, DWORD* status) {
  DWORD size = sizeof(DWORD);
  *status = 0;
  return WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, status, &size, WINHTTP_NO_HEADER_INDEX) != FALSE;
}

bool QueryContentType(HINTERNET request, std::wstring* content_type) {
  DWORD size = 0;
  WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                      WINHTTP_NO_HEADER_INDEX);
  if (size == 0) {
    return false;
  }
  std::wstring buffer(size / sizeof(wchar_t), L'\0');
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size,
                          WINHTTP_NO_HEADER_INDEX) == FALSE) {
    return false;
  }
  buffer.resize(size / sizeof(wchar_t));
  while (!buffer.empty() && buffer.back() == L'\0') {
    buffer.pop_back();
  }
  *content_type = buffer;
  return true;
}

bool StartsWithImageType(const std::wstring& content_type) {
  const wchar_t kPrefix[] = L"image/";
  if (content_type.size() < 6) {
    return false;
  }
  for (size_t i = 0; i < 6; ++i) {
    wchar_t c = content_type[i];
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    if (c != kPrefix[i]) {
      return false;
    }
  }
  return true;
}

bool ReadResponseBody(HINTERNET request, const Deadline& deadline, std::string* out) {
  out->clear();
  for (;;) {
    if (deadline.Expired()) {
      return false;
    }
    DWORD available = 0;
    if (WinHttpQueryDataAvailable(request, &available) == FALSE) {
      return false;
    }
    if (available == 0) {
      return true;
    }
    if (available > kMaxResponseBytes) {
      available = kMaxResponseBytes;
    }
    const size_t offset = out->size();
    out->resize(offset + available);
    DWORD read = 0;
    if (WinHttpReadData(request, out->data() + offset, available, &read) == FALSE) {
      return false;
    }
    out->resize(offset + read);
    if (read == 0 || out->size() >= kMaxResponseBytes) {
      return true;
    }
  }
}

Result UploadMultipartBody(const std::string& body, const std::string& boundary) {
  if (body.size() > 0xffffffffull) {
    return Failure("body-too-large");
  }
  const Deadline deadline(kTotalTimeoutMs);
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    return Failure("session-open-failed");
  }
  WinHttpHandle connection(WinHttpConnect(session.get(), kUploadHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    return Failure("connect-failed");
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"POST", kUploadPath, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    return Failure("request-open-failed");
  }
  const std::wstring headers = L"Content-Type: multipart/form-data; boundary=" + Utils::ToWString(boundary);
  const DWORD body_size = static_cast<DWORD>(body.size());
  if (WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                         body_size, body_size, 0) == FALSE) {
    return Failure("send-failed");
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return Failure("receive-failed");
  }
  DWORD status = 0;
  if (!QueryStatusCode(request.get(), &status)) {
    return Failure("status-query-failed");
  }
  std::string response;
  if (!ReadResponseBody(request.get(), deadline, &response)) {
    if (status == 200 || status == 412) {
      return ClassifyUploadResponse(status, response);
    }
    return Failure("read-failed");
  }
  return ClassifyUploadResponse(status, response);
}

bool CrackUrl(const std::string& url, std::wstring* host, std::wstring* path) {
  const std::wstring wide = Utils::ToWString(url);
  if (wide.empty()) {
    return false;
  }
  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components) == FALSE) {
    return false;
  }
  if (components.nScheme != INTERNET_SCHEME_HTTPS || components.lpszHostName == nullptr ||
      components.dwHostNameLength == 0) {
    return false;
  }
  *host = std::wstring(components.lpszHostName, components.dwHostNameLength);
  std::wstring whole_path;
  if (components.lpszUrlPath != nullptr && components.dwUrlPathLength > 0) {
    whole_path.assign(components.lpszUrlPath, components.dwUrlPathLength);
  }
  if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength > 0) {
    whole_path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  if (whole_path.empty()) {
    whole_path = L"/";
  }
  *path = whole_path;
  return true;
}

// Proves the upload is actually retrievable: a bare GET (no added headers)
// must answer 200 with an image/* content type.
bool HealthCheckUrl(const std::string& url) {
  const Deadline deadline(kTotalTimeoutMs);
  std::wstring host;
  std::wstring path;
  if (!CrackUrl(url, &host, &path)) {
    return false;
  }
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    return false;
  }
  WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    return false;
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    return false;
  }
  if (WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE) {
    return false;
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return false;
  }
  DWORD status = 0;
  if (!QueryStatusCode(request.get(), &status) || status != 200) {
    return false;
  }
  std::wstring content_type;
  if (!QueryContentType(request.get(), &content_type)) {
    return false;
  }
  return StartsWithImageType(content_type);
}

}  // namespace

// --- Public interface -------------------------------------------------------

namespace CoverPublisher {

void Configure(const std::string& cache_path) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_cache_path = cache_path;
  g_cache.clear();
  if (!cache_path.empty()) {
    LoadCacheLocked();
  }
}

Result Publish(const std::vector<unsigned char>& image_bytes, const char* mime) {
  try {
    if (image_bytes.empty()) {
      return Failure("empty-image");
    }
    const std::string key = Sha256Hex(image_bytes);
    const long long now = static_cast<long long>(std::time(nullptr));
    Result cached;
    if (CacheLookup(key, now, &cached)) {
      return cached;
    }

    const std::string content_type = SanitizeMime(mime);
    const std::string boundary = MakeBoundary();
    const std::string body = BuildMultipartBody(boundary, content_type, image_bytes);
    Result result = UploadMultipartBody(body, boundary);
    if (result.ok && !HealthCheckUrl(result.url)) {
      result = Failure("health-check-failed");
    }
    CacheStore(key, result, now);
    return result;
  } catch (...) {
    return Failure("exception");
  }
}

void ClearCache() {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_cache.clear();
  if (!g_cache_path.empty()) {
    const std::wstring wide = Utils::ToWString(g_cache_path);
    if (!wide.empty()) {
      DeleteFileW(wide.c_str());
    }
  }
}

}  // namespace CoverPublisher
