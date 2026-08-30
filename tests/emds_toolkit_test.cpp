#include <cassert>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include <emds-toolkit/emds_toolkit.hpp>

namespace {

template <typename FunctionT>
auto throws_invalid_argument(FunctionT&& function, std::string_view expected_message) -> bool {
    try {
        function();
    } catch (const std::invalid_argument& exception) {
        return exception.what() == expected_message;
    }

    return false;
}

}  // namespace

auto main() -> int {
    static_assert(emds::io::IOBuffer::AlignBytes == 4096);
    static_assert(!std::is_copy_constructible_v<emds::io::IOBuffer>);
    static_assert(!std::is_copy_assignable_v<emds::io::IOBuffer>);
    static_assert(std::is_nothrow_move_constructible_v<emds::io::IOBuffer>);
    static_assert(std::is_nothrow_move_assignable_v<emds::io::IOBuffer>);

    emds::common::require_argument(true, "satisfied requirement");

    assert(throws_invalid_argument(
        [] { emds::common::require_argument(false, "unsatisfied requirement"); },
        "unsatisfied requirement"));
    assert(throws_invalid_argument(
        [] { (void)emds::io::IOBuffer::make({}, 0); },
        "IOBuffer capacity must be greater than zero"));
    assert(throws_invalid_argument(
        [] { (void)emds::io::IOBuffer::make({}, 1); },
        "IOBuffer capacity must be a multiple of 4096 bytes"));

    return 0;
}
