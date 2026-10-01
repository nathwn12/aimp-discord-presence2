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

#include "aimp_core.h"
#include "aimp_filemanager.h"
#include "aimp_messages.h"
#include "aimp_player.h"
#include "presence_layout.h"
#include "utils.h"

namespace {

constexpr int kPlayerStateStopped = 0;
constexpr int kPlayerStatePaused = 1;
constexpr int kPlayerStatePlaying = 2;

// A position difference larger than this means the user seeked and the
// timestamp Discord is counting down locally has to be corrected.
constexpr double kSeekDriftSeconds = 3.0;

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

  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&ResolveDebugLogPath),
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

  return directory.substr(0, separator + 1) + Utils::ToString(configured);
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

  album_art_.SetCallback(
      [this](const std::string& artist, const std::string& album, const std::string& url) {
        ApplyResolvedArtwork(artist, album, url);
      });
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

  info.title = Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kTitle));
  info.artist = Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kArtist));
  info.album = Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kAlbum));
  info.is_url = IsStreamUrl(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kFileName));
  info.key = info.artist + "\n" + info.album + "\n" + info.title;

  return info;
}

void AimpDiscordPresence::RefreshPresence(bool request_artwork) {
  const TrackInfo info = ReadTrackInfo();

  {
    std::lock_guard<std::mutex> lock(presence_mutex_);
    SetInfo(info);
    SetSmallImage(info);
    SetTimestamp(info);
  }

  track_key_ = info.key;
  track_artist_ = info.artist;
  track_album_ = info.album;

  if (request_artwork) {
    if (settings.use_albumart && settings.use_albumart_online && !info.artist.empty() &&
        !info.album.empty()) {
      // Non-blocking: the worker resolves the URL and publishes it, the next
      // notification applies it.
      album_art_.Request(info.artist, info.album);
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

  const bool art_matches =
      settings.use_albumart && !artwork_url_.empty() &&
      artwork_key_ == AlbumArt::BuildAlbumKey(info.artist, info.album);
  const std::string large_image =
      PresenceLayout::BuildLargeImage(art_matches ? artwork_url_ : std::string());
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
                                               const std::string& url) {
  // Runs on the album art worker thread; publishing the value is all that is
  // allowed here.
  std::lock_guard<std::mutex> lock(presence_mutex_);
  artwork_key_ = AlbumArt::BuildAlbumKey(artist, album);
  artwork_url_ = url;
}

bool AimpDiscordPresence::ApplyPendingArtwork() {
  std::lock_guard<std::mutex> lock(presence_mutex_);
  if (track_artist_.empty() || track_album_.empty() || artwork_url_.empty() ||
      artwork_url_ == last_large_image_) {
    return false;
  }

  // The shared builder keeps this comparison in step with SetInfo(); artwork
  // belongs to an album, not to the individual track on it.
  if (artwork_key_ != AlbumArt::BuildAlbumKey(track_artist_, track_album_)) {
    return false;
  }

  last_large_image_ = artwork_url_;
  activity_.large_image = artwork_url_;
  return true;
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
