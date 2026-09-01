add_executable(emds_toolkit_test emds_toolkit_test.cpp)
target_link_libraries(emds_toolkit_test PRIVATE emds-toolkit::emds-toolkit)

add_test(NAME emds_toolkit_test COMMAND emds_toolkit_test)

add_executable(direct_io_buffer_test direct_io_buffer_test.cpp)
target_link_libraries(
    direct_io_buffer_test
    PRIVATE
        emds-toolkit::emds-toolkit
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(direct_io_buffer_test)
