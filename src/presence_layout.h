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

#ifndef AIMPDISCORDPRESENCE_SRC_PRESENCE_LAYOUT_H_
#define AIMPDISCORDPRESENCE_SRC_PRESENCE_LAYOUT_H_

#include <string>

// The Discord presence field mapping, kept in one place (Spotify parity):
//
//   details    (line 2)  the track title; falls back to the artist, then to
//                        the literal "AIMP", so the line is never blank
//   state      (line 3)  the artist; falls back to the album, and is
//                        omitted when that would repeat `details`
//   large_image          the resolved album art URL; empty (omitted) when no
//                        art was resolved, so Discord renders its native
//                        placeholder (never an asset key, never a
//                        stand-in image URL)
//
// Spotify parity is two text lines only: there is no album third line, so no
// `large_text` is ever emitted; the cover art image is kept via `large_image`
// alone. `TextFields::large_text` is therefore always empty (retained only so
// existing initializers keep compiling).
//
// No field repeats a value already emitted by an earlier one; empty values
// count as absent.
//
// With status_display_type = 2 (Details), Discord renders `details` - the
// track title - in the member-list status line.
namespace PresenceLayout {

// There is no stand-in image: when no album art URL was resolved the field
// stays empty and is omitted from the wire JSON (AppendImageField skips
// empties), so the card shows Discord's native placeholder. A bundled asset
// key such as `aimp` must never be used either: it belongs to a Discord
// application this plugin does NOT own, so it would resolve only while that
// foreign application happens to serve it.

// Literal shown on line 2 when both the artist and track title tags are empty.
constexpr const char* kFallbackDetails = "AIMP";

struct TextFields {
  std::string details;     // title, else artist, else kFallbackDetails
  std::string state;       // artist, else album; empty omits the field
  std::string large_text;  // always empty: no album third line (Spotify parity)
};

// Title -> artist, de-duplicated (Spotify parity: the track title is the
// prominent line, the artist is second). `details` is never empty; `state`
// keeps the artist when the tag exists, falls back to the album only when the
// artist is empty or would repeat `details`, and is omitted when it would
// repeat `details` - so line 2 always reads as the artist/band when known,
// falls back to the album for untagged tracks, and never duplicates line 1.
// `large_text` is always cleared: the card is two text lines plus the cover
// image, with no album tooltip line. Truncation stays downstream
// (NormalizeTextField, 128 codepoints), so this builder does no cutting
// itself.
inline TextFields BuildTextFields(const std::string& artist,
                                  const std::string& album,
                                  const std::string& title) {
  TextFields fields;
  fields.details = !title.empty() ? title
                                  : (!artist.empty() ? artist : kFallbackDetails);

  fields.state = artist;
  if (fields.state.empty() || fields.state == fields.details) {
    fields.state = album;
  }
  if (fields.state.empty() || fields.state == fields.details) {
    fields.state.clear();
  }

  fields.large_text.clear();

  return fields;
}

// Whether the keyless online chain should be asked for the current track: both
// artwork settings are on and there is an artist to search with. The album tag
// is deliberately not part of this decision - an album-less track has a
// file-path identity, so an empty album must not suppress the lookup.
inline bool ShouldRequestOnlineArtwork(bool use_albumart, bool use_online,
                                       const std::string& artist,
                                       const std::string& album) {
  (void)album;
  return use_albumart && use_online && !artist.empty();
}

// The large_image for `album_key` from the resolved sources: a published local
// cover wins over the online chain's URL; when neither resolved the field is
// empty (omitted on the wire). Each source is adopted only when its own key
// equals `album_key`, so a value resolved for one track identity can never be
// shown for another. `album_key` is the only identity this needs, so an
// album-less track resolves exactly like an albumed one. `source` names the
// winner for the DebugLog when non-null.
inline std::string ResolveLargeImage(const std::string& album_key,
                                     const std::string& local_cover_key,
                                     const std::string& local_cover_url,
                                     const std::string& online_key,
                                     const std::string& online_url,
                                     std::string* source = nullptr) {
  if (!local_cover_url.empty() && local_cover_key == album_key) {
    if (source != nullptr) {
      *source = "local";
    }
    return local_cover_url;
  }
  if (!online_url.empty() && online_key == album_key) {
    if (source != nullptr) {
      *source = "online";
    }
    return online_url;
  }
  if (source != nullptr) {
    *source = "none";
  }
  return std::string();
}

// The cover chain as four explicit layers, in priority order. This is the
// single statement of the required fallback order; the resolver above is its
// URL-producing form. A track descends the chain only by surviving every layer
// above it - a host failure inside kLocalEmbedded/kLocalSidecar is still that
// same layer and must not skip kOnlineKeyless.
enum class CoverLayer {
  // 1. The track's own embedded art, published to a host.
  kLocalEmbedded = 1,
  // 2. A sidecar image in the track folder (or up to 4 ancestors), published.
  kLocalSidecar = 2,
  // 3. The keyless online chain (Deezer / iTunes / MusicBrainz+CoverArtArchive).
  kOnlineKeyless = 3,
  // 4. No image. Only reached when 1, 2 and 3 all fail; the field is omitted
  // and Discord renders its native placeholder.
  kNoImage = 4,
};

inline const char* CoverLayerName(CoverLayer layer) {
  switch (layer) {
    case CoverLayer::kLocalEmbedded:
      return "local-embedded";
    case CoverLayer::kLocalSidecar:
      return "local-sidecar";
    case CoverLayer::kOnlineKeyless:
      return "online-keyless";
    case CoverLayer::kNoImage:
      return "no-image";
  }
  return "no-image";
}

// The layer that actually supplies the image, given what each layer produced.
// `embedded_url` and `sidecar_url` are published local URLs (empty means that
// layer did not deliver - extraction failed, or the upload did not prove
// retrievable); `online_url` is the keyless chain's URL (empty means it did not
// resolve). Every URL is already key-guarded by the caller, so this function is
// pure ordering. No image is returned only when all three URLs are empty.
inline CoverLayer ResolveCoverLayer(const std::string& embedded_url,
                                    const std::string& sidecar_url,
                                    const std::string& online_url) {
  if (!embedded_url.empty()) {
    return CoverLayer::kLocalEmbedded;
  }
  if (!sidecar_url.empty()) {
    return CoverLayer::kLocalSidecar;
  }
  if (!online_url.empty()) {
    return CoverLayer::kOnlineKeyless;
  }
  return CoverLayer::kNoImage;
}

// Whether the online rung must be asked for this track given the local layers'
// state. The online rung is requested whenever the artist is known and online
// is enabled - including when local art was found, because a local extract can
// still fail to publish and must then fall through to online. If online also
// resolves nothing, no image is sent.
// `local_art_found` and `local_published` therefore both keep the request live:
// they never suppress it. Only the settings and an empty artist do.
inline bool ShouldRequestOnlineForTrack(bool use_albumart, bool use_online,
                                        const std::string& artist,
                                        bool local_art_found,
                                        bool local_published) {
  (void)local_art_found;
  (void)local_published;
  return ShouldRequestOnlineArtwork(use_albumart, use_online, artist, std::string());
}

}  // namespace PresenceLayout

#endif  // AIMPDISCORDPRESENCE_SRC_PRESENCE_LAYOUT_H_
