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

#ifndef AIMPDISCORDPRESENCE_SRC_AIMP_DISCORD_PRESENCE_H_
#define AIMPDISCORDPRESENCE_SRC_AIMP_DISCORD_PRESENCE_H_

#include <memory>
#include <mutex>
#include <string>

#include "aimp_implements.h"
#include "aimp_plugin.h"
#include "aimp_core.h"

#include "album_art.h"
#include "discord_ipc.h"
#include "local_art.h"

class AimpDiscordPresence :
  public Aimp::Implements<Aimp::Plugin, Aimp::ExternalSettingsDialog> {
 public:
  PWCHAR GetInfo(int index) override;
  DWORD GetCategory() override;
  bool Load() override;
  bool Unload() override;
  void Notification(int id, IUnknown* data) override;
  void ShowSettings(HWND parent_wnd) override;

 private:
  struct TrackInfo {
    std::string key;
    std::string title;
    std::string artist;
    std::string album;
    bool is_url = false;
  };

  void OnStreamStartSubtrack();
  void OnPlayerState(DWORD message, int param1 = NULL);
  void OnPropertyValue(DWORD message, int param1 = NULL);
  void OnPlayerUpdatePosition();

 private:
  void InitializeMessageDispatcher();

 private:
  // Rebuilds and queues the Discord presence. `request_artwork` starts a new
  // album art lookup for the current track.
  void RefreshPresence(bool request_artwork);
  void SendActivity();

  TrackInfo ReadTrackInfo();

  // Fill the activity from AIMP's currently playing file. Must run on AIMP's
  // message thread.
  void SetInfo(const TrackInfo& info);
  void SetSmallImage(const TrackInfo& info, int state = -1);
  void SetTimestamp(const TrackInfo& info);

  // Called on the album art worker thread: only publishes the value.
  void ApplyResolvedArtwork(const std::string& artist, const std::string& album,
                            const std::string& url);
  // Called on AIMP's message thread: applies a newly resolved URL.
  bool ApplyPendingArtwork();

  // Called on AIMP's message thread for a new track: extracts the local
  // (tags/sidecar) cover just to fingerprint it in the DebugLog. Never
  // uploads, decodes or alters the image.
  void LogLocalArt(const TrackInfo& info);

 private:
  void LoadConfig();

  template <typename T>
  void LoadConfigValue(Aimp::Core::Service::Config config, const std::wstring& key, T value);

  struct Properties {
    int64_t application_id = 429559336982020107LL;
    bool timestamp = false;
    bool use_albumart = true;
    bool use_albumart_online = true;
    // What the member-list status line shows: 0 = name, 1 = state (album),
    // 2 = details (artist). Defaults to 2, the artist-first layout.
    int status_display_type = 2;
    // Optional file for the Discord IPC frame log. Empty (the default) leaves
    // logging off. A bare filename resolves next to the plugin DLL; an
    // absolute path is used as given.
    std::wstring debug_log;
    struct State {
      bool use_play = false;
      std::wstring play_image = L"aimp_play";
      bool use_pause = false;
      std::wstring pause_image = L"aimp_pause";
      bool use_radio = true;
      std::wstring radio_image = L"https://raw.githubusercontent.com/Exle/aimp-discord-presence/main/"
                           L".github/aimp_icons/animated/aimp_radio.gif";
    };
    State status;
  };

  Properties settings;

  std::unique_ptr<DiscordIpc::Client> client_;
  AlbumArt::Resolver album_art_;

  // Guards activity_ and the resolved artwork state, which is touched by the
  // album art worker thread.
  std::mutex presence_mutex_;
  DiscordIpc::Activity activity_;
  std::string artwork_key_;
  std::string artwork_url_;

  // AIMP message thread only.
  std::string track_key_;
  std::string track_artist_;
  std::string track_album_;
  std::string last_large_image_;
  // Last track whose local cover was fingerprinted; AIMP message thread only.
  std::string local_art_key_;
  double sent_position_ = 0.0;
  int64_t sent_at_seconds_ = 0;
  bool paused_ = false;
};

#endif  // AIMPDISCORDPRESENCE_SRC_AIMP_DISCORD_PRESENCE_H_
