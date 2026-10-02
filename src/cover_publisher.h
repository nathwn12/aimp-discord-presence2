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

#ifndef AIMPDISCORDPRESENCE_SRC_COVER_PUBLISHER_H_
#define AIMPDISCORDPRESENCE_SRC_COVER_PUBLISHER_H_

#include <string>
#include <vector>

// Anonymous cover hosting for the presence's cover art.
//
// Publish() prepares the image (downscale to at most 600 px on the longest side
// and JPEG re-encode whenever that shrinks the transfer), uploads it to
// litterbox, then proves the upload is actually retrievable (a bare GET of the
// returned URL must answer 200 with an image/* content type) before reporting
// success. Distinct images are uploaded once: the returned URL is cached on
// disk, keyed by the SHA-256 of the bytes that were uploaded. A transport
// failure is retried once after a short pause; host answers (a 412, an empty
// body) are never retried. Failures are cached briefly too, so a host outage is
// not hammered from the player's callback.
//
// Every call blocks; each HTTP operation gets a ~20 second budget, so an upload
// attempt plus its retry plus the health check can take longer than that. The
// module never logs, prints, or persists image bytes - only sizes and hashes.
// The on-disk cache is best effort: an unwritable path degrades to no cache.

namespace CoverPublisher {

struct Result {
  bool ok = false;
  std::string url;
  std::string reason;
  // The host that produced this outcome: the winner on success, the last host
  // tried on failure (UTF-8, e.g. "litter.catbox.moe" or "uguu.se").
  std::string host;
  // Total upload attempts spent across every host tried for this request.
  int attempts = 0;
  // One-line size/dimension record for the caller's log (never image bytes),
  // populated on every outcome including cache hits.
  std::string detail;
};

// Routes the publisher's own diagnostic lines (host success / all-hosts-failed)
// to a sink; when unset they are dropped. Distinct from Result.detail, which is
// per-outcome, and invoked once per publish attempt on the calling thread.
void SetLogger(void (*logger)(const std::string& line));

// Names the cache file (UTF-8). An empty path disables the cache entirely.
// Reloads whatever the file already holds.
void Configure(const std::string& cache_path);

// Uploads image_bytes (mime names their type, e.g. "image/png") and returns the
// public URL on success. On failure reason names the cause: "empty-body" when
// the host answered HTTP 200 with an empty body, "http-<code>" for statuses
// without a useful body, the response body text for 412, and phase-tagged
// transport failures ("receive-failed phase=receive winhttp=12002",
// "health-check-...") otherwise.
Result Publish(const std::vector<unsigned char>& image_bytes, const char* mime);

// Forgets every cached result and deletes the cache file (best effort).
void ClearCache();

}  // namespace CoverPublisher

#endif  // AIMPDISCORDPRESENCE_SRC_COVER_PUBLISHER_H_
