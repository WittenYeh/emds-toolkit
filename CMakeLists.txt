cmake_minimum_required(VERSION 3.20)

project(emds_toolkit LANGUAGES CXX)

add_library(emds_toolkit INTERFACE)
add_library(emds-toolkit::emds-toolkit ALIAS emds_toolkit)

target_include_directories(
    emds_toolkit
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(emds_toolkit INTERFACE cxx_std_20)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(EMDS_TOOLKIT_BUILD_TESTS_DEFAULT ON)
else()
    set(EMDS_TOOLKIT_BUILD_TESTS_DEFAULT OFF)
endif()

option(
    EMDS_TOOLKIT_BUILD_TESTS
    "Build emds-toolkit tests"
    ${EMDS_TOOLKIT_BUILD_TESTS_DEFAULT}
)

unset(EMDS_TOOLKIT_BUILD_TESTS_DEFAULT)

if(EMDS_TOOLKIT_BUILD_TESTS)
    include(FetchContent)

    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
        URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(tests)
endif()
