////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
//      and may be used under the terms of the MIT license. See the LICENSE file for details.     //
//                        Copyright: (c) 2019 German Aerospace Center (DLR)                       //
////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef CSP_WEB_API_PLUGIN_HPP
#define CSP_WEB_API_PLUGIN_HPP

#include "../../../src/cs-core/PluginBase.hpp"
#include "../../../src/cs-utils/DefaultProperty.hpp"

#include <pistache/endpoint.h>
#include <pistache/router.h>

namespace csp::webapi {

class Plugin : public cs::core::PluginBase {
 public:
  struct Settings {
    cs::utils::Property<uint16_t> mPort;
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

  int mOnLoadConnection = -1;
  int mOnSaveConnection = -1;
};

} // namespace csp::webapi

#endif // CSP_WEB_API_PLUGIN_HPP
