// D-Bus control channel exposed by the extension.
//
// The engine acts as a client: it calls Register/Unregister/RequestMonitors and
// listens for the signals emitted here. The extension listens for the engine's
// FrameReady signal, which is only a wake-up (the pixels travel through shared
// memory).

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

export const BUS_NAME = 'org.gnome.Shell.Extensions.LinuxWallpaperEngine';
export const OBJECT_PATH = '/org/linuxwallpaperengine/GnomeBridge';
export const INTERFACE = 'org.linuxwallpaperengine.GnomeBridge';

const INTERFACE_XML = `
<node>
  <interface name="${INTERFACE}">
    <method name="Register">
      <arg type="s" direction="in" name="shmName"/>
      <arg type="u" direction="in" name="width"/>
      <arg type="u" direction="in" name="height"/>
      <arg type="u" direction="in" name="stride"/>
      <arg type="u" direction="in" name="format"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="Unregister"/>
    <method name="RequestMonitors"/>
    <signal name="FrameReady">
      <arg type="u" name="frameId"/>
    </signal>
    <signal name="PointerMoved">
      <arg type="d" name="x"/>
      <arg type="d" name="y"/>
    </signal>
    <signal name="MonitorsChanged">
      <arg type="a(iiiis)" name="monitors"/>
      <arg type="s" name="primary"/>
    </signal>
    <signal name="Pause"/>
    <signal name="Resume"/>
    <signal name="Shutdown"/>
  </interface>
</node>`;

export class WallpaperBridge {
    constructor(state) {
        this._state = state;
        this._impl = null;
        this._nameId = 0;
        this._frameSignalId = 0;
    }

    export() {
        this._impl = Gio.DBusExportedObject.wrapJSObject(INTERFACE_XML, this);
        this._impl.export(Gio.DBus.session, OBJECT_PATH);

        this._nameId = Gio.bus_own_name_on_connection(
            Gio.DBus.session, BUS_NAME, Gio.BusNameOwnerFlags.NONE, null, null);

        // FrameReady is emitted by the engine; use it as a wake-up to read shared memory
        this._frameSignalId = Gio.DBus.session.signal_subscribe(
            null, INTERFACE, 'FrameReady', OBJECT_PATH, null, Gio.DBusSignalFlags.NONE,
            () => {
                this._state.onFrameReady();
            });
    }

    destroy() {
        if (this._frameSignalId !== 0) {
            Gio.DBus.session.signal_unsubscribe(this._frameSignalId);
            this._frameSignalId = 0;
        }

        if (this._nameId !== 0) {
            Gio.bus_unown_name(this._nameId);
            this._nameId = 0;
        }

        if (this._impl !== null) {
            this._impl.unexport();
            this._impl = null;
        }
    }

    // --- methods called by the engine -------------------------------------

    Register(shmName, width, height, stride, format) {
        return this._state.registerFrameBuffer(shmName, width, height, stride, format);
    }

    Unregister() {
        this._state.unregisterFrameBuffer();
    }

    RequestMonitors() {
        this._state.reportMonitors();
    }

    // --- signals sent to the engine ---------------------------------------

    emitMonitorsChanged(monitors, primary) {
        this._emit('MonitorsChanged', new GLib.Variant('(a(iiiis)s)', [monitors, primary]));
    }

    emitPointerMoved(x, y) {
        this._emit('PointerMoved', new GLib.Variant('(dd)', [x, y]));
    }

    emitPause() {
        this._emit('Pause', null);
    }

    emitResume() {
        this._emit('Resume', null);
    }

    emitShutdown() {
        this._emit('Shutdown', null);
    }

    _emit(name, parameters) {
        if (this._impl !== null)
            this._impl.emit_signal(name, parameters);
    }
}