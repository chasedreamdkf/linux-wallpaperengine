#include "GnomeShellIpc.h"

#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"

#include <chrono>
#include <thread>

using namespace WallpaperEngine::Render::Drivers::GnomeShell;
using WallpaperEngine::Data::Utils::ScopeGuard;

GnomeShellIpc::GnomeShellIpc () {
    DBusError err;
    dbus_error_init (&err);

    this->m_connection = dbus_bus_get (DBUS_BUS_SESSION, &err);

    if (this->m_connection == nullptr) {
	sLog.error ("GnomeShellIpc: cannot connect to the session bus: ", err.message ?: "unknown error");
	dbus_error_free (&err);
	return;
    }

    // the engine must not take the whole process down when the shell restarts
    dbus_connection_set_exit_on_disconnect (this->m_connection, false);

    this->m_uniqueName = dbus_bus_get_unique_name (this->m_connection) ?: "";

    const std::string match = std::string ("type='signal',interface='") + GNOME_BRIDGE_INTERFACE + "',path='"
	+ GNOME_BRIDGE_PATH + "'";

    dbus_bus_add_match (this->m_connection, match.c_str (), &err);

    if (dbus_error_is_set (&err)) {
	sLog.error ("GnomeShellIpc: cannot add signal match: ", err.message ?: "unknown error");
	dbus_error_free (&err);
    }

    dbus_connection_add_filter (this->m_connection, &GnomeShellIpc::messageFilter, this, nullptr);
    dbus_connection_flush (this->m_connection);
}

GnomeShellIpc::~GnomeShellIpc () {
    if (this->m_connection == nullptr) {
	return;
    }

    dbus_connection_remove_filter (this->m_connection, &GnomeShellIpc::messageFilter, this);
    dbus_connection_unref (this->m_connection);
    this->m_connection = nullptr;
}

bool GnomeShellIpc::isConnected () const { return this->m_connection != nullptr; }

bool GnomeShellIpc::waitForExtension (uint32_t timeoutMs) const {
    if (this->m_connection == nullptr) {
	return false;
    }

    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (timeoutMs);

    while (true) {
	DBusError err;
	dbus_error_init (&err);

	const dbus_bool_t hasOwner = dbus_bus_name_has_owner (this->m_connection, GNOME_BRIDGE_NAME, &err);

	if (dbus_error_is_set (&err)) {
	    dbus_error_free (&err);
	}

	if (hasOwner) {
	    return true;
	}

	if (std::chrono::steady_clock::now () >= deadline) {
	    return false;
	}

	std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
}

bool GnomeShellIpc::registerFrameBuffer (
    const std::string& shmName, uint32_t width, uint32_t height, uint32_t stride, uint32_t format
) {
    if (this->m_connection == nullptr) {
	return false;
    }

    DBusMessage* message
	= dbus_message_new_method_call (GNOME_BRIDGE_NAME, GNOME_BRIDGE_PATH, GNOME_BRIDGE_INTERFACE, "Register");

    if (message == nullptr) {
	return false;
    }

    ScopeGuard messageGuard ([message] { dbus_message_unref (message); });

    const char* name = shmName.c_str ();
    dbus_message_append_args (
	message, DBUS_TYPE_STRING, &name, DBUS_TYPE_UINT32, &width, DBUS_TYPE_UINT32, &height, DBUS_TYPE_UINT32, &stride,
	DBUS_TYPE_UINT32, &format, DBUS_TYPE_INVALID
    );

    DBusError err;
    dbus_error_init (&err);

    DBusMessage* reply
	= dbus_connection_send_with_reply_and_block (this->m_connection, message, DBUS_TIMEOUT_USE_DEFAULT, &err);

    if (reply == nullptr) {
	sLog.error ("GnomeShellIpc: Register() failed: ", err.message ?: "unknown error");
	dbus_error_free (&err);
	return false;
    }

    ScopeGuard replyGuard ([reply] { dbus_message_unref (reply); });

    dbus_bool_t ok = FALSE;

    if (!dbus_message_get_args (reply, nullptr, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID)) {
	return false;
    }

    return ok == TRUE;
}

void GnomeShellIpc::unregisterFrameBuffer () {
    if (this->m_connection == nullptr) {
	return;
    }

    DBusMessage* message
	= dbus_message_new_method_call (GNOME_BRIDGE_NAME, GNOME_BRIDGE_PATH, GNOME_BRIDGE_INTERFACE, "Unregister");

    if (message == nullptr) {
	return;
    }

    ScopeGuard guard ([message] { dbus_message_unref (message); });

    dbus_connection_send (this->m_connection, message, nullptr);
    dbus_connection_flush (this->m_connection);
}

void GnomeShellIpc::requestMonitors () {
    if (this->m_connection == nullptr) {
	return;
    }

    DBusMessage* message
	= dbus_message_new_method_call (GNOME_BRIDGE_NAME, GNOME_BRIDGE_PATH, GNOME_BRIDGE_INTERFACE, "RequestMonitors");

    if (message == nullptr) {
	return;
    }

    ScopeGuard guard ([message] { dbus_message_unref (message); });

    dbus_connection_send (this->m_connection, message, nullptr);
    dbus_connection_flush (this->m_connection);
}

void GnomeShellIpc::notifyFrameReady (uint32_t frameId) {
    if (this->m_connection == nullptr) {
	return;
    }

    DBusMessage* message
	= dbus_message_new_signal (GNOME_BRIDGE_PATH, GNOME_BRIDGE_INTERFACE, "FrameReady");

    if (message == nullptr) {
	return;
    }

    ScopeGuard guard ([message] { dbus_message_unref (message); });

    dbus_message_append_args (message, DBUS_TYPE_UINT32, &frameId, DBUS_TYPE_INVALID);
    dbus_connection_send (this->m_connection, message, nullptr);
}

void GnomeShellIpc::dispatch () {
    if (this->m_connection == nullptr) {
	return;
    }

    dbus_connection_flush (this->m_connection);
    dbus_connection_read_write (this->m_connection, 0);

    while (dbus_connection_dispatch (this->m_connection) == DBUS_DISPATCH_DATA_REMAINS)
	;
}

DBusHandlerResult GnomeShellIpc::messageFilter (DBusConnection*, DBusMessage* message, void* userData) {
    auto* self = static_cast<GnomeShellIpc*> (userData);

    if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_SIGNAL
	|| !dbus_message_has_interface (message, GNOME_BRIDGE_INTERFACE)
	|| !dbus_message_has_path (message, GNOME_BRIDGE_PATH)) {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    // ignore our own outgoing signals (e.g. FrameReady)
    const char* sender = dbus_message_get_sender (message);

    if (sender != nullptr && self->m_uniqueName == sender) {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (dbus_message_is_signal (message, GNOME_BRIDGE_INTERFACE, "PointerMoved")) {
	double x = 0;
	double y = 0;

	if (dbus_message_get_args (message, nullptr, DBUS_TYPE_DOUBLE, &x, DBUS_TYPE_DOUBLE, &y, DBUS_TYPE_INVALID)
	    && self->onPointerMoved) {
	    self->onPointerMoved (x, y);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal (message, GNOME_BRIDGE_INTERFACE, "MonitorsChanged")) {
	self->handleMonitorsChanged (message);
	return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal (message, GNOME_BRIDGE_INTERFACE, "Pause")) {
	if (self->onPause) {
	    self->onPause ();
	}

	return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal (message, GNOME_BRIDGE_INTERFACE, "Resume")) {
	if (self->onResume) {
	    self->onResume ();
	}

	return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal (message, GNOME_BRIDGE_INTERFACE, "Shutdown")) {
	if (self->onShutdown) {
	    self->onShutdown ();
	}

	return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void GnomeShellIpc::handleMonitorsChanged (DBusMessage* message) {
    DBusMessageIter iter;

    if (!dbus_message_iter_init (message, &iter) || dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY) {
	return;
    }

    std::vector<MonitorInfo> monitors;
    DBusMessageIter array;
    dbus_message_iter_recurse (&iter, &array);

    while (dbus_message_iter_get_arg_type (&array) == DBUS_TYPE_STRUCT) {
	DBusMessageIter entry;
	dbus_message_iter_recurse (&array, &entry);

	MonitorInfo info;

	dbus_message_iter_get_basic (&entry, &info.x);
	dbus_message_iter_next (&entry);
	dbus_message_iter_get_basic (&entry, &info.y);
	dbus_message_iter_next (&entry);
	dbus_message_iter_get_basic (&entry, &info.width);
	dbus_message_iter_next (&entry);
	dbus_message_iter_get_basic (&entry, &info.height);
	dbus_message_iter_next (&entry);

	const char* name = nullptr;
	dbus_message_iter_get_basic (&entry, &name);
	info.name = name ?: "";

	monitors.push_back (std::move (info));
	dbus_message_iter_next (&array);
    }

    std::string primary;

    if (dbus_message_iter_next (&iter) && dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_STRING) {
	const char* value = nullptr;
	dbus_message_iter_get_basic (&iter, &value);
	primary = value ?: "";
    }

    if (this->onMonitorsChanged) {
	this->onMonitorsChanged (monitors, primary);
    }
}