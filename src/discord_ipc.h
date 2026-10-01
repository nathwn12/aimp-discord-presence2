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

#ifndef AIMPDISCORDPRESENCE_SRC_DISCORD_IPC_H_
#define AIMPDISCORDPRESENCE_SRC_DISCORD_IPC_H_

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// Minimal, self-contained Discord local RPC client.
//
// Speaks the local IPC protocol directly over the \\.\pipe\discord-ipc-{0..9}
// named pipes, so the plugin does not depend on the abandoned discord-rpc
// library (which cannot express the activity `type` field).
//
// Discord must already be running; this client never launches it. A background
// worker keeps the connection alive, reconnects with a capped backoff and
// coalesces throttled updates so the caller can never exceed Discord's limit
// of 5 presence updates per 20 seconds.
//
// All pipe I/O uses overlapped operations with a bounded wait, so a stalled
// Discord turns into a disconnect instead of blocking AIMP's message thread.
namespace DiscordIpc {

// Discord activity schema limits (characters, i.e. UTF-8 codepoints).
constexpr size_t kTextFieldMinLength = 2;
constexpr size_t kTextFieldMaxLength = 128;
constexpr size_t kImageFieldMaxLength = 300;

// Discord allows 5 presence updates per 20 seconds.
constexpr size_t kRateLimitUpdates = 5;
constexpr int kRateLimitWindowSeconds = 20;

// Reconnection backoff.
constexpr int kReconnectDelaySeconds = 5;
constexpr int kReconnectDelayMaxSeconds = 30;

enum class ActivityType : int {
  kPlaying = 0,
  kListening = 2,
  kWatching = 3,
  kCompeting = 5,
};

enum class StatusDisplayType : int {
  kName = 0,
  kState = 1,
  kDetails = 2,
};

struct Activity {
  int type = static_cast<int>(ActivityType::kListening);
  std::string details;  // track title
  std::string state;    // artist
  std::string large_image;
  std::string large_text;
  std::string small_image;
  std::string small_text;
  int64_t start_timestamp = 0;  // unix seconds, 0 omits the field
  int64_t end_timestamp = 0;    // unix seconds, 0 omits the field
  int status_display_type = static_cast<int>(StatusDisplayType::kDetails);
};

// --- Pure helpers, unit tested separately from the pipe plumbing -----------

// Escapes a UTF-8 string for a JSON string literal. Invalid UTF-8 bytes are
// replaced with U+FFFD so the payload is always valid UTF-8.
std::string EscapeJsonString(const std::string& utf8);

// Truncates valid UTF-8 to at most max_codepoints codepoints without ever
// splitting a multi-byte sequence.
std::string TruncateUtf8(const std::string& utf8, size_t max_codepoints);

// Normalizes a text field: empty stays empty, one codepoint is padded to the
// schema minimum, longer values are truncated to the schema maximum.
std::string NormalizeTextField(const std::string& utf8,
                               size_t max_codepoints = kTextFieldMaxLength);

std::string BuildHandshakePayload(const std::string& application_id,
                                  const std::string& nonce);
std::string BuildActivityJson(const Activity& activity);
std::string BuildSetActivityPayload(const Activity& activity, int64_t pid,
                                    const std::string& nonce, bool include_activity);

// --- Connection ------------------------------------------------------------

class Client {
 public:
  explicit Client(const std::string& application_id);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Requests a connection attempt when disconnected and reports whether the
  // pipe is currently usable. Never blocks.
  bool EnsureConnected();

  // Queues an activity update. The worker sends it as soon as the rate limit
  // window allows; rapid calls coalesce onto the latest value.
  bool SetActivity(const Activity& activity);

  // Queues a presence clear (SET_ACTIVITY without an activity object).
  void Clear();

  // Stops the worker and closes the pipe.
  void Shutdown();

  bool Connected() const { return connected_.load(); }

 private:
  void WorkerMain();
  bool Connect();
  void Disconnect();
  bool WriteFrame(uint32_t opcode, const std::string& payload);
  // Overlapped pipe I/O with a bounded wait; a timeout cancels the request and
  // reports failure so the caller disconnects instead of blocking forever.
  bool WriteAll(const char* data, size_t size);
  bool ReadChunk(char* buffer, DWORD size, DWORD* read);
  bool PipeAlive();
  void DrainPipe();
  void SendPending(std::chrono::steady_clock::time_point now);
  bool WindowHasRoom(std::chrono::steady_clock::time_point now);
  std::chrono::steady_clock::time_point NextDeadline(
      std::chrono::steady_clock::time_point now);

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::thread worker_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  bool connected_now_ = false;
  std::atomic<bool> connected_{false};
  std::string application_id_;

  bool has_pending_ = false;
  bool pending_is_clear_ = false;
  bool had_activity_ = false;
  Activity pending_;

  std::chrono::steady_clock::time_point next_connect_attempt_{};
  std::chrono::steady_clock::time_point next_liveness_check_{};
  int reconnect_delay_seconds_ = kReconnectDelaySeconds;

  std::deque<std::chrono::steady_clock::time_point> send_history_;
};

}  // namespace DiscordIpc

#endif  // AIMPDISCORDPRESENCE_SRC_DISCORD_IPC_H_
