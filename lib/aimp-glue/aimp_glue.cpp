//  In-repo replacement for the retired aimp-sdk-cpp-wrapper facade.
//
//  Implements the service handles (IAIMPServiceConfig, IAIMPServicePlayer,
//  IAIMPServiceMessageDispatcher) declared by the aimp-glue headers on top of
//  the official AIMP SDK interfaces, plus the message hook registry.

#include <windows.h>
#include <unknwn.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "aimp_core.h"
#include "aimp_messages.h"
#include "aimp_plugin.h"

namespace Aimp {

namespace {

// ---------------------------------------------------------------------------
// Core pointer and message hook registry
// ---------------------------------------------------------------------------

std::atomic<IAIMPCore*> g_core{nullptr};

struct HookEntry {
  IAIMPServiceMessageDispatcher* service = nullptr;
  class MessageHook* hook = nullptr;
};

std::mutex& HookMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<HookEntry>& HookList() {
  static std::vector<HookEntry> hooks;
  return hooks;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

class ScopedString {
 public:
  explicit ScopedString(const std::wstring& value) {
    IAIMPCore* core = g_core.load(std::memory_order_acquire);
    if (core == nullptr) {
      return;
    }

    IAIMPString* string = nullptr;
    if (FAILED(core->CreateObject(IID_IAIMPString, reinterpret_cast<void**>(&string)))) {
      return;
    }

    std::wstring buffer = value;
    if (FAILED(string->SetData(buffer.data(), static_cast<int>(buffer.size())))) {
      string->Release();
      return;
    }
    string_ = string;
  }

  ScopedString(const ScopedString&) = delete;
  ScopedString& operator=(const ScopedString&) = delete;

  ~ScopedString() {
    if (string_ != nullptr) {
      string_->Release();
    }
  }

  IAIMPString* get() const { return string_; }

 private:
  IAIMPString* string_ = nullptr;
};

std::wstring ToWString(IAIMPString* string) {
  if (string == nullptr) {
    return std::wstring();
  }
  WCHAR* data = string->GetData();
  const int length = string->GetLength();
  if (data == nullptr || length <= 0) {
    return std::wstring();
  }
  return std::wstring(data, static_cast<size_t>(length));
}

}  // namespace

namespace Detail {

IAIMPCore* GetCore() { return g_core.load(std::memory_order_acquire); }

void SetCore(IAIMPCore* core) { g_core.store(core, std::memory_order_release); }

std::wstring FileInfoGetString(IAIMPFileInfo* info, int property_id) {
  if (info == nullptr) {
    return std::wstring();
  }

  IAIMPString* string = nullptr;
  if (FAILED(info->GetValueAsObject(property_id, IID_IAIMPString,
                                    reinterpret_cast<void**>(&string)))) {
    return std::wstring();
  }

  const std::wstring result = ToWString(string);
  if (string != nullptr) {
    string->Release();
  }
  return result;
}

bool ConfigGet(IAIMPServiceConfig* service, const std::wstring& key, std::wstring* value) {
  if (service == nullptr || value == nullptr) {
    return false;
  }

  ScopedString path(key);
  if (path.get() == nullptr) {
    return false;
  }

  IAIMPString* string = nullptr;
  if (FAILED(service->GetValueAsString(path.get(), &string)) || string == nullptr) {
    return false;
  }

  *value = ToWString(string);
  string->Release();
  return true;
}

bool ConfigGet(IAIMPServiceConfig* service, const std::wstring& key, int64_t* value) {
  if (service == nullptr || value == nullptr) {
    return false;
  }

  ScopedString path(key);
  if (path.get() == nullptr) {
    return false;
  }

  INT64 result = 0;
  if (FAILED(service->GetValueAsInt64(path.get(), &result))) {
    return false;
  }

  *value = static_cast<int64_t>(result);
  return true;
}

bool ConfigGet(IAIMPServiceConfig* service, const std::wstring& key, int* value) {
  if (service == nullptr || value == nullptr) {
    return false;
  }

  ScopedString path(key);
  if (path.get() == nullptr) {
    return false;
  }

  int result = 0;
  if (FAILED(service->GetValueAsInt32(path.get(), &result))) {
    return false;
  }

  *value = result;
  return true;
}

bool ConfigGet(IAIMPServiceConfig* service, const std::wstring& key, bool* value) {
  int result = 0;
  if (!ConfigGet(service, key, &result)) {
    return false;
  }

  *value = result != 0;
  return true;
}

void ConfigSet(IAIMPServiceConfig* service, const std::wstring& key, const std::wstring& value) {
  if (service == nullptr) {
    return;
  }

  ScopedString path(key);
  if (path.get() == nullptr) {
    return;
  }

  IAIMPCore* core = g_core.load(std::memory_order_acquire);
  if (core == nullptr) {
    return;
  }

  IAIMPString* string = nullptr;
  if (FAILED(core->CreateObject(IID_IAIMPString, reinterpret_cast<void**>(&string)))) {
    return;
  }

  std::wstring buffer = value;
  if (SUCCEEDED(string->SetData(buffer.data(), static_cast<int>(buffer.size())))) {
    service->SetValueAsString(path.get(), string);
  }
  string->Release();
}

void ConfigSet(IAIMPServiceConfig* service, const std::wstring& key, int value) {
  if (service == nullptr) {
    return;
  }

  ScopedString path(key);
  if (path.get() != nullptr) {
    service->SetValueAsInt32(path.get(), value);
  }
}

void ConfigSet(IAIMPServiceConfig* service, const std::wstring& key, int64_t value) {
  if (service == nullptr) {
    return;
  }

  ScopedString path(key);
  if (path.get() != nullptr) {
    service->SetValueAsInt64(path.get(), static_cast<INT64>(value));
  }
}

void ConfigSet(IAIMPServiceConfig* service, const std::wstring& key, bool value) {
  ConfigSet(service, key, value ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Message hooks
// ---------------------------------------------------------------------------

}  // namespace Detail

namespace {

class MessageHook final : public IAIMPMessageHook {
 public:
  MessageHook(int event, std::function<void(DWORD, int, void*, HRESULT*)> callback)
      : event_(static_cast<DWORD>(event)), callback_(std::move(callback)) {}

  HRESULT WINAPI QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;

    if (riid == IID_IUnknown || riid == IID_IAIMPMessageHook) {
      *object = static_cast<IAIMPMessageHook*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG WINAPI AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&reference_count_)); }

  ULONG WINAPI Release() override {
    const LONG remaining = InterlockedDecrement(&reference_count_);
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  void WINAPI CoreMessage(DWORD message, int param1, void* param2, HRESULT* result) override {
    if (message == event_ && callback_) {
      callback_(message, param1, param2, result);
    }
  }

 private:
  ~MessageHook() = default;

  LONG reference_count_ = 1;
  DWORD event_ = 0;
  std::function<void(DWORD, int, void*, HRESULT*)> callback_;
};

}  // namespace

namespace Detail {

void HookRegister(IAIMPServiceMessageDispatcher* service, int event,
                  const std::function<void(DWORD, int, void*, HRESULT*)>& callback) {
  if (service == nullptr) {
    return;
  }

  MessageHook* hook = new MessageHook(event, callback);
  if (FAILED(service->Hook(hook))) {
    hook->Release();
    return;
  }

  const std::lock_guard<std::mutex> lock(HookMutex());
  HookList().push_back(HookEntry{service, hook});
}

}  // namespace Detail

namespace Core {
namespace Service {

Config::Config() : service_(Detail::QueryService<IAIMPServiceConfig>(IID_IAIMPServiceConfig)) {}

Config::Config(const Config& other) : service_(other.service_) {
  if (service_ != nullptr) {
    service_->AddRef();
  }
}

Config& Config::operator=(const Config& other) {
  if (this != &other) {
    if (other.service_ != nullptr) {
      other.service_->AddRef();
    }
    if (service_ != nullptr) {
      service_->Release();
    }
    service_ = other.service_;
  }
  return *this;
}

Config::~Config() {
  if (service_ != nullptr) {
    service_->Release();
  }
}

}  // namespace Service
}  // namespace Core

namespace Messages {
namespace Service {

MessageDispatcher::MessageDispatcher()
    : service_(Detail::QueryService<IAIMPServiceMessageDispatcher>(IID_IAIMPServiceMessageDispatcher)) {}

MessageDispatcher::~MessageDispatcher() {
  if (service_ != nullptr) {
    service_->Release();
  }
}

void MessageDispatcher::UnhookAll() {
  std::vector<HookEntry> entries;
  {
    const std::lock_guard<std::mutex> lock(HookMutex());
    entries.swap(HookList());
  }

  for (const HookEntry& entry : entries) {
    if (entry.service != nullptr && entry.hook != nullptr) {
      entry.service->Unhook(entry.hook);
    }
    if (entry.hook != nullptr) {
      entry.hook->Release();
    }
  }
}

}  // namespace Service
}  // namespace Messages

HRESULT WINAPI Plugin::Initialize(IAIMPCore* Core) {
  Detail::SetCore(Core);
  if (!Load()) {
    Detail::SetCore(nullptr);
    return E_FAIL;
  }
  return S_OK;
}

HRESULT WINAPI Plugin::Finalize() {
  const bool unloaded = Unload();
  Detail::SetCore(nullptr);
  return unloaded ? S_OK : E_FAIL;
}

}  // namespace Aimp
