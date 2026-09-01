# emds-toolkit

**emds-toolkit** is a header-only C++ library for external-memory data structures. It provides lightweight and reusable building blocks for designing I/O-efficient data structures that scale beyond main memory.

## Requirements

- A C++20-compatible compiler
- CMake 3.20 or newer when consuming the provided CMake target
- Linux with Direct I/O support for `emds::io::DirectIOBuffer`

## Usage

Include the complete public API:

```cpp
#include <emds-toolkit/emds_toolkit.hpp>
```

Or include individual components:

```cpp
#include <emds-toolkit/common/requires.hpp>
#include <emds-toolkit/io/direct_io_buffer.hpp>
```

The public namespaces currently provided are `emds::common` and `emds::io`.

When the repository is added with CMake, link the header-only target:

```cmake
add_subdirectory(path/to/emds-toolkit)
target_link_libraries(your_target PRIVATE emds-toolkit::emds-toolkit)
```

## Tests

Tests are enabled by default for a standalone build and disabled when the project is included as a subdirectory:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
