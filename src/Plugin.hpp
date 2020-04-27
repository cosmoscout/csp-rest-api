////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
//      and may be used under the terms of the MIT license. See the LICENSE file for details.     //
//                        Copyright: (c) 2019 German Aerospace Center (DLR)                       //
////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef CSP_WEB_API_PLUGIN_HPP
#define CSP_WEB_API_PLUGIN_HPP

#include "../../../src/cs-core/PluginBase.hpp"
#include "../../../src/cs-utils/DefaultProperty.hpp"
#include "../../../src/cs-utils/SafeQueue.hpp"

#include <VistaBase/VistaBaseTypes.h>
#include <condition_variable>

class CivetServer;
class CivetHandler;

namespace csp::webapi {

class Plugin : public cs::core::PluginBase {
 public:
  struct Settings {
    cs::utils::Property<uint16_t> mPort;
    std::optional<std::string>    mPage;
  };

  void init() override;
  void deInit() override;

  void update() override;

 private:
  void onLoad();
  void startServer(uint16_t port);
  void quitServer();

  Settings                      mPluginSettings;
  std::unique_ptr<CivetServer>  mServer;
  std::unique_ptr<CivetHandler> mRootHandler;
  std::unique_ptr<CivetHandler> mLogHandler;
  std::unique_ptr<CivetHandler> mCaptureHandler;
  std::unique_ptr<CivetHandler> mJSHandler;

  cs::utils::SafeQueue<std::string> mJavaScriptQueue;

  std::mutex                   mScreenShotMutex;
  std::condition_variable      mScreenShotDone;
  bool                         mScreenShotRequested = false;
  int32_t                      mScreenShotWidth     = 0;
  int32_t                      mScreenShotHeight    = 0;
  int32_t                      mScreenShotDelay     = 0;
  bool                         mScreenShotGui       = false;
  int32_t                      mCaptureAtFrame      = 0;
  std::vector<VistaType::byte> mScreenShot;

  std::mutex              mLogMutex;
  std::deque<std::string> mLogMessages;

  int mOnLoadConnection       = -1;
  int mOnSaveConnection       = -1;
  int mOnLogMessageConnection = -1;
};

} // namespace csp::webapi

#endif // CSP_WEB_API_PLUGIN_HPP
