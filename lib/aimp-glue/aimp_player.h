//  In-repo replacement for the retired aimp-sdk-cpp-wrapper facade.
//
//  Aimp::Player::Service::Player is a thin, reference counted handle over
//  IAIMPServicePlayer. It queries the service for the duration of a single
//  call site and releases it again, as recommended by the AIMP SDK.

#ifndef AIMP_GLUE_AIMP_PLAYER_H_
#define AIMP_GLUE_AIMP_PLAYER_H_

#include <windows.h>
#include <unknwn.h>

#include "aimp_core.h"
#include "aimp_filemanager.h"
#include "apiPlayer.h"

namespace Aimp {
namespace Player {
namespace Service {

class Player {
 public:
  Player() : service_(Detail::QueryService<IAIMPServicePlayer>(IID_IAIMPServicePlayer)) {}
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  ~Player() {
    if (service_ != nullptr) {
      service_->Release();
    }
  }

  explicit operator bool() const { return service_ != nullptr; }

  int State() const {
    if (service_ == nullptr) {
      return 0;
    }
    return service_->GetState();
  }

  double Position() const {
    double seconds = 0.0;
    if (service_ != nullptr) {
      service_->GetPosition(&seconds);
    }
    return seconds;
  }

  double Duration() const {
    double seconds = 0.0;
    if (service_ != nullptr) {
      service_->GetDuration(&seconds);
    }
    return seconds;
  }

  FileManager::FileInfo GetInfo() const {
    IAIMPFileInfo* info = nullptr;
    if (service_ != nullptr && SUCCEEDED(service_->GetInfo(&info)) && info != nullptr) {
      return FileManager::FileInfo(info);
    }
    return FileManager::FileInfo();
  }

 private:
  IAIMPServicePlayer* service_ = nullptr;
};

}  // namespace Service
}  // namespace Player
}  // namespace Aimp

#endif  // AIMP_GLUE_AIMP_PLAYER_H_
