#include "GnomeShellOpenGLDriver.h"

#include "VideoFactories.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Drivers/Output/GnomeShellOutput.h"

#include <EGL/eglext.h>

#include <cstring>
#include <unistd.h>

using namespace WallpaperEngine::Render::Drivers;
using namespace WallpaperEngine::Render::Drivers::GnomeShell;
using namespace WallpaperEngine::Render::Drivers::Output;

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

namespace {
/** How long to wait for the GNOME Shell extension to show up on the bus */
constexpr uint32_t EXTENSION_WAIT_MS = 5000;
} // namespace

GnomeShellOpenGLDriver::GnomeShellOpenGLDriver (ApplicationContext& context, WallpaperApplication& app) :
    VideoDriver (app, m_mouseInput), m_context (context), m_mouseInput (*this),
    m_startTime (std::chrono::steady_clock::now ()) {
    if (!this->m_ipc.isConnected ()) {
	sLog.exception ("Cannot connect to the D-Bus session bus required by the GNOME Shell backend");
    }

    if (!this->m_ipc.waitForExtension (EXTENSION_WAIT_MS)) {
	sLog.exception (
	    "The GNOME Shell bridge extension did not register itself on the bus. Make sure the "
	    "linux-wallpaperengine-gnome extension is installed and enabled"
	);
    }

    this->m_ipc.onPointerMoved = [this] (double x, double y) -> void { this->m_mouseInput.setPosition (x, y); };
    this->m_ipc.onPause = [this] () -> void { this->m_paused = true; };
    this->m_ipc.onResume = [this] () -> void { this->m_paused = false; };
    this->m_ipc.onShutdown = [this] () -> void { this->m_closeRequested = true; };

    // the OpenGL context must exist before the offscreen framebuffer is created
    this->setupContext ();
    // the output blocks until the extension reports the monitor layout
    this->m_output = new GnomeShellOutput (context, *this, this->m_ipc);
    this->recreateFramebuffer ();
}

GnomeShellOpenGLDriver::~GnomeShellOpenGLDriver () {
    delete this->m_output;
    this->m_output = nullptr;

    this->destroyPixelBuffers ();

    if (this->m_framebuffer != 0) {
	glDeleteFramebuffers (1, &this->m_framebuffer);
	this->m_framebuffer = 0;
    }

    if (this->m_framebufferTexture != 0) {
	glDeleteTextures (1, &this->m_framebufferTexture);
	this->m_framebufferTexture = 0;
    }

    if (this->m_usingEgl) {
	eglMakeCurrent (this->m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

	if (this->m_eglContext != EGL_NO_CONTEXT) {
	    eglDestroyContext (this->m_eglDisplay, this->m_eglContext);
	}

	eglTerminate (this->m_eglDisplay);
	eglReleaseThread ();
    } else if (this->m_window != nullptr) {
	glfwDestroyWindow (this->m_window);
	glfwTerminate ();
    }
}

Output::Output& GnomeShellOpenGLDriver::getOutput () { return *this->m_output; }

float GnomeShellOpenGLDriver::getRenderTime () const {
    return std::chrono::duration<float> (std::chrono::steady_clock::now () - this->m_startTime).count ();
}

bool GnomeShellOpenGLDriver::closeRequested () { return this->m_closeRequested; }

void GnomeShellOpenGLDriver::resizeWindow (glm::ivec2 size) { }

void GnomeShellOpenGLDriver::resizeWindow (glm::ivec4 sizeandpos) { }

void GnomeShellOpenGLDriver::showWindow () { }

void GnomeShellOpenGLDriver::hideWindow () { }

glm::ivec2 GnomeShellOpenGLDriver::getFramebufferSize () const {
    if (this->m_output == nullptr) {
	return { 0, 0 };
    }

    return { this->m_output->getFullWidth (), this->m_output->getFullHeight () };
}

uint32_t GnomeShellOpenGLDriver::getFrameCounter () const { return this->m_frameCounter; }

void GnomeShellOpenGLDriver::dispatchEventQueue () {
    static float startTime, endTime, minimumTime = 1.0f / this->m_context.settings.render.maximumFPS;

    // process input/lifecycle signals coming from the extension (monitors, pause, shutdown, pointer)
    this->m_ipc.dispatch ();

    startTime = this->getRenderTime ();

    if (this->m_paused) {
	endTime = this->getRenderTime ();

	if ((endTime - startTime) < minimumTime) {
	    usleep ((minimumTime - (endTime - startTime)) * CLOCKS_PER_SEC);
	}

	return;
    }

    // the bounding box may have changed if the extension reported a new monitor layout
    this->recreateFramebuffer ();

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    for (const auto& [screen, viewport] : this->m_output->getViewports ()) {
	this->getApp ().update (viewport);
    }

    // Start the asynchronous readback of this frame. Reading into a pixel buffer
    // object returns immediately, so the GPU does not have to finish the frame
    // before the next one can be submitted.
    if (this->m_output->haveImageBuffer ()) {
	const int width = this->m_output->getFullWidth ();
	const int height = this->m_output->getFullHeight ();

	if (this->m_pixelBuffers[0] != 0) {
	    glBindBuffer (GL_PIXEL_PACK_BUFFER, this->m_pixelBuffers[this->m_pixelBufferIndex]);
	    glReadPixels (0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	    glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);

	    this->m_pixelBufferIndex ^= 1;
	}

	// Hand the previous frame to the extension. By now its readback has almost
	// always completed, so mapping it does not block.
	if (this->m_pixelBufferPending) {
	    glBindBuffer (GL_PIXEL_PACK_BUFFER, this->m_pixelBuffers[this->m_pixelBufferIndex]);

	    const void* pixels
		= glMapBufferRange (GL_PIXEL_PACK_BUFFER, 0, this->m_pixelBufferSize, GL_MAP_READ_BIT);

	    if (pixels != nullptr) {
		void* buffer = this->m_output->getImageBuffer ();

		if (buffer != nullptr) {
		    std::memcpy (buffer, pixels, this->m_pixelBufferSize);
		}

		glUnmapBuffer (GL_PIXEL_PACK_BUFFER);

		if (buffer != nullptr) {
		    this->m_output->updateRender ();
		}
	    }

	    glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
	}

	this->m_pixelBufferPending = this->m_pixelBuffers[0] != 0;
    }

    this->m_output->updateRender ();
    this->m_frameCounter++;

    endTime = this->getRenderTime ();

    // ensure the frame time is correct to not overrun FPS
    if ((endTime - startTime) < minimumTime) {
	usleep ((minimumTime - (endTime - startTime)) * CLOCKS_PER_SEC);
    }
}

void* GnomeShellOpenGLDriver::getProcAddress (const char* name) const {
    if (this->m_usingEgl) {
	return reinterpret_cast<void*> (eglGetProcAddress (name));
    }

    return reinterpret_cast<void*> (glfwGetProcAddress (name));
}

void GnomeShellOpenGLDriver::setupContext () {
    if (this->setupEglContext ()) {
	sLog.out ("GNOME Shell OpenGL renderer: ", glGetString (GL_RENDERER));
	return;
    }

    sLog.out ("EGL surfaceless context unavailable, falling back to a hidden GLFW window");
    this->setupGlfwContext ();
    sLog.out ("GNOME Shell OpenGL renderer: ", glGetString (GL_RENDERER));
}

bool GnomeShellOpenGLDriver::setupEglContext () {
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay
	= reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC> (eglGetProcAddress ("eglGetPlatformDisplayEXT"));

    if (getPlatformDisplay != nullptr) {
	this->m_eglDisplay = getPlatformDisplay (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    }

    if (this->m_eglDisplay == EGL_NO_DISPLAY) {
	this->m_eglDisplay = eglGetDisplay (EGL_DEFAULT_DISPLAY);
    }

    if (this->m_eglDisplay == EGL_NO_DISPLAY) {
	return false;
    }

    EGLint major = 0;
    EGLint minor = 0;

    if (eglInitialize (this->m_eglDisplay, &major, &minor) == EGL_FALSE) {
	return false;
    }

    if (eglBindAPI (EGL_OPENGL_API) == EGL_FALSE) {
	return false;
    }

    const EGLint configAttributes[] = {
	EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	EGL_RED_SIZE,	     8,		 EGL_GREEN_SIZE,	 8,
	EGL_BLUE_SIZE,	     8,		 EGL_ALPHA_SIZE,	 8,
	EGL_NONE,
    };

    EGLint matchedConfigs = 0;
    EGLConfig config = nullptr;

    if (eglChooseConfig (this->m_eglDisplay, configAttributes, &config, 1, &matchedConfigs) == EGL_FALSE
	|| matchedConfigs == 0) {
	return false;
    }

    const EGLint contextAttributes[] = {
	EGL_CONTEXT_MAJOR_VERSION_KHR,
	3,
	EGL_CONTEXT_MINOR_VERSION_KHR,
	3,
	EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
	EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
	EGL_NONE,
    };

    this->m_eglContext
	= eglCreateContext (this->m_eglDisplay, config, EGL_NO_CONTEXT, contextAttributes);

    if (this->m_eglContext == EGL_NO_CONTEXT) {
	return false;
    }

    if (eglMakeCurrent (this->m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, this->m_eglContext) == EGL_FALSE) {
	eglDestroyContext (this->m_eglDisplay, this->m_eglContext);
	this->m_eglContext = EGL_NO_CONTEXT;
	return false;
    }

    this->m_usingEgl = true;

    glewExperimental = GL_TRUE;

    if (const GLenum result = glewInit (); result != GLEW_OK && result != GLEW_ERROR_NO_GLX_DISPLAY) {
	sLog.error ("Failed to initialize GLEW: ", glewGetErrorString (result));
    }

    return true;
}

void GnomeShellOpenGLDriver::setupGlfwContext () {
    if (glfwInit () == GLFW_FALSE) {
	sLog.exception ("Failed to initialize glfw");
    }

    glfwWindowHint (GLFW_SAMPLES, 4);
    glfwWindowHint (GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint (GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint (GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint (GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHintString (GLFW_X11_CLASS_NAME, "linux-wallpaperengine");
    glfwWindowHintString (GLFW_X11_INSTANCE_NAME, "linux-wallpaperengine");

    this->m_window = glfwCreateWindow (640, 480, "wallpaperengine", nullptr, nullptr);

    if (this->m_window == nullptr) {
	sLog.exception ("Cannot create the hidden GLFW window required by the GNOME Shell backend");
    }

    glfwMakeContextCurrent (this->m_window);

    glewExperimental = GL_TRUE;

    if (const GLenum result = glewInit (); result != GLEW_OK) {
	sLog.error ("Failed to initialize GLEW: ", glewGetErrorString (result));
    }
}

void GnomeShellOpenGLDriver::recreateFramebuffer () {
    const int width = this->m_output->getFullWidth ();
    const int height = this->m_output->getFullHeight ();

    if (width <= 0 || height <= 0) {
	return;
    }

    if (this->m_framebuffer != 0 && width == this->m_framebufferSize.x && height == this->m_framebufferSize.y) {
	return;
    }

    if (this->m_framebuffer != 0) {
	glDeleteFramebuffers (1, &this->m_framebuffer);
	this->m_framebuffer = 0;
    }

    if (this->m_framebufferTexture != 0) {
	glDeleteTextures (1, &this->m_framebufferTexture);
	this->m_framebufferTexture = 0;
    }

    glGenTextures (1, &this->m_framebufferTexture);
    glBindTexture (GL_TEXTURE_2D, this->m_framebufferTexture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture (GL_TEXTURE_2D, 0);

    glGenFramebuffers (1, &this->m_framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_framebufferTexture, 0);

    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
	sLog.exception ("The offscreen framebuffer for the GNOME Shell backend is incomplete");
    }

    glBindFramebuffer (GL_FRAMEBUFFER, 0);

    this->m_framebufferSize = { width, height };
    this->recreatePixelBuffers (width, height);

    // route the wallpapers' final composite into this framebuffer
    this->getApp ().setDestinationFramebuffer (this->m_framebuffer);

    sLog.out ("GNOME Shell offscreen framebuffer is ", width, "x", height);
}

void GnomeShellOpenGLDriver::recreatePixelBuffers (int width, int height) {
    const auto size = static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 4;

    if (this->m_pixelBuffers[0] != 0 && size == this->m_pixelBufferSize) {
	return;
    }

    this->destroyPixelBuffers ();

    glGenBuffers (2, this->m_pixelBuffers);

    for (const GLuint pixelBuffer : this->m_pixelBuffers) {
	glBindBuffer (GL_PIXEL_PACK_BUFFER, pixelBuffer);
	glBufferData (GL_PIXEL_PACK_BUFFER, size, nullptr, GL_STREAM_READ);
    }

    glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);

    this->m_pixelBufferSize = size;
}

void GnomeShellOpenGLDriver::destroyPixelBuffers () {
    if (this->m_pixelBuffers[0] != 0) {
	glDeleteBuffers (2, this->m_pixelBuffers);
	this->m_pixelBuffers[0] = 0;
	this->m_pixelBuffers[1] = 0;
    }

    this->m_pixelBufferSize = 0;
    this->m_pixelBufferIndex = 0;
    this->m_pixelBufferPending = false;
}

__attribute__ ((constructor)) void registerGnomeShellOpenGLDriver () {
    sVideoFactories.registerDriver (
	ApplicationContext::GNOME_SHELL, DEFAULT_WINDOW_NAME,
	[] (ApplicationContext& context, WallpaperApplication& application) -> std::unique_ptr<VideoDriver> {
	    return std::make_unique<GnomeShellOpenGLDriver> (context, application);
	}
    );
}