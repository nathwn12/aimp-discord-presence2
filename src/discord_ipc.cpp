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

#include "discord_ipc.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace DiscordIpc {
namespace {

constexpr uint32_t kOpcodeHandshake = 0;
constexpr uint32_t kOpcodeFrame = 1;
constexpr uint32_t kOpcodeClose = 2;

constexpr int kLivenessCheckSeconds = 10;
constexpr DWORD kPipeReadChunk = 4096;
constexpr size_t kMaxPipeDrainBytes = 64 * 1024;
constexpr char kReplacementChar[] = "\xEF\xBF\xBD";  // U+FFFD

bool IsContinuationByte(unsigned char value) { return (value & 0xC0) == 0x80; }

// Length in bytes of the UTF-8 sequence starting at `index`, or 1 when the
// sequence is invalid or truncated (callers replace such bytes).
size_t Utf8SequenceLength(const std::string& input, size_t index) {
  const unsigned char lead = static_cast<unsigned char>(input[index]);
  const size_t remaining = input.size() - index;

  if (lead < 0x80) {
    return 1;
  }
  if ((lead & 0xE0) == 0xC0) {
    if (remaining >= 2 && IsContinuationByte(static_cast<unsigned char>(input[index + 1]))) {
      return 2;
    }
    return 1;
  }
  if ((lead & 0xF0) == 0xE0) {
    if (remaining >= 3 && IsContinuationByte(static_cast<unsigned char>(input[index + 1])) &&
        IsContinuationByte(static_cast<unsigned char>(input[index + 2]))) {
      return 3;
    }
    return 1;
  }
  if ((lead & 0xF8) == 0xF0) {
    if (remaining >= 4 && IsContinuationByte(static_cast<unsigned char>(input[index + 1])) &&
        IsContinuationByte(static_cast<unsigned char>(input[index + 2])) &&
        IsContinuationByte(static_cast<unsigned char>(input[index + 3]))) {
      return 4;
    }
    return 1;
  }
  return 1;
}

void AppendUint32(std::vector<char>& buffer, uint32_t value) {
  buffer.push_back(static_cast<char>(value & 0xFF));
  buffer.push_back(static_cast<char>((value >> 8) & 0xFF));
  buffer.push_back(static_cast<char>((value >> 16) & 0xFF));
  buffer.push_back(static_cast<char>((value >> 24) & 0xFF));
}

std::string NextNonce() {
  static std::atomic<uint64_t> counter{0};
  const uint64_t value = counter.fetch_add(1);
  const uint64_t ticks = static_cast<uint64_t>(GetTickCount64());
  return std::to_string(ticks) + "-" + std::to_string(value);
}

void AppendEscapedField(std::string& json, const char* key, const std::string& value) {
  json += ",\"";
  json += key;
  json += "\":\"";
  json += EscapeJsonString(value);
  json += "\"";
}

void AppendTextField(std::string& json, const char* key, const std::string& value) {
  const std::string normalized = NormalizeTextField(value, kTextFieldMaxLength);
  if (normalized.empty()) {
    return;
  }
  AppendEscapedField(json, key, normalized);
}

void AppendImageField(std::string& json, const char* key, const std::string& value) {
  const std::string normalized = TruncateUtf8(value, kImageFieldMaxLength);
  if (normalized.empty()) {
    return;
  }
  AppendEscapedField(json, key, normalized);
}

}  // namespace

std::string EscapeJsonString(const std::string& utf8) {
  static const char* kHexDigits = "0123456789abcdef";

  std::string escaped;
  escaped.reserve(utf8.size());

  for (size_t i = 0; i < utf8.size();) {
    const unsigned char value = static_cast<unsigned char>(utf8[i]);

    if (value == '"' || value == '\\') {
      escaped.push_back('\\');
      escaped.push_back(static_cast<char>(value));
      ++i;
    } else if (value < 0x20) {
      escaped += "\\u00";
      escaped.push_back(kHexDigits[(value >> 4) & 0x0F]);
      escaped.push_back(kHexDigits[value & 0x0F]);
      ++i;
    } else if (value < 0x80) {
      escaped.push_back(static_cast<char>(value));
      ++i;
    } else {
      const size_t length = Utf8SequenceLength(utf8, i);
      if (length > 1) {
        escaped.append(utf8, i, length);
        i += length;
      } else {
        escaped += kReplacementChar;
        ++i;
      }
    }
  }

  return escaped;
}

std::string TruncateUtf8(const std::string& utf8, size_t max_codepoints) {
  size_t codepoints = 0;
  size_t index = 0;

  while (index < utf8.size() && codepoints < max_codepoints) {
    index += Utf8SequenceLength(utf8, index);
    ++codepoints;
  }

  return utf8.substr(0, index);
}

std::string NormalizeTextField(const std::string& utf8, size_t max_codepoints) {
  if (utf8.empty()) {
    return std::string();
  }

  std::string normalized = TruncateUtf8(utf8, max_codepoints);
  if (normalized.empty()) {
    return normalized;
  }

  // Discord rejects details/state/assets text shorter than 2 characters, so a
  // single-character value is padded instead of being dropped.
  if (TruncateUtf8(normalized, 1).size() == normalized.size()) {
    normalized.push_back(' ');
  }

  return normalized;
}

std::string BuildHandshakePayload(const std::string& application_id,
                                  const std::string& nonce) {
  std::string payload = "{\"cmd\":\"HANDSHAKE\",\"nonce\":\"";
  payload += EscapeJsonString(nonce);
  payload += "\",\"args\":{\"v\":1,\"client_id\":\"";
  payload += EscapeJsonString(application_id);
  payload += "\"}}";
  return payload;
}

std::string BuildActivityJson(const Activity& activity) {
  std::string json = "{\"type\":";
  json += std::to_string(activity.type);

  AppendTextField(json, "details", activity.details);
  AppendTextField(json, "state", activity.state);

  if (activity.start_timestamp > 0 || activity.end_timestamp > 0) {
    json += ",\"timestamps\":{";
    bool needs_comma = false;
    if (activity.start_timestamp > 0) {
      json += "\"start\":";
      json += std::to_string(activity.start_timestamp);
      needs_comma = true;
    }
    if (activity.end_timestamp > 0) {
      if (needs_comma) {
        json += ",";
      }
      json += "\"end\":";
      json += std::to_string(activity.end_timestamp);
    }
    json += "}";
  }

  std::string assets;
  AppendImageField(assets, "large_image", activity.large_image);
  AppendTextField(assets, "large_text", activity.large_text);
  AppendImageField(assets, "small_image", activity.small_image);
  AppendTextField(assets, "small_text", activity.small_text);
  if (!assets.empty()) {
    json += ",\"assets\":{";
    json += assets.substr(1);  // drop the leading comma the helpers prepend
    json += "}";
  }

  if (activity.status_display_type >= 0 && activity.status_display_type <= 2) {
    json += ",\"status_display_type\":";
    json += std::to_string(activity.status_display_type);
  }

  json += "}";
  return json;
}

std::string BuildSetActivityPayload(const Activity& activity, int64_t pid,
                                    const std::string& nonce, bool include_activity) {
  std::string args = "{\"pid\":";
  args += std::to_string(pid);
  if (include_activity) {
    args += ",\"activity\":";
    args += BuildActivityJson(activity);
  }
  args += "}";

  std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"";
  payload += EscapeJsonString(nonce);
  payload += "\",\"args\":";
  payload += args;
  payload += "}";
  return payload;
}

std::string BuildClosePayload() {
  return "{\"cmd\":\"CLOSE\",\"nonce\":\"" + EscapeJsonString(NextNonce()) + "\"}";
}

Client::Client(const std::string& application_id) : application_id_(application_id) {
  next_connect_attempt_ = std::chrono::steady_clock::now();
  worker_ = std::thread(&Client::WorkerMain, this);
}

Client::~Client() { Shutdown(); }

bool Client::EnsureConnected() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_now_) {
      const auto now = std::chrono::steady_clock::now();
      if (next_connect_attempt_ > now) {
        next_connect_attempt_ = now;
      }
    }
  }
  wake_.notify_all();
  return connected_.load();
}

void Client::SetApplicationId(const std::string& application_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (application_id_ == application_id) {
      return;
    }
    application_id_ = application_id;
    Disconnect();
    next_connect_attempt_ = std::chrono::steady_clock::now();
  }
  wake_.notify_all();
}

bool Client::SetActivity(const Activity& activity) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return false;
    }
    pending_ = activity;
    pending_is_clear_ = false;
    has_pending_ = true;
    had_activity_ = true;

    const auto now = std::chrono::steady_clock::now();
    if (!connected_now_ && next_connect_attempt_ > now) {
      next_connect_attempt_ = now;
    }
  }
  wake_.notify_all();
  return true;
}

void Client::Clear() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    pending_ = Activity();
    pending_is_clear_ = true;
    has_pending_ = true;
    had_activity_ = false;

    const auto now = std::chrono::steady_clock::now();
    if (!connected_now_ && next_connect_attempt_ > now) {
      next_connect_attempt_ = now;
    }
  }
  wake_.notify_all();
}

void Client::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  wake_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }

  if (pipe_ != INVALID_HANDLE_VALUE) {
    WriteFrame(kOpcodeClose, BuildClosePayload());
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  connected_now_ = false;
  connected_.store(false);
}

bool Client::Connect() {
  static const wchar_t kPipePrefix[] = L"\\\\.\\pipe\\discord-ipc-";

  for (int index = 0; index < 10; ++index) {
    const std::wstring pipe_name = std::wstring(kPipePrefix) + std::to_wstring(index);
    const HANDLE handle = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      continue;
    }

    pipe_ = handle;
    if (!WriteFrame(kOpcodeHandshake, BuildHandshakePayload(application_id_, NextNonce()))) {
      Disconnect();
      continue;
    }

    DrainPipe();
    connected_now_ = true;
    connected_.store(true);
    reconnect_delay_seconds_ = kReconnectDelaySeconds;
    next_liveness_check_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(kLivenessCheckSeconds);

    // Self-heal: after a reconnect (for example when Discord was restarted),
    // push the last known activity again, since Discord forgot it.
    if (had_activity_) {
      has_pending_ = true;
      pending_is_clear_ = false;
    }
    return true;
  }

  next_connect_attempt_ = std::chrono::steady_clock::now() +
                          std::chrono::seconds(reconnect_delay_seconds_);
  reconnect_delay_seconds_ = (std::min)(reconnect_delay_seconds_ * 2, kReconnectDelayMaxSeconds);
  return false;
}

void Client::Disconnect() {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  if (connected_now_) {
    connected_now_ = false;
    connected_.store(false);
    // A dropped connection usually means Discord restarted: re-arm quickly.
    next_connect_attempt_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(kReconnectDelaySeconds);
    reconnect_delay_seconds_ = kReconnectDelaySeconds;
  }
}

bool Client::WriteFrame(uint32_t opcode, const std::string& payload) {
  if (pipe_ == INVALID_HANDLE_VALUE || payload.size() > 0xFFFFFFFFull) {
    return false;
  }

  std::vector<char> frame;
  frame.reserve(payload.size() + 8);
  AppendUint32(frame, opcode);
  AppendUint32(frame, static_cast<uint32_t>(payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());

  DWORD written = 0;
  const BOOL ok = WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr);
  if (!ok || written != static_cast<DWORD>(frame.size())) {
    return false;
  }
  return true;
}

bool Client::PipeAlive() {
  if (pipe_ == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD available = 0;
  if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
    return false;
  }

  if (available > 0) {
    DrainPipe();
  }
  return true;
}

void Client::DrainPipe() {
  if (pipe_ == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD available = 0;
  if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
    return;
  }

  std::vector<char> scratch;
  size_t drained = 0;
  while (available > 0 && drained < kMaxPipeDrainBytes) {
    const DWORD chunk = (std::min)(available, kPipeReadChunk);
    scratch.resize(chunk);
    DWORD read = 0;
    if (!ReadFile(pipe_, scratch.data(), chunk, &read, nullptr) || read == 0) {
      break;
    }
    drained += read;
    if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
      break;
    }
  }
}

bool Client::WindowHasRoom(std::chrono::steady_clock::time_point now) {
  const auto window = std::chrono::seconds(kRateLimitWindowSeconds);
  while (!send_history_.empty() && now - send_history_.front() >= window) {
    send_history_.pop_front();
  }
  return send_history_.size() < kRateLimitUpdates;
}

void Client::SendPending(std::chrono::steady_clock::time_point now) {
  const std::string payload = BuildSetActivityPayload(
      pending_, static_cast<int64_t>(GetCurrentProcessId()), NextNonce(), !pending_is_clear_);

  if (!WriteFrame(kOpcodeFrame, payload)) {
    Disconnect();
    return;
  }

  has_pending_ = false;
  send_history_.push_back(now);
}

std::chrono::steady_clock::time_point Client::NextDeadline(
    std::chrono::steady_clock::time_point now) {
  auto deadline = now + std::chrono::hours(1);

  if (!connected_now_) {
    deadline = (std::min)(deadline, next_connect_attempt_);
  } else {
    deadline = (std::min)(deadline, next_liveness_check_);
    if (has_pending_) {
      if (WindowHasRoom(now) || send_history_.empty()) {
        deadline = now;
      } else {
        deadline = (std::min)(deadline, send_history_.front() +
                                           std::chrono::seconds(kRateLimitWindowSeconds));
      }
    }
  }

  return deadline;
}

void Client::WorkerMain() {
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_) {
      break;
    }

    const auto now = std::chrono::steady_clock::now();

    if (connected_now_ && now >= next_liveness_check_) {
      next_liveness_check_ = now + std::chrono::seconds(kLivenessCheckSeconds);
      if (!PipeAlive()) {
        Disconnect();
      }
    }

    if (!connected_now_ && now >= next_connect_attempt_) {
      Connect();
    }

    if (connected_now_ && has_pending_ && WindowHasRoom(std::chrono::steady_clock::now())) {
      SendPending(std::chrono::steady_clock::now());
      continue;
    }

    const auto deadline = NextDeadline(std::chrono::steady_clock::now());
    wake_.wait_until(lock, deadline);
  }

  Disconnect();
}

}  // namespace DiscordIpc
