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

#ifndef AIMPDISCORDPRESENCE_SRC_LOCAL_ART_H_
#define AIMPDISCORDPRESENCE_SRC_LOCAL_ART_H_

#include <windows.h>
#include <unknwn.h>

#include <string>

#include "apiAlbumArt.h"
#include "apiFileManager.h"

// Extraction leg of the local-album-art feature: asks AIMP's album art
// service for the artwork of one file, restricted to local providers (file
// tags and sidecar files), and fingerprints the raw image container bytes.
// The bytes are never decoded, resized, re-encoded or uploaded.
namespace LocalArt {

struct Result {
  bool found = false;
  DWORD size = 0;
  long width = 0;   // container-reported pixel dimensions
  long height = 0;
  int aimp_format = AIMP_IMAGE_FORMAT_UNKNOWN;  // from Container.GetInfo
  std::string format;      // "PNG", "JPEG" or "other", sniffed from magic bytes
  std::string sha256_hex;  // lowercase hex, empty when not found
};

// Blocking, AIMP main thread only. Runs an offline-only album art request for
// `file_info` and returns the fingerprint of the delivered container. The
// AIMP receive callback is invoked on the calling thread before this returns,
// so no state outlives the call. `service` stays owned by the caller.
Result Extract(IAIMPServiceAlbumArt* service, IAIMPFileInfo* file_info);

}  // namespace LocalArt

#endif  // AIMPDISCORDPRESENCE_SRC_LOCAL_ART_H_
