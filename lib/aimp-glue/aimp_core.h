//  In-repo replacement for the retired aimp-sdk-cpp-wrapper facade.
//
//  Aimp::Core::Service::Config is a thin, reference counted handle over
//  IAIMPServiceConfig. The AIMP SDK recommends obtaining a service through
//  IAIMPCore::QueryInterface and not keeping it cached, so every handle here
//  queries on construction and releases on destruction.

#ifndef AIMP_GLUE_AIMP_CORE_H_
#define AIMP_GLUE_AIMP_CORE_H_

#include <windows.h>
#include <unknwn.h>

#include <cstdint>
#include <string>

#include "apiCore.h"
#include "apiObjects.h"

namespace Aimp {

namespace Detail {

// The IAIMPCore pointer handed to IAIMPPlugin::Initialize. Owned by AIMP, kept
// only for the lifetime of the plugin (set in Initialize, cleared in Finalize).
IAIMPCore* GetCore();
void SetCore(IAIMPCore* core);

template <typename TService>
TService* QueryService(const IID& iid) {
  IAIMPCore* core = GetCore();
  if (core == nullptr) {
    return nullptr;
  }

  TService* service = nullptr;
  if (FAILED(core->QueryInterface(iid, reinterpret_cast<void**>(&service)))) {
    return nullptr;
  }
  return service;
}

// Typed config access. Returns true only when the value was read successfully,
// matching the plugin's "read, and write the default when the read fails" flow.
bool ConfigGet(IAIMPServiceConfig* service, const std::wstring& key, bool* value);
bool ConfigGet(IAIMPServiceConfig* service, const std::wstring& key, int* value);
bool ConfigGet(IAIMPServiceConfig* service, const std::wstring& key, int64_t* value);
bool ConfigGet(IAIMPServiceConfig* service, const std::wstring& key, std::wstring* value);

void ConfigSet(IAIMPServiceConfig* service, const std::wstring& key, bool value);
void ConfigSet(IAIMPServiceConfig* service, const std::wstring& key, int value);
void ConfigSet(IAIMPServiceConfig* service, const std::wstring& key, int64_t value);
void ConfigSet(IAIMPServiceConfig* service, const std::wstring& key, const std::wstring& value);

}  // namespace Detail

namespace Core {
namespace Service {

class Config {
 public:
  Config();
  Config(const Config& other);
  Config& operator=(const Config& other);
  ~Config();

  explicit operator bool() const { return service_ != nullptr; }

  template <typename T>
  bool Get(const std::wstring& key, T* value) {
    return Detail::ConfigGet(service_, key, value);
  }

  template <typename T>
  void Set(const std::wstring& key, const T& value) {
    Detail::ConfigSet(service_, key, value);
  }

 private:
  IAIMPServiceConfig* service_ = nullptr;
};

}  // namespace Service
}  // namespace Core

}  // namespace Aimp

#endif  // AIMP_GLUE_AIMP_CORE_H_
