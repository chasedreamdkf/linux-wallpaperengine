#include "GnomeShellOutput.h"

#include "GLFWOutputViewport.h"
#include "WallpaperEngine/Logging/Log.h"

#include <chrono>
#include <climits>
#include <thread>

using namespace WallpaperEngine::Render::Drivers::Output;
using namespace WallpaperEngine::Render::Drivers::GnomeShell;

namespace {
/** How long to wait for the extension to report the monitor layout at startup */
constexpr uint32_t MONITOR_WAIT_MS = 5000;
} // namespace

GnomeShellOutput::GnomeShellOutput (ApplicationContext& context, VideoDriver& driver, GnomeShellIpc& ipc) :
    Output (context, driver), m_ipc (ipc) {
    this->m_ipc.onMonitorsChanged
	= [this] (const std::vector<MonitorInfo>& monitors, const std::string& primary) -> void {
	      this->applyMonitors (monitors, primary);
	  };

    this->m_ipc.requestMonitors ();
    this->waitForMonitors (MONITOR_WAIT_MS);

    if (this->m_monitors.empty ()) {
	sLog.exception (
	    "The GNOME Shell bridge extension did not report any monitor. Make sure the "
	    "linux-wallpaperengine-gnome extension is installed and enabled"
	);
    }
}

GnomeShellOutput::~GnomeShellOutput () {
    this->m_ipc.onMonitorsChanged = nullptr;

    if (this->m_frameBuffer != nullptr) {
	this->m_ipc.unregisterFrameBuffer ();
	delete this->m_frameBuffer;
	this->m_frameBuffer = nullptr;
    }

    this->m_viewports.clear ();
    delete this->m_viewport;
    this->m_viewport = nullptr;
}

void GnomeShellOutput::reset () { this->m_ipc.requestMonitors (); }

bool GnomeShellOutput::renderVFlip () const { return false; }

bool GnomeShellOutput::renderMultiple () const { return false; }

bool GnomeShellOutput::haveImageBuffer () const { return true; }

void* GnomeShellOutput::getImageBuffer () const {
    if (this->m_frameBuffer == nullptr) {
	return nullptr;
    }

    this->m_writeInProgress = true;
    return this->m_frameBuffer->beginWrite ();
}

uint32_t GnomeShellOutput::getImageBufferSize () const {
    if (this->m_frameBuffer == nullptr) {
	return 0;
    }

    return this->m_frameBuffer->getStride () * this->m_frameBuffer->getHeight ();
}

void GnomeShellOutput::updateRender () const {
    if (this->m_frameBuffer == nullptr || !this->m_writeInProgress) {
	return;
    }

    this->m_frameBuffer->endWrite ();
    this->m_writeInProgress = false;
    this->m_ipc.notifyFrameReady (this->m_frameBuffer->publishFrameId ());
}

void GnomeShellOutput::waitForMonitors (uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (timeoutMs);

    while (this->m_monitors.empty () && std::chrono::steady_clock::now () < deadline) {
	this->m_ipc.dispatch ();

	if (!this->m_monitors.empty ()) {
	    break;
	}

	std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
}

void GnomeShellOutput::applyMonitors (const std::vector<MonitorInfo>& monitors, const std::string& primary) {
    if (monitors.empty ()) {
	return;
    }

    int minX = INT_MAX;
    int minY = INT_MAX;
    int maxX = INT_MIN;
    int maxY = INT_MIN;

    for (const auto& monitor : monitors) {
	minX = std::min (minX, monitor.x);
	minY = std::min (minY, monitor.y);
	maxX = std::max (maxX, monitor.x + monitor.width);
	maxY = std::max (maxY, monitor.y + monitor.height);
    }

    const int width = maxX - minX;
    const int height = maxY - minY;

    if (width <= 0 || height <= 0) {
	return;
    }

    this->m_monitors = monitors;
    this->m_primary = primary;
    this->m_boundsPosition = { minX, minY };

    const bool sizeChanged = this->m_fullWidth != width || this->m_fullHeight != height;

    this->m_fullWidth = width;
    this->m_fullHeight = height;

    if (sizeChanged || this->m_frameBuffer == nullptr) {
	sLog.out (
	    "GNOME Shell desktop bounding box is ", width, "x", height, " at (", minX, ",", minY, ") across ",
	    monitors.size (), " monitor(s)"
	);
	this->rebuildFrameBuffer ();
    }
}

void GnomeShellOutput::rebuildFrameBuffer () {
    this->m_viewports.clear ();
    delete this->m_viewport;
    this->m_viewport = nullptr;

    if (this->m_frameBuffer != nullptr) {
	this->m_ipc.unregisterFrameBuffer ();
	delete this->m_frameBuffer;
	this->m_frameBuffer = nullptr;
    }

    const auto width = static_cast<uint32_t> (this->m_fullWidth);
    const auto height = static_cast<uint32_t> (this->m_fullHeight);
    const auto stride = width * 4;

    this->m_frameBuffer = new SharedFrameBuffer (width, height, stride);

    if (!this->m_ipc.registerFrameBuffer (
	    this->m_frameBuffer->mapName (), width, height, stride, SharedFrameBuffer::FORMAT_RGBA8888
	)) {
	sLog.error ("The GNOME Shell bridge extension rejected the frame buffer registration");
    }

    auto* viewport = new GLFWOutputViewport { { 0, 0, this->m_fullWidth, this->m_fullHeight }, "default" };
    viewport->globalPosition = this->m_boundsPosition;
    viewport->logicalSize = { this->m_fullWidth, this->m_fullHeight };

    this->m_viewport = viewport;
    this->m_viewports["default"] = viewport;
}