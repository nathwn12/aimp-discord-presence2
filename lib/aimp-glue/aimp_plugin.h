//  In-repo replacement for the retired aimp-sdk-cpp-wrapper facade.
//
//  Aimp::Plugin and Aimp::ExternalSettingsDialog adapt the plain C++ extension
//  points implemented by the plugin (GetInfo / GetCategory / Load / Unload /
//  Notification / ShowSettings) onto the official IAIMPPlugin and
//  IAIMPExternalSettingsDialog interfaces from the AIMP SDK.

#ifndef AIMP_GLUE_AIMP_PLUGIN_H_
#define AIMP_GLUE_AIMP_PLUGIN_H_

#include <windows.h>
#include <unknwn.h>

#include "apiCore.h"
#include "apiPlugin.h"

// Plugin metadata identifiers, as used by the plugin's GetInfo / GetCategory
// overrides. They alias the official AIMP constants.
namespace Info {
enum {
  kName = AIMP_PLUGIN_INFO_NAME,
  kAuthor = AIMP_PLUGIN_INFO_AUTHOR,
};
namespace Description {
enum {
  kShort = AIMP_PLUGIN_INFO_SHORT_DESCRIPTION,
};
}  // namespace Description
}  // namespace Info

namespace Category {
enum {
  kAddons = AIMP_PLUGIN_CATEGORY_ADDONS,
};
}  // namespace Category

namespace Aimp {

// Extension points a plugin implements; forwarded to IAIMPPlugin by Aimp::Plugin.
class Plugin : public IAIMPPlugin {
 public:
  // IAIMPPlugin: names are reported before Initialize is ever called.
  PWCHAR WINAPI InfoGet(int Index) override { return GetInfo(Index); }
  DWORD WINAPI InfoGetCategories() override { return GetCategory(); }
  HRESULT WINAPI Initialize(IAIMPCore* Core) override;
  HRESULT WINAPI Finalize() override;
  void WINAPI SystemNotification(int NotifyID, IUnknown* Data) override { Notification(NotifyID, Data); }

  virtual PWCHAR GetInfo(int index) = 0;
  virtual DWORD GetCategory() = 0;
  virtual bool Load() = 0;
  virtual bool Unload() = 0;
  virtual void Notification(int id, IUnknown* data) = 0;
};

// The plugin's settings dialog, registered at the same object level as
// IAIMPPlugin (see the AIMP SDK notes for IAIMPPlugin.SystemNotification).
class ExternalSettingsDialog : public IAIMPExternalSettingsDialog {
 public:
  static const IID& iid() {
    static const IID id = IID_IAIMPExternalSettingsDialog;
    return id;
  }

  void WINAPI Show(HWND ParentWindow) override { ShowSettings(ParentWindow); }

  virtual void ShowSettings(HWND parent_wnd) = 0;
};

}  // namespace Aimp

#endif  // AIMP_GLUE_AIMP_PLUGIN_H_
