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
constexpr DWORD kTotalTimeoutMs = 20000;
constexpr DWORD kRetryBackoffMs = 500;
constexpr DWORD kMaxResponseBytes = 64 * 1024;
constexpr size_t kMaxCacheBytes = 1024 * 1024;
constexpr long long kNegativeCacheTtlSeconds = 60;
// 29 bytes keep the closing delimiter ("\r\n--<boundary>--\r\n") at 37 bytes,
// matching the live measurement of the host's request framing.
constexpr size_t kBoundaryLength = 29;

// Cover transfer shaping: the longest side is scaled down to at most 600 px and
// the result is encoded as JPEG q85, unless the source is already both at or
// below 600 px and at most 256 KB, in which case it goes up untouched.
constexpr UINT kMaxImageDimension = 600;
constexpr size_t kSkipReencodeMaxBytes = 256 * 1024;
constexpr float kJpegQuality = 0.85f;

constexpr wchar_t kAgentName[] = L"AIMP-Discord-Presence/2.0";

// Upload hosts are tried in order until one returns a usable URL, so a host
// that is down or blocked for this network (litterbox now answers HTTP 403 to a
// BunkerWeb WAF) no longer costs the cover. litterbox stays first because it
// may work again on other networks/regions; uguu.se is the keyless fallback
// measured working (POST /upload?output=text with a `files[]` field, responding
// with the bare URL as text). Each host owns both its endpoint and the shape of
// the multipart body it accepts.
enum class UploadShape {
  // litterbox: POST /resources/internals/api.php with reqtype=fileupload,
  // time=72h and the image in `fileToUpload`.
  kLitterbox,
  // uguu: POST /upload?output=text with no extra fields and the image in
  // `files[]`. The response body is the bare URL as text.
  kUguu,
};

struct UploadHost {
  const wchar_t* name;   // log marker, e.g. "litter.catbox.moe"
  const wchar_t* host;   // for WinHttpConnect
  const wchar_t* path;   // request target, query string included
  UploadShape shape;
};

// The log marker as UTF-8, so it can be concatenated into a std::string line.
std::string HostNameUtf8(const UploadHost& upload_host) {
  if (upload_host.name == nullptr) {
    return std::string();
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, upload_host.name, -1, nullptr, 0, nullptr,
                                         nullptr);
  if (needed <= 1) {
    return std::string();
  }
  std::string out(static_cast<size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, upload_host.name, -1, out.data(), needed, nullptr, nullptr);
  return out;
}

constexpr size_t kUploadHostCount = 2;
constexpr UploadHost kUploadHosts[kUploadHostCount] = {
    {L"litter.catbox.moe", L"litterbox.catbox.moe", L"/resources/internals/api.php", UploadShape::kLitterbox},
    {L"uguu.se", L"uguu.se", L"/upload?output=text", UploadShape::kUguu},
};

// The log markers as narrow strings, index-parallel to kUploadHosts, so a
// caller can name a host without a UTF-8 conversion at each use.
constexpr const char* kUploadHostNames[kUploadHostCount] = {"litter.catbox.moe", "uguu.se"};

// Each host owns the exact URL shape it answers with, so a lookalike host
// ("...moe.evil.test") or a body that is not that host's URL is rejected. The
// success response is a bare https URL matching the host that answered; a WAF
// or JSON error page, or HTTP 200 with an empty body, is a failure.
// uguu round-robins across single-label subdomains (n., h., ...), so the
// subdomain is matched generally rather than pinned to one letter - pinning it
// rejected real responses as "unexpected body".
constexpr char kLitterboxUrlPattern[] = R"(^https://litter\.catbox\.moe/[a-z0-9]{6}\.(png|jpg|jpeg)$)";
constexpr char kUguuUrlPattern[] = R"(^https://([A-Za-z0-9-]+\.)?uguu\.se/[A-Za-z0-9]+\.(png|jpg|jpeg|gif|webp|bmp)$)";
// Fallback when no host is specified (kept for callers that classify a bare
// response): the strict litterbox shape.
std::string UploadUrlPatternFor(const UploadHost& upload_host) {
  return upload_host.shape == UploadShape::kUguu ? std::string(kUguuUrlPattern)
                                                 : std::string(kLitterboxUrlPattern);
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
  return std::regex_match(text, litterbox_pattern) || std::regex_match(text, uguu_pattern);
}

// The per-host predicate used by the classifier: a 200 is a success only when
// the body is the bare URL of the host that answered.
bool LooksLikeHostUrl(const std::string& text, const UploadHost& upload_host) {
  const char* url_pattern =
      upload_host.shape == UploadShape::kUguu ? kUguuUrlPattern : kLitterboxUrlPattern;
  static const std::map<std::string, std::regex> patterns = {
      {kLitterboxUrlPattern, std::regex(kLitterboxUrlPattern)},
      {kUguuUrlPattern, std::regex(kUguuUrlPattern)},
  };
  const auto found = patterns.find(url_pattern);
  return found != patterns.end() && std::regex_match(text, found->second);
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
  const UINT longest = width > height ? width : height;
  if (longest <= kMaxImageDimension && byte_size <= kSkipReencodeMaxBytes) {
    return plan;
  }
  plan.reencode = true;
  if (longest <= kMaxImageDimension) {
    plan.width = width;
    plan.height = height;
    return plan;
  }
  const double scale = static_cast<double>(kMaxImageDimension) / static_cast<double>(longest);
  const double scaled_width = std::round(static_cast<double>(width) * scale);
  const double scaled_height = std::round(static_cast<double>(height) * scale);
  plan.width = static_cast<UINT>(scaled_width < 1.0 ? 1.0 : scaled_width);
  plan.height = static_cast<UINT>(scaled_height < 1.0 ? 1.0 : scaled_height);
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
      Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
      Microsoft::WRL::ComPtr<IStream> output_stream;
      Microsoft::WRL::ComPtr<IWICStream> wic_output;
      Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
      Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> encoder_frame;
      Microsoft::WRL::ComPtr<IPropertyBag2> properties;

      hr = factory->CreateBitmapScaler(&scaler);
      if (SUCCEEDED(hr)) {
        hr = scaler->Initialize(frame.Get(), plan.width, plan.height, WICBitmapInterpolationModeFant);
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

ReadOutcome ReadResponseBody(HINTERNET request, const Deadline& deadline, std::string* out) {
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
    if (available > kMaxResponseBytes) {
      available = kMaxResponseBytes;
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
    if (read == 0 || out->size() >= kMaxResponseBytes) {
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
  WinHttpHandle connection(WinHttpConnect(session.get(), upload_host.host, INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    return TransportFailure("connect-failed", "connect", GetLastError());
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"POST", upload_host.path, nullptr, WINHTTP_NO_REFERER,
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
  const std::string content_type = SanitizeMime(prepared.mime.c_str());
  const std::string boundary = MakeBoundary();
  const std::string body = BuildMultipartBody(boundary, content_type, prepared.bytes, upload_host.shape);
  UploadAttempt attempt = UploadMultipartBody(upload_host, body, boundary);
  outcome.attempts = 1;
  if (attempt.transport_failure) {
    Sleep(kRetryBackoffMs);
    attempt = UploadMultipartBody(upload_host, body, boundary);
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

// Every configured host, in order, in one comma-separated token for the failure
// marker: "litter.catbox.moe,uguu.se".
std::string HostListToken() {
  std::string token;
  for (size_t i = 0; i < kUploadHostCount; ++i) {
    if (i != 0) {
      token += ",";
    }
    token += Utils::ToString(kUploadHosts[i].name);
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

constexpr size_t ConfiguredHostCount() { return kUploadHostCount; }

const char* ConfiguredHostName(size_t index) {
  return index < kUploadHostCount ? kUploadHostNames[index] : "";
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
      cached.detail = prepared.note + " key=" + key + " cache=hit";
      return cached;
    }

    // Ordered fallback inside one publish: try litterbox, then uguu. Both are
    // the same layer - a host failure never skips the online rung. Each host
    // builds its own multipart shape and is retried once on a transport failure
    // only.
    Result result;
    int total_attempts = 0;
    for (size_t i = 0; i < kUploadHostCount; ++i) {
      const HostOutcome outcome = TryUploadHost(kUploadHosts[i], prepared);
      total_attempts += outcome.attempts;
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
