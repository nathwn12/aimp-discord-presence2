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

#ifndef AIMPDISCORDPRESENCE_SRC_ALBUM_ART_H_
#define AIMPDISCORDPRESENCE_SRC_ALBUM_ART_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// Resolves album art URLs from keyless public APIs (Deezer first, iTunes as a
// fallback) on a background worker so AIMP's message thread is never blocked
// on the network.
//
// The resolved URL is handed to a callback that runs on the worker thread; the
// callback must not call AIMP services, it should only publish the value to
// the plugin which applies it on the next player notification.
namespace AlbumArt {

// Discord accepts external asset URLs up to 300 characters.
constexpr size_t kMaxUrlLength = 300;

// Public image endpoints used by Discord.
constexpr const char* kDefaultAssetKey = "aimp";

enum class Provider {
  kNone = 0,
  kDeezer,
  kItunes,
};

// --- Pure helpers, unit tested separately ---------------------------------

// Builds the per-album identity key (artist + album) used both as the resolver
// cache key and to match a resolved artwork URL with the track currently
// playing. It deliberately ignores the track title: artwork is per-album, and
// one shared builder keeps the resolver and the plugin from drifting apart.
std::string BuildAlbumKey(const std::string& artist, const std::string& album);

// Percent-encodes a UTF-8 string for use as a query parameter value.
std::string UriEncodeUtf8(const std::string& utf8);

// Returns the value of the first `"key":"value"` pair in a JSON document,
// with JSON escapes decoded. Returns an empty string when the key is absent.
std::string ExtractJsonStringField(const std::string& json, const std::string& key);

// Rewrites an iTunes artwork URL (`.../100x100bb.jpg`) to its 600x600 variant,
// which is the largest size iTunes serves for every artwork.
std::string UpscaleItunesArtwork(const std::string& url);

std::string BuildDeezerSearchUrl(const std::string& artist, const std::string& album);
std::string BuildItunesSearchUrl(const std::string& artist, const std::string& album);

// Extracts the artwork URL for a provider from a search response. Returns an
// empty string when no usable (https, <= 300 characters) URL is present.
std::string ExtractArtworkUrl(const std::string& body, Provider provider);

// --- Background resolver ---------------------------------------------------

class Resolver {
 public:
  using Callback = std::function<void(const std::string& artist,
                                      const std::string& album,
                                      const std::string& url)>;

  Resolver();
  ~Resolver();

  Resolver(const Resolver&) = delete;
  Resolver& operator=(const Resolver&) = delete;

  // When disabled no network request is ever made; requests resolve to "".
  void SetOnlineEnabled(bool enabled);

  void SetCallback(Callback callback);

  // Queues a lookup for `artist` + `album` (UTF-8). Replaces any pending
  // request; stale completions are dropped.
  void Request(const std::string& artist, const std::string& album);

  // Blocking lookup (network + cache). Exposed for the worker and tests.
  std::string Lookup(const std::string& artist, const std::string& album);

  void Shutdown();

 private:
  void WorkerMain();
  std::string LookupUncached(const std::string& artist, const std::string& album,
                             bool* definitive);
  bool TakeCached(const std::string& key, std::string* url);
  void StoreCached(const std::string& key, const std::string& url);

  std::thread worker_;

  std::mutex mutex_;
  std::condition_variable wake_;
  // Set by Shutdown() and polled by the worker between provider requests so an
  // in-flight resolve aborts promptly instead of running the full timeout
  // budget while AIMP unloads.
  std::atomic<bool> stopping_{false};
  bool online_ = true;

  bool has_request_ = false;
  uint64_t generation_ = 0;
  std::string request_artist_;
  std::string request_album_;

  Callback callback_;

  std::unordered_map<std::string, std::string> cache_;
  std::deque<std::string> cache_order_;

  static constexpr size_t kMaxCacheEntries = 200;
};

}  // namespace AlbumArt

#endif  // AIMPDISCORDPRESENCE_SRC_ALBUM_ART_H_
