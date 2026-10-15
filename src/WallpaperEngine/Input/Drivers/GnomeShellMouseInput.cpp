#include "GnomeShellMouseInput.h"

#include "WallpaperEngine/Render/Drivers/GnomeShellOpenGLDriver.h"

#include <glm/common.hpp>

using namespace WallpaperEngine::Input::Drivers;

GnomeShellMouseInput::GnomeShellMouseInput (const Render::Drivers::GnomeShellOpenGLDriver& driver) : m_driver (driver) { }

void GnomeShellMouseInput::setPosition (double x, double y) { this->m_mousePosition = { x, y }; }

void GnomeShellMouseInput::update () {
    if (!this->m_driver.getApp ().getContext ().settings.mouse.enabled) {
	this->m_reportedPosition = { 0, 0 };
	return;
    }

    // The extension reports a top-left origin, the engine expects a bottom-left one
    const glm::ivec2 framebufferSize = this->m_driver.getFramebufferSize ();
    const glm::dvec2 converted = { this->m_mousePosition.x, framebufferSize.y - this->m_mousePosition.y };

    this->m_reportedPosition = glm::mix (this->m_reportedPosition, converted, 1.0);
}

glm::dvec2 GnomeShellMouseInput::position () const { return this->m_reportedPosition; }

WallpaperEngine::Input::MouseClickStatus GnomeShellMouseInput::leftClick () const {
    return MouseClickStatus::Released;
}

WallpaperEngine::Input::MouseClickStatus GnomeShellMouseInput::rightClick () const {
    return MouseClickStatus::Released;
}