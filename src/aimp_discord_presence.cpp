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

#include "aimp_discord_presence.h"

#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "aimp_core.h"
#include "aimp_filemanager.h"
#include "aimp_messages.h"
#include "aimp_player.h"
#include "cover_publisher.h"
#include "presence_layout.h"
#include "utils.h"

namespace {

// The publisher logs through a plain function pointer (it owns no plugin
// object). This trampoline forwards to the live instance's LogCover, which
// routes to the DebugLog client. Set once in Load(), cleared in Unload().
AimpDiscordPresence* g_publisher_log_target = nullptr;

void PublisherLogTrampoline(const std::string& line) {
  if (g_publisher_log_target != nullptr) {
    g_publisher_log_target->LogCover(line);
  }
}

constexpr int kPlayerStateStopped = 0;
constexpr int kPlayerStatePaused = 1;
constexpr int kPlayerStatePlaying = 2;

// A position difference larger than this means the user seeked and the
// timestamp Discord is counting down locally has to be corrected.
constexpr double kSeekDriftSeconds = 3.0;

// Input cap only: a cover above this size is skipped before the publisher sees
// it (a pathological source is not compressed into submission). Anything that
// passes is downscaled and re-encoded before transfer.
constexpr size_t kMaxCoverBytes = 20 * 1024 * 1024;

int64_t UnixSecondsNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool IsStreamUrl(const std::wstring& url) {
  if (url.size() >= 7 && url.compare(0, 7, L"http://") == 0) {
    return true;
  }
  return url.size() >= 8 && url.compare(0, 8, L"https://") == 0;
}

// Drive-absolute ("C:\x") or UNC ("\\server\x") paths are used as configured;
// anything else is resolved below the plugin DLL's directory.
bool IsAbsolutePath(const std::wstring& path) {
  if (path.size() >= 3 && path[1] == L':' &&
      (path[2] == L'\\' || path[2] == L'/')) {
    return true;
  }
  return path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') &&
         (path[1] == L'\\' || path[1] == L'/');
}

// Directory of the loaded plugin DLL, with a trailing separator, or an empty
// string when it cannot be determined.
std::string PluginDirectory() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&PluginDirectory),
                          &module)) {
    return std::string();
  }

  char module_path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(module, module_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::string();
  }

  std::string directory(module_path, length);
  const size_t separator = directory.find_last_of("\\/");
  if (separator == std::string::npos) {
    return std::string();
  }

  return directory.substr(0, separator + 1);
}

// Turns the configured DebugLog value into the UTF-8 path the IPC client
// expects. Returns an empty string when the DLL directory cannot be
// determined, in which case logging stays off rather than writing somewhere
// unpredictable.
std::string ResolveDebugLogPath(const std::wstring& configured) {
  if (configured.empty()) {
    return std::string();
  }
  if (IsAbsolutePath(configured)) {
    return Utils::ToString(configured);
  }

  const std::string directory = PluginDirectory();
  if (directory.empty()) {
    return std::string();
  }

  return directory + Utils::ToString(configured);
}

// The cover cache defaults to cover-cache.txt next to the DLL, so an empty
// CoverCache still has a home. A DLL directory that cannot be determined
// leaves the path empty, which turns the publisher's on-disk cache off rather
// than writing somewhere unpredictable.
std::string ResolveCoverCachePath(const std::wstring& configured) {
  if (IsAbsolutePath(configured)) {
    return Utils::ToString(configured);
  }

  const std::string directory = PluginDirectory();
  if (directory.empty()) {
    return std::string();
  }

  return directory + Utils::ToString(configured.empty() ? L"cover-cache.txt"
                                                        : configured);
}

}  // namespace

__declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(void** Header) {
  *Header = new AimpDiscordPresence();
  return S_OK;
}

LPWSTR AimpDiscordPresence::GetInfo(int index) {
  switch (index) {
    case Info::kName:
      return const_cast<LPWSTR>(L"Discord Presence");
    case Info::kAuthor:
      return const_cast<LPWSTR>(L"Exle (original), nathwn12 (fork)");
    case Info::Description::kShort:
      return const_cast<LPWSTR>(L"Discord Rich Presence for AIMP \u2014 fork of Exle's plugin");
  }

  return nullptr;
}

DWORD AimpDiscordPresence::GetCategory() {
  return Category::kAddons;
}

bool AimpDiscordPresence::Load() {
  if (!Aimp::Player::Service::Player()) {
    return false;
  }

  LoadConfig();
  InitializeMessageDispatcher();

  client_.reset(new DiscordIpc::Client(std::to_string(settings.application_id)));

  // Opt-in frame log. The client writes nothing while the setting is empty
  // and silently ignores a path it cannot open, so a bad value cannot
  // disturb the connection.
  if (!settings.debug_log.empty()) {
    client_->SetLogPath(ResolveDebugLogPath(settings.debug_log));
  }

  // Publish target for local covers: the cache is keyed by image SHA-256, so a
  // cover is uploaded once and reused. Configuring it never touches the
  // network; an unusable path only costs the on-disk cache.
  CoverPublisher::Configure(ResolveCoverCachePath(settings.cover_cache));
  // Route the publisher's host-choice and all-hosts-failed lines into the same
  // DebugLog as the cover vocabulary.
  g_publisher_log_target = this;
  CoverPublisher::SetLogger(&PublisherLogTrampoline);

  // Single publisher worker. It owns no AIMP object, only the bytes it is
  // handed, and it is joined in Unload().
  cover_worker_ = std::thread(&AimpDiscordPresence::CoverWorkerMain, this);

  album_art_.SetCallback(
      [this](const std::string& artist, const std::string& album,
             const std::string& file_path, const std::string& url) {
        ApplyResolvedArtwork(artist, album, file_path, url);
      });
  // Online-rung diagnostics go through the same DebugLog path as the cover
  // code; the hook is invoked on the resolver worker thread, never with image
  // bytes.
  album_art_.SetLogger([this](const std::string& line) { LogCover(line); });
  album_art_.SetOnlineEnabled(settings.use_albumart_online);

  client_->EnsureConnected();
  RefreshPresence(true);

  return true;
}

void AimpDiscordPresence::LoadConfig() {
  Aimp::Core::Service::Config config;

  LoadConfigValue(config, L"DiscordPresence\\ApplicationID", &settings.application_id);
  LoadConfigValue(config, L"DiscordPresence\\Timestamp", &settings.timestamp);
  LoadConfigValue(config, L"DiscordPresence\\UseAlbumArt", &settings.use_albumart);
  LoadConfigValue(config, L"DiscordPresence\\UseAlbumArtOnline", &settings.use_albumart_online);
  LoadConfigValue(config, L"DiscordPresence\\LocalCover", &settings.local_cover);
  LoadConfigValue(config, L"DiscordPresence\\CoverCache", &settings.cover_cache);
  LoadConfigValue(config, L"DiscordPresence\\StatusDisplayType", &settings.status_display_type);
  LoadConfigValue(config, L"DiscordPresence\\DebugLog", &settings.debug_log);
  LoadConfigValue(config, L"DiscordPresence\\State.UsePlay", &settings.status.use_play);
  LoadConfigValue(config, L"DiscordPresence\\State.PlayImage", &settings.status.play_image);
  LoadConfigValue(config, L"DiscordPresence\\State.UsePause", &settings.status.use_pause);
  LoadConfigValue(config, L"DiscordPresence\\State.PauseImage", &settings.status.pause_image);
  LoadConfigValue(config, L"DiscordPresence\\State.UseRadio", &settings.status.use_radio);
  LoadConfigValue(config, L"DiscordPresence\\State.RadioImage", &settings.status.radio_image);
}

void AimpDiscordPresence::InitializeMessageDispatcher() {
  Aimp::Messages::Service::MessageDispatcher message_dispatcher;

  message_dispatcher.Hook(Aimp::Messages::Events::kPlayerState,
                          [&](DWORD message, int param1, void*, HRESULT*) {
                            OnPlayerState(message, param1);
                          });

  message_dispatcher.Hook(Aimp::Messages::Events::Stream::Start::kSubtrack,
                          [&](DWORD, int, void*, HRESULT*) {
                            OnStreamStartSubtrack();
                          });

  message_dispatcher.Hook(Aimp::Messages::Events::kPropertyValue,
                          [&](DWORD message, int param1, void*, HRESULT*) {
                            OnPropertyValue(message, param1);
                          });

  // The every-second position tick; the asynchronous artwork result needs a
  // notification after it completes, and the seek-only property event is not
  // one.
  message_dispatcher.Hook(Aimp::Messages::Events::kPlayerUpdatePosition,
                          [&](DWORD, int, void*, HRESULT*) {
                            OnPlayerUpdatePosition();
                          });
}

template<typename T>
void AimpDiscordPresence::LoadConfigValue(Aimp::Core::Service::Config config, const std::wstring& key, T value) {
  if (!config.Get(key, value)) {
    config.Set(key, *value);
  }
}

bool AimpDiscordPresence::Unload() {
  Aimp::Messages::Service::MessageDispatcher().UnhookAll();

  // Stop the publisher's log hook before the instance goes away, so a late
  // worker line can never touch a dead plugin.
  CoverPublisher::SetLogger(nullptr);
  g_publisher_log_target = nullptr;

  // Joins the publisher worker: it holds only queued image bytes, so it can be
  // stopped without an AIMP object ever crossing threads.
  StopCoverWorker();

  // Joins the artwork worker, so no callback can touch this plugin afterwards.
  album_art_.Shutdown();

  if (client_) {
    client_->Clear();
    client_->Shutdown();
    client_.reset();
  }

  return true;
}

void AimpDiscordPresence::Notification(int, IUnknown*) {}
void AimpDiscordPresence::ShowSettings(HWND) {}

void AimpDiscordPresence::OnPlayerState(DWORD, int param1) {
  if (param1 == kPlayerStateStopped ||
      (param1 == kPlayerStatePaused && !settings.status.use_pause)) {
    track_key_.clear();
    sent_at_seconds_ = 0;
    if (client_) {
      client_->Clear();
    }
    return;
  }

  paused_ = (param1 == kPlayerStatePaused);
  RefreshPresence(true);
}

void AimpDiscordPresence::OnStreamStartSubtrack() {
  Aimp::Player::Service::Player player;
  paused_ = (player.State() == kPlayerStatePaused);
  RefreshPresence(true);
}

void AimpDiscordPresence::OnPropertyValue(DWORD, int param1) {
  if (param1 != Aimp::Messages::Properties::Player::kPosition) {
    return;
  }

  Aimp::Player::Service::Player player;
  if (player.State() != kPlayerStatePlaying) {
    return;
  }

  // Track change: AIMP does not raise a dedicated event for every source, so
  // the track identity is compared on the position notification instead.
  const TrackInfo info = ReadTrackInfo();
  if (info.key != track_key_) {
    RefreshPresence(true);
    return;
  }

  // Seek: the timestamps are absolute, so Discord counts down by itself and no
  // periodic update is required. Only a real drift triggers a new update.
  if (sent_at_seconds_ != 0) {
    const double elapsed = static_cast<double>(UnixSecondsNow() - sent_at_seconds_);
    if (std::fabs(player.Position() - (sent_position_ + elapsed)) > kSeekDriftSeconds) {
      RefreshPresence(false);
      return;
    }
  }

  if (ApplyPendingArtwork()) {
    SendActivity();
  }
}

void AimpDiscordPresence::OnPlayerUpdatePosition() {
  Aimp::Player::Service::Player player;
  if (player.State() != kPlayerStatePlaying) {
    return;
  }

  // The artwork lookup is asynchronous (it can take seconds), so by the time
  // it resolves the seek-only property event may have already passed. This
  // event fires every second while a track plays, so the late result is
  // applied and sent promptly.
  if (ApplyPendingArtwork()) {
    SendActivity();
  }
}

AimpDiscordPresence::TrackInfo AimpDiscordPresence::ReadTrackInfo() {
  TrackInfo info;

  Aimp::Player::Service::Player player;
  Aimp::FileManager::FileInfo fileinfo = player.GetInfo();

  const std::wstring file_name =
      fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kFileName);
  info.title = Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kTitle));
  info.artist = Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kArtist));
  info.album = Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kAlbum));
  info.file = Utils::ToString(file_name);
  info.is_url = IsStreamUrl(file_name);
  info.key = info.artist + "\n" + info.album + "\n" + info.title;

  return info;
}

void AimpDiscordPresence::RefreshPresence(bool request_artwork) {
  const TrackInfo info = ReadTrackInfo();

  // Local (offline) cover: extract the track's own artwork on the message
  // thread once per new track, log its fingerprint, and either apply a URL
  // already known for that image or hand the bytes to the publisher worker.
  // A URL is only ever adopted when an upload proved retrievable, so the
  // online chain's URL or the black fallback stays in place until then. Runs
  // before SetInfo() so a known URL lands in this same update.
  // Local cover layers (1 and 2): the track's own embedded art, or a sidecar
  // image the extractor found. `local_art_found` records that a local cover
  // exists; when publishing it fails, the online rung below must still be asked
  // - black is only for a track where every layer failed.
  bool local_art_found = false;
  bool local_published = false;
  if (request_artwork && info.key != local_art_key_) {
    local_art_key_ = info.key;
    // A cover queued for the previous track must not be applied to this one.
    local_cover_sha_.clear();
    if (!info.is_url) {
      LocalArt::Result art =
          ExtractLocalArt(info, settings.use_albumart && settings.local_cover);
      if (art.found) {
        local_art_found = true;
        local_cover_sha_ = art.sha256_hex;
        MaybePublishLocalCover(info, std::move(art));
        // MaybePublishLocalCover applies a URL synchronously only on a known
        // hit; an in-flight upload leaves local_cover_url_ empty, and that is
        // exactly the case that must fall through to online rather than black.
        std::lock_guard<std::mutex> lock(presence_mutex_);
        local_published = !local_cover_url_.empty();
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(presence_mutex_);
    SetInfo(info);
    SetSmallImage(info);
    SetTimestamp(info);
  }

  track_key_ = info.key;
  track_artist_ = info.artist;
  track_album_ = info.album;
  track_file_ = info.file;

  if (request_artwork) {
    if (PresenceLayout::ShouldRequestOnlineForTrack(
            settings.use_albumart, settings.use_albumart_online, info.artist,
            local_art_found, local_published)) {
      // Non-blocking: the worker resolves the URL and publishes it, the next
      // notification applies it. The album tag is not required: an album-less
      // track still gets a keyless lookup, keyed by artist + file path. A local
      // cover that was found never suppresses this request: if its publish
      // fails, the online rung below is what keeps the card off black.
      album_art_.Request(info.artist, info.album, info.file);
    } else {
      std::lock_guard<std::mutex> lock(presence_mutex_);
      artwork_key_.clear();
      artwork_url_.clear();
    }
  }

  SendActivity();
}

void AimpDiscordPresence::SetInfo(const TrackInfo& info) {
  activity_.type = static_cast<int>(DiscordIpc::ActivityType::kListening);
  activity_.status_display_type = settings.status_display_type;

  // Artist First -> Album Next: details is the artist (falling back to the
  // track title), state is the album, and the track title is the large-image
  // tooltip. Empty values omit their field.
  const PresenceLayout::TextFields fields =
      PresenceLayout::BuildTextFields(info.artist, info.album, info.title);
  activity_.details = fields.details;
  activity_.state = fields.state;
  activity_.large_text = fields.large_text;

  // The local-cover layer takes priority over the online chain: a cover read
  // from the file itself is always the right one. Both are matched by track
  // identity key, which the shared builder keeps in step with
  // ApplyPendingArtwork(); an empty album does not suppress the image.
  const std::string large_image = ResolveLargeImageLocked(
      AlbumArt::BuildAlbumKey(info.artist, info.album, info.file), nullptr);
  activity_.large_image = large_image;
  last_large_image_ = large_image;
}

void AimpDiscordPresence::SetSmallImage(const TrackInfo& info, int state) {
  activity_.small_image.clear();
  activity_.small_text.clear();

  Aimp::Player::Service::Player player;
  if (state == -1) {
    state = player.State();
  }

  std::string small_image;
  if (state == kPlayerStatePaused) {
    if (settings.status.use_pause && !settings.status.pause_image.empty()) {
      small_image = Utils::ToString(settings.status.pause_image);
    }
  } else if (state == kPlayerStatePlaying) {
    if (info.is_url && settings.status.use_radio && !settings.status.radio_image.empty()) {
      small_image = Utils::ToString(settings.status.radio_image);
      activity_.small_text = "Listening to URL";
    } else if (settings.status.use_play && !settings.status.play_image.empty()) {
      small_image = Utils::ToString(settings.status.play_image);
    }
  }

  activity_.small_image = small_image;
}

void AimpDiscordPresence::SetTimestamp(const TrackInfo& info) {
  activity_.start_timestamp = 0;
  activity_.end_timestamp = 0;

  if (paused_) {
    return;
  }

  Aimp::Player::Service::Player player;
  const int position = static_cast<int>(round(player.Position()));
  const int64_t start = UnixSecondsNow();

  if (settings.timestamp && !info.is_url) {
    const int duration = static_cast<int>(round(player.Duration()));
    activity_.start_timestamp = start;
    if (duration - position > 0) {
      activity_.end_timestamp = start + duration - position;
    }
  } else {
    activity_.start_timestamp = start - position;
  }
}

void AimpDiscordPresence::ApplyResolvedArtwork(const std::string& artist, const std::string& album,
                                               const std::string& file_path,
                                               const std::string& url) {
  // Runs on the album art worker thread; publishing the value is all that is
  // allowed here.
  std::lock_guard<std::mutex> lock(presence_mutex_);
  artwork_key_ = AlbumArt::BuildAlbumKey(artist, album, file_path);
  artwork_url_ = url;
}

bool AimpDiscordPresence::ApplyPendingArtwork() {
  // A finished publish, if any. The URL is adopted only when the publisher
  // proved the upload retrievable (Result.ok); a failure is logged and leaves
  // the online chain's URL or the black fallback in place.
  {
    std::string hash;
    bool ok = false;
    std::string url;
    std::string reason;
    std::string detail;
    std::string host;
    {
      std::lock_guard<std::mutex> lock(cover_mutex_);
      if (cover_result_ready_) {
        hash = cover_result_hash_;
        ok = cover_result_ok_;
        url = cover_result_url_;
        reason = cover_result_reason_;
        detail = cover_result_detail_;
        host = cover_result_host_;
        cover_result_hash_.clear();
        cover_result_url_.clear();
        cover_result_reason_.clear();
        cover_result_detail_.clear();
        cover_result_host_.clear();
        cover_result_ready_ = false;
      }
    }

    if (!hash.empty()) {
      if (!ok) {
        // The gate at work: no local URL is adopted, the host that failed is
        // on record, and the online rung (or black) stays in place. The
        // publisher's own negative cache keeps a dead host from being retried
        // immediately.
        LogCover("local-cover failed hash=" + hash +
                 " host=" + (host.empty() ? "unknown" : host) +
                 " reason=" + (reason.empty() ? "unknown" : reason) +
                 " detail=" + (detail.empty() ? "unknown" : detail));
      } else {
        // Remember the URL even when the track has moved on, so the image is
        // never uploaded twice in one session.
        local_cover_urls_[hash] = url;
        // Once per cover: which host served it, what was prepared, what was
        // transferred, which key the cache used. Sizes and hashes only - never
        // bytes.
        LogCover("local-cover published hash=" + hash +
                 " host=" + (host.empty() ? "unknown" : host) + " url=" + url +
                 " detail=" + (detail.empty() ? "unknown" : detail));
        if (hash == local_cover_sha_) {
          std::lock_guard<std::mutex> lock(presence_mutex_);
          local_cover_url_ = url;
          local_cover_key_ =
              AlbumArt::BuildAlbumKey(track_artist_, track_album_, track_file_);
        } else {
          LogCover("local-cover dropped hash=" + hash + " reason=stale-track");
        }
      }
    }
  }

  std::string applied_image;
  std::string applied_source;
  std::string applied_artist;
  std::string applied_album;
  {
    std::lock_guard<std::mutex> lock(presence_mutex_);

    // The shared builder keeps this comparison in step with SetInfo(). The
    // resolved image is applied for the track identity it was resolved for,
    // album or not: only large_text (the caption) depends on the album tag, so
    // an empty album must not suppress the artwork.
    const std::string album_key =
        AlbumArt::BuildAlbumKey(track_artist_, track_album_, track_file_);
    const std::string large_image = ResolveLargeImageLocked(album_key, &applied_source);
    if (large_image == last_large_image_) {
      return false;
    }

    last_large_image_ = large_image;
    activity_.large_image = large_image;
    applied_image = large_image;
    applied_artist = track_artist_;
    applied_album = track_album_;
  }

  LogCover("large-image applied artist=\"" + applied_artist + "\" album=\"" +
           applied_album + "\" source=" + applied_source + " url=" + applied_image);
  return true;
}

std::string AimpDiscordPresence::ResolveLargeImageLocked(const std::string& album_key,
                                                         std::string* source) const {
  // The setting turns both sources off; the black fallback then stays in place.
  const std::string local_url = settings.use_albumart ? local_cover_url_ : std::string();
  const std::string online_url = settings.use_albumart ? artwork_url_ : std::string();
  return PresenceLayout::ResolveLargeImage(album_key, local_cover_key_, local_url,
                                           artwork_key_, online_url, source);
}

void AimpDiscordPresence::MaybePublishLocalCover(const TrackInfo& info,
                                                 LocalArt::Result art) {
  if (!settings.use_albumart || !settings.local_cover) {
    return;
  }
  if (!art.found || art.bytes.empty() || art.sha256_hex.empty()) {
    return;
  }

  const std::string hash = art.sha256_hex;
  const size_t size = art.bytes.size();

  // Only the container shapes the host serves back (and the publisher
  // therefore accepts) are uploaded; anything else would spend an upload on a
  // URL the publisher has to reject.
  const char* mime = nullptr;
  if (art.format == "PNG") {
    mime = "image/png";
  } else if (art.format == "JPEG") {
    mime = "image/jpeg";
  }
  if (mime == nullptr) {
    LogCover("local-cover skipped hash=" + hash + " format=" + art.format +
             " reason=unsupported-format");
    return;
  }

  if (size > kMaxCoverBytes) {
    LogCover("local-cover skipped hash=" + hash + " size=" + std::to_string(size) +
             " limit=" + std::to_string(kMaxCoverBytes) + " reason=too-large");
    return;
  }

  const auto known = local_cover_urls_.find(hash);
  if (known != local_cover_urls_.end()) {
    {
      std::lock_guard<std::mutex> lock(presence_mutex_);
      local_cover_url_ = known->second;
      local_cover_key_ = AlbumArt::BuildAlbumKey(info.artist, info.album, info.file);
    }
    LogCover("local-cover hit hash=" + hash + " size=" + std::to_string(size) +
             " url=" + known->second);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(cover_mutex_);
    if (cover_inflight_hash_ == hash ||
        (cover_result_ready_ && cover_result_hash_ == hash)) {
      // Already queued, running or waiting to be picked up.
      return;
    }
    cover_request_hash_ = hash;
    cover_request_mime_ = mime;
    cover_request_bytes_ = std::move(art.bytes);
    cover_inflight_hash_ = hash;
    cover_has_request_ = true;
  }
  cover_wake_.notify_one();

  LogCover("local-cover queued hash=" + hash + " size=" + std::to_string(size) +
           " mime=" + mime);
}

void AimpDiscordPresence::CoverWorkerMain() {
  for (;;) {
    std::string hash;
    std::string mime;
    std::vector<unsigned char> bytes;
    {
      std::unique_lock<std::mutex> lock(cover_mutex_);
      cover_wake_.wait(lock, [this] { return cover_stopping_ || cover_has_request_; });
      if (cover_stopping_) {
        return;
      }
      hash = std::move(cover_request_hash_);
      mime = std::move(cover_request_mime_);
      bytes = std::move(cover_request_bytes_);
      cover_request_hash_.clear();
      cover_request_mime_.clear();
      cover_request_bytes_.clear();
      cover_has_request_ = false;
    }

    // Blocking, but on this worker only: the message thread is never held up
    // by the upload, which is bounded by the publisher's own budget.
    const CoverPublisher::Result result = CoverPublisher::Publish(bytes, mime.c_str());
    std::vector<unsigned char>().swap(bytes);

    {
      std::lock_guard<std::mutex> lock(cover_mutex_);
      if (cover_inflight_hash_ == hash) {
        cover_inflight_hash_.clear();
      }
      cover_result_hash_ = hash;
      cover_result_ok_ = result.ok;
      cover_result_url_ = result.url;
      cover_result_reason_ = result.reason;
      cover_result_detail_ = result.detail;
      cover_result_host_ = result.host;
      cover_result_ready_ = true;
    }
  }
}

void AimpDiscordPresence::StopCoverWorker() {
  {
    std::lock_guard<std::mutex> lock(cover_mutex_);
    cover_stopping_ = true;
  }
  cover_wake_.notify_all();
  if (cover_worker_.joinable()) {
    cover_worker_.join();
  }
}

void AimpDiscordPresence::LogCover(const std::string& line) {
  if (client_) {
    client_->LogLine(line);
  }
}

LocalArt::Result AimpDiscordPresence::ExtractLocalArt(const TrackInfo& info,
                                                      bool want_bytes) {
  LocalArt::Result result;

  Aimp::Player::Service::Player player;
  const Aimp::FileManager::FileInfo file_info = player.GetInfo();
  IAIMPFileInfo* raw_info = file_info.get();

  const std::wstring file =
      file_info.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kFileName);
  if (raw_info == nullptr || file.empty()) {
    return result;
  }

  IAIMPServiceAlbumArt* service =
      Aimp::Detail::QueryService<IAIMPServiceAlbumArt>(IID_IAIMPServiceAlbumArt);
  if (service == nullptr) {
    return result;
  }

  const LocalArt::Result art = LocalArt::Extract(service, raw_info, want_bytes);
  service->Release();

  std::string online_url;
  {
    std::lock_guard<std::mutex> lock(presence_mutex_);
    online_url = artwork_url_;
  }

  std::string line = "local-art track=\"" + info.artist + " - " + info.title +
                     "\" album=\"" + info.album + "\" file=\"" + Utils::ToString(file) + "\"";
  LocalArt::Result sidecar_art;
  if (art.found) {
    line += " found=1 source=sdk size=" + std::to_string(art.size) + " sha256=" + art.sha256_hex +
            " format=" + art.format + " aimp_format=" + std::to_string(art.aimp_format) +
            " dims=" + std::to_string(art.width) + "x" + std::to_string(art.height);
    line += " provider=offline-only flags=WAITFOR|OFFLINE|ORIGINAL|NOCACHE";
  } else {
    // AIMP's own offline providers found nothing (the multi-disc case: the art
    // lives in an ancestor folder). Fall back to a conventional sidecar file,
    // then hand its bytes down the exact same path the SDK result would take
    // (same size cap, same format gate, same publish).
    const std::wstring sidecar = LocalArt::FindSidecarArt(file);
    if (!sidecar.empty() && want_bytes) {
      sidecar_art = LocalArt::LoadSidecarBytes(sidecar);
    }

    line += " found=" + std::to_string(sidecar_art.found ? 1 : 0);
    if (sidecar_art.found) {
      line += " source=sidecar path=\"" + Utils::ToString(sidecar) + "\"";
      line += " size=" + std::to_string(sidecar_art.size) + " sha256=" + sidecar_art.sha256_hex +
              " format=" + sidecar_art.format +
              " aimp_format=" + std::to_string(sidecar_art.aimp_format) +
              " dims=" + std::to_string(sidecar_art.width) + "x" +
              std::to_string(sidecar_art.height);
    } else {
      line += " source=sdk";
      if (!sidecar.empty()) {
        line += " sidecar_path=\"" + Utils::ToString(sidecar) + "\" sidecar_found=0";
      }
    }
    line += " provider=offline-only flags=WAITFOR|OFFLINE|ORIGINAL|NOCACHE";
  }
  line += " online_url=\"";
  line += online_url;
  line += "\"";

  LogCover(line);
  return art.found ? art : sidecar_art;
}

void AimpDiscordPresence::SendActivity() {
  DiscordIpc::Activity snapshot;
  {
    std::lock_guard<std::mutex> lock(presence_mutex_);
    snapshot = activity_;
  }

  Aimp::Player::Service::Player player;
  sent_position_ = player.Position();
  sent_at_seconds_ = UnixSecondsNow();

  if (client_) {
    client_->SetActivity(snapshot);
  }
}
