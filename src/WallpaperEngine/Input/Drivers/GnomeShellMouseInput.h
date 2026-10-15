#pragma once

#include "WallpaperEngine/Input/MouseInput.h"

#include <glm/vec2.hpp>

namespace WallpaperEngine::Render::Drivers {
class GnomeShellOpenGLDriver;
}

namespace WallpaperEngine::Input::Drivers {
/**
 * Mouse input backed by the GNOME Shell bridge extension.
 *
 * The extension reports the pointer position over D-Bus (relative to the desktop
 * bounding box, top-left origin); this class caches it and, on update(), converts
 * it to the engine's OpenGL convention (bottom-left origin). Clicks are never
 * reported: an actor living in Mutter's background group does not receive pointer
 * events, which matches Hidamari's limitation.
 */
class GnomeShellMouseInput final : public MouseInput {
public:
    explicit GnomeShellMouseInput (const Render::Drivers::GnomeShellOpenGLDriver& driver);

    /**
     * Stores the latest pointer position reported by the extension
     *
     * @param x X coordinate relative to the desktop bounding box
     * @param y Y coordinate relative to the desktop bounding box (top-left origin)
     */
    void setPosition (double x, double y);

    /**
     * Refreshes the reported position, converting it to the engine's coordinate system
     */
    void update () override;

    [[nodiscard]] glm::dvec2 position () const override;
    [[nodiscard]] MouseClickStatus leftClick () const override;
    [[nodiscard]] MouseClickStatus rightClick () const override;

private:
    const Render::Drivers::GnomeShellOpenGLDriver& m_driver;
    glm::dvec2 m_mousePosition = {};
    glm::dvec2 m_reportedPosition = {};
};
} // namespace WallpaperEngine::Input::Drivers