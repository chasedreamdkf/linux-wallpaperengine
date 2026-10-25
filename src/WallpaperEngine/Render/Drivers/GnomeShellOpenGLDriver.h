#pragma once

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Input/Drivers/GnomeShellMouseInput.h"
#include "WallpaperEngine/Render/Drivers/GnomeShell/GnomeShellIpc.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"

#include <EGL/egl.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstddef>
#include <glm/vec2.hpp>

namespace WallpaperEngine::Render::Drivers {
using namespace WallpaperEngine::Application;

/**
 * Offscreen video driver for the Hidamari-style GNOME Shell backend.
 *
 * Instead of creating a visible window it renders the whole desktops bounding
 * box into its own framebuffer object and reads the pixels back into a shared
 * memory segment (see GnomeShellOutput) that the GNOME Shell extension maps and
 * paints. The OpenGL context comes from EGL's surfaceless platform; if that is
 * unavailable the driver falls back to a hidden GLFW window.
 */
class GnomeShellOpenGLDriver final : public VideoDriver {
public:
    GnomeShellOpenGLDriver (ApplicationContext& context, WallpaperApplication& app);
    ~GnomeShellOpenGLDriver () override;

    [[nodiscard]] Output::Output& getOutput () override;
    [[nodiscard]] float getRenderTime () const override;
    bool closeRequested () override;
    void resizeWindow (glm::ivec2 size) override;
    void resizeWindow (glm::ivec4 sizeandpos) override;
    void showWindow () override;
    void hideWindow () override;
    [[nodiscard]] glm::ivec2 getFramebufferSize () const override;
    [[nodiscard]] uint32_t getFrameCounter () const override;
    void dispatchEventQueue () override;
    [[nodiscard]] void* getProcAddress (const char* name) const override;

private:
    /** Creates the OpenGL context, preferring EGL surfaceless over a hidden GLFW window */
    void setupContext ();
    /** @return Whether an EGL surfaceless context could be created */
    bool setupEglContext ();
    /** Fallback context creation through a hidden GLFW window */
    void setupGlfwContext ();
    /** (Re)creates the offscreen framebuffer matching the desktop bounding box */
    void recreateFramebuffer ();
    /** (Re)creates the pixel buffer objects used for the asynchronous readback */
    void recreatePixelBuffers (int width, int height);
    /** Releases the pixel buffer objects */
    void destroyPixelBuffers ();

    ApplicationContext& m_context;
    GnomeShell::GnomeShellIpc m_ipc;
    Input::Drivers::GnomeShellMouseInput m_mouseInput;
    Output::Output* m_output = nullptr;
    uint32_t m_frameCounter = 0;
    bool m_closeRequested = false;
    bool m_paused = false;
    std::chrono::steady_clock::time_point m_startTime;

    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    bool m_usingEgl = false;
    GLFWwindow* m_window = nullptr;

    GLuint m_framebuffer = 0;
    GLuint m_framebufferTexture = 0;
    glm::ivec2 m_framebufferSize = { 0, 0 };

    // Double buffered pixel buffer objects. One frame is copied into the PBO while
    // the previous one is mapped out of the other one, which keeps the GPU from
    // stalling on the readback. This costs one frame of latency.
    GLuint m_pixelBuffers[2] = { 0, 0 };
    std::size_t m_pixelBufferSize = 0;
    int m_pixelBufferIndex = 0;
    bool m_pixelBufferPending = false;
};
} // namespace WallpaperEngine::Render::Drivers