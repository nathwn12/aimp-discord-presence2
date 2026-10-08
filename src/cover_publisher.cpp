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
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include "utils.h"

// WIC and the COM stream plumbing it needs; declared here so the plugin and the
// offline test binary link the same way without extra project wiring.
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

using CoverPublisher::Result;

// --- Transport constants ----------------------------------------------------

// Per-HTTP-operation budget. Each upload attempt and each health check gets
// this much time; a transport failure is retried once after a short pause, so
// an upload is bounded by two attempts plus the backoff.
//
// Sized against live measurement, not guessed: a ~30-300 KB cover uploads to
// uguu in 0.8-4.1 s and a blocked host answers 403 immediately. The budget only
// has to catch a HANG, and a hang is better answered by moving to the next host
// than by waiting - the previous 20 s per host made a stalled host cost the
// owner ~40 s of black card before the chain gave up.
constexpr DWORD kTotalTimeoutMs = 6000;
constexpr DWORD kRetryBackoffMs = 500;
constexpr DWORD kMaxResponseBytes = 64 * 1024;
constexpr size_t kMaxCacheBytes = 1024 * 1024;
constexpr long long kNegativeCacheTtlSeconds = 60;
// 29 bytes keep the closing delimiter ("\r\n--<boundary>--\r\n") at 37 bytes,
// matching the live measurement of the host's request framing.
constexpr size_t kBoundaryLength = 29;

// Cover transfer shaping: the image is centre-cropped to a square (the shorter
// side) and then scaled down to at most 512 px - Discord re-crops every large
// image to a centre square anyway, so cropping here keeps the payload small and
// correctly framed. The result is encoded as JPEG q85, unless the source is
// already both at or below 512 px and at most 256 KB, in which case it goes up
// untouched (the existing skip rule).
constexpr UINT kMaxImageDimension = 512;
constexpr size_t kSkipReencodeMaxBytes = 256 * 1024;
constexpr float kJpegQuality = 0.85f;

// The GitHub contents response carries a base64 `content` object and can run to
// a few hundred KB; the keyless hosts answer in bytes, so the larger limit
// applies to the GitHub request only.
constexpr size_t kMaxGitHubResponseBytes = 1024 * 1024;

constexpr wchar_t kAgentName[] = L"AIMP-Discord-Presence/2.0";

// Upload hosts are tried in order until one returns a usable URL. GitHub, when
// a token is configured, is FIRST because it is the owner's own permanent repo;
// catbox.moe is a permanent keyless host; uguu.se is the host measured working
// from this network; litter.catbox.moe is kept LAST as a dormant fallback - it
// answers HTTP 403 to a BunkerWeb WAF here, so it is never reached when an
// earlier host succeeds, but it may still serve other networks. A host that
// fails costs its round trip only, and never skips a layer of the cover chain.
// Each host owns both its endpoint and the shape of its request body.
enum class UploadShape {
  // GitHub: PUT /repos/{owner}/{repo}/contents/covers/{name}.jpg with a JSON
  // body (base64 image). The response is JSON; the URL is content.download_url.
  kGitHub,
  // catbox: POST /user/api.php with reqtype=fileupload and the image in
  // `fileToUpload`. The response body is the bare URL as text.
  kCatbox,
  // uguu: POST /upload?output=text with no extra fields and the image in
  // `files[]`. The response body is the bare URL as text.
  kUguu,
  // litterbox: POST /resources/internals/api.php with reqtype=fileupload,
  // time=72h and the image in `fileToUpload`.
  kLitterbox,
};

struct UploadHost {
  std::wstring name;   // log marker, e.g. "litter.catbox.moe"
  std::wstring host;   // for WinHttpConnect
  std::wstring path;   // request target, query string included
  UploadShape shape;
};

// The log marker as UTF-8, so it can be concatenated into a std::string line.
std::string HostNameUtf8(const UploadHost& upload_host) {
  return Utils::ToString(upload_host.name);
}

// The keyless hosts, always present and always in this order: catbox, then
// uguu, then the dormant litterbox. The GitHub host, when configured, is
// prepended to these by ActiveHosts().
const size_t kBaseUploadHostCount = 3;
const UploadHost kBaseUploadHosts[kBaseUploadHostCount] = {
    {L"catbox.moe", L"catbox.moe", L"/user/api.php", UploadShape::kCatbox},
    {L"uguu.se", L"uguu.se", L"/upload?output=text", UploadShape::kUguu},
    {L"litter.catbox.moe", L"litterbox.catbox.moe", L"/resources/internals/api.php", UploadShape::kLitterbox},
};

// The default GitHub repository: the owner's permanent art repo. CoverRepo
// overrides it; CoverToken (empty by default) enables the host at all.
const wchar_t kDefaultGitHubRepo[] = L"nathwn12/aimp-discord-presence-art";

// The GitHub host configuration. `token` is only ever sent to api.github.com;
// it is never logged, cached, or put into a Result. Guarded by g_host_mutex
// because ConfigureAuth runs before the publisher worker starts, but the worker
// reads it for each publish.
std::mutex g_host_mutex;
std::string g_github_token;
std::wstring g_github_owner;
std::wstring g_github_repo;

// Splits "owner/name" into its two parts. A malformed value yields false, which
// skips the GitHub host rather than building a nonsense request target.
bool ParseOwnerRepo(const std::wstring& repo, std::wstring* owner, std::wstring* name) {
  const size_t slash = repo.find(L'/');
  if (slash == std::wstring::npos || slash == 0 || slash + 1 >= repo.size()) {
    return false;
  }
  *owner = repo.substr(0, slash);
  *name = repo.substr(slash + 1);
  return name->find(L'/') == std::wstring::npos;
}

// Every host to try, in order. GitHub first only when a token and a parseable
// repo are configured; the keyless hosts otherwise (unchanged chain).
std::vector<UploadHost> ActiveHosts() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  std::vector<UploadHost> hosts;
  if (!g_github_token.empty() && !g_github_owner.empty() && !g_github_repo.empty()) {
    UploadHost github;
    github.name = L"github.com";
    github.host = L"api.github.com";
    github.path = L"/repos/" + g_github_owner + L"/" + g_github_repo + L"/contents/covers/";
    github.shape = UploadShape::kGitHub;
    hosts.push_back(github);
  }
  for (size_t i = 0; i < kBaseUploadHostCount; ++i) {
    hosts.push_back(kBaseUploadHosts[i]);
  }
  return hosts;
}

bool GitHubTokenPresent() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return !g_github_token.empty();
}

// The GitHub host is usable only when a token AND a parseable repo are
// configured - the same condition ActiveHosts() uses to prepend it. A token
// with a malformed repo is present-but-unused, exactly like no token.
bool GitHubUploadActive() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return !g_github_token.empty() && !g_github_owner.empty() && !g_github_repo.empty();
}

// Each host owns the exact URL shape it answers with, so a lookalike host
// ("...moe.evil.test") or a body that is not that host's URL is rejected. The
// success response is a bare https URL matching the host that answered; a WAF
// or JSON error page, or HTTP 200 with an empty body, is a failure.
// uguu round-robins across single-label subdomains (n., h., ...), so the
// subdomain is matched generally rather than pinned to one letter - pinning it
// rejected real responses as "unexpected body".
constexpr char kLitterboxUrlPattern[] = R"(^https://litter\.catbox\.moe/[a-z0-9]{6}\.(png|jpg|jpeg)$)";
constexpr char kUguuUrlPattern[] = R"(^https://([A-Za-z0-9-]+\.)?uguu\.se/[A-Za-z0-9]+\.(png|jpg|jpeg|gif|webp|bmp)$)";
constexpr char kCatboxUrlPattern[] = R"(^https://files\.catbox\.moe/[A-Za-z0-9]+\.(png|jpg|jpeg|gif|webp|bmp)$)";
// A GitHub raw cover URL, generic over owner/repo so the cache can validate a
// stored URL; the per-request classifier pins the exact owner and repo.
constexpr char kGitHubRawUrlPattern[] =
    R"(^https://raw\.githubusercontent\.com/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/main/covers/[0-9a-f]{64}\.jpg$)";
// Fallback when no host is specified (kept for callers that classify a bare
// response): the strict litterbox shape.
std::string UploadUrlPatternFor(const UploadHost& upload_host) {
  switch (upload_host.shape) {
    case UploadShape::kCatbox:
      return std::string(kCatboxUrlPattern);
    case UploadShape::kUguu:
      return std::string(kUguuUrlPattern);
    case UploadShape::kGitHub:
      return std::string(kGitHubRawUrlPattern);
    case UploadShape::kLitterbox:
    default:
      return std::string(kLitterboxUrlPattern);
  }
}
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

// A URL is accepted only when it matches one configured host's exact shape. The
// strictness per host is what keeps a lookalike host, an HTML page or a JSON
// blob from ever being adopted as a cover.
bool LooksLikeUploadUrl(const std::string& text) {
  static const std::regex litterbox_pattern(kLitterboxUrlPattern);
  static const std::regex uguu_pattern(kUguuUrlPattern);
  static const std::regex catbox_pattern(kCatboxUrlPattern);
  static const std::regex github_pattern(kGitHubRawUrlPattern);
  return std::regex_match(text, litterbox_pattern) || std::regex_match(text, uguu_pattern) ||
         std::regex_match(text, catbox_pattern) || std::regex_match(text, github_pattern);
}

// The per-host predicate used by the classifier: a 200 is a success only when
// the body is the bare URL of the host that answered. GitHub never uses this
// form (its response is JSON, validated with its own owner/repo pin), so its
// pattern here is the generic raw-URL shape.
bool LooksLikeHostUrl(const std::string& text, const UploadHost& upload_host) {
  const std::string url_pattern = UploadUrlPatternFor(upload_host);
  static const std::map<std::string, std::regex> patterns = {
      {kLitterboxUrlPattern, std::regex(kLitterboxUrlPattern)},
      {kUguuUrlPattern, std::regex(kUguuUrlPattern)},
      {kCatboxUrlPattern, std::regex(kCatboxUrlPattern)},
      {kGitHubRawUrlPattern, std::regex(kGitHubRawUrlPattern)},
  };
  const auto found = patterns.find(url_pattern);
  return found != patterns.end() && std::regex_match(text, found->second);
}

// A cached URL on a keyless host (catbox, uguu, litterbox) is ephemeral: it
// can die while the cache entry lives (measured: a uguu URL answering 404
// months later). A GitHub raw URL is the owner's own permanent repo. The
// cache only ever stores URLs that pass LooksLikeUploadUrl, so an unknown
// shape passes through as durable - it can only be a future host's URL.
bool IsEphemeralCachedUrl(const std::string& url) {
  return LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kCatbox}) ||
         LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kUguu}) ||
         LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kLitterbox});
}

bool IsGitHubCachedUrl(const std::string& url) {
  return LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kGitHub});
}

// What a cache hit needs: use it as-is, re-publish the local bytes to the
// permanent GitHub host, or revalidate the stored URL before trusting it.
// Pure, so the policy is testable without a network.
enum class CachedUrlAction {
  kUse,             // durable (GitHub) or unknown shape: answer the hit
  kUpgradeToGitHub,  // ephemeral URL, token configured: re-publish to GitHub
  kRevalidate,       // ephemeral URL, no token: health-check, re-publish if dead
};

CachedUrlAction PlanCachedUrl(const std::string& url, bool github_active) {
  if (!IsEphemeralCachedUrl(url)) {
    return CachedUrlAction::kUse;
  }
  return github_active ? CachedUrlAction::kUpgradeToGitHub : CachedUrlAction::kRevalidate;
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

std::string MultipartPrefix(const std::string& boundary, const std::string& mime,
                            UploadShape shape) {
  const std::string delimiter = "--" + boundary + "\r\n";
  std::string prefix;
  if (shape == UploadShape::kLitterbox) {
    // Three parts: reqtype, time, then the file in `fileToUpload`.
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"reqtype\"\r\n\r\n";
    prefix += "fileupload\r\n";
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"time\"\r\n\r\n";
    prefix += "72h\r\n";
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"";
  } else if (shape == UploadShape::kCatbox) {
    // Two parts: reqtype, then the file in `fileToUpload`. Catbox stores
    // permanently, so unlike litterbox there is no time field.
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"reqtype\"\r\n\r\n";
    prefix += "fileupload\r\n";
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"";
  } else {
    // One part only, in `files[]`, with no extra fields.
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"files[]\"; filename=\"";
  }
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
                               const std::vector<unsigned char>& image,
                               UploadShape shape = UploadShape::kLitterbox) {
  std::string body = MultipartPrefix(boundary, mime, shape);
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

// `shape` names the host that answered, so the 200 body must be that host's own
// bare URL. The default is litterbox, which keeps the strict two-argument form
// (and its callers) clamped to the original host.
Result ClassifyUploadResponse(DWORD status, const std::string& body,
                              UploadShape shape = UploadShape::kLitterbox) {
  const std::string text = TrimWhitespace(body);
  if (status == 200) {
    if (text.empty()) {
      return Failure("empty-body");
    }
    if (LooksLikeHostUrl(text, UploadHost{L"", L"", L"", shape})) {
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

// --- GitHub contents API ----------------------------------------------------
//
// The GitHub host is the owner's own permanent repo. A cover is written once,
// named by the SHA-256 of the uploaded bytes, so re-uploading the same image is
// idempotent: the contents API answers 422 (or 409) when the file is already
// present, and that is treated as success via the raw URL. The access token is
// only ever placed in the Authorization header of a request to api.github.com;
// it never reaches a Result, the cache, or a log line.

std::string Base64Encode(const unsigned char* data, size_t length) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((length + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= length) {
    const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
    encoded.push_back(kAlphabet[chunk & 0x3f]);
    i += 3;
  }
  const size_t remaining = length - i;
  if (remaining == 1) {
    const uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded += "==";
  } else if (remaining == 2) {
    const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
    encoded.push_back('=');
  }
  return encoded;
}

// A small, dependency-free JSON string field reader in the same style as
// album_art.cpp's extractor: find the quoted key, then read the quoted string
// that follows it. GitHub's URLs never contain escapes, but a backslash is
// consumed so a surprising response cannot desync the scan.
std::string ExtractJsonStringField(const std::string& json, const std::string& key) {
  const size_t position = json.find("\"" + key + "\"");
  if (position == std::string::npos) {
    return std::string();
  }
  size_t cursor = position + key.size() + 2;
  const auto skip_space = [&]() {
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                    json[cursor] == '\r' || json[cursor] == '\n')) {
      ++cursor;
    }
  };
  skip_space();
  if (cursor >= json.size() || json[cursor] != ':') {
    return std::string();
  }
  ++cursor;
  skip_space();
  if (cursor >= json.size() || json[cursor] != '"') {
    return std::string();
  }
  ++cursor;
  std::string value;
  while (cursor < json.size() && json[cursor] != '"') {
    if (json[cursor] == '\\' && cursor + 1 < json.size()) {
      ++cursor;
    }
    value.push_back(json[cursor]);
    ++cursor;
  }
  return value;
}

std::string GitHubRawUrl(const std::wstring& owner, const std::wstring& repo,
                         const std::string& name) {
  return "https://raw.githubusercontent.com/" + Utils::ToString(owner) + "/" +
         Utils::ToString(repo) + "/main/covers/" + name + ".jpg";
}

// The exact request the GitHub host sends. Split out as a pure builder so the
// method, target, headers and JSON body are testable without a network.
struct GitHubRequest {
  std::wstring method;
  std::wstring host;
  std::wstring path;
  std::wstring headers;
  std::string body;
};

GitHubRequest BuildGitHubRequest(const std::string& token, const std::wstring& owner,
                                 const std::wstring& repo, const std::string& name,
                                 const std::vector<unsigned char>& image) {
  GitHubRequest request;
  request.method = L"PUT";
  request.host = L"api.github.com";
  request.path =
      L"/repos/" + owner + L"/" + repo + L"/contents/covers/" + Utils::ToWString(name) + L".jpg";
  request.headers = L"Authorization: Bearer " + Utils::ToWString(token) + L"\r\n" +
                    L"Accept: application/vnd.github+json\r\n" +
                    L"User-Agent: AIMP-Discord-Presence\r\n" +
                    L"X-GitHub-Api-Version: 2022-11-28\r\n" +
                    L"Content-Type: application/json\r\n";
  const std::string encoded =
      Base64Encode(image.empty() ? nullptr : image.data(), image.size());
  const std::string short_sha = name.size() >= 7 ? name.substr(0, 7) : name;
  request.body = "{\"message\":\"add cover " + short_sha + "\",\"content\":\"" + encoded +
                 "\",\"branch\":\"main\"}";
  return request;
}

// Classifies a GitHub contents PUT. The success body is JSON and the URL is
// content.download_url, pinned to this exact owner and repo; a body without
// that field is never adopted as a URL. 422/409 mean the immutable file is
// already there, so the constructed raw URL is returned and the caller's health
// check proves it retrievable.
Result ClassifyGitHubResponse(DWORD status, const std::string& body, const std::wstring& owner,
                              const std::wstring& repo, const std::string& name) {
  if (status == 200 || status == 201) {
    const std::string text = TrimWhitespace(body);
    if (text.empty()) {
      return Failure("empty-body");
    }
    const std::string download = ExtractJsonStringField(text, "download_url");
    const std::string prefix = "https://raw.githubusercontent.com/" + Utils::ToString(owner) +
                               "/" + Utils::ToString(repo) + "/";
    if (download.size() > prefix.size() && download.compare(0, prefix.size(), prefix) == 0 &&
        LooksLikeHostUrl(download, UploadHost{L"", L"", L"", UploadShape::kGitHub})) {
      Result result;
      result.ok = true;
      result.url = download;
      return result;
    }
    return Failure("http-" + std::to_string(status) + "-unexpected-body");
  }
  if (status == 422 || status == 409) {
    Result result;
    result.ok = true;
    result.url = GitHubRawUrl(owner, repo, name);
    return result;
  }
  // 401/403 (bad or expired token) and every other status name only the status;
  // the response body is never echoed, so nothing it contains can leak.
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

// --- Image preparation (WIC) ------------------------------------------------

// Compression is a best-effort transfer-size reduction, never a gate: any WIC
// failure returns the original bytes untouched. The sizing decision is split
// out as a pure function so it is testable without an encoder.
//
// Discord crops the large image to a centre square, so the re-encode first
// centre-crops to the shorter side and then scales that square down to at most
// kMaxImageDimension (512). The existing skip rule still holds: an image that
// is already at or below 512 px on both sides and at most 256 KB is not touched.
struct CompressionPlan {
  bool reencode = false;
  UINT width = 0;
  UINT height = 0;
};

CompressionPlan PlanCompression(UINT width, UINT height, size_t byte_size) {
  CompressionPlan plan;
  if (width == 0 || height == 0) {
    return plan;
  }
  if (width <= kMaxImageDimension && height <= kMaxImageDimension &&
      byte_size <= kSkipReencodeMaxBytes) {
    return plan;
  }
  plan.reencode = true;
  const UINT shorter = width < height ? width : height;
  const UINT side = shorter > kMaxImageDimension ? kMaxImageDimension : shorter;
  plan.width = side;
  plan.height = side;
  return plan;
}

struct PreparedImage {
  std::vector<unsigned char> bytes;  // exactly what gets uploaded
  std::string mime;                  // announces those bytes
  std::string note;                  // one-line size/dimension record for the log
  bool reencoded = false;
};

std::string HexHResult(HRESULT hr) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(hr));
  return buffer;
}

std::string ImageNote(size_t input_bytes, size_t output_bytes, UINT source_width, UINT source_height,
                      UINT output_width, UINT output_height, const char* mode) {
  return "input=" + std::to_string(input_bytes) + " output=" + std::to_string(output_bytes) +
         " dims=" + std::to_string(source_width) + "x" + std::to_string(source_height) + "->" +
         std::to_string(output_width) + "x" + std::to_string(output_height) + " mode=" + mode;
}

PreparedImage OriginalImage(const std::vector<unsigned char>& source, const char* mime, UINT width, UINT height,
                            const std::string& wic_error) {
  PreparedImage prepared;
  prepared.bytes = source;
  prepared.mime = mime != nullptr ? std::string(mime) : std::string();
  prepared.note = ImageNote(source.size(), source.size(), width, height, width, height, "original");
  if (!wic_error.empty()) {
    prepared.note += " wic-failed=" + wic_error;
  }
  return prepared;
}

PreparedImage PrepareImageForUpload(const std::vector<unsigned char>& source, const char* mime) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool own_com = SUCCEEDED(com);
  PreparedImage prepared;
  UINT source_width = 0;
  UINT source_height = 0;
  std::string error;

  {
    // Nested scope so every COM object is released before CoUninitialize.
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICStream> input_stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) {
      hr = factory->CreateStream(&input_stream);
    }
    if (SUCCEEDED(hr)) {
      hr = input_stream->InitializeFromMemory(const_cast<BYTE*>(source.data()), static_cast<DWORD>(source.size()));
    }
    if (SUCCEEDED(hr)) {
      hr = factory->CreateDecoderFromStream(input_stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    }
    if (SUCCEEDED(hr)) {
      hr = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->GetSize(&source_width, &source_height);
    }
    if (FAILED(hr)) {
      error = "decode:" + HexHResult(hr);
    }

    const CompressionPlan plan = PlanCompression(source_width, source_height, source.size());
    if (error.empty() && !plan.reencode) {
      prepared = OriginalImage(source, mime, source_width, source_height, std::string());
    } else if (error.empty()) {
      Microsoft::WRL::ComPtr<IWICBitmapClipper> clipper;
      Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
      Microsoft::WRL::ComPtr<IStream> output_stream;
      Microsoft::WRL::ComPtr<IWICStream> wic_output;
      Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
      Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> encoder_frame;
      Microsoft::WRL::ComPtr<IPropertyBag2> properties;

      // Centre-crop to the shorter side first, so the encode is square exactly
      // as Discord frames it; the scaler then brings that square within 512 px.
      hr = factory->CreateBitmapClipper(&clipper);
      if (SUCCEEDED(hr)) {
        const UINT shorter = source_width < source_height ? source_width : source_height;
        WICRect crop = {};
        crop.X = static_cast<INT>((source_width - shorter) / 2);
        crop.Y = static_cast<INT>((source_height - shorter) / 2);
        crop.Width = static_cast<INT>(shorter);
        crop.Height = static_cast<INT>(shorter);
        hr = clipper->Initialize(frame.Get(), &crop);
      }
      if (SUCCEEDED(hr)) {
        hr = factory->CreateBitmapScaler(&scaler);
      }
      if (SUCCEEDED(hr)) {
        hr = scaler->Initialize(clipper.Get(), plan.width, plan.height, WICBitmapInterpolationModeFant);
      }
      if (SUCCEEDED(hr)) {
        hr = CreateStreamOnHGlobal(nullptr, TRUE, output_stream.GetAddressOf());
      }
      if (SUCCEEDED(hr)) {
        hr = factory->CreateStream(&wic_output);
      }
      if (SUCCEEDED(hr)) {
        hr = wic_output->InitializeFromIStream(output_stream.Get());
      }
      if (SUCCEEDED(hr)) {
        hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(wic_output.Get(), WICBitmapEncoderNoCache);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&encoder_frame, &properties);
      }
      if (SUCCEEDED(hr)) {
        wchar_t quality_name[] = L"ImageQuality";
        PROPBAG2 option = {};
        option.pstrName = quality_name;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = kJpegQuality;
        hr = properties->Write(1, &option, &value);
        VariantClear(&value);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->Initialize(properties.Get());
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->SetSize(plan.width, plan.height);
      }
      WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->SetPixelFormat(&pixel_format);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->WriteSource(scaler.Get(), nullptr);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->Commit();
      }
      if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
      }

      HGLOBAL memory = nullptr;
      const void* data = nullptr;
      SIZE_T size = 0;
      if (SUCCEEDED(hr)) {
        hr = GetHGlobalFromStream(output_stream.Get(), &memory);
      }
      if (SUCCEEDED(hr) && memory != nullptr) {
        data = GlobalLock(memory);
        size = GlobalSize(memory);
        if (data == nullptr || size == 0) {
          hr = E_FAIL;
        }
      } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
      }
      if (SUCCEEDED(hr)) {
        prepared.bytes.assign(static_cast<const unsigned char*>(data),
                              static_cast<const unsigned char*>(data) + size);
        prepared.mime = "image/jpeg";
        prepared.note = ImageNote(source.size(), prepared.bytes.size(), source_width, source_height, plan.width,
                                  plan.height, "jpeg-q85");
        prepared.reencoded = true;
      } else {
        error = "encode:" + HexHResult(hr);
      }
      if (data != nullptr) {
        GlobalUnlock(memory);
      }
    }
  }

  if (own_com) {
    CoUninitialize();
  }
  if (!error.empty()) {
    return OriginalImage(source, mime, source_width, source_height, error);
  }
  return prepared;
}

// The cache key is the hash of the bytes that are actually uploaded. Hashing
// the source instead would let two different originals that compress alike
// collide, and would hide a changed encoder behind a stale key.
std::string UploadCacheKey(const PreparedImage& image) { return Sha256Hex(image.bytes); }

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

bool QueryStatusCode(HINTERNET request, DWORD* status, DWORD* error) {
  DWORD size = sizeof(DWORD);
  *status = 0;
  *error = 0;
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, status, &size, WINHTTP_NO_HEADER_INDEX) == FALSE) {
    *error = GetLastError();
    return false;
  }
  return true;
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

// Names the exact failing call and carries GetLastError from it, so a body-read
// failure is never reported as the generic "receive-failed" again.
struct ReadOutcome {
  bool ok = false;
  const char* tag = "read-failed";
  DWORD error = 0;
};

ReadOutcome ReadResponseBody(HINTERNET request, const Deadline& deadline, std::string* out,
                             size_t max_bytes = kMaxResponseBytes) {
  out->clear();
  ReadOutcome outcome;
  for (;;) {
    if (deadline.Expired()) {
      outcome.tag = "read-timeout";
      outcome.error = ERROR_WINHTTP_TIMEOUT;  // our own budget ran out
      return outcome;
    }
    DWORD available = 0;
    if (WinHttpQueryDataAvailable(request, &available) == FALSE) {
      outcome.tag = "data-available-failed";
      outcome.error = GetLastError();
      return outcome;
    }
    if (available == 0) {
      outcome.ok = true;
      return outcome;
    }
    if (available > max_bytes) {
      available = static_cast<DWORD>(max_bytes);
    }
    const size_t offset = out->size();
    out->resize(offset + available);
    DWORD read = 0;
    if (WinHttpReadData(request, out->data() + offset, available, &read) == FALSE) {
      outcome.tag = "read-data-failed";
      outcome.error = GetLastError();
      return outcome;
    }
    out->resize(offset + read);
    if (read == 0 || out->size() >= max_bytes) {
      outcome.ok = true;
      return outcome;
    }
  }
}

// A failure counts as a transport failure only when it happened on the wire
// (connect/send/receive/timeout). Host answers - a 412, an empty body, any HTTP
// status - are real answers and are never retried.
struct UploadAttempt {
  Result result;
  bool transport_failure = false;
};

std::string TransportReason(const char* tag, const char* phase, DWORD error) {
  const char* category = error == ERROR_WINHTTP_TIMEOUT ? "timeout" : phase;
  return std::string(tag) + " phase=" + category + " winhttp=" + std::to_string(error);
}

UploadAttempt TransportFailure(const char* tag, const char* phase, DWORD error) {
  UploadAttempt attempt;
  attempt.result = Failure(TransportReason(tag, phase, error));
  attempt.transport_failure = true;
  return attempt;
}

UploadAttempt AttemptResult(const Result& result) {
  UploadAttempt attempt;
  attempt.result = result;
  return attempt;
}

UploadAttempt UploadMultipartBody(const UploadHost& upload_host, const std::string& body,
                                  const std::string& boundary) {
  if (body.size() > 0xffffffffull) {
    return AttemptResult(Failure("body-too-large"));
  }
  const Deadline deadline(kTotalTimeoutMs);
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    return TransportFailure("session-open-failed", "connect", GetLastError());
  }
  WinHttpHandle connection(WinHttpConnect(session.get(), upload_host.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    return TransportFailure("connect-failed", "connect", GetLastError());
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"POST", upload_host.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    return TransportFailure("request-open-failed", "send", GetLastError());
  }
  const std::wstring headers = L"Content-Type: multipart/form-data; boundary=" + Utils::ToWString(boundary);
  const DWORD body_size = static_cast<DWORD>(body.size());
  // totalLen == optionalLen == the exact body size, so the request carries a
  // Content-Length and never falls back to chunked transfer encoding.
  if (WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                         body_size, body_size, 0) == FALSE) {
    return TransportFailure("send-failed", "send", GetLastError());
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return TransportFailure("receive-failed", "receive", GetLastError());
  }
  DWORD status = 0;
  DWORD status_error = 0;
  if (!QueryStatusCode(request.get(), &status, &status_error)) {
    // The host already answered; a retry would upload a second copy.
    return AttemptResult(Failure("status-query-failed phase=receive winhttp=" + std::to_string(status_error)));
  }
  std::string response;
  const ReadOutcome read = ReadResponseBody(request.get(), deadline, &response);
  if (!read.ok) {
    if (status == 200 || status == 412) {
      return AttemptResult(ClassifyUploadResponse(status, response, upload_host.shape));
    }
    return TransportFailure(read.tag, "receive", read.error);
  }
  return AttemptResult(ClassifyUploadResponse(status, response, upload_host.shape));
}

// Sends the GitHub contents PUT. The token is read from the guarded config and
// placed only in this request's Authorization header; the request and its
// headers are stack-local and are never logged. The success response is parsed
// for content.download_url; 422/409 is the immutable file already being there.
UploadAttempt UploadGitHubBody(const std::wstring& owner, const std::wstring& repo,
                               const std::string& name,
                               const std::vector<unsigned char>& image) {
  std::string token;
  {
    std::lock_guard<std::mutex> lock(g_host_mutex);
    token = g_github_token;
  }
  if (token.empty()) {
    return AttemptResult(Failure("github-no-token"));
  }
  const GitHubRequest github = BuildGitHubRequest(token, owner, repo, name, image);
  if (github.body.size() > 0xffffffffull) {
    return AttemptResult(Failure("body-too-large"));
  }
  const Deadline deadline(kTotalTimeoutMs);
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    return TransportFailure("session-open-failed", "connect", GetLastError());
  }
  WinHttpHandle connection(
      WinHttpConnect(session.get(), github.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    return TransportFailure("connect-failed", "connect", GetLastError());
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), github.method.c_str(),
                                           github.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    return TransportFailure("request-open-failed", "send", GetLastError());
  }
  const DWORD body_size = static_cast<DWORD>(github.body.size());
  if (WinHttpSendRequest(request.get(), github.headers.c_str(), static_cast<DWORD>(-1),
                         const_cast<char*>(github.body.data()), body_size, body_size, 0) == FALSE) {
    return TransportFailure("send-failed", "send", GetLastError());
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return TransportFailure("receive-failed", "receive", GetLastError());
  }
  DWORD status = 0;
  DWORD status_error = 0;
  if (!QueryStatusCode(request.get(), &status, &status_error)) {
    return AttemptResult(Failure("status-query-failed phase=receive winhttp=" +
                                 std::to_string(status_error)));
  }
  std::string response;
  const ReadOutcome read =
      ReadResponseBody(request.get(), deadline, &response, kMaxGitHubResponseBytes);
  if (!read.ok) {
    if (status == 200 || status == 201 || status == 422 || status == 409 || status == 401 ||
        status == 403) {
      return AttemptResult(ClassifyGitHubResponse(status, response, owner, repo, name));
    }
    return TransportFailure(read.tag, "receive", read.error);
  }
  return AttemptResult(ClassifyGitHubResponse(status, response, owner, repo, name));
}

// Dispatches one attempt to the transport that matches the host's shape. The
// GitHub host needs the owner/repo (from the guarded config) and the cover name;
// the keyless hosts build their multipart body here.
UploadAttempt AttemptHostUpload(const UploadHost& upload_host, const PreparedImage& prepared) {
  if (upload_host.shape == UploadShape::kGitHub) {
    std::wstring owner;
    std::wstring repo;
    {
      std::lock_guard<std::mutex> lock(g_host_mutex);
      owner = g_github_owner;
      repo = g_github_repo;
    }
    return UploadGitHubBody(owner, repo, UploadCacheKey(prepared), prepared.bytes);
  }
  const std::string content_type = SanitizeMime(prepared.mime.c_str());
  const std::string boundary = MakeBoundary();
  const std::string body = BuildMultipartBody(boundary, content_type, prepared.bytes, upload_host.shape);
  return UploadMultipartBody(upload_host, body, boundary);
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

struct HealthCheck {
  bool ok = false;
  std::string reason;
};

// Proves the upload is actually retrievable: a bare GET (no added headers)
// must answer 200 with an image/* content type.
HealthCheck HealthCheckUrl(const std::string& url) {
  HealthCheck check;
  check.reason = "health-check-failed";
  const Deadline deadline(kTotalTimeoutMs);
  std::wstring host;
  std::wstring path;
  if (!CrackUrl(url, &host, &path)) {
    check.reason = "health-check-crack-url-failed";
    return check;
  }
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    check.reason = TransportReason("health-check-session-open-failed", "connect", GetLastError());
    return check;
  }
  WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    check.reason = TransportReason("health-check-connect-failed", "connect", GetLastError());
    return check;
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    check.reason = TransportReason("health-check-request-open-failed", "send", GetLastError());
    return check;
  }
  if (WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE) {
    check.reason = TransportReason("health-check-send-failed", "send", GetLastError());
    return check;
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    check.reason = TransportReason("health-check-receive-failed", "receive", GetLastError());
    return check;
  }
  DWORD status = 0;
  DWORD status_error = 0;
  if (!QueryStatusCode(request.get(), &status, &status_error)) {
    check.reason = "health-check-status-query-failed phase=receive winhttp=" + std::to_string(status_error);
    return check;
  }
  if (status != 200) {
    check.reason = "health-check-status=" + std::to_string(status);
    return check;
  }
  std::wstring content_type;
  if (!QueryContentType(request.get(), &content_type)) {
    check.reason = "health-check-content-type-missing";
    return check;
  }
  if (!StartsWithImageType(content_type)) {
    check.reason = "health-check-content-type=" + Utils::ToString(content_type);
    return check;
  }
  check.ok = true;
  check.reason.clear();
  return check;
}

// Tries one host: builds that host's own multipart shape, uploads, retries once
// on a transport failure only, and - on success - proves the returned URL is
// retrievable. Records the attempt count and a log marker naming the host. A
// host answer (403/412/empty body) is final for this host; the caller moves on.
struct HostOutcome {
  Result result;
  int attempts = 0;
};

HostOutcome TryUploadHost(const UploadHost& upload_host, const PreparedImage& prepared) {
  HostOutcome outcome;
  UploadAttempt attempt = AttemptHostUpload(upload_host, prepared);
  outcome.attempts = 1;
  if (attempt.transport_failure) {
    Sleep(kRetryBackoffMs);
    attempt = AttemptHostUpload(upload_host, prepared);
    outcome.attempts = 2;
  }
  outcome.result = attempt.result;
  if (outcome.result.ok) {
    const HealthCheck health = HealthCheckUrl(outcome.result.url);
    if (!health.ok) {
      outcome.result = Failure(health.reason);
    }
  }
  outcome.result.host = HostNameUtf8(upload_host);
  outcome.result.attempts = outcome.attempts;
  return outcome;
}

// Uploads to the permanent GitHub host only: the upgrade path for a cache hit
// on an ephemeral keyless URL. The caller falls back to revalidation when this
// fails, so a dead ephemeral URL is never sent. Answers github-not-configured
// without touching the network when the host is not active.
HostOutcome TryGitHubUpload(const PreparedImage& prepared) {
  const std::vector<UploadHost> hosts = ActiveHosts();
  for (size_t i = 0; i < hosts.size(); ++i) {
    if (hosts[i].shape == UploadShape::kGitHub) {
      return TryUploadHost(hosts[i], prepared);
    }
  }
  HostOutcome outcome;
  outcome.result = Failure("github-not-configured");
  return outcome;
}

// Every configured host, in order, in one comma-separated token for the failure
// marker: "github.com,catbox.moe,uguu.se,litter.catbox.moe".
std::string HostListToken() {
  const std::vector<UploadHost> hosts = ActiveHosts();
  std::string token;
  for (size_t i = 0; i < hosts.size(); ++i) {
    if (i != 0) {
      token += ",";
    }
    token += Utils::ToString(hosts[i].name);
  }
  return token;
}

// The publisher's diagnostic sink; never image bytes. One small line per publish
// so a host choice - and the fall-through to online - is greppable in DebugLog.
void (*g_logger)(const std::string&) = nullptr;

void EmitLog(const std::string& line) {
  if (g_logger != nullptr) {
    g_logger(line);
  }
}

// Names the GitHub host's authentication state, never the token: a configured
// token logs "auth=present", an empty one "auth=absent". A 401/403 from GitHub
// logs "auth=rejected" with the status only.
void LogGitHubAuthState() {
  EmitLog(std::string("local-cover github auth=") + (GitHubTokenPresent() ? "present" : "absent"));
}

void LogGitHubAuthRejected(const std::string& status) {
  EmitLog("local-cover github auth=rejected status=" + status);
}

void LogHostSuccess(const Result& result) {
  EmitLog("local-cover published host=" + result.host + " url=" + result.url);
}

void LogHostFailure(const std::string& reason, const std::string& host_list, int attempts,
                    const PreparedImage& prepared) {
  EmitLog("local-cover failed host=" + host_list + " reason=" +
          (reason.empty() ? std::string("unknown") : reason) + " attempts=" +
          std::to_string(attempts) + " detail=" + prepared.note);
}

// The publisher is healthy when at least one configured host is reachable - a
// single dead host is never the whole publisher's verdict. Pure, so the rule is
// testable without a network: `reachable` is parallel to the configured hosts.
bool AnyHostHealthy(const bool* reachable, size_t count) {
  if (reachable == nullptr || count == 0) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (reachable[i]) {
      return true;
    }
  }
  return false;
}

size_t ConfiguredHostCount() { return ActiveHosts().size(); }

std::string ConfiguredHostName(size_t index) {
  const std::vector<UploadHost> hosts = ActiveHosts();
  return index < hosts.size() ? Utils::ToString(hosts[index].name) : std::string();
}

}  // namespace

// --- Public interface -------------------------------------------------------

namespace CoverPublisher {

void SetLogger(void (*logger)(const std::string& line)) {
  g_logger = logger;
}

void Configure(const std::string& cache_path) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_cache_path = cache_path;
  g_cache.clear();
  if (!cache_path.empty()) {
    LoadCacheLocked();
  }
}

void ConfigureAuth(const std::string& token, const std::string& repo) {
  std::wstring owner;
  std::wstring name;
  const std::wstring configured = repo.empty() ? std::wstring(kDefaultGitHubRepo) : Utils::ToWString(repo);
  ParseOwnerRepo(configured, &owner, &name);
  std::lock_guard<std::mutex> lock(g_host_mutex);
  g_github_token = token;
  g_github_owner = owner;
  g_github_repo = name;
}

Result Publish(const std::vector<unsigned char>& image_bytes, const char* mime) {
  try {
    if (image_bytes.empty()) {
      return Failure("empty-image");
    }
    const PreparedImage prepared = PrepareImageForUpload(image_bytes, mime);
    // The key covers exactly the bytes that go on the wire (compressed bytes
    // when compression happened), so the cache can never answer for content the
    // host has not actually seen.
    const std::string key = UploadCacheKey(prepared);
    const long long now = static_cast<long long>(std::time(nullptr));
    Result cached;
    if (CacheLookup(key, now, &cached)) {
      if (!cached.ok) {
        cached.detail = prepared.note + " key=" + key + " cache=hit";
        return cached;
      }
      // A hit on an ephemeral keyless URL is never trusted blindly: with a
      // token the local bytes are re-published to the permanent GitHub host
      // and the cache is updated; without one the stored URL is revalidated
      // and a dead entry falls through to a fresh keyless upload below, which
      // overwrites it. Either way a dead cached URL is never sent.
      const CachedUrlAction action = PlanCachedUrl(cached.url, GitHubUploadActive());
      if (action == CachedUrlAction::kUse) {
        cached.detail = prepared.note + " key=" + key + " cache=hit";
        return cached;
      }
      if (action == CachedUrlAction::kUpgradeToGitHub) {
        const HostOutcome upgrade = TryGitHubUpload(prepared);
        if (upgrade.result.ok) {
          Result result = upgrade.result;
          LogHostSuccess(result);
          result.detail = prepared.note + " key=" + key +
                          " cache=upgraded-from-ephemeral attempts=" +
                          std::to_string(upgrade.attempts) + " host=" + result.host;
          CacheStore(key, result, now);
          return result;
        }
        EmitLog("local-cover upgrade-failed key=" + key + " reason=" +
                upgrade.result.reason + " host=" + upgrade.result.host);
        // Fall through to revalidation: a failed upgrade must not resurrect a
        // possibly-dead ephemeral URL.
      }
      const HealthCheck health = HealthCheckUrl(cached.url);
      if (health.ok) {
        cached.detail = prepared.note + " key=" + key + " cache=hit-revalidated";
        return cached;
      }
      EmitLog("local-cover cache-stale key=" + key + " reason=" + health.reason);
    }

    // Ordered fallback inside one publish: GitHub first when a token is set
    // (the owner's permanent repo), then catbox, then uguu, then the dormant
    // litterbox. All are the same layer - a host failure never skips the online
    // rung. Each host is retried once on a transport failure only.
    LogGitHubAuthState();
    const std::vector<UploadHost> hosts = ActiveHosts();
    Result result;
    int total_attempts = 0;
    for (size_t i = 0; i < hosts.size(); ++i) {
      const HostOutcome outcome = TryUploadHost(hosts[i], prepared);
      total_attempts += outcome.attempts;
      if (!outcome.result.ok && hosts[i].shape == UploadShape::kGitHub) {
        const std::string& reason = outcome.result.reason;
        if (reason == "http-401" || reason == "http-403") {
          LogGitHubAuthRejected(reason.substr(5));
        }
      }
      if (outcome.result.ok) {
        result = outcome.result;
        break;
      }
      result = outcome.result;  // last failure names the final host
    }
    if (result.ok) {
      LogHostSuccess(result);
    } else {
      LogHostFailure(result.reason, HostListToken(), total_attempts, prepared);
    }
    result.detail = prepared.note + " key=" + key + " attempts=" + std::to_string(total_attempts) +
                    " host=" + (result.host.empty() ? HostListToken() : result.host);
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
