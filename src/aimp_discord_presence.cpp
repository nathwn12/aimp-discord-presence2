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

  // Spotify-like layout: details is the track title, state is the artist.
  activity_.details = info.title;
  activity_.state = info.artist;
  activity_.large_text = info.album;

  std::string large_image = AlbumArt::kDefaultAssetKey;
  if (settings.use_albumart && !artwork_url_.empty() && artwork_key_ == info.key) {
    large_image = artwork_url_;
  }
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
  artwork_key_ = artist + "\n" + album;
  artwork_url_ = url;
}

bool AimpDiscordPresence::ApplyPendingArtwork() {
  std::lock_guard<std::mutex> lock(presence_mutex_);
  if (track_key_.empty() || artwork_key_ != track_key_ || artwork_url_.empty() ||
      artwork_url_ == last_large_image_) {
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
