// Copyright 2026 Weitang Ye
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <emds-toolkit/common/requires.hpp>

namespace emds::io {

/**
 * @brief Owns a read/write Direct I/O file descriptor and an aligned user-space buffer.
 *
 * IOBuffer is move-only. It closes the file descriptor and releases the aligned buffer when the
 * object is destroyed.
 *
 * @warning Direct I/O offset and transfer-size alignment must still be respected by I/O operations.
 */
class IOBuffer {
public:
    /** @brief Alignment, in bytes, used for the buffer address and capacity. */
    static constexpr std::size_t AlignBytes = 4096;

    /**
     * @brief Opens a file for Direct I/O and allocates an aligned read/write buffer.
     *
     * @param[in] file_path Path to the file opened with `O_RDWR | O_DIRECT | O_CLOEXEC`.
     * @param[in] capacity_bytes Number of bytes allocated for the buffer. It must be a nonzero
     * multiple of @ref AlignBytes.
     * @return An IOBuffer that owns both the opened file descriptor and the allocated buffer.
     *
     * @throws std::invalid_argument If `capacity_bytes` is zero or is not aligned.
     * @throws std::system_error If opening the file or allocating aligned memory fails.
     */
    [[nodiscard]] static auto make(
        const std::filesystem::path& file_path,
        std::size_t capacity_bytes
    ) -> IOBuffer {
        // Full-buffer Direct I/O operations require a nonzero, suitably aligned transfer size.
        common::require_argument(
            capacity_bytes != 0, "IOBuffer capacity must be greater than zero");
        common::require_argument(capacity_bytes % AlignBytes == 0,
            "IOBuffer capacity must be a multiple of 4096 bytes");

        // Request direct I/O and prevent the descriptor from leaking across exec-family calls.
        const auto file_descriptor = ::open(file_path.c_str(), O_RDWR | O_DIRECT | O_CLOEXEC);
        if (file_descriptor == -1) {
            throw std::system_error(errno, std::generic_category(), "failed to open file with O_DIRECT");
        }

        // posix_memalign writes the aligned address to buf_addr and returns an error code directly.
        void* buf_addr = nullptr;
        const auto alloc_error = ::posix_memalign(&buf_addr, AlignBytes, capacity_bytes);
        if (alloc_error != 0) {
            // The descriptor was acquired first, so release it before reporting allocation failure.
            ::close(file_descriptor);
            throw std::system_error(
                alloc_error, std::generic_category(),
                "failed to allocate aligned I/O buffer");
        }

        // Transfer ownership of both successfully acquired resources to the returned object.
        return IOBuffer{file_descriptor, static_cast<std::byte*>(buf_addr), capacity_bytes};
    }

    /** @brief Releases the aligned buffer and closes the owned file descriptor. */
    ~IOBuffer() noexcept { release(); }

    /** @brief Copy construction is disabled because IOBuffer has unique resource ownership. */
    IOBuffer(const IOBuffer&) = delete;

    /** @brief Copy assignment is disabled because IOBuffer has unique resource ownership. */
    auto operator=(const IOBuffer&) -> IOBuffer& = delete;

    /**
     * @brief Moves ownership from another IOBuffer.
     *
     * @param[in,out] other Source object. It is empty after the move.
     */
    IOBuffer(IOBuffer&& other) noexcept
        : file_descriptor_(std::exchange(other.file_descriptor_, -1)),
          buf_addr_(std::exchange(other.buf_addr_, nullptr)),
          capacity_(std::exchange(other.capacity_, 0)) {}

    /**
     * @brief Replaces the current resources with resources moved from another IOBuffer.
     *
     * @param[in,out] other Source object. It is empty after a successful non-self move.
     * @return A reference to this object.
     */
    auto operator=(IOBuffer&& other) noexcept -> IOBuffer& {
        if (this != &other) {
            // Release the currently owned resources before taking ownership from other.
            release();
            file_descriptor_ = std::exchange(other.file_descriptor_, -1);
            buf_addr_ = std::exchange(other.buf_addr_, nullptr);
            capacity_ = std::exchange(other.capacity_, 0);
        }

        return *this;
    }

    /**
     * @brief Reads from a file offset into a region of the aligned buffer.
     *
     * @param[in] file_offset Byte offset from the beginning of the file.
     * @param[in] buf_offset Byte offset from @ref buf_addr_.
     * @param[in] size_bytes Maximum number of bytes to read.
     * @return The number of bytes read. Zero indicates end of file.
     *
     * @throws std::invalid_argument If an offset, size, alignment, or range is invalid.
     * @throws std::logic_error If this object has no owned resources.
     * @throws std::system_error If the underlying `pread` operation fails.
     */
    [[nodiscard]] auto read_at(
        std::uint64_t file_offset,
        std::size_t buf_offset,
        std::size_t size_bytes
    ) -> std::size_t {
        validate_io_request(file_offset, buf_offset, size_bytes);

        // Retry only interrupted calls; successful short reads are returned to the caller.
        ssize_t bytes_read = -1;
        do {
            bytes_read = ::pread(
                file_descriptor_, buf_addr_ + buf_offset, size_bytes,
                static_cast<off_t>(file_offset));
        } while (bytes_read == -1 && errno == EINTR);

        if (bytes_read == -1) {
            throw std::system_error(errno, std::generic_category(), "Direct I/O read failed");
        }

        return static_cast<std::size_t>(bytes_read);
    }

    /**
     * @brief Writes a region of the aligned buffer at a file offset.
     *
     * @param[in] file_offset Byte offset from the beginning of the file.
     * @param[in] buf_offset Byte offset from @ref buf_addr_.
     * @param[in] size_bytes Maximum number of bytes to write.
     * @return The number of bytes written.
     *
     * @throws std::invalid_argument If an offset, size, alignment, or range is invalid.
     * @throws std::logic_error If this object has no owned resources.
     * @throws std::system_error If the underlying `pwrite` operation fails.
     * @warning A Direct I/O write error can leave the target file region partially modified.
     */
    [[nodiscard]] auto write_at(
        std::uint64_t file_offset,
        std::size_t buf_offset,
        std::size_t size_bytes
    ) -> std::size_t {
        validate_io_request(file_offset, buf_offset, size_bytes);

        // Retry only interrupted calls; successful short writes are returned to the caller.
        ssize_t bytes_written = -1;
        do {
            bytes_written = ::pwrite(
                file_descriptor_, buf_addr_ + buf_offset, size_bytes,
                static_cast<off_t>(file_offset));
        } while (bytes_written == -1 && errno == EINTR);

        if (bytes_written == -1) {
            throw std::system_error(errno, std::generic_category(), "Direct I/O write failed");
        }

        return static_cast<std::size_t>(bytes_written);
    }

    /**
     * @brief Returns the writable aligned buffer address.
     *
     * @return The buffer address, or `nullptr` if this object was moved from.
     */
    [[nodiscard]] auto buf_addr() noexcept -> std::byte* { return buf_addr_; }

    /**
     * @brief Returns the read-only aligned buffer address.
     *
     * @return The buffer address, or `nullptr` if this object was moved from.
     */
    [[nodiscard]] auto buf_addr() const noexcept -> const std::byte* { return buf_addr_; }

    /**
     * @brief Returns the allocated buffer capacity.
     *
     * @return The capacity in bytes, or zero if this object was moved from.
     */
    [[nodiscard]] auto capacity() const noexcept -> std::size_t { return capacity_; }

    /**
     * @brief Returns the Direct I/O file descriptor without transferring ownership.
     *
     * @return The owned file descriptor, or `-1` if this object was moved from.
     * @warning The caller must not close the returned descriptor.
     */
    [[nodiscard]] auto file_descriptor() const noexcept -> int { return file_descriptor_; }

private:
    /**
     * @brief Adopts an opened file descriptor and an allocated aligned buffer.
     *
     * @param[in] file_descriptor File descriptor whose ownership is transferred to this object.
     * @param[in] data Aligned address whose ownership is transferred to this object.
     * @param[in] capacity Number of bytes available at `data`.
     */
    IOBuffer(int file_descriptor, std::byte* data, std::size_t capacity) noexcept
        : file_descriptor_(file_descriptor), buf_addr_(data), capacity_(capacity) {}

    /**
     * @brief Tests whether a byte address, offset, or size satisfies Direct I/O alignment.
     *
     * @param[in] mem_addr Value to test against @ref AlignBytes.
     * @return `true` if `mem_addr` is aligned; otherwise, `false`.
     */
    [[nodiscard]] static constexpr auto is_aligned(std::uint64_t mem_addr) noexcept -> bool {
        return mem_addr % AlignBytes == 0;
    }

    /**
     * @brief Validates an aligned positional I/O request against this object's resources.
     *
     * @param[in] file_offset Byte offset from the beginning of the file.
     * @param[in] buf_offset Byte offset from @ref buf_addr_.
     * @param[in] size_bytes Number of bytes requested.
     *
     * @throws std::invalid_argument If an offset, size, alignment, or range is invalid.
     * @throws std::logic_error If this object has no owned resources.
     */
    auto validate_io_request(
        std::uint64_t file_offset,
        std::size_t buf_offset,
        std::size_t size_bytes
    ) const -> void {
        if (file_descriptor_ == -1 || buf_addr_ == nullptr) [[unlikely]] {
            throw std::logic_error("I/O operation on an empty IOBuffer");
        }

        common::require_argument(size_bytes != 0, "I/O size must be greater than zero");
        common::require_argument(is_aligned(file_offset),
            "file offset must be a multiple of 4096 bytes");
        common::require_argument(is_aligned(buf_offset),
            "buffer offset must be a multiple of 4096 bytes");
        common::require_argument(is_aligned(size_bytes),
            "I/O size must be a multiple of 4096 bytes");
        common::require_argument(
            buf_offset <= capacity_ && size_bytes <= capacity_ - buf_offset,
            "I/O range exceeds IOBuffer capacity");
        common::require_argument(std::in_range<ssize_t>(size_bytes),
            "I/O size exceeds the supported system-call range");
        common::require_argument(std::in_range<off_t>(file_offset),
            "file offset exceeds the supported system-call range");
        common::require_argument(static_cast<std::uint64_t>(size_bytes - 1) <=
                static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - file_offset,
            "file I/O range exceeds the supported offset range");
    }

    /** @brief Releases all owned resources and leaves this object empty. */
    auto release() noexcept -> void {
        std::free(buf_addr_);
        buf_addr_ = nullptr;
        capacity_ = 0;

        if (file_descriptor_ != -1) {
            ::close(file_descriptor_);
            file_descriptor_ = -1;
        }
    }

    /** @brief Owned Direct I/O file descriptor, or `-1` when this object is empty. */
    int file_descriptor_ = -1;

    /** @brief Owned aligned buffer address, or `nullptr` when this object is empty. */
    std::byte* buf_addr_ = nullptr;

    /** @brief Number of allocated bytes at @ref buf_addr_, or zero when this object is empty. */
    std::size_t capacity_ = 0;
};  // class IOBuffer

}  // namespace emds::io
