////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
//      and may be used under the terms of the MIT license. See the LICENSE file for details.     //
//                        Copyright: (c) 2019 German Aerospace Center (DLR)                       //
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "Plugin.hpp"

#include "../../../src/cs-core/Settings.hpp"
#include "../../../src/cs-utils/logger.hpp"
#include "logger.hpp"

////////////////////////////////////////////////////////////////////////////////////////////////////

EXPORT_FN cs::core::PluginBase* create() {
  return new csp::webapi::Plugin;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

EXPORT_FN void destroy(cs::core::PluginBase* pluginBase) {
  delete pluginBase; // NOLINT(cppcoreguidelines-owning-memory)
}

////////////////////////////////////////////////////////////////////////////////////////////////////

namespace csp::webapi {

////////////////////////////////////////////////////////////////////////////////////////////////////

void from_json(nlohmann::json const& j, Plugin::Settings& o) {
  cs::core::Settings::deserialize(j, "port", o.mPort);
}

void to_json(nlohmann::json& j, Plugin::Settings const& o) {
  cs::core::Settings::serialize(j, "port", o.mPort);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::init() {

  logger().info("Loading plugin...");

  mRestAPI = std::make_unique<Pistache::Rest::Router>();

  Pistache::Rest::Routes::Get(*mRestAPI, "/test",
      [this](Pistache::Rest::Request const& request, Pistache::Http::ResponseWriter response) {
        logger().info("Got request: {}", request.query().parameters()[0]);
        response.send(Pistache::Http::Code::Ok, "Hello, World");
        return Pistache::Rest::Route::Result::Ok;
      });

  mOnLoadConnection = mAllSettings->onLoad().connect([this]() { onLoad(); });
  mOnSaveConnection = mAllSettings->onSave().connect(
      [this]() { mAllSettings->mPlugins["csp-web-api"] = mPluginSettings; });

  mPluginSettings.mPort.connect([this](uint16_t port) { startServer(port); });

  // Load settings.
  onLoad();

  logger().info("Loading done.");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::deInit() {
  logger().info("Unloading plugin...");

  mAllSettings->onLoad().disconnect(mOnLoadConnection);
  mAllSettings->onSave().disconnect(mOnSaveConnection);

  quitServer();

  mRestAPI.reset();

  logger().info("Unloading done.");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::update() {
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::startServer(uint16_t port) {
  quitServer();

  try {
    auto addr = Pistache::Address(Pistache::Ipv4::any(), Pistache::Port(port));
    mServer   = std::make_unique<Pistache::Http::Endpoint>(addr);
    auto opts = Pistache::Http::Endpoint::options().threads(1).flags(
        Pistache::Tcp::Options::ReuseAddr | Pistache::Tcp::Options::ReusePort);
    mServer->init(opts);
    mServer->setHandler(mRestAPI->handler());
    mServer->serveThreaded();
  } catch (std::exception const& e) { logger().warn("Failed to start server: {}!", e.what()); }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::quitServer() {
  try {
    if (mServer) {
      mServer->shutdown();
      mServer.reset();
    }
  } catch (std::exception const& e) { logger().warn("Failed to quit server: {}!", e.what()); }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::onLoad() {
  // Read settings from JSON.
  from_json(mAllSettings->mPlugins.at("csp-web-api"), mPluginSettings);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace csp::webapi
