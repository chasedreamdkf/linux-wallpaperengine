#include "SharedFrameBuffer.h"

#include "WallpaperEngine/Logging/Log.h"

#include <atomic>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace WallpaperEngine::Render::Drivers::GnomeShell;

namespace {
std::atomic<uint32_t> s_instanceCounter { 0 };

std::string buildSegmentName () {
    return "lwe-gnome-" + std::to_string (getpid ()) + "-" + std::to_string (s_instanceCounter.fetch_add (1));
}
} // namespace

SharedFrameBuffer::SharedFrameBuffer (uint32_t width, uint32_t height, uint32_t stride) :
    m_width (width), m_height (height), m_stride (stride) {
    this->m_name = buildSegmentName ();
    this->m_path = "/dev/shm/" + this->m_name;

    const size_t pixelBufferSize = static_cast<size_t> (stride) * height;
    this->m_mappingSize = sizeof (FrameBufferHeader) + pixelBufferSize * 2;

    // prefer an anonymous memfd that we hard-link into /dev/shm: the kernel
    // reclaims it automatically if we crash, preventing stale segments
    this->m_fd = memfd_create (this->m_name.c_str (), 0);

    if (this->m_fd >= 0) {
	if (ftruncate (this->m_fd, this->m_mappingSize) != 0
	    || linkat (this->m_fd, "", AT_FDCWD, this->m_path.c_str (), AT_EMPTY_PATH) != 0) {
	    ::close (this->m_fd);
	    this->m_fd = -1;
	}
    }

    // fall back to a classic named POSIX shared memory object
    if (this->m_fd < 0) {
	this->m_fd = shm_open (this->m_name.c_str (), O_RDWR | O_CREAT | O_EXCL, 0600);

	if (this->m_fd < 0) {
	    sLog.exception ("Cannot create shared memory segment ", this->m_path, ": ", std::strerror (errno));
	}

	if (ftruncate (this->m_fd, this->m_mappingSize) != 0) {
	    sLog.exception ("Cannot size shared memory segment ", this->m_path, ": ", std::strerror (errno));
	}
    }

    this->m_linked = true;

    this->m_mapping = mmap (nullptr, this->m_mappingSize, PROT_READ | PROT_WRITE, MAP_SHARED, this->m_fd, 0);

    if (this->m_mapping == MAP_FAILED) {
	this->m_mapping = nullptr;
	sLog.exception ("Cannot map shared memory segment ", this->m_path, ": ", std::strerror (errno));
    }

    this->m_header = static_cast<FrameBufferHeader*> (this->m_mapping);
    this->m_pixels = static_cast<uint8_t*> (this->m_mapping) + sizeof (FrameBufferHeader);

    this->m_header->magic = MAGIC;
    this->m_header->version = VERSION;
    this->m_header->seq.store (0, std::memory_order_release);
    this->m_header->width = width;
    this->m_header->height = height;
    this->m_header->stride = stride;
    this->m_header->format = FORMAT_RGBA8888;
    this->m_header->backIndex = 0;

    std::memset (this->m_pixels, 0, pixelBufferSize * 2);

    sLog.out ("Allocated GNOME Shell frame buffer ", this->m_path, " (", width, "x", height, ")");
}

SharedFrameBuffer::~SharedFrameBuffer () { this->close (); }

const std::string& SharedFrameBuffer::mapName () const { return this->m_path; }

uint32_t SharedFrameBuffer::getWidth () const { return this->m_width; }

uint32_t SharedFrameBuffer::getHeight () const { return this->m_height; }

uint32_t SharedFrameBuffer::getStride () const { return this->m_stride; }

uint8_t* SharedFrameBuffer::bufferAt (uint32_t index) const {
    return this->m_pixels + static_cast<size_t> (index) * this->m_stride * this->m_height;
}

void* SharedFrameBuffer::beginWrite () {
    const uint32_t back = this->m_header->backIndex;
    // odd sequence so readers know this buffer is being written to
    this->m_header->seq.store (back * 2 + 1, std::memory_order_release);
    return this->bufferAt (back);
}

void SharedFrameBuffer::endWrite () {
    const uint32_t back = this->m_header->backIndex;
    // even sequence so the buffer can be read as (seq / 2)
    this->m_header->seq.store (back * 2, std::memory_order_release);
    this->m_header->backIndex = back ^ 1;
}

uint32_t SharedFrameBuffer::publishFrameId () { return ++this->m_frameId; }

void SharedFrameBuffer::close () {
    if (this->m_mapping != nullptr) {
	munmap (this->m_mapping, this->m_mappingSize);
	this->m_mapping = nullptr;
	this->m_header = nullptr;
	this->m_pixels = nullptr;
    }

    if (this->m_fd >= 0) {
	::close (this->m_fd);
	this->m_fd = -1;
    }

    if (this->m_linked) {
	shm_unlink (this->m_name.c_str ());
	this->m_linked = false;
    }
}