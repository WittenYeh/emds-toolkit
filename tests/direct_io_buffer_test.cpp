#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include <emds-toolkit/io/direct_io_buffer.hpp>

/**
 * @file direct_io_buffer_test.cpp
 * @brief Tests DirectIOBuffer open modes, aligned I/O, validation, and resource ownership.
 */

namespace {

using emds::io::DirectIOBuffer;
using emds::io::FileOpenMode;

constexpr auto PageBytes = DirectIOBuffer::AlignBytes;

static_assert(!std::is_copy_constructible_v<DirectIOBuffer>);
static_assert(!std::is_copy_assignable_v<DirectIOBuffer>);
static_assert(std::is_nothrow_move_constructible_v<DirectIOBuffer>);
static_assert(std::is_nothrow_move_assignable_v<DirectIOBuffer>);

/** @brief Provides an isolated temporary directory and one-page Direct I/O helpers per test. */
class DirectIOBufferTest : public testing::Test {
protected:
    /** @brief Creates a unique temporary directory for the current test. */
    auto SetUp() -> void override {
        auto path_template =
            (std::filesystem::temp_directory_path() / "emds-direct-io-test-XXXXXX").string();
        path_template.push_back('\0');

        const auto* created_path = ::mkdtemp(path_template.data());
        if (created_path == nullptr) {
            throw std::system_error(
                errno, std::generic_category(), "failed to create test directory");
        }

        test_directory_ = created_path;
    }

    /** @brief Removes files and the temporary directory created by the current test. */
    auto TearDown() -> void override {
        std::error_code error;
        std::filesystem::remove_all(test_directory_, error);
    }

    /** @brief Returns a test-local path without creating the file. */
    [[nodiscard]] auto file_path(std::string name) const -> std::filesystem::path {
        return test_directory_ / std::move(name);
    }

    /** @brief Fills the complete aligned buffer with one byte value. */
    static auto fill(DirectIOBuffer& buffer, std::byte value) -> void {
        std::fill_n(buffer.buf_addr(), PageBytes, value);
    }

    /** @brief Verifies that every byte in the aligned buffer equals the expected value. */
    static auto expect_filled(const DirectIOBuffer& buffer, std::byte value) -> void {
        EXPECT_TRUE(std::all_of(buffer.buf_addr(), buffer.buf_addr() + PageBytes,
            [value](std::byte byte) { return byte == value; }));
    }

    /** @brief Exclusively creates a file and writes one aligned page with the supplied value. */
    static auto create_file_with_page(
        const std::filesystem::path& path,
        std::byte value
    ) -> void {
        auto buffer = DirectIOBuffer::make(path, PageBytes, FileOpenMode::create_new);
        fill(buffer, value);
        ASSERT_EQ(buffer.write_at(0, 0, PageBytes), PageBytes);
    }

private:
    std::filesystem::path test_directory_;
};

/** @brief Verifies that zero, misaligned capacity and unknown open-mode values are rejected. */
TEST_F(DirectIOBufferTest, RejectsInvalidCapacityAndOpenMode) {
    EXPECT_THROW((void)DirectIOBuffer::make({}, 0), std::invalid_argument);
    EXPECT_THROW((void)DirectIOBuffer::make({}, 1), std::invalid_argument);
    EXPECT_THROW(
        (void)DirectIOBuffer::make(
            file_path("invalid-mode"), PageBytes, static_cast<FileOpenMode>(0xff)),
        std::invalid_argument);
}

/** @brief Verifies one-page read/write I/O and compatibility of the original two-argument factory. */
TEST_F(DirectIOBufferTest, ReadWriteAndLegacyFactoriesRoundTripOnePage) {
    const auto path = file_path("read-write");
    create_file_with_page(path, std::byte{0x11});

    {
        auto buffer = DirectIOBuffer::make(path, PageBytes, FileOpenMode::read_write);
        fill(buffer, std::byte{0x22});
        ASSERT_EQ(buffer.write_at(0, 0, PageBytes), PageBytes);
    }

    auto legacy_buffer = DirectIOBuffer::make(path, PageBytes);
    fill(legacy_buffer, std::byte{0x00});
    ASSERT_EQ(legacy_buffer.read_at(0, 0, PageBytes), PageBytes);
    expect_filled(legacy_buffer, std::byte{0x22});
}

/** @brief Verifies that read-only mode can read an existing page but rejects write_at(). */
TEST_F(DirectIOBufferTest, ReadOnlyReadsAndRejectsWrites) {
    const auto path = file_path("read-only");
    create_file_with_page(path, std::byte{0x33});

    auto buffer = DirectIOBuffer::make(path, PageBytes, FileOpenMode::read_only);
    fill(buffer, std::byte{0x00});
    ASSERT_EQ(buffer.read_at(0, 0, PageBytes), PageBytes);
    expect_filled(buffer, std::byte{0x33});
    EXPECT_THROW((void)buffer.write_at(0, 0, PageBytes), std::logic_error);
}

/** @brief Verifies that create-new reports EEXIST and preserves an existing file's contents. */
TEST_F(DirectIOBufferTest, CreateNewNeverOverwritesAnExistingFile) {
    const auto path = file_path("create-new");
    create_file_with_page(path, std::byte{0x44});

    try {
        (void)DirectIOBuffer::make(path, PageBytes, FileOpenMode::create_new);
        FAIL() << "create_new unexpectedly opened an existing file";
    } catch (const std::system_error& error) {
        EXPECT_EQ(error.code(), std::make_error_code(std::errc::file_exists));
    }

    auto reader = DirectIOBuffer::make(path, PageBytes, FileOpenMode::read_only);
    ASSERT_EQ(reader.read_at(0, 0, PageBytes), PageBytes);
    expect_filled(reader, std::byte{0x44});
}

/** @brief Verifies that move construction and assignment transfer resources and read-only mode. */
TEST_F(DirectIOBufferTest, MoveTransfersResourcesAndReadOnlyMode) {
    const auto source_path = file_path("move-source");
    create_file_with_page(source_path, std::byte{0x55});

    auto source = DirectIOBuffer::make(source_path, PageBytes, FileOpenMode::read_only);
    auto moved = std::move(source);

    EXPECT_THROW((void)source.read_at(0, 0, PageBytes), std::logic_error);
    ASSERT_EQ(moved.read_at(0, 0, PageBytes), PageBytes);
    expect_filled(moved, std::byte{0x55});
    EXPECT_THROW((void)moved.write_at(0, 0, PageBytes), std::logic_error);

    const auto target_path = file_path("move-target");
    auto target = DirectIOBuffer::make(target_path, PageBytes, FileOpenMode::create_new);
    target = std::move(moved);

    EXPECT_THROW((void)moved.read_at(0, 0, PageBytes), std::logic_error);
    ASSERT_EQ(target.read_at(0, 0, PageBytes), PageBytes);
    expect_filled(target, std::byte{0x55});
    EXPECT_THROW((void)target.write_at(0, 0, PageBytes), std::logic_error);
}

/** @brief Verifies rejection of misaligned offsets/sizes and requests beyond buffer capacity. */
TEST_F(DirectIOBufferTest, RejectsMisalignedAndOutOfRangeRequests) {
    auto buffer = DirectIOBuffer::make(
        file_path("invalid-request"), PageBytes, FileOpenMode::create_new);

    EXPECT_THROW((void)buffer.write_at(1, 0, PageBytes), std::invalid_argument);
    EXPECT_THROW((void)buffer.write_at(0, 1, PageBytes), std::invalid_argument);
    EXPECT_THROW((void)buffer.write_at(0, 0, 1), std::invalid_argument);
    EXPECT_THROW((void)buffer.write_at(0, 0, PageBytes * 2), std::invalid_argument);
}

}  // namespace
