// Shared memory reader for the frames published by the linux-wallpaperengine
// GNOME Shell backend.
//
// The segment layout mirrors src/WallpaperEngine/Render/Drivers/GnomeShell/SharedFrameBuffer.h:
//
//   offset 0  : FrameBufferHeader (8 * uint32, little-endian)
//   offset 32 : buffer[0] pixels (stride * height bytes)
//   offset 32 + stride * height : buffer[1] pixels
//
// The `seq` field is a seqlock: an even value means the buffer at index (seq / 2)
// is stable, an odd value means a write is in progress.

import GLib from 'gi://GLib';

const HEADER_SIZE = 32;
const SEQ_OFFSET = 8;

export class SharedFrame {
    constructor(path, width, height, stride, format) {
        this._path = path;
        this._width = width;
        this._height = height;
        this._stride = stride;
        this._format = format;
        this._frameSize = stride * height;
        this._mapped = null;
        this._bytes = null;
        this._data = null;

        this._map();
    }

    _map() {
        this._mapped = new GLib.MappedFile(this._path, false);
        this._bytes = this._mapped.get_bytes();
        this._data = this._bytes.get_data();

        const required = HEADER_SIZE + 2 * this._frameSize;

        if (this._data === null || this._data.length < required) {
            throw new Error(`shared frame buffer ${this._path} is smaller than expected`);
        }
    }

    /**
     * @returns {Uint8Array|null} a view over the stable buffer, or null if a writer is active
     */
    readFrame() {
        if (this._data === null)
            return null;

        const view = new DataView(this._data.buffer, this._data.byteOffset, this._data.byteLength);

        for (let attempt = 0; attempt < 16; attempt++) {
            const seq = view.getUint32(SEQ_OFFSET, true);

            // odd means a writer is filling a buffer right now
            if ((seq & 1) !== 0)
                continue;

            // make sure no write started while we were picking the buffer
            if (view.getUint32(SEQ_OFFSET, true) !== seq)
                continue;

            const index = (seq >>> 1) & 1;
            const offset = HEADER_SIZE + index * this._frameSize;

            return this._data.subarray(offset, offset + this._frameSize);
        }

        return null;
    }

    close() {
        this._data = null;
        this._bytes = null;
        this._mapped = null;
    }

    get width() {
        return this._width;
    }

    get height() {
        return this._height;
    }

    get stride() {
        return this._stride;
    }

    get format() {
        return this._format;
    }
}