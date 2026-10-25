#pragma once

#include <string>
#include <vector>

#include <glm/vec2.hpp>

#include "Output.h"
#include "WallpaperEngine/Render/Drivers/GnomeShell/GnomeShellIpc.h"
#include "WallpaperEngine/Render/Drivers/GnomeShell/SharedFrameBuffer.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"

namespace WallpaperEngine::Render::Drivers::Output {
/**
 * Frame sink for the Hidamari-style GNOME Shell backend.
 *
 * It never talks to a window system directly: the whole-desktop bounding box the
 * extension reported is rendered into a shared memory segment that the GJS
 * extension maps and paints into Mutter's background group. Each monitor gets
 * its own viewport (named after the monitor index) so that --gnome-shell-screen
 * can give every monitor a different wallpaper.
 */
class GnomeShellOutput final : public Output {
public:
    GnomeShellOutput (ApplicationContext& context, VideoDriver& driver, GnomeShell::GnomeShellIpc& ipc);
    ~GnomeShellOutput () override;

    void reset () override;

    bool renderVFlip () const override;
    bool renderMultiple () const override;
    bool haveImageBuffer () const override;
    void* getImageBuffer () const override;
    uint32_t getImageBufferSize () const override;
    void updateRender () const override;

private:
    /** Pumps the IPC bus until the extension reports a monitor layout or the timeout elapses */
    void waitForMonitors (uint32_t timeoutMs);
    /** Stores the monitor layout and (re)builds the bounding box + frame buffer when it changed */
    void applyMonitors (const std::vector<GnomeShell::MonitorInfo>& monitors, const std::string& primary);
    /** (Re)allocates the shared memory, registers it with the extension and rebuilds the viewports */
    void rebuildFrameBuffer ();

    GnomeShell::GnomeShellIpc& m_ipc;
    GnomeShell::SharedFrameBuffer* m_frameBuffer = nullptr;
    std::vector<GnomeShell::MonitorInfo> m_monitors = {};
    std::string m_primary;
    glm::ivec2 m_boundsPosition = { 0, 0 };
    /** Size of the desktop bounding box in desktop (unscaled) pixels */
    glm::ivec2 m_boundsSize = { 0, 0 };
    /** Fraction of the desktop resolution the offscreen frames are rendered at */
    float m_frameScale = 1.0f;
    /** Owned viewports, one per monitor; Output::m_viewports holds non-owning aliases of them */
    std::vector<OutputViewport*> m_screens = {};
    mutable bool m_writeInProgress = false;
};
} // namespace WallpaperEngine::Render::Drivers::Output