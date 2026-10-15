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
//
// GLib.MappedFile cannot be used here: GJS turns the GBytes returned by
// MappedFile.get_bytes() into a *copy* taken when the file is mapped, so the
// reader would keep re-reading the segment as it looked at mapping time. The
// frames are read from the file instead (one read per frame, see the README).

import Gio from 'gi://Gio';
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
        this._requiredSize = HEADER_SIZE + 2 * this._frameSize;
        this._stream = null;

        this._open();
    }

    _open() {
        const file = Gio.File.new_for_path(this._path);
        const info = file.query_info('standard::size', Gio.FileQueryInfoFlags.NONE, null);

        if (info.get_size() < this._requiredSize)
            throw new Error(`shared frame buffer ${this._path} is smaller than expected`);

        this._stream = file.read(null);
    }

    _readAt(offset, size) {
        this._stream.seek(offset, GLib.SeekType.SET, null);

        const bytes = this._stream.read_bytes(size, null);

        if (bytes.get_size() < size)
            return null;

        return bytes.get_data();
    }

    _readSequence() {
        const header = this._readAt(SEQ_OFFSET, 4);

        if (header === null)
            return null;

        return new DataView(header.buffer, header.byteOffset, 4).getUint32(0, true);
    }

    /**
     * @returns {Uint8Array|null} the stable frame, or null if a writer is active
     */
    readFrame() {
        for (let attempt = 0; attempt < 8; attempt++) {
            const seq = this._readSequence();

            if (seq === null || (seq & 1) !== 0)
                continue;

            const index = (seq >>> 1) & 1;
            const pixels = this._readAt(HEADER_SIZE + index * this._frameSize, this._frameSize);

            if (pixels === null)
                return null;

            // a new frame was published while this one was being copied, retry
            if (this._readSequence() !== seq)
                continue;

            return pixels;
        }

        return null;
    }

    close() {
        if (this._stream !== null) {
            this._stream.close(null);
            this._stream = null;
        }
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