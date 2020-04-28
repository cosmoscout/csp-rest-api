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

#define STB_IMAGE_WRITE_IMPLEMENTATION
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
// pretty unsafe, but I think it's the only way to make stb_image write to a std::vector<char>.
void pngWriteToVector(void* context, void* data, int len) {
  auto* vector   = static_cast<std::vector<char>*>(context);
  auto* charData = static_cast<char*>(data);
  // NOLINTNEXTLINE (cppcoreguidelines-pro-bounds-pointer-arithmetic)
  *vector = std::vector<char>(charData, charData + len);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

// A simple wrapper class which basically allows registering of lambdas as endpoint handlers for
// our CivetServer. This one handles GET requests.
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

// A simple wrapper class which basically allows registering of lambdas as endpoint handlers for
// our CivetServer. This one handles POST requests.
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

// A small helper method which returns the value of a parameter from a request URL. If the parameter
// is not present, a given default value is returned.
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

  // We store all emitted log messages (up to a maximum of 1000) in a std::deque in order to be able
  // to answer to /log requests.
  mOnLogMessageConnection = cs::utils::onLogMessage().connect(
      [this](
          std::string const& logger, spdlog::level::level_enum level, std::string const& message) {
        const std::unordered_map<spdlog::level::level_enum, std::string> mapping = {
            {spdlog::level::trace, "T"}, {spdlog::level::debug, "D"}, {spdlog::level::info, "I"},
            {spdlog::level::warn, "W"}, {spdlog::level::err, "E"}, {spdlog::level::critical, "C"}};

        std::lock_guard<std::mutex> lock(mLogMutex);
        mLogMessages.push_front("[" + mapping.at(level) + "] " + logger + message);

        if (mLogMessages.size() > 1000) {
          mLogMessages.pop_back();
        }
      });

  // Return the landing page when the root document is requested. If not landing page is configured,
  // we just send back a simple message.
  mHandlers.emplace("/", std::make_unique<GetHandler>([this](mg_connection* conn) {
    if (mPluginSettings.mPage) {
      mg_send_mime_file(conn, mPluginSettings.mPage.value().c_str(), "text/html");
    } else {
      std::string response = "CosmoScout VR is running. You can modify this page with "
                             "the 'page' key in the configuration of 'csp-web-api'.";
      mg_send_http_ok(conn, "text/plain", response.length());
      mg_write(conn, response.data(), response.length());
    }
  }));

  // Return a json array of log messages for /log requests.
  mHandlers.emplace("/log", std::make_unique<GetHandler>([this](mg_connection* conn) {
    auto           length = getParam<uint32_t>(conn, "length", 100U);
    nlohmann::json json;

    {
      std::lock_guard<std::mutex> lock(mLogMutex);
      auto                        it = mLogMessages.begin();
      while (json.size() < length && it != mLogMessages.end()) {
        json.push_back(*it);
        ++it;
      }
    }

    std::string response = json.dump();
    mg_send_http_ok(conn, "application/json", response.length());
    mg_write(conn, response.data(), response.length());
  }));

  // The /capture endpoint is a little bit more involved. As it takes several frames for the
  // screenshot to be completed (first we have to resize CosmoScout's window to the requested size,
  // then we have to wait some frames so that everything is loaded properly), we have to do some
  // more synchronization here.
  mHandlers.emplace("/capture", std::make_unique<GetHandler>([this](mg_connection* conn) {
    // First acquire the lock to make sure the mScreenShot* members are not currently read by the
    // main thread.
    std::unique_lock<std::mutex> lock(mScreenShotMutex);

    // Read all paramters.
    mScreenShotDelay  = std::clamp(getParam<int32_t>(conn, "delay", 50), 1, 200);
    mScreenShotWidth  = std::clamp(getParam<int32_t>(conn, "width", 800), 10, 2000);
    mScreenShotHeight = std::clamp(getParam<int32_t>(conn, "height", 600), 10, 2000);
    mScreenShotGui    = getParam<std::string>(conn, "gui", "false") == "true";

    // This tells the main thread that a capture request is pending.
    mScreenShotRequested = true;

    // Now we use a condition variable to wait for the screenshot. It is actually captured in the
    // Plugin::update() method further below.
    mScreenShotDone.wait(lock);

    // The screenshot has been capture, return the result!
    mg_send_http_ok(conn, "image/png", mScreenShot.size());
    mg_write(conn, mScreenShot.data(), mScreenShot.size());
  }));

  // All POST requests received on /run-js are stored in a queue. They are executed in the main
  // thread in the Plugin::update() method further below.
  mHandlers.emplace("/run-js", std::make_unique<PostHandler>([this](mg_connection* conn) {
    std::string response = "Done.";
    mg_send_http_ok(conn, "text/plain", response.length());
    mg_write(conn, response.data(), response.length());

    std::lock_guard<std::mutex> lock(mJavaScriptCallsMutex);
    mJavaScriptCalls.push(CivetServer::getPostData(conn));
  }));

  mOnLoadConnection = mAllSettings->onLoad().connect([this]() { onLoad(); });
  mOnSaveConnection = mAllSettings->onSave().connect(
      [this]() { mAllSettings->mPlugins["csp-web-api"] = mPluginSettings; });

  // Restart the server if the port changes.
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

  // Execute all JavaScript requests received since the last call to update().
  {
    std::lock_guard<std::mutex> lock(mJavaScriptCallsMutex);
    while (!mJavaScriptCalls.empty()) {
      auto request = mJavaScriptCalls.front();
      mJavaScriptCalls.pop();
      logger().debug("Executing 'run-js' request: '{}'", request);
      mGuiManager->getGui()->executeJavascript(request);
    }
  }

  // If a screen shot has been requested, we first resize the image to the given size. Then we wait
  // mScreenShotDelay frames until we actually read the pixels.
  std::lock_guard<std::mutex> lock(mScreenShotMutex);
  if (mScreenShotRequested) {
    auto* window = GetVistaSystem()->GetDisplayManager()->GetWindows().begin()->second;
    window->GetWindowProperties()->SetSize(mScreenShotWidth, mScreenShotHeight);
    mCaptureAtFrame = GetVistaSystem()->GetFrameLoop()->GetFrameCount() + mScreenShotDelay;
    mAllSettings->pEnableUserInterface = mScreenShotGui;
    mScreenShotRequested               = false;
  }

  // Now we waited several frames. We read the pixels, encode the data as png and notify the
  // server's worker thread that the screen shot is done.
  if (mCaptureAtFrame > 0 && mCaptureAtFrame == GetVistaSystem()->GetFrameLoop()->GetFrameCount()) {
    logger().info("Capture screenshot {}x{}; show gui: {}", mScreenShotWidth, mScreenShotHeight,
        mScreenShotGui);

    auto* window = GetVistaSystem()->GetDisplayManager()->GetWindows().begin()->second;
    window->GetWindowProperties()->GetSize(mScreenShotWidth, mScreenShotHeight);

    std::vector<VistaType::byte> screenshot(mScreenShotWidth * mScreenShotHeight * 3);
    glReadPixels(
        0, 0, mScreenShotWidth, mScreenShotHeight, GL_RGB, GL_UNSIGNED_BYTE, &screenshot[0]);

    // We encode the png data in the main thread as this is not thread-safe.
    stbi_flip_vertically_on_write(1);
    stbi_write_png_to_func(&pngWriteToVector, &mScreenShot, mScreenShotWidth, mScreenShotHeight, 3,
        screenshot.data(), mScreenShotWidth * 3);
    stbi_flip_vertically_on_write(0);

    mCaptureAtFrame = 0;
    mScreenShotDone.notify_one();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::startServer(uint16_t port) {

  // First quit the server as it may be running already.
  quitServer();

  try {
    // We start the server with one thread only, as we do not want to process requests in parallel.
    std::vector<std::string> options{"listening_ports", std::to_string(port), "num_threads", "1"};
    mServer = std::make_unique<CivetServer>(options);

    for (auto const& handler : mHandlers) {
      mServer->addHandler(handler.first, *handler.second);
    }

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
