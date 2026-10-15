#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <dbus/dbus.h>

namespace WallpaperEngine::Render::Drivers::GnomeShell {
/** Well-known name the GNOME Shell extension owns */
constexpr const char* GNOME_BRIDGE_NAME = "org.gnome.Shell.Extensions.LinuxWallpaperEngine";
/** Object path shared by both peers */
constexpr const char* GNOME_BRIDGE_PATH = "/org/linuxwallpaperengine/GnomeBridge";
/** Interface shared by both peers */
constexpr const char* GNOME_BRIDGE_INTERFACE = "org.linuxwallpaperengine.GnomeBridge";

/** A single monitor as reported by the extension */
struct MonitorInfo {
    std::string name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/**
 * D-Bus control channel between the engine and the GNOME Shell bridge extension.
 *
 * The engine acts as a client: it calls the extension's methods (Register,
 * Unregister, RequestMonitors) and listens for its signals (PointerMoved,
 * MonitorsChanged, Pause, Resume, Shutdown). Frame notifications go the other
 * way as a lightweight `FrameReady` signal; the actual pixels travel through
 * the shared memory segment handled by SharedFrameBuffer.
 */
class GnomeShellIpc {
public:
    GnomeShellIpc ();
    ~GnomeShellIpc ();

    GnomeShellIpc (const GnomeShellIpc&) = delete;
    GnomeShellIpc& operator= (const GnomeShellIpc&) = delete;

    /** @return Whether the session bus connection was established */
    [[nodiscard]] bool isConnected () const;

    /** Blocks until the extension owns its well-known name or the timeout elapses */
    bool waitForExtension (uint32_t timeoutMs) const;

    /** Tells the extension which shared memory segment to map. @return Whether it succeeded */
    bool registerFrameBuffer (const std::string& shmName, uint32_t width, uint32_t height, uint32_t stride, uint32_t format);

    /** Tells the extension to stop using the shared memory segment */
    void unregisterFrameBuffer ();

    /** Asks the extension to (re-)report the current monitor layout */
    void requestMonitors ();

    /** Emits the FrameReady signal carrying the given frame id */
    void notifyFrameReady (uint32_t frameId);

    /** Reads and dispatches pending messages. Should be called once per frame */
    void dispatch ();

    /** Invoked when the extension reports a new pointer position (relative to the desktop bounding box, top-left origin) */
    std::function<void (double, double)> onPointerMoved;
    /** Invoked when the extension reports the monitor layout */
    std::function<void (const std::vector<MonitorInfo>&, const std::string&)> onMonitorsChanged;
    /** Invoked when the extension asks the engine to pause */
    std::function<void ()> onPause;
    /** Invoked when the extension asks the engine to resume */
    std::function<void ()> onResume;
    /** Invoked when the extension asks the engine to shut down */
    std::function<void ()> onShutdown;

private:
    static DBusHandlerResult messageFilter (DBusConnection* connection, DBusMessage* message, void* userData);

    void handleMonitorsChanged (DBusMessage* message);

    DBusConnection* m_connection = nullptr;
    std::string m_uniqueName;
};
} // namespace WallpaperEngine::Render::Drivers::GnomeShell