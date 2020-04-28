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

#include <CivetServer.h>
#include <GL/glew.h>
#include <VistaKernel/DisplayManager/VistaDisplayManager.h>
#include <VistaKernel/DisplayManager/VistaWindow.h>
#include <VistaKernel/VistaFrameLoop.h>
#include <VistaKernel/VistaSystem.h>
#include <curlpp/cURLpp.hpp>
#include <stb_image_write.h>
#include <utility>

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

////////////////////////////////////////////////////////////////////////////////////////////////////

// Converts a void* to a std::vector<char> (which is given through a void* as well). So this is
// pretty unsafe, but I think it's the only way to make stb_image write to a std::vector<char>. If
// anybody has a better idea...
void pngWriteToVector(void* context, void* data, int len) {
  // NOLINTNEXTLINE (cppcoreguidelines-pro-type-reinterpret-cast)
  auto vector = reinterpret_cast<std::vector<char>*>(context);
  // NOLINTNEXTLINE (cppcoreguidelines-pro-type-reinterpret-cast)
  auto charData = reinterpret_cast<char*>(data);
  // NOLINTNEXTLINE (cppcoreguidelines-pro-bounds-pointer-arithmetic)
  *vector = std::vector<char>(charData, charData + len);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

class GetHandler : public CivetHandler {
 public:
  explicit GetHandler(std::function<void(mg_connection*)> handler)
      : mHandler(std::move(handler)) {
  }

  bool handleGet(CivetServer* /*server*/, mg_connection* conn) override {
    mHandler(conn);
    return true;
  }

 private:
  std::function<void(mg_connection*)> mHandler;
};

////////////////////////////////////////////////////////////////////////////////////////////////////

class PostHandler : public CivetHandler {
 public:
  explicit PostHandler(std::function<void(mg_connection*)> handler)
      : mHandler(std::move(handler)) {
  }

  bool handlePost(CivetServer* /*server*/, mg_connection* conn) override {
    mHandler(conn);
    return true;
  }

 private:
  std::function<void(mg_connection*)> mHandler;
};

////////////////////////////////////////////////////////////////////////////////////////////////////

template <typename T>
T getParam(mg_connection* conn, std::string const& name, T const& defaultValue) {
  std::string s;
  if (CivetServer::getParam(conn, name.c_str(), s)) {
    return cs::utils::fromString<T>(s);
  }
  return defaultValue;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

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
  mRootHandler = std::make_unique<GetHandler>([this](mg_connection* conn) {
    if (mPluginSettings.mPage) {
      mg_send_mime_file(conn, mPluginSettings.mPage.value().c_str(), "text/html");
    } else {
      std::string response = "CosmoScout VR is running. You can modify this page with "
                             "the 'page' key in the configuration of 'csp-web-api'.";
      mg_send_http_ok(conn, "text/plain", response.length());
      mg_write(conn, response.data(), response.length());
    }
  });

  mLogHandler = std::make_unique<GetHandler>([this](mg_connection* conn) {
    auto length = getParam<uint32_t>(conn, "length", 100U);

    std::unique_lock<std::mutex> lock(mLogMutex);
    nlohmann::json               json;

    auto it = mLogMessages.begin();
    while (json.size() < length && it != mLogMessages.end()) {
      json.push_back(*it);
      ++it;
    }

    std::string response = json.dump();
    mg_send_http_ok(conn, "application/json", response.length());
    mg_write(conn, response.data(), response.length());
  });

  mCaptureHandler = std::make_unique<GetHandler>([this](mg_connection* conn) {
    std::unique_lock<std::mutex> lock(mScreenShotMutex);

    mScreenShotDelay     = std::clamp(getParam<int32_t>(conn, "delay", 50), 1, 200);
    mScreenShotWidth     = std::clamp(getParam<int32_t>(conn, "width", 800), 10, 2000);
    mScreenShotHeight    = std::clamp(getParam<int32_t>(conn, "height", 600), 10, 2000);
    mScreenShotGui       = getParam<std::string>(conn, "gui", "false") == "true";
    mScreenShotRequested = true;

    mScreenShotDone.wait(lock);

    std::vector<char> pngData;

    stbi_flip_vertically_on_write(1);
    stbi_write_png_to_func(&pngWriteToVector, &pngData, mScreenShotWidth, mScreenShotHeight, 3,
        mScreenShot.data(), mScreenShotWidth * 3);
    stbi_flip_vertically_on_write(0);

    mg_send_http_ok(conn, "image/png", pngData.size());
    mg_write(conn, pngData.data(), pngData.size());
  });

  mJSHandler = std::make_unique<PostHandler>([this](mg_connection* conn) {
    mJavaScriptQueue.push(CivetServer::getPostData(conn));
    std::string response = "Done.";
    mg_send_http_ok(conn, "text/plain", response.length());
    mg_write(conn, response.data(), response.length());
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

  logger().info("Unloading done.");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::update() {
  if (!mJavaScriptQueue.empty()) {
    auto request = mJavaScriptQueue.front();
    mJavaScriptQueue.pop();
    logger().debug("Executing 'run-js' request: '{}'", request);
    mGuiManager->getGui()->executeJavascript(request);
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
    std::vector<std::string> options{
        "document_root", "/", "listening_ports", std::to_string(port), "num_threads", "1"};
    mServer = std::make_unique<CivetServer>(options);
    mServer->addHandler("/", *mRootHandler);
    mServer->addHandler("/log", *mLogHandler);
    mServer->addHandler("/capture", *mCaptureHandler);
    mServer->addHandler("/run-js", *mJSHandler);
  } catch (std::exception const& e) { logger().warn("Failed to start server: {}!", e.what()); }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::quitServer() {
  try {
    if (mServer) {
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
