// GNOME Shell extension entry point for the linux-wallpaperengine Hidamari-style
// backend. The engine renders offscreen and publishes frames to a shared memory
// segment; this extension maps it and paints the frames in the background group.

import { Extension } from 'resource:///org/gnome/shell/extensions/extension.js';

import { WallpaperBridge } from './dbus.js';
import { WallpaperState } from './state.js';

export default class LinuxWallpaperEngineExtension extends Extension {
    enable() {
        this._state = new WallpaperState();
        this._bridge = new WallpaperBridge(this._state);
        this._state.setBridge(this._bridge);
        this._bridge.export();
    }

    disable() {
        // the state emits Shutdown through the bridge, so tear it down first
        this._state?.destroy();
        this._bridge?.destroy();
        this._state = null;
        this._bridge = null;
    }
}