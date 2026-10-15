#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace WallpaperEngine::Render::Drivers::GnomeShell {
/**
 * Header living at the very beginning of the shared memory segment.
 *
 * The layout is a plain, little-endian POD so the GJS side can parse it with a
 * simple DataView. The `seq` field implements a seqlock over a double buffer:
 *
 *   even -> the buffer at index (seq / 2) is stable and safe to read
 *   odd  -> a write to the buffer at index (seq / 2) is in progress
 */
struct FrameBufferHeader {
    /** Magic value identifying the segment, spells "LWEG" in memory */
    uint32_t magic;
    /** Layout version, bumped whenever the header changes */
    uint32_t version;
    /** Seqlock sequence, see above */
    std::atomic<uint32_t> seq;
    /** Frame width in pixels */
    uint32_t width;
    /** Frame height in pixels */
    uint32_t height;
    /** Bytes per row (may be larger than width * 4) */
    uint32_t stride;
    /** Pixel format, see SharedFrameBuffer::FORMAT_* */
    uint32_t format;
    /** Index (0/1) of the buffer the next frame will be written to */
    uint32_t backIndex;
};

/**
 * Double-buffered, seqlock-protected shared memory frame channel.
 *
 * The engine writes whole frames into the back buffer and publishes them by
 * flipping the seqlock sequence. The GNOME Shell extension maps the very same
 * segment by name (`/dev/shm/lwe-gnome-<pid>-<n>`) and reads the stable buffer.
 */
class SharedFrameBuffer {
public:
    /** Magic value stored in FrameBufferHeader::magic */
    static constexpr uint32_t MAGIC = 0x4745574C; // "LWEG" read as a little-endian uint32
    /** Current header layout version */
    static constexpr uint32_t VERSION = 1;
    /** RGBA8888 pixel format, the only one currently supported */
    static constexpr uint32_t FORMAT_RGBA8888 = 0;

    SharedFrameBuffer (uint32_t width, uint32_t height, uint32_t stride);
    ~SharedFrameBuffer ();

    SharedFrameBuffer (const SharedFrameBuffer&) = delete;
    SharedFrameBuffer& operator= (const SharedFrameBuffer&) = delete;

    /** @return The absolute /dev/shm path the extension has to map */
    [[nodiscard]] const std::string& mapName () const;
    [[nodiscard]] uint32_t getWidth () const;
    [[nodiscard]] uint32_t getHeight () const;
    [[nodiscard]] uint32_t getStride () const;

    /**
     * Marks the start of a frame write and returns the back buffer.
     * The caller must fill exactly (stride * height) bytes before endWrite().
     */
    void* beginWrite ();

    /**
     * Publishes the buffer returned by the matching beginWrite() call and flips
     * the back buffer so the next frame goes to the other one.
     */
    void endWrite ();

    /** @return A monotonically increasing frame identifier */
    uint32_t publishFrameId ();

    /** Unmaps and unlinks the segment. Safe to call more than once. */
    void close ();

private:
    [[nodiscard]] uint8_t* bufferAt (uint32_t index) const;

    std::string m_name;
    std::string m_path;
    int m_fd = -1;
    bool m_linked = false;
    void* m_mapping = nullptr;
    size_t m_mappingSize = 0;
    FrameBufferHeader* m_header = nullptr;
    uint8_t* m_pixels = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_stride = 0;
    uint32_t m_frameId = 0;
};
} // namespace WallpaperEngine::Render::Drivers::GnomeShell