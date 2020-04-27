////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
//      and may be used under the terms of the MIT license. See the LICENSE file for details.     //
//                        Copyright: (c) 2019 German Aerospace Center (DLR)                       //
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "Plugin.hpp"

#include "../../../src/cs-core/GuiManager.hpp"
#include "../../../src/cs-core/Settings.hpp"
#include "../../../src/cs-utils/logger.hpp"
#include "../../../src/cs-utils/utils.hpp"
#include "logger.hpp"

#include <GL/glew.h>
#include <VistaKernel/DisplayManager/VistaDisplayManager.h>
#include <VistaKernel/DisplayManager/VistaWindow.h>
#include <VistaKernel/VistaFrameLoop.h>
#include <VistaKernel/VistaSystem.h>
#include <curlpp/cURLpp.hpp>
#include <stb_image_write.h>

////////////////////////////////////////////////////////////////////////////////////////////////////

EXPORT_FN cs::core::PluginBase* create() {
  return new csp::webapi::Plugin;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

EXPORT_FN void destroy(cs::core::PluginBase* pluginBase) {
  delete pluginBase; // NOLINT(cppcoreguidelines-owning-memory)
}

////////////////////////////////////////////////////////////////////////////////////////////////////

namespace {
void pngWriteToVector(void* context, void* data, int len) {
  auto vector   = reinterpret_cast<std::vector<char>*>(context);
  auto charData = reinterpret_cast<char*>(data);
  *vector       = std::vector<char>(charData, charData + len);
}
} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////

namespace csp::webapi {

////////////////////////////////////////////////////////////////////////////////////////////////////

void from_json(nlohmann::json const& j, Plugin::Settings& o) {
  cs::core::Settings::deserialize(j, "port", o.mPort);
  cs::core::Settings::deserialize(j, "page", o.mPage);
}

void to_json(nlohmann::json& j, Plugin::Settings const& o) {
  cs::core::Settings::serialize(j, "port", o.mPort);
  cs::core::Settings::serialize(j, "page", o.mPage);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::init() {

  logger().info("Loading plugin...");

  stbi_flip_vertically_on_write(1);

  mRestAPI = std::make_unique<Pistache::Rest::Router>();

  mOnLogMessageConnection = cs::utils::onLogMessage().connect(
      [this](
          std::string const& logger, spdlog::level::level_enum level, std::string const& message) {
        const std::unordered_map<spdlog::level::level_enum, std::string> mapping = {
            {spdlog::level::trace, "T"}, {spdlog::level::debug, "D"}, {spdlog::level::info, "I"},
            {spdlog::level::warn, "W"}, {spdlog::level::err, "E"}, {spdlog::level::critical, "C"}};

        std::unique_lock<std::mutex> lock(mLogMutex);
        mLogMessages.push_front("[" + mapping.at(level) + "] " + logger + message);

        if (mLogMessages.size() > 1000) {
          mLogMessages.pop_back();
        }
      });

  // Return the landing page when the root document is requested.
  mRestAPI->get(
      "/", [this](Pistache::Rest::Request const& request, Pistache::Http::ResponseWriter response) {
        if (mPluginSettings.mPage) {
          Pistache::Http::serveFile(response, *mPluginSettings.mPage);
        } else {
          response.send(
              Pistache::Http::Code::Ok, "CosmoScout VR is running. You can modify this page with "
                                        "the 'page' key in the configuration of 'csp-web-api'.");
        }
        return Pistache::Rest::Route::Result::Ok;
      });

  // We won't return any files, except for the favicon.
  mRestAPI->get("/favicon.ico",
      [](Pistache::Rest::Request const& request, Pistache::Http::ResponseWriter response) {
        Pistache::Http::serveFile(response, "../share/resources/icons/icon.ico");
        return Pistache::Rest::Route::Result::Ok;
      });

  mRestAPI->get("/log",
      [this](Pistache::Rest::Request const& request, Pistache::Http::ResponseWriter response) {
        uint32_t length =
            cs::utils::fromString<uint32_t>(request.query().get("length").getOrElse("100"));

        std::unique_lock<std::mutex> lock(mLogMutex);
        nlohmann::json               json;

        auto it = mLogMessages.begin();
        while (json.size() < length && it != mLogMessages.end()) {
          json.push_back(*it);
          ++it;
        }

        response.headers().add<Pistache::Http::Header::ContentType>("application/json");
        response.send(Pistache::Http::Code::Ok, json.dump());
        return Pistache::Rest::Route::Result::Ok;
      });

  mRestAPI->get("/capture", [this](Pistache::Rest::Request const& request,
                                Pistache::Http::ResponseWriter    response) {
    std::unique_lock<std::mutex> lock(mScreenShotMutex);

    mScreenShotDelay = std::clamp(
        cs::utils::fromString<int32_t>(request.query().get("delay").getOrElse("50")), 1, 200);
    mScreenShotWidth = std::clamp(
        cs::utils::fromString<int32_t>(request.query().get("width").getOrElse("800")), 10, 2000);
    mScreenShotHeight = std::clamp(
        cs::utils::fromString<int32_t>(request.query().get("height").getOrElse("600")), 10, 2000);
    mScreenShotGui       = request.query().get("gui").getOrElse("false") == "true";
    mScreenShotRequested = true;

    mScreenShotDone.wait(lock);

    std::vector<char> pngData;

    stbi_write_png_to_func(&pngWriteToVector, &pngData, mScreenShotWidth, mScreenShotHeight, 3,
        mScreenShot.data(), mScreenShotWidth * 3);

    response.headers().add<Pistache::Http::Header::ContentType>("image/png");
    response.send(Pistache::Http::Code::Ok, pngData.data(), pngData.size());
    return Pistache::Rest::Route::Result::Ok;
  });

  mRestAPI->post("/run-js",
      [this](Pistache::Rest::Request const& request, Pistache::Http::ResponseWriter response) {
        mJavaScriptQueue.push(request.body());
        response.headers().add<Pistache::Http::Header::ContentType>("text/plain");
        response.send(Pistache::Http::Code::Ok, "Done");
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
  cs::utils::onLogMessage().disconnect(mOnLogMessageConnection);

  quitServer();

  mRestAPI.reset();

  logger().info("Unloading done.");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::update() {
  if (!mJavaScriptQueue.empty()) {
    auto request = mJavaScriptQueue.popSafe();
    logger().debug("Executing 'run-js' request: '{}'", *request);
    mGuiManager->getGui()->executeJavascript(*request);
  }

  std::unique_lock<std::mutex> lock(mScreenShotMutex);
  if (mScreenShotRequested) {
    auto window = GetVistaSystem()->GetDisplayManager()->GetWindows().begin()->second;
    window->GetWindowProperties()->SetSize(mScreenShotWidth, mScreenShotHeight);
    mCaptureAtFrame = GetVistaSystem()->GetFrameLoop()->GetFrameCount() + mScreenShotDelay;
    mAllSettings->pEnableUserInterface = mScreenShotGui;
    mScreenShotRequested               = false;
  }

  if (mCaptureAtFrame > 0 && mCaptureAtFrame == GetVistaSystem()->GetFrameLoop()->GetFrameCount()) {
    logger().info("Capture screenshot {}x{}; show gui: {}", mScreenShotWidth, mScreenShotHeight,
        mScreenShotGui);

    auto window = GetVistaSystem()->GetDisplayManager()->GetWindows().begin()->second;
    window->GetWindowProperties()->GetSize(mScreenShotWidth, mScreenShotHeight);
    mScreenShot.resize(mScreenShotWidth * mScreenShotHeight * 3);
    glReadPixels(
        0, 0, mScreenShotWidth, mScreenShotHeight, GL_RGB, GL_UNSIGNED_BYTE, &mScreenShot[0]);

    mCaptureAtFrame = 0;
    mScreenShotDone.notify_one();
  }
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
