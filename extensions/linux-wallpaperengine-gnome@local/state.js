// Keeps the Wallpaper Engine actor in sync with the shell state: it owns the
// Clutter actor inserted into the background group, maps the shared frame buffer,
// reports the monitor layout / pointer position and reacts to the session mode
// (lock screen) changes.

import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import { SharedFrame } from './shm.js';

/** Pointer polling interval, matching a ~30Hz update rate */
const POINTER_INTERVAL_MS = 33;

export class WallpaperState {
    constructor() {
        this._bridge = null;
        this._frame = null;

        this._image = new Clutter.Image();

        this._actor = new Clutter.Actor({
            name: 'linux-wallpaperengine',
            reactive: false,
        });
        // Non-public hack (same as Hidamari): a barely visible background colour keeps
        // Mutter from skipping the actor while it has no content yet.
        this._actor.set_style('background-color:rgba(0,0,0,0.01);');
        this._actor.set_content(this._image);
        this._actor.hide();

        this._bounds = { x: 0, y: 0, width: 0, height: 0 };

        // `_backgroundGroup` is not public API, but it is where the desktop wallpaper lives
        Main.layoutManager._backgroundGroup.add_child(this._actor);

        this._layoutSignalId = Main.layoutManager.connect('monitors-changed', () => this.reportMonitors());
        this._sessionSignalId = Main.sessionMode.connect('updated', () => this._syncSessionMode());
        this._pointerTimerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, POINTER_INTERVAL_MS, () => {
            this._reportPointer();
            return GLib.SOURCE_CONTINUE;
        });

        this._syncSessionMode();
        this.reportMonitors();
    }

    setBridge(bridge) {
        this._bridge = bridge;
    }

    // --- called by the bridge ---------------------------------------------

    registerFrameBuffer(shmName, width, height, stride, format) {
        this.unregisterFrameBuffer();

        try {
            this._frame = new SharedFrame(shmName, width, height, stride, format);
        } catch (e) {
            logError(e, 'linux-wallpaperengine: cannot map the shared frame buffer');
            this._frame = null;
            return false;
        }

        this._actor.set_size(width, height);
        this._syncSessionMode();

        return true;
    }

    unregisterFrameBuffer() {
        if (this._frame !== null) {
            this._frame.close();
            this._frame = null;
        }
    }

    onFrameReady() {
        if (this._frame === null)
            return;

        const pixels = this._frame.readFrame();

        if (pixels === null)
            return;

        this._image.set_data(
            pixels, Cogl.PixelFormat.RGBA_8888,
            this._frame.width, this._frame.height, this._frame.stride);
    }

    reportMonitors() {
        const monitors = Main.layoutManager.monitors ?? [];
        const primary = Main.layoutManager.primaryMonitor;

        if (monitors.length === 0)
            return;

        let minX = Infinity;
        let minY = Infinity;
        let maxX = -Infinity;
        let maxY = -Infinity;
        const reported = [];

        for (const monitor of monitors) {
            minX = Math.min(minX, monitor.x);
            minY = Math.min(minY, monitor.y);
            maxX = Math.max(maxX, monitor.x + monitor.width);
            maxY = Math.max(maxY, monitor.y + monitor.height);
            reported.push([monitor.x, monitor.y, monitor.width, monitor.height, monitor.index.toString()]);
        }

        this._bounds = { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
        this._actor.set_position(minX, minY);

        if (this._frame !== null)
            this._actor.set_size(this._bounds.width, this._bounds.height);

        if (this._bridge !== null)
            this._bridge.emitMonitorsChanged(reported, primary !== undefined ? primary.index.toString() : '');
    }

    destroy() {
        if (this._pointerTimerId !== 0) {
            GLib.source_remove(this._pointerTimerId);
            this._pointerTimerId = 0;
        }

        if (this._layoutSignalId !== 0) {
            Main.layoutManager.disconnect(this._layoutSignalId);
            this._layoutSignalId = 0;
        }

        if (this._sessionSignalId !== 0) {
            Main.sessionMode.disconnect(this._sessionSignalId);
            this._sessionSignalId = 0;
        }

        if (this._bridge !== null)
            this._bridge.emitShutdown();

        this.unregisterFrameBuffer();
        this._actor.destroy();
    }

    // --- internals ---------------------------------------------------------

    _syncSessionMode() {
        const locked = Main.sessionMode.isLocked === true;
        const shouldShow = this._frame !== null && !locked;

        if (shouldShow)
            this._actor.show();
        else
            this._actor.hide();

        if (this._bridge === null)
            return;

        if (locked)
            this._bridge.emitPause();
        else
            this._bridge.emitResume();
    }

    _reportPointer() {
        if (this._bridge === null || this._frame === null)
            return;

        const [x, y] = global.get_pointer();

        this._bridge.emitPointerMoved(x - this._bounds.x, y - this._bounds.y);
    }
}