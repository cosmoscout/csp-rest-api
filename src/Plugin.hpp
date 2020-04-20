////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
//      and may be used under the terms of the MIT license. See the LICENSE file for details.     //
//                        Copyright: (c) 2019 German Aerospace Center (DLR)                       //
////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef CSP_WEB_API_PLUGIN_HPP
#define CSP_WEB_API_PLUGIN_HPP

#include "../../../src/cs-core/PluginBase.hpp"
#include "../../../src/cs-utils/DefaultProperty.hpp"

#include <VistaBase/VistaBaseTypes.h>
#include <pistache/endpoint.h>
#include <pistache/mailbox.h>
#include <pistache/router.h>

namespace csp::webapi {

class Plugin : public cs::core::PluginBase {
 public:
  struct Settings {
    cs::utils::Property<uint16_t>    mPort;
    cs::utils::Property<std::string> mPage;
  };

  void init() override;
  void deInit() override;

  void update() override;

 private:
  void onLoad();
  void startServer(uint16_t port);
  void quitServer();

  Settings                                  mPluginSettings;
  std::unique_ptr<Pistache::Http::Endpoint> mServer;
  std::unique_ptr<Pistache::Rest::Router>   mRestAPI;

  Pistache::Queue<std::string> mJavaScriptQueue;

  std::mutex                   mScreenShotMutex;
  std::condition_variable      mScreenShotDone;
  bool                         mScreenShotRequested = false;
  int32_t                      mScreenShotWidth     = 0;
  int32_t                      mScreenShotHeight    = 0;
  int32_t                      mScreenShotDelay     = 0;
  bool                         mScreenShotGui       = false;
  int32_t                      mCaptureAtFrame      = 0;
  std::vector<VistaType::byte> mScreenShot;

  int mOnLoadConnection = -1;
  int mOnSaveConnection = -1;
};

} // namespace csp::webapi

#endif // CSP_WEB_API_PLUGIN_HPP
