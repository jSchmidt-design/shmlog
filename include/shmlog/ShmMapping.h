#pragma once

#include <cstddef>
#include <string>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace shmlog {

// Lightweight, dependency-free Windows named-shared-memory helper.
//
// Takes names by const std::string& rather than std::string_view because the
// Win32 entry points need a null-terminated string; a string_view parameter
// would have to allocate one on every call.
class ShmMapping {
public:
    enum class Mode {
        ReadOnly,
        ReadWrite
    };

    ShmMapping() noexcept = default;
    ~ShmMapping();

    // Non-copyable.
    ShmMapping(const ShmMapping&) = delete;
    ShmMapping& operator=(const ShmMapping&) = delete;

    // Movable.
    ShmMapping(ShmMapping&& other) noexcept;
    ShmMapping& operator=(ShmMapping&& other) noexcept;

    // Open an existing partition.  Returns true on success; on failure, and
    // when this object already holds a mapping, nothing is modified.
    [[nodiscard]] bool open(const std::string& name, std::size_t size, Mode mode);

    // Create a partition, or open it if the name already exists.
    // On success, and only on success, `isNewOut` (when non-null) reports
    // whether this call created the mapping.
    [[nodiscard]] bool create(const std::string& name,
                              std::size_t size,
                              Mode mode,
                              bool* isNewOut = nullptr);

    // Non-owning pointer to the mapped view, owned by this object and released
    // on close()/destruction.  Null while closed.
    [[nodiscard]] void* getPtr() const noexcept { return m_ptr; }

    [[nodiscard]] bool isOpen() const noexcept { return m_ptr != nullptr; }

    // Release the view and OS handle.  Safe on an already-closed object.
    void close() noexcept;

private:
    HANDLE m_hMemMapFile{nullptr};
    void*  m_ptr{nullptr};
};

} // namespace shmlog
