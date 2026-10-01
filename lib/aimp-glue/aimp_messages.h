//  In-repo replacement for the retired aimp-sdk-cpp-wrapper facade.
//
//  Aimp::Messages::Service::MessageDispatcher registers IAIMPMessageHook
//  objects and demultiplexes the dispatcher's message stream by event id.
//
//  The plugin creates a short lived dispatcher object to register its hooks and
//  a separate one to unhook everything later, so the hooks outlive the handle
//  that registered them: they live in a process wide registry owned by
//  aimp_glue.cpp until UnhookAll is called.

#ifndef AIMP_GLUE_AIMP_MESSAGES_H_
#define AIMP_GLUE_AIMP_MESSAGES_H_

#include <windows.h>
#include <unknwn.h>

#include <functional>
#include <utility>

#include "aimp_core.h"
#include "apiMessages.h"

namespace Aimp {

namespace Detail {

void HookRegister(IAIMPServiceMessageDispatcher* service, int event,
                  const std::function<void(DWORD, int, void*, HRESULT*)>& callback);

}  // namespace Detail

namespace Messages {

namespace Events {

const int kPlayerState = AIMP_MSG_EVENT_PLAYER_STATE;
const int kPropertyValue = AIMP_MSG_EVENT_PROPERTY_VALUE;
// Fires every second by timer while a track plays; unlike the position
// property's AIMP_MSG_EVENT_PROPERTY_VALUE, which only fires when the user
// changes the position (see apiMessages.h).
const int kPlayerUpdatePosition = AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION;

namespace Stream {
namespace Start {
const int kSubtrack = AIMP_MSG_EVENT_STREAM_START_SUBTRACK;
}  // namespace Start
}  // namespace Stream

}  // namespace Events

namespace Properties {
namespace Player {
const int kPosition = AIMP_MSG_PROPERTY_PLAYER_POSITION;
}  // namespace Player
}  // namespace Properties

namespace Service {

class MessageDispatcher {
 public:
  MessageDispatcher();
  MessageDispatcher(const MessageDispatcher&) = delete;
  MessageDispatcher& operator=(const MessageDispatcher&) = delete;
  ~MessageDispatcher();

  explicit operator bool() const { return service_ != nullptr; }

  template <typename TCallback>
  void Hook(int event, TCallback callback) {
    if (service_ != nullptr) {
      Detail::HookRegister(service_, event,
                           std::function<void(DWORD, int, void*, HRESULT*)>(std::move(callback)));
    }
  }

  void UnhookAll();

 private:
  IAIMPServiceMessageDispatcher* service_ = nullptr;
};

}  // namespace Service

}  // namespace Messages

}  // namespace Aimp

#endif  // AIMP_GLUE_AIMP_MESSAGES_H_
