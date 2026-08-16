#include "shmlog/ShmMapping.h"

#include <cstdint>

namespace shmlog {

namespace {

DWORD MapAccessFor(ShmMapping::Mode mode) noexcept {
    return mode == ShmMapping::Mode::ReadOnly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
}

// Win32 splits a 64-bit size across two DWORDs; on a 32-bit size_t the high
// word is simply zero.
DWORD SizeHigh(std::size_t size) noexcept {
    return static_cast<DWORD>(static_cast<uint64_t>(size) >> 32);
}
DWORD SizeLow(std::size_t size) noexcept {
    return static_cast<DWORD>(static_cast<uint64_t>(size) & 0xFFFF'FFFFu);
}

} // namespace

ShmMapping::~ShmMapping() {
    close();
}

ShmMapping::ShmMapping(ShmMapping&& other) noexcept
    : m_hMemMapFile{other.m_hMemMapFile}
    , m_ptr{other.m_ptr}
{
    other.m_hMemMapFile = nullptr;
    other.m_ptr = nullptr;
}

ShmMapping& ShmMapping::operator=(ShmMapping&& other) noexcept {
    if (this != &other) {
        close();
        m_hMemMapFile = other.m_hMemMapFile;
        m_ptr = other.m_ptr;
        other.m_hMemMapFile = nullptr;
        other.m_ptr = nullptr;
    }
    return *this;
}

bool ShmMapping::open(const std::string& name, std::size_t size, Mode mode) {
    if (isOpen()) {
        return false;
    }

    const DWORD mapAccess = MapAccessFor(mode);

    m_hMemMapFile = ::OpenFileMappingA(mapAccess, FALSE, name.c_str());
    if (m_hMemMapFile == nullptr) {
        return false;
    }

    m_ptr = ::MapViewOfFile(m_hMemMapFile, mapAccess, 0, 0, size);
    if (m_ptr == nullptr) {
        ::CloseHandle(m_hMemMapFile);
        m_hMemMapFile = nullptr;
        return false;
    }

    return true;
}

bool ShmMapping::create(const std::string& name, std::size_t size, Mode mode, bool* isNewOut) {
    if (isOpen()) {
        return false;
    }

    const DWORD protect   = (mode == Mode::ReadOnly) ? PAGE_READONLY : PAGE_READWRITE;
    const DWORD mapAccess = MapAccessFor(mode);

    m_hMemMapFile = ::CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        protect,
        SizeHigh(size),
        SizeLow(size),
        name.c_str());
    if (m_hMemMapFile == nullptr) {
        return false;
    }

    // Must be sampled immediately after the call that sets it.  Note that an
    // existing mapping of a *different* size is returned as-is rather than
    // failing, which is why the header carries a validated layout stamp.
    const bool isNew = (::GetLastError() != ERROR_ALREADY_EXISTS);

    m_ptr = ::MapViewOfFile(m_hMemMapFile, mapAccess, 0, 0, size);
    if (m_ptr == nullptr) {
        ::CloseHandle(m_hMemMapFile);
        m_hMemMapFile = nullptr;
        return false;
    }

    if (isNewOut) {
        *isNewOut = isNew;
    }
    return true;
}

void ShmMapping::close() noexcept {
    if (m_ptr) {
        ::UnmapViewOfFile(m_ptr);
        m_ptr = nullptr;
    }
    if (m_hMemMapFile) {
        ::CloseHandle(m_hMemMapFile);
        m_hMemMapFile = nullptr;
    }
}

} // namespace shmlog
