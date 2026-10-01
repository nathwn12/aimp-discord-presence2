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
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//  DEALINGS IN THE SOFTWARE.

#include "local_art.h"

#include <bcrypt.h>

#include <cstddef>

// SHA-256 comes from CNG; linking it here keeps the extraction leg
// self-contained.
#pragma comment(lib, "bcrypt.lib")

namespace {

// The receive callback writes into this stack-owned context. WAITFOR makes
// AIMP deliver the callback before IAIMPServiceAlbumArt::Get2 returns.
struct ReceiveContext {
  LocalArt::Result result;
  bool want_bytes = false;
};

std::string ToHex(const unsigned char* bytes, size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    hex.push_back(kDigits[bytes[i] >> 4]);
    hex.push_back(kDigits[bytes[i] & 0x0F]);
  }
  return hex;
}

std::string DetectFormat(const byte* data, DWORD size, int aimp_format) {
  if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E &&
      data[3] == 0x47 && data[4] == 0x0D && data[5] == 0x0A &&
      data[6] == 0x1A && data[7] == 0x0A) {
    return "PNG";
  }
  if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
    return "JPEG";
  }
  if (size >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
    return "GIF";
  }
  if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
    return "BMP";
  }

  switch (aimp_format) {
    case AIMP_IMAGE_FORMAT_PNG:
      return "PNG";
    case AIMP_IMAGE_FORMAT_JPG:
      return "JPEG";
    case AIMP_IMAGE_FORMAT_GIF:
      return "GIF";
    case AIMP_IMAGE_FORMAT_BMP:
      return "BMP";
    default:
      return "other";
  }
}

bool Sha256(const byte* data, DWORD size, std::string* hex) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
    return false;
  }

  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(data), size, 0) >= 0;

  unsigned char digest[32] = {};
  if (ok) {
    ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
  }

  if (hash != nullptr) {
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);

  if (ok) {
    *hex = ToHex(digest, sizeof(digest));
  }
  return ok;
}

void CALLBACK OnReceive(IAIMPImage* /*image*/, IAIMPImageContainer* container,
                        void* user_data) {
  auto* context = static_cast<ReceiveContext*>(user_data);
  if (container == nullptr) {
    return;
  }

  byte* data = container->GetData();
  const DWORD size = container->GetDataSize();
  if (data == nullptr || size == 0) {
    return;
  }

  SIZE dimensions = {};
  int aimp_format = AIMP_IMAGE_FORMAT_UNKNOWN;
  container->GetInfo(&dimensions, &aimp_format);

  std::string sha256_hex;
  if (!Sha256(data, size, &sha256_hex)) {
    return;
  }

  context->result.found = true;
  context->result.size = size;
  context->result.width = dimensions.cx;
  context->result.height = dimensions.cy;
  context->result.aimp_format = aimp_format;
  context->result.format = DetectFormat(data, size, aimp_format);
  context->result.sha256_hex = sha256_hex;
  if (context->want_bytes) {
    context->result.bytes.assign(data, data + size);
  }
}

}  // namespace

namespace LocalArt {

Result Extract(IAIMPServiceAlbumArt* service,
               IAIMPFileInfo* file_info,
               bool want_bytes) {
  Result result;
  if (service == nullptr || file_info == nullptr) {
    return result;
  }

  ReceiveContext context;
  context.want_bytes = want_bytes;
  void* task_id = nullptr;

  // OFFLINE disables Internet providers; ORIGINAL suppresses AIMP's display
  // downscaling so the container holds the bytes exactly as stored; NOCACHE
  // keeps a previously downloaded cover out of this answer; WAITFOR delivers
  // the callback before return, keeping the context stack-owned.
  const DWORD flags = AIMP_SERVICE_ALBUMART_FLAGS_WAITFOR |
                      AIMP_SERVICE_ALBUMART_FLAGS_OFFLINE |
                      AIMP_SERVICE_ALBUMART_FLAGS_ORIGINAL |
                      AIMP_SERVICE_ALBUMART_FLAGS_NOCACHE;

  const HRESULT hr = service->Get2(file_info, flags, &OnReceive, &context, &task_id);
  if (FAILED(hr)) {
    return result;
  }
  return context.result;
}

}  // namespace LocalArt
