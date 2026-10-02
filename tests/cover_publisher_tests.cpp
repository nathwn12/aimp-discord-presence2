// Offline unit tests for src/cover_publisher.cpp and the pure album-art
// identity helpers (src/album_art.cpp, src/presence_layout.h).
//
// These tests never touch the network. The cover_publisher implementation is
// compiled into this translation unit, so its pure helpers (URL
// classification, multipart assembly, boundary generation, SHA-256 cache keys,
// cache file handling) are exercised directly; album_art.cpp is compiled as a
// second translation unit and linked, and the presence layout is header-only.
//
// Build (from the repository root):
//   cl /nologo /std:c++17 /W4 /WX /EHsc tests\cover_publisher_tests.cpp src\album_art.cpp /link winhttp.lib
// Run (the optional argument names the directory for the on-disk cache test):
//   cover_publisher_tests.exe [cache-directory]
// The default directory is %TEMP%\cover_publisher_cache_tests. The run creates
// and removes its own scratch file inside that directory.

#include "../src/cover_publisher.cpp"

#include "../src/album_art.h"
#include "../src/presence_layout.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& label) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", label.c_str());
  }
}

bool FileExists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring CacheDirectoryFromArgs(int argc, char** argv) {
  if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
    return Utils::ToWString(argv[1]);
  }
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, buffer);
  std::wstring directory(buffer, length);
  directory += L"cover_publisher_cache_tests";
  return directory;
}

// --- URL shape --------------------------------------------------------------

void TestUrlShape() {
  Check(LooksLikeUploadUrl("https://litter.catbox.moe/abc123.png"), "accept png url");
  Check(LooksLikeUploadUrl("https://litter.catbox.moe/a1b2c3.jpg"), "accept jpg url");
  Check(LooksLikeUploadUrl("https://litter.catbox.moe/0z9y8x.jpeg"), "accept jpeg url");
  Check(!LooksLikeUploadUrl(""), "reject empty url");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/abc123.png\n"), "reject trailing newline");
  Check(!LooksLikeUploadUrl(" https://litter.catbox.moe/abc123.png"), "reject leading space");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/abc12.png"), "reject short id");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/abc1234.png"), "reject long id");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/ABC123.png"), "reject uppercase id");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/abc123.gif"), "reject other extension");
  Check(!LooksLikeUploadUrl("http://litter.catbox.moe/abc123.png"), "reject plain http");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe/abc123.png?x=1"), "reject query string");
  Check(!LooksLikeUploadUrl("https://litter.catbox.moe.evil.test/abc123.png"), "reject other host");
}

// --- Response classification ------------------------------------------------

void TestClassifier() {
  const std::string url = "https://litter.catbox.moe/abc123.png";

  const Result ok = ClassifyUploadResponse(200, url);
  Check(ok.ok && ok.url == url && ok.reason.empty(), "200 + url -> ok");

  const Result trimmed = ClassifyUploadResponse(200, url + "\r\n");
  Check(trimmed.ok && trimmed.url == url, "200 + url with newline -> ok, trimmed");

  const Result empty = ClassifyUploadResponse(200, "");
  Check(!empty.ok && empty.reason == "empty-body", "200 + empty body -> empty-body");

  const Result blank = ClassifyUploadResponse(200, "  \r\n\t");
  Check(!blank.ok && blank.reason == "empty-body", "200 + whitespace body -> empty-body");

  const Result rejected = ClassifyUploadResponse(412, "File is too large");
  Check(!rejected.ok && rejected.reason == "File is too large", "412 -> reason is the body text");

  const Result rejectedEmpty = ClassifyUploadResponse(412, "");
  Check(!rejectedEmpty.ok && rejectedEmpty.reason == "http-412", "412 + empty body -> status reason");

  const Result serverError = ClassifyUploadResponse(500, "boom");
  Check(!serverError.ok && serverError.reason.find("500") != std::string::npos, "500 -> reason names status");

  const Result junk = ClassifyUploadResponse(200, "not a url");
  Check(!junk.ok && junk.reason.find("200") != std::string::npos, "200 + junk -> reason names status");
}

// --- Multipart arithmetic ---------------------------------------------------

// The live transport measurement reported a 37-byte closing suffix and a
// one-byte-larger part for JPEG. This builder keeps both invariants. Its prefix
// is 315 bytes for a PNG part: measured arithmetic (347) came from a probe whose
// filename/headers differed (the 32-byte delta is exactly one shorter filename
// plus header choices), so the assertion pins this builder's own framing
// instead of a probe-specific constant.
void TestMultipartArithmetic() {
  const std::vector<unsigned char> image(4321, 0x7f);
  const std::string boundary = MakeBoundary();
  const std::string prefix = MultipartPrefix(boundary, "image/png");
  const std::string suffix = MultipartSuffix(boundary);
  const std::string body = BuildMultipartBody(boundary, "image/png", image);
  const std::string jpeg_prefix = MultipartPrefix(boundary, "image/jpeg");

  Check(prefix.size() == 315, "png prefix is 315 bytes");
  Check(suffix.size() == 37, "suffix is 37 bytes");
  Check(jpeg_prefix.size() == 316, "jpeg prefix is 316 bytes");
  Check(jpeg_prefix.size() == prefix.size() + 1, "jpeg part is exactly one byte larger");
  Check(body.size() == prefix.size() + image.size() + suffix.size(), "body = prefix + image + suffix");
  Check(body.size() == 315 + image.size() + 37, "body arithmetic holds numerically");
  Check(std::memcmp(body.data() + prefix.size(), image.data(), image.size()) == 0, "image bytes land at the prefix offset");
  Check(body.compare(body.size() - suffix.size(), suffix.size(), suffix) == 0, "body ends with the closing delimiter");

  const size_t reqtype = body.find("name=\"reqtype\"");
  const size_t time = body.find("name=\"time\"");
  const size_t file = body.find("name=\"fileToUpload\"");
  Check(reqtype != std::string::npos && reqtype < time && time < file, "fields are in order reqtype, time, file");
  Check(body.find("filename=\"cover.png\"") != std::string::npos, "file part carries a filename");
  Check(body.find("\r\nContent-Type: image/png\r\n") != std::string::npos, "file part carries the content type");
  Check(body.find("fileupload\r\n") != std::string::npos && body.find("72h\r\n") != std::string::npos,
        "field values are present");

  const std::string unknown = MultipartPrefix(boundary, "application/octet-stream");
  Check(unknown.find("filename=\"cover.bin\"") != std::string::npos &&
            unknown.find("Content-Type: application/octet-stream\r\n") != std::string::npos,
        "unknown mime keeps a truthful filename and content type");
}

void TestBoundary() {
  const std::string first = MakeBoundary();
  const std::string second = MakeBoundary();
  Check(first != second, "boundaries differ across two calls");
  Check(first.size() == 29 && second.size() == 29, "boundary is 29 bytes");
  Check(first.compare(0, 4, "----") == 0, "boundary keeps its readable prefix");

  bool alphabet_only = true;
  for (char c : first) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    alphabet_only = alphabet_only && allowed;
  }
  Check(alphabet_only, "boundary only uses boundary-safe characters");
}

// --- Track identity keys (album vs album-less) ------------------------------

void TestAlbumIdentityKeys() {
  const std::string artist = "ZWE1HVNDXR Feat yatashigang";
  const std::string album = "LOVELY BASTARDS";
  const std::string file =
      "D:\\Software\\SoulseekQt\\downloads\\complete\\ZWE1HVNDXR Feat yatashigang - LOVELY BASTARDS.flac";
  const std::string other_file =
      "D:\\Software\\SoulseekQt\\downloads\\complete\\ZWE1HVNDXR - untitled take.flac";
  const std::string cover = "https://litter.catbox.moe/ofx201.jpg";

  // An album tag is still the whole identity: artist + "\n" + album, with the
  // file path never part of an albumed key.
  const std::string album_key = AlbumArt::BuildAlbumKey(artist, album, file);
  Check(album_key == artist + "\n" + album, "album tag -> key is artist + newline + album");
  Check(album_key.find(file) == std::string::npos, "an albumed key never embeds the file path");

  // ...and a cover resolved under that key is still adopted.
  std::string source;
  Check(PresenceLayout::ResolveLargeImage(album_key, album_key, cover, "", "", &source) == cover &&
            source == "local",
        "a resolved cover is applied for an albumed track");

  // An album-less track has no album identity, but it still has an identity:
  // the old collapsed key (artist + "\n") must not come back, because a cover
  // resolved for it is applied even though the album tag is empty.
  const std::string albumless_key = AlbumArt::BuildAlbumKey(artist, "", file);
  Check(albumless_key != artist + "\n", "album-less key no longer collapses to artist + newline");
  Check(PresenceLayout::ResolveLargeImage(albumless_key, albumless_key, cover, "", "", &source) == cover &&
            source == "local",
        "an empty album does not suppress the resolved cover");

  // The caption is still governed by the album tag alone: only the image
  // survives an empty album, never large_text.
  const PresenceLayout::TextFields fields =
      PresenceLayout::BuildTextFields(artist, "", "LOVELY BASTARDS");
  Check(fields.large_text.empty(), "an album-less track still omits the large_text caption");

  // Two album-less files by one artist are two identities, and one's cover is
  // never adopted for the other.
  const std::string other_key = AlbumArt::BuildAlbumKey(artist, "", other_file);
  Check(albumless_key != other_key, "two album-less files by one artist get different keys");
  Check(PresenceLayout::ResolveLargeImage(other_key, albumless_key, cover, "", "", &source) ==
            PresenceLayout::kFallbackLargeImageUrl,
        "an album-less file cannot inherit another file's cover");

  // An album-less key never collides with a real album's key for the same
  // artist, in either direction, so neither can overwrite the other's cover.
  Check(albumless_key != album_key, "album-less key never equals a real album's key");
  Check(PresenceLayout::ResolveLargeImage(album_key, albumless_key, cover, "", "", &source) ==
            PresenceLayout::kFallbackLargeImageUrl,
        "an album-less cover cannot overwrite a real album's stored cover");
  Check(PresenceLayout::ResolveLargeImage(albumless_key, album_key, cover, "", "", &source) ==
            PresenceLayout::kFallbackLargeImageUrl,
        "a real album's cover cannot be applied to an album-less file");
}

// --- Final cover fallback chain: local -> online -> black PNG ---------------

void TestCoverFallbackChain() {
  const std::string artist = "ZWE1HVNDXR Feat yatashigang";
  const std::string album = "LOVELY BASTARDS";
  const std::string file =
      "D:\\Software\\SoulseekQt\\downloads\\complete\\ZWE1HVNDXR Feat yatashigang - LOVELY BASTARDS.flac";
  const std::string key = AlbumArt::BuildAlbumKey(artist, album, file);
  const std::string local_url = "https://litter.catbox.moe/ofx201.jpg";
  const std::string online_url =
      "https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg";
  std::string source;

  // (a) A published local cover wins; neither the online URL nor the black
  // fallback is used.
  const std::string local_win =
      PresenceLayout::ResolveLargeImage(key, key, local_url, key, online_url, &source);
  Check(local_win == local_url, "chain: local cover is the resolved image");
  Check(source == "local", "chain: local cover names its source");
  Check(local_win != online_url && local_win != PresenceLayout::kFallbackLargeImageUrl,
        "chain: online and black are not used when local wins");

  // (b) With no local cover, the online chain's URL is used; black is not.
  const std::string online_win =
      PresenceLayout::ResolveLargeImage(key, "", "", key, online_url, &source);
  Check(online_win == online_url, "chain: online url is the resolved image");
  Check(source == "online", "chain: online url names its source");
  Check(online_win != PresenceLayout::kFallbackLargeImageUrl,
        "chain: black is not used when online wins");

  // (c) With neither source resolved, the black PNG URL is the last resort.
  const std::string fallback =
      PresenceLayout::ResolveLargeImage(key, "", "", "", "", &source);
  Check(fallback == PresenceLayout::kFallbackLargeImageUrl,
        "chain: black png url is the last resort");
  Check(source == "fallback", "chain: last resort names its source");

  // (d) The Task 1 regression: an album-less track with no local cover still
  // triggers the online lookup (the album tag is not part of the gate), and
  // its result is adopted under the file-path identity.
  Check(PresenceLayout::ShouldRequestOnlineArtwork(true, true, artist, ""),
        "chain: album-less track still triggers the online lookup");
  const std::string albumless_key = AlbumArt::BuildAlbumKey(artist, "", file);
  Check(PresenceLayout::ResolveLargeImage(albumless_key, "", "", albumless_key,
                                          online_url, &source) == online_url &&
            source == "online",
        "chain: an album-less track adopts the online url");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(true, false, artist, ""),
        "chain: online disabled never triggers a lookup");

  // (e) The literal "aimp" - a foreign Discord application's bundled asset -
  // is never the resolved image in any case.
  Check(local_win != "aimp" && online_win != "aimp" && fallback != "aimp",
        "chain: literal aimp is never the resolved image");
  Check(std::string(PresenceLayout::kFallbackLargeImageUrl) != "aimp",
        "chain: fallback constant is a url, never the aimp asset key");

  // The fallback URL fits Discord's external-asset length limit.
  Check(std::strlen(PresenceLayout::kFallbackLargeImageUrl) <= AlbumArt::kMaxUrlLength,
        "chain: black png url fits the discord asset url limit");
}

// --- Online rung: decision outcomes (offline, pure functions) ---------------

void TestOnlineRungDecision() {
  const std::string artist = "Daft Punk";
  const std::string album = "Discovery";
  const std::string key = AlbumArt::BuildAlbumKey(artist, album, "");
  std::string source;

  // The request predicate: a cover-less track with a non-empty album is looked
  // up. The album tag is not part of the gate, and neither is the local cover.
  Check(PresenceLayout::ShouldRequestOnlineArtwork(true, true, artist, album),
        "online rung: a cover-less albumed track triggers the lookup");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(false, true, artist, album),
        "online rung: use_albumart off suppresses the lookup");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(true, false, artist, album),
        "online rung: use_online off suppresses the lookup");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(true, true, "", album),
        "online rung: an empty artist suppresses the lookup");

  // Resolved: a provider hit that survives the identity guard yields its URL,
  // and the resolved URL is what the plugin applies - black is not used.
  const std::string deezer_body =
      "{\"data\":[{\"title\":\"Discovery\",\"artist\":{\"name\":\"Daft Punk\"},"
      "\"cover_xl\":\"https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg\"}],"
      "\"total\":1}";
  Check(AlbumArt::SearchResultMatches(deezer_body, AlbumArt::Provider::kDeezer, artist, album),
        "online rung: a deezer hit for the requested album passes the guard");
  const std::string deezer_url =
      AlbumArt::ExtractArtworkUrl(deezer_body, AlbumArt::Provider::kDeezer);
  Check(deezer_url ==
            "https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg",
        "online rung: the deezer cover_xl url is the resolved url");
  const std::string applied =
      PresenceLayout::ResolveLargeImage(key, "", "", key, deezer_url, &source);
  Check(applied == deezer_url && source == "online",
        "online rung: a resolved url is applied as the online image");
  Check(applied != PresenceLayout::kFallbackLargeImageUrl,
        "online rung: black is not used when a provider resolved");

  const std::string itunes_match =
      "{\"resultCount\":1,\"results\":[{\"artistName\":\"Daft Punk\","
      "\"collectionName\":\"Discovery\","
      "\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/image/thumb/Music/xyz/100x100bb.jpg\"}]}";
  Check(AlbumArt::SearchResultMatches(itunes_match, AlbumArt::Provider::kItunes, artist, album),
        "online rung: a matching itunes hit passes the guard");
  Check(AlbumArt::ExtractArtworkUrl(itunes_match, AlbumArt::Provider::kItunes) ==
            "https://is1-ssl.mzstatic.com/image/thumb/Music/xyz/600x600bb.jpg",
        "online rung: the itunes artwork url is upscaled to 600x600");

  // Not found: every provider answered and none matched, so the resolver
  // reports no URL and the plugin falls back to the black PNG.
  const std::string dbz_artist = "Dragon Ball Z";
  const std::string dbz_album = "Dragon Ball Z BGM Collection Disc 1";
  const std::string dbz_key = AlbumArt::BuildAlbumKey(dbz_artist, dbz_album, "");
  const std::string empty_deezer = "{\"data\":[],\"total\":0}";
  Check(!AlbumArt::SearchResultMatches(empty_deezer, AlbumArt::Provider::kDeezer, dbz_artist,
                                       dbz_album),
        "online rung: an empty deezer answer is not a match");
  Check(AlbumArt::ExtractArtworkUrl(empty_deezer, AlbumArt::Provider::kDeezer).empty(),
        "online rung: an empty deezer answer yields no url");

  // The real-world iTunes case: a hit for another album must be rejected, not
  // adopted. The guard is what keeps an unrelated cover off the card.
  const std::string itunes_unrelated =
      "{\"resultCount\":1,\"results\":[{\"artistName\":\"Dragon Ball Z\","
      "\"collectionName\":\"Dragon '98 Special Live\","
      "\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/image/thumb/Music/abc/100x100bb.jpg\"}]}";
  Check(!AlbumArt::SearchResultMatches(itunes_unrelated, AlbumArt::Provider::kItunes, dbz_artist,
                                       dbz_album),
        "online rung: an unrelated itunes hit is rejected by the identity guard");

  const std::string musicbrainz_empty = "{\"count\":0,\"release-groups\":[]}";
  Check(!AlbumArt::SearchResultMatches(musicbrainz_empty, AlbumArt::Provider::kMusicBrainz,
                                       dbz_artist, dbz_album),
        "online rung: an empty musicbrainz answer is not a match");
  Check(AlbumArt::ExtractMusicBrainzReleaseGroupId(musicbrainz_empty).empty(),
        "online rung: an empty musicbrainz answer yields no mbid");

  const std::string fallback =
      PresenceLayout::ResolveLargeImage(dbz_key, "", "", dbz_key, "", &source);
  Check(fallback == PresenceLayout::kFallbackLargeImageUrl && source == "fallback",
        "online rung: not-found falls back to the black png");
  Check(fallback != deezer_url, "online rung: the fallback is not another album's url");

  // A URL resolved for a different identity is never adopted for this one.
  const std::string other_key = AlbumArt::BuildAlbumKey(artist, "Homework", "");
  Check(PresenceLayout::ResolveLargeImage(key, "", "", other_key, deezer_url, &source) ==
            PresenceLayout::kFallbackLargeImageUrl,
        "online rung: another album's resolved url is never applied");
}

// --- Album-less online rung: the six album-empty guarantees -----------------
//
// (1) the request predicate fires; (2) album-empty is no longer a definitive
// nothing-to-query case; (3) an artist-only hit is accepted; (4) a wrong-album
// hit is still rejected when the request names an album; (5) album-less keys
// stay file-path distinct; (6) the decision table picks online over black.

void TestAlbumlessOnlineRung() {
  const std::string artist = "Daft Punk";
  const std::string file_a = "D:\\music\\untitled one.flac";
  const std::string file_b = "D:\\music\\untitled two.flac";
  const std::string online_url =
      "https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg";
  std::string source;

  // (1) ShouldRequestOnlineArtwork is true for artist-present + album-empty, and
  // the other combinations are unchanged.
  Check(PresenceLayout::ShouldRequestOnlineArtwork(true, true, artist, ""),
        "albumless: artist present + album empty requests online artwork");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(true, true, "", ""),
        "albumless: no artist still suppresses the request");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(false, true, artist, ""),
        "albumless: use_albumart off still suppresses the request");
  Check(!PresenceLayout::ShouldRequestOnlineArtwork(true, false, artist, ""),
        "albumless: use_online off still suppresses the request");

  // (2) The resolver's genuine nothing-to-query case is an empty artist only: an
  // empty album is an artist-only search, not a definitive miss. This is proven
  // through observable behavior - the request predicate used to be the thing
  // that refused album-empty, and it must now let it through; the identity guard
  // is exercised below to show the artist-only path is reachable and accepted.
  const std::string artist_only_body =
      "{\"data\":[{\"title\":\"Random Access Memories\",\"artist\":{\"name\":\"Daft Punk\"},"
      "\"cover_xl\":\"https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg\"}],"
      "\"total\":1}";
  // (3) An artist-only match is ACCEPTED when the requested album is empty, even
  // though the hit names a real album the caller did not ask about.
  Check(AlbumArt::SearchResultMatches(artist_only_body, AlbumArt::Provider::kDeezer, artist, ""),
        "albumless: an artist-only hit is accepted when the requested album is empty");
  Check(AlbumArt::MetadataMatches("", "Random Access Memories"),
        "albumless: an empty expected album never vetoes a present hit album");
  Check(!AlbumArt::MetadataMatches("Daft Punk", ""),
        "albumless: an empty candidate is still rejected");

  // (4) REGRESSION: a wrong-album hit is still REJECTED when the requested album
  // is non-empty, so the guard did not become a rubber stamp.
  const std::string wrong_album_body =
      "{\"data\":[{\"title\":\"Homework\",\"artist\":{\"name\":\"Daft Punk\"},"
      "\"cover_xl\":\"https://e-cdns-images.dzcdn.net/images/cover/abc123/600x600-000000-80-0-0.jpg\"}],"
      "\"total\":1}";
  Check(!AlbumArt::SearchResultMatches(wrong_album_body, AlbumArt::Provider::kDeezer, artist,
                                       "Discovery"),
        "albumless regression: a wrong-album hit is rejected when an album is requested");
  Check(!AlbumArt::MetadataMatches("Discovery", "Homework"),
        "albumless regression: MetadataMatches rejects a different present album");

  // (5) REGRESSION: an album-less BuildAlbumKey still embeds the file path, so
  // two album-less tracks by one artist produce DIFFERENT keys.
  const std::string key_a = AlbumArt::BuildAlbumKey(artist, "", file_a);
  const std::string key_b = AlbumArt::BuildAlbumKey(artist, "", file_b);
  Check(key_a.find(file_a) != std::string::npos,
        "albumless: the key embeds the file path");
  Check(key_a != key_b,
        "albumless: two album-less tracks by one artist get different keys");
  Check(key_a != artist + "\n",
        "albumless: the key does not collapse to artist + newline");

  // (6) Decision table: album-less + online-resolved => the ONLINE url is chosen,
  // not the black PNG.
  const std::string applied =
      PresenceLayout::ResolveLargeImage(key_a, "", "", key_a, online_url, &source);
  Check(applied == online_url && source == "online",
        "albumless: the online url is chosen over the black png");
  Check(applied != PresenceLayout::kFallbackLargeImageUrl,
        "albumless: the black png is not chosen when online resolved");
  const std::string unresolved =
      PresenceLayout::ResolveLargeImage(key_b, "", "", key_a, online_url, &source);
  Check(unresolved == PresenceLayout::kFallbackLargeImageUrl && source == "fallback",
        "albumless: a url resolved for another file's key is not adopted");
}

// --- SHA-256 cache keys -----------------------------------------------------

void TestSha256() {
  const std::vector<unsigned char> abc = {'a', 'b', 'c'};
  Check(Sha256Hex(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "sha256 matches the abc known answer");
  const std::vector<unsigned char> empty;
  Check(Sha256Hex(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "sha256 matches the empty-input known answer");

  const std::string two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  const std::vector<unsigned char> two_block_bytes(two_block.begin(), two_block.end());
  Check(Sha256Hex(two_block_bytes) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "sha256 matches the 56-byte known answer");

  const std::vector<unsigned char> image_a(1000, 0x11);
  const std::vector<unsigned char> image_b(1000, 0x12);
  Check(Sha256Hex(image_a) == Sha256Hex(image_a), "cache key is stable for the same bytes");
  Check(Sha256Hex(image_a) != Sha256Hex(image_b), "cache key differs for different bytes");
}

// --- Compression decision ---------------------------------------------------

void TestCompressionPlan() {
  const CompressionPlan large = PlanCompression(2000, 1500, 5 * 1024 * 1024);
  Check(large.reencode, "large image is re-encoded");
  Check(large.width == 600 && large.height == 450, "longest side clamps to 600 with aspect preserved");

  const CompressionPlan portrait = PlanCompression(900, 2400, 400 * 1024);
  Check(portrait.reencode, "portrait image is re-encoded");
  Check(portrait.width == 225 && portrait.height == 600, "portrait long side clamps to 600");

  const CompressionPlan already_small = PlanCompression(320, 240, 100 * 1024);
  Check(!already_small.reencode, "image at or below 600 px and 256 KB skips re-encoding");

  const CompressionPlan at_bound = PlanCompression(600, 600, 256 * 1024);
  Check(!at_bound.reencode, "the 600 px / 256 KB boundary is still 'already small'");

  const CompressionPlan heavy = PlanCompression(320, 240, 300 * 1024);
  Check(heavy.reencode, "small dimensions but heavy bytes are re-encoded");
  Check(heavy.width == 320 && heavy.height == 240, "an in-place re-encode never upscales");

  const CompressionPlan just_over = PlanCompression(601, 600, 100 * 1024);
  Check(just_over.reencode && just_over.width == 600 && just_over.height == 599,
        "601 px on the longest side is scaled under the bound");

  const CompressionPlan unknown = PlanCompression(0, 0, 1024);
  Check(!unknown.reencode && unknown.width == 0 && unknown.height == 0, "unknown dimensions are left alone");
}

// --- Compression round trip (real WIC, still offline) -----------------------

std::vector<unsigned char> EncodeTestPng(UINT width, UINT height, bool noise) {
  std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
  if (noise) {
    uint32_t state = 0x9e3779b9u;
    for (size_t i = 0; i < pixels.size(); i += 4) {
      state = state * 1664525u + 1013904223u;
      pixels[i] = static_cast<unsigned char>(state >> 24);
      pixels[i + 1] = static_cast<unsigned char>(state >> 16);
      pixels[i + 2] = static_cast<unsigned char>(state >> 8);
      pixels[i + 3] = 0xff;
    }
  } else {
    for (UINT y = 0; y < height; ++y) {
      for (UINT x = 0; x < width; ++x) {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        pixels[i] = static_cast<unsigned char>(x * 255 / width);
        pixels[i + 1] = static_cast<unsigned char>(y * 255 / height);
        pixels[i + 2] = 0x40;
        pixels[i + 3] = 0xff;
      }
    }
  }

  std::vector<unsigned char> png;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    Microsoft::WRL::ComPtr<IStream> output_stream;
    Microsoft::WRL::ComPtr<IWICStream> wic_output;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> properties;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) {
      hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
                                           static_cast<UINT>(pixels.size()), pixels.data(), &bitmap);
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
      hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(hr)) {
      hr = encoder->Initialize(wic_output.Get(), WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr)) {
      hr = encoder->CreateNewFrame(&frame, &properties);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->Initialize(properties.Get());
    }
    if (SUCCEEDED(hr)) {
      hr = frame->SetSize(width, height);
    }
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) {
      hr = frame->SetPixelFormat(&pixel_format);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->WriteSource(bitmap.Get(), nullptr);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->Commit();
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
    }
    if (SUCCEEDED(hr)) {
      png.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + size);
    }
    if (data != nullptr) {
      GlobalUnlock(memory);
    }
  }
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return png;
}

bool DecodeTestDimensions(const std::vector<unsigned char>& bytes, UINT* width, UINT* height) {
  *width = 0;
  *height = 0;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) {
      hr = factory->CreateStream(&stream);
    }
    if (SUCCEEDED(hr)) {
      hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()));
    }
    if (SUCCEEDED(hr)) {
      hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    }
    if (SUCCEEDED(hr)) {
      hr = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->GetSize(width, height);
    }
    ok = SUCCEEDED(hr);
  }
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return ok;
}

void TestCompressionPreparation() {
  const std::vector<unsigned char> large = EncodeTestPng(1600, 1000, true);
  Check(!large.empty(), "fixture encodes a 1600x1000 png");

  const PreparedImage prepared = PrepareImageForUpload(large, "image/png");
  Check(prepared.reencoded, "large png is re-encoded");
  Check(prepared.mime == "image/jpeg", "re-encoded bytes are announced as jpeg");
  Check(!prepared.bytes.empty() && prepared.bytes.size() < large.size(),
        "re-encoded bytes are smaller than the source");
  Check(prepared.bytes != large, "re-encoded bytes differ from the source");
  Check(UploadCacheKey(prepared) == Sha256Hex(prepared.bytes), "cache key is the hash of the uploaded bytes");
  Check(UploadCacheKey(prepared) != Sha256Hex(large), "cache key is not the source hash");
  Check(prepared.note.find("input=" + std::to_string(large.size())) != std::string::npos,
        "note records the input size");
  Check(prepared.note.find("output=" + std::to_string(prepared.bytes.size())) != std::string::npos,
        "note records the output size");
  Check(prepared.note.find("mode=jpeg-q85") != std::string::npos, "note records the encode mode");

  UINT width = 0;
  UINT height = 0;
  Check(DecodeTestDimensions(prepared.bytes, &width, &height), "re-encoded bytes decode");
  Check(width == 600 && height == 375, "longest side is exactly 600 and aspect is preserved");

  const std::vector<unsigned char> compact = EncodeTestPng(320, 240, false);
  Check(!compact.empty() && compact.size() <= kSkipReencodeMaxBytes, "fixture is already small");
  const PreparedImage untouched = PrepareImageForUpload(compact, "image/png");
  Check(!untouched.reencoded, "already-small png skips re-encoding");
  Check(untouched.bytes == compact, "original bytes pass through unchanged");
  Check(untouched.mime == "image/png", "original mime is kept");
  Check(UploadCacheKey(untouched) == Sha256Hex(compact), "skipped image is keyed by its original bytes");

  const std::vector<unsigned char> garbage(300 * 1024, 0x5a);
  const PreparedImage fallback = PrepareImageForUpload(garbage, "image/png");
  Check(!fallback.reencoded, "undecodable input is not re-encoded");
  Check(fallback.bytes == garbage, "undecodable input falls back to the original bytes");
  Check(fallback.note.find("wic-failed=") != std::string::npos, "fallback note names the WIC failure");
}

// --- Cache hit/miss plus negative TTL --------------------------------------

void TestCache(const std::wstring& directory) {
  CreateDirectoryW(directory.c_str(), nullptr);
  const std::wstring path = directory + L"\\cover-publisher-cache.txt";
  const std::string cache_path = Utils::ToString(path);
  const std::string url = "https://litter.catbox.moe/abc123.png";
  const std::string key = Sha256Hex(std::vector<unsigned char>{'c', 'a', 'c', 'h', 'e'});
  const long long now = static_cast<long long>(std::time(nullptr));
  DeleteFileW(path.c_str());

  CoverPublisher::Configure(cache_path);
  Result hit;
  Check(!CacheLookup(key, now, &hit), "cache miss on an empty cache");

  Result stored;
  stored.ok = true;
  stored.url = url;
  CacheStore(key, stored, now);
  Check(CacheLookup(key, now, &hit), "cache hit right after store");
  Check(hit.ok && hit.url == url, "cache hit returns the stored url");
  Check(FileExists(path), "store writes the cache file");

  CoverPublisher::Configure(cache_path);  // reload from disk
  Check(CacheLookup(key, now, &hit) && hit.url == url, "cache survives a reload from disk");

  const std::string other_key = Sha256Hex(std::vector<unsigned char>{'o', 't', 'h', 'e', 'r'});
  Result failed;
  failed.reason = "empty-body";
  CacheStore(other_key, failed, now);
  Check(CacheLookup(other_key, now, &hit) && !hit.ok && hit.reason == "empty-body",
        "negative result is cached");
  Check(!CacheLookup(other_key, now + kNegativeCacheTtlSeconds + 1, &hit),
        "negative result expires after its TTL");

  const std::wstring missing = directory + L"\\missing-subdirectory\\cache.txt";
  CoverPublisher::Configure(Utils::ToString(missing));
  CacheStore(key, stored, now);
  Check(CacheLookup(key, now, &hit) && hit.url == url,
        "an unwritable cache path still serves memory only");

  CoverPublisher::Configure(cache_path);
  CacheStore(key, stored, now);
  CoverPublisher::ClearCache();
  Check(!CacheLookup(key, now, &hit), "ClearCache forgets memory");
  Check(!FileExists(path), "ClearCache removes the cache file");

  DeleteFileW(path.c_str());
  RemoveDirectoryW(directory.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  const std::wstring directory = CacheDirectoryFromArgs(argc, argv);
  std::printf("cache directory: %s\n", Utils::ToString(directory).c_str());

  TestUrlShape();
  TestClassifier();
  TestMultipartArithmetic();
  TestBoundary();
  TestAlbumIdentityKeys();
  TestCoverFallbackChain();
  TestOnlineRungDecision();
  TestAlbumlessOnlineRung();
  TestSha256();
  TestCompressionPlan();
  TestCompressionPreparation();
  TestCache(directory);

  std::printf("cover_publisher tests: %d checks, %d failures\n", g_checks, g_failures);
  std::printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
