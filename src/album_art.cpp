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

#include "album_art.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace AlbumArt {
namespace {

constexpr size_t kMaxResponseBytes = 512 * 1024;
constexpr int kTimeoutMilliseconds = 1500;

constexpr const char* kDeezerHost = "api.deezer.com";
constexpr const char* kItunesHost = "itunes.apple.com";

constexpr const char* kHexDigits = "0123456789ABCDEF";

bool IsUnreserved(unsigned char value) {
  if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
      (value >= '0' && value <= '9')) {
    return true;
  }
  return value == '-' || value == '_' || value == '.' || value == '~';
}

std::wstring WidenUtf8(const std::string& utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(),
                      length);
  return wide;
}

void AppendUtf8Codepoint(std::string& output, unsigned int codepoint) {
  if (codepoint <= 0x7F) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

int HexValue(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

// Reads a JSON string literal that starts right after the opening quote and
// decodes its escapes.
std::string DecodeJsonStringAt(const std::string& json, size_t position) {
  std::string decoded;

  for (size_t i = position; i < json.size(); ++i) {
    const char current = json[i];
    if (current == '"') {
      break;
    }
    if (current != '\\') {
      decoded.push_back(current);
      continue;
    }

    if (i + 1 >= json.size()) {
      break;
    }
    const char escape = json[++i];
    switch (escape) {
      case '"':
      case '\\':
      case '/':
        decoded.push_back(escape);
        break;
      case 'b':
        decoded.push_back('\b');
        break;
      case 'f':
        decoded.push_back('\f');
        break;
      case 'n':
        decoded.push_back('\n');
        break;
      case 'r':
        decoded.push_back('\r');
        break;
      case 't':
        decoded.push_back('\t');
        break;
      case 'u': {
        if (i + 4 >= json.size()) {
          break;
        }
        unsigned int codepoint = 0;
        bool valid = true;
        for (size_t digit = 0; digit < 4; ++digit) {
          const int value = HexValue(json[i + 1 + digit]);
          if (value < 0) {
            valid = false;
            break;
          }
          codepoint = (codepoint << 4) | static_cast<unsigned int>(value);
        }
        if (valid) {
          AppendUtf8Codepoint(decoded, codepoint);
        }
        i += 4;
        break;
      }
      default:
        decoded.push_back(escape);
        break;
    }
  }

  return decoded;
}

// Returns the first non-empty value for `key` that is an https URL within
// Discord's length limit.
std::string ExtractFirstUsableUrl(const std::string& body, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t search_from = 0;

  while (search_from < body.size()) {
    const size_t position = body.find(needle, search_from);
    if (position == std::string::npos) {
      return std::string();
    }
    search_from = position + needle.size();

    size_t cursor = search_from;
    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
      ++cursor;
    }
    if (cursor >= body.size() || body[cursor] != ':') {
      continue;
    }
    ++cursor;
    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
      ++cursor;
    }
    if (cursor >= body.size() || body[cursor] != '"') {
      continue;
    }

    const std::string value = DecodeJsonStringAt(body, cursor + 1);
    if (value.size() >= 8 && value.compare(0, 8, "https://") == 0 &&
        value.size() <= kMaxUrlLength) {
      return value;
    }
  }

  return std::string();
}

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

  HINTERNET get() const { return handle_; }
  bool valid() const { return handle_ != nullptr; }

 private:
  HINTERNET handle_;
};

std::string HttpGet(const std::string& url) {
  const std::wstring wide_url = WidenUtf8(url);
  if (wide_url.empty()) {
    return std::string();
  }

  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  components.lpszHostName = host;
  components.dwHostNameLength = static_cast<DWORD>(std::size(host));
  components.lpszUrlPath = path;
  components.dwUrlPathLength = static_cast<DWORD>(std::size(path));

  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
    return std::string();
  }

  WinHttpHandle session(WinHttpOpen(L"AIMP-Discord-Presence/2.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.valid()) {
    return std::string();
  }
  WinHttpSetTimeouts(session.get(), kTimeoutMilliseconds, kTimeoutMilliseconds,
                     kTimeoutMilliseconds, kTimeoutMilliseconds);

  WinHttpHandle connection(WinHttpConnect(session.get(), host, components.nPort, 0));
  if (!connection.valid()) {
    return std::string();
  }

  const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path, nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           flags));
  if (!request.valid()) {
    return std::string();
  }

  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.get(), nullptr)) {
    return std::string();
  }

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(request.get(),
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                           WINHTTP_NO_HEADER_INDEX) ||
      status != 200) {
    return std::string();
  }

  std::string body;
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(request.get(), &available) && available > 0 &&
         body.size() < kMaxResponseBytes) {
    const DWORD chunk = (std::min)(
        available, static_cast<DWORD>(kMaxResponseBytes - body.size()));
    std::string buffer(static_cast<size_t>(chunk), '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), buffer.data(), chunk, &read) || read == 0) {
      break;
    }
    body.append(buffer, 0, static_cast<size_t>(read));
  }

  return body;
}

std::string CacheKey(const std::string& artist, const std::string& album) {
  return artist + "\n" + album;
}

}  // namespace

std::string UriEncodeUtf8(const std::string& utf8) {
  std::string encoded;
  encoded.reserve(utf8.size() * 3);

  for (const char raw : utf8) {
    const unsigned char value = static_cast<unsigned char>(raw);
    if (IsUnreserved(value)) {
      encoded.push_back(static_cast<char>(value));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHexDigits[(value >> 4) & 0x0F]);
      encoded.push_back(kHexDigits[value & 0x0F]);
    }
  }

  return encoded;
}

std::string ExtractJsonStringField(const std::string& json, const std::string& key) {
  const size_t position = json.find("\"" + key + "\"");
  if (position == std::string::npos) {
    return std::string();
  }

  size_t cursor = position + key.size() + 2;
  while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor])) != 0) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != ':') {
    return std::string();
  }
  ++cursor;
  while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor])) != 0) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != '"') {
    return std::string();
  }

  return DecodeJsonStringAt(json, cursor + 1);
}

std::string UpscaleItunesArtwork(const std::string& url) {
  const size_t marker = url.rfind("bb.");
  if (marker == std::string::npos) {
    return url;
  }

  // Walk back over "<width>x<height>" to find the start of the size token.
  size_t start = marker;
  while (start > 0) {
    const char previous = url[start - 1];
    if (std::isdigit(static_cast<unsigned char>(previous)) != 0 || previous == 'x') {
      --start;
    } else {
      break;
    }
  }

  if (start == marker || url[start] == 'x') {
    return url;
  }

  return url.substr(0, start) + "600x600bb." + url.substr(marker + 3);
}

std::string BuildDeezerSearchUrl(const std::string& artist, const std::string& album) {
  // Deezer retired the `artist:"..." album:"..."` advanced syntax: it now
  // answers 200 with {"data":[],"total":0} (verified 2026-10-01). The plain
  // free-text query returns the same album objects, including `cover_xl`.
  std::string url = "https://";
  url += kDeezerHost;
  url += "/search?q=";
  url += UriEncodeUtf8(artist + " " + album);
  return url;
}

std::string BuildItunesSearchUrl(const std::string& artist, const std::string& album) {
  std::string url = "https://";
  url += kItunesHost;
  url += "/search?term=";
  url += UriEncodeUtf8(artist + " " + album);
  url += "&media=music&limit=1";
  return url;
}

std::string ExtractArtworkUrl(const std::string& body, Provider provider) {
  if (body.empty()) {
    return std::string();
  }

  if (provider == Provider::kDeezer) {
    for (const char* key : {"cover_xl", "cover_big", "cover"}) {
      const std::string url = ExtractFirstUsableUrl(body, key);
      if (!url.empty()) {
        return url;
      }
    }
    return std::string();
  }

  if (provider == Provider::kItunes) {
    for (const char* key : {"artworkUrl100", "artworkUrl60", "artworkUrl30"}) {
      const std::string url = ExtractFirstUsableUrl(body, key);
      if (!url.empty()) {
        return UpscaleItunesArtwork(url);
      }
    }
    return std::string();
  }

  return std::string();
}

Resolver::Resolver() { worker_ = std::thread(&Resolver::WorkerMain, this); }

Resolver::~Resolver() { Shutdown(); }

void Resolver::SetOnlineEnabled(bool enabled) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    online_ = enabled;
  }
  wake_.notify_all();
}

void Resolver::SetCallback(Callback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

void Resolver::Request(const std::string& artist, const std::string& album) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    request_artist_ = artist;
    request_album_ = album;
    has_request_ = true;
    ++generation_;
  }
  wake_.notify_all();
}

std::string Resolver::Lookup(const std::string& artist, const std::string& album) {
  const std::string key = CacheKey(artist, album);

  std::string cached;
  if (TakeCached(key, &cached)) {
    return cached;
  }

  const std::string url = LookupUncached(artist, album);
  StoreCached(key, url);
  return url;
}

bool Resolver::TakeCached(const std::string& key, std::string* url) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = cache_.find(key);
  if (found == cache_.end()) {
    return false;
  }
  *url = found->second;
  return true;
}

void Resolver::StoreCached(const std::string& key, const std::string& url) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto inserted = cache_.insert({key, url});
  if (!inserted.second) {
    return;
  }
  cache_order_.push_back(key);

  while (cache_order_.size() > kMaxCacheEntries) {
    const std::string oldest = cache_order_.front();
    cache_order_.pop_front();
    cache_.erase(oldest);
  }
}

std::string Resolver::LookupUncached(const std::string& artist, const std::string& album) {
  if (artist.empty() || album.empty()) {
    return std::string();
  }

  const std::string deezer_body = HttpGet(BuildDeezerSearchUrl(artist, album));
  const std::string deezer_url = ExtractArtworkUrl(deezer_body, Provider::kDeezer);
  if (!deezer_url.empty()) {
    return deezer_url;
  }

  const std::string itunes_body = HttpGet(BuildItunesSearchUrl(artist, album));
  return ExtractArtworkUrl(itunes_body, Provider::kItunes);
}

void Resolver::WorkerMain() {
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [this] { return stopping_ || has_request_; });
    if (stopping_) {
      break;
    }

    has_request_ = false;
    const uint64_t generation = generation_;
    const bool online = online_;
    const std::string artist = request_artist_;
    const std::string album = request_album_;
    const Callback callback = callback_;
    lock.unlock();

    std::string url;
    if (online) {
      url = Lookup(artist, album);
    }

    lock.lock();
    const bool stale = stopping_ || generation != generation_;
    lock.unlock();

    if (!stale && callback) {
      callback(artist, album, url);
    }
  }
}

void Resolver::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  wake_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }
}

}  // namespace AlbumArt
