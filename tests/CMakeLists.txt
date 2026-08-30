add_executable(emds_toolkit_test emds_toolkit_test.cpp)
target_link_libraries(emds_toolkit_test PRIVATE emds-toolkit::emds-toolkit)

add_test(NAME emds_toolkit_test COMMAND emds_toolkit_test)
