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

// The Discord presence field mapping, kept in one place:
//
//   details    (line 2)  the artist; falls back to the track title, then to
//                        the literal "AIMP", so the line is never blank
//   state      (line 3)  the track title; falls back to the album, and is
//                        omitted when that would repeat `details`
//   large_text           the album (the album-art tooltip); omitted when it
//                        would repeat `details` or `state`
//   large_image          the resolved album art URL; the bundled `aimp` asset
//                        when no art was resolved
//
// No field repeats a value already emitted by an earlier one; empty values
// count as absent.
//
// With status_display_type = 2 (Details), Discord renders `details` - the
// artist - in the member-list status line.
namespace PresenceLayout {

// Bundled Discord asset used when no album art URL was resolved. Owned by this
// layer so the fallback policy does not depend on the resolver.
constexpr const char* kFallbackLargeImageKey = "aimp";

// Literal shown on line 2 when both the artist and track title tags are empty.
constexpr const char* kFallbackDetails = "AIMP";

struct TextFields {
  std::string details;     // artist, else title, else kFallbackDetails
  std::string state;       // title, else album; empty omits the field
  std::string large_text;  // album; empty omits the field
};

// Artist -> song title -> album, de-duplicated. `details` is never empty;
// `state` falls back to the album only when the title would repeat `details`;
// `large_text` is dropped when the album would repeat either line.
inline TextFields BuildTextFields(const std::string& artist,
                                  const std::string& album,
                                  const std::string& title) {
  TextFields fields;
  fields.details = !artist.empty() ? artist
                                   : (!title.empty() ? title : kFallbackDetails);

  fields.state = title;
  if (fields.state.empty() || fields.state == fields.details) {
    fields.state = album;
  }
  if (fields.state.empty() || fields.state == fields.details) {
    fields.state.clear();
  }

  fields.large_text = album;
  if (fields.large_text.empty() || fields.large_text == fields.details ||
      fields.large_text == fields.state) {
    fields.large_text.clear();
  }

  return fields;
}

// The resolved album art URL when there is one, otherwise the bundled asset.
inline std::string BuildLargeImage(const std::string& artwork_url) {
  return artwork_url.empty() ? std::string(kFallbackLargeImageKey) : artwork_url;
}

}  // namespace PresenceLayout

#endif  // AIMPDISCORDPRESENCE_SRC_PRESENCE_LAYOUT_H_
