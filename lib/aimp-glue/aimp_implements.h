//  In-repo replacement for the retired aimp-sdk-cpp-wrapper facade.
//
//  Provides the Aimp::Implements<> helper that gives a plugin class the
//  IUnknown bookkeeping (QueryInterface / AddRef / Release) for the AIMP
//  interfaces it inherits, on top of the official AIMP SDK headers that ship
//  in lib/aimp-sdk/Sources/Cpp.

#ifndef AIMP_GLUE_AIMP_IMPLEMENTS_H_
#define AIMP_GLUE_AIMP_IMPLEMENTS_H_

#include <windows.h>
#include <unknwn.h>

#include <tuple>
#include <type_traits>

namespace Aimp {

namespace Detail {

// Detects whether an interface wrapper publishes a real interface id.
// IAIMPPlugin has no IID in the AIMP SDK (plugins are handed to AIMP through
// the AIMPPluginGetHeader export), so the Aimp::Plugin wrapper deliberately
// does not declare iid() and is skipped by QueryInterface instead of having a
// fabricated GUID invented for it.
template <typename TInterface, typename = void>
struct HasIID : std::false_type {};

template <typename TInterface>
struct HasIID<TInterface, std::void_t<decltype(TInterface::iid())>> : std::true_type {};

}  // namespace Detail

// Reference counted implementation of one or more AIMP extension interfaces.
template <typename... TInterfaces>
class Implements : public TInterfaces... {
 public:
  Implements() = default;
  virtual ~Implements() = default;

  HRESULT WINAPI QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;

    if (riid == IID_IUnknown) {
      *object = static_cast<std::tuple_element_t<0, std::tuple<TInterfaces...>>*>(this);
      AddRef();
      return S_OK;
    }

    (((void)TryQuery<TInterfaces>(riid, object)), ...);
    return *object != nullptr ? S_OK : E_NOINTERFACE;
  }

  ULONG WINAPI AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&reference_count_));
  }

  ULONG WINAPI Release() override {
    const LONG remaining = InterlockedDecrement(&reference_count_);
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

 private:
  template <typename TInterface>
  void TryQuery(REFIID riid, void** object) {
    if constexpr (Detail::HasIID<TInterface>::value) {
      if (*object == nullptr && riid == TInterface::iid()) {
        *object = static_cast<TInterface*>(this);
        AddRef();
      }
    }
  }

  LONG reference_count_ = 1;
};

}  // namespace Aimp

#endif  // AIMP_GLUE_AIMP_IMPLEMENTS_H_
