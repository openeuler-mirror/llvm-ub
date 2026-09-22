# Standalone CMakeLists.txt for building uballoc examples against the
# installed library. This file is shipped in the tarball at
# share/uballoc/examples/CMakeLists.txt.
#
# Usage:
#   mkdir build && cd build
#   cmake .. -DCMAKE_PREFIX_PATH=/opt/uballoc   # or wherever you installed
#   cmake --build . -j
#
# All examples use #include <uballoc.hpp> and link against uballoc::uballoc,
# which propagates the correct include paths and C++17 requirement.

cmake_minimum_required(VERSION 3.16)
project(uballoc_examples CXX)

find_package(uballoc CONFIG REQUIRED)

add_executable(distributed_example          distributed_example.cpp)
add_executable(ub_example                  ub_example.cpp)
add_executable(stl_full_shm_example        stl_full_shm_example.cpp)
add_executable(stl_data_only_example       stl_data_only_example.cpp)
add_executable(published_example           published_example.cpp)
add_executable(dynamic_discovery_example   dynamic_discovery_example.cpp)
add_executable(fault_handler_example       fault_handler_example.cpp)
add_executable(uffd_example                 uffd_example.cpp)
add_executable(reclaim_example             reclaim_example.cpp)

foreach(exe
    distributed_example
    ub_example
    stl_full_shm_example
    stl_data_only_example
    published_example
    dynamic_discovery_example
    fault_handler_example
    uffd_example
    reclaim_example
)
    target_link_libraries(${exe} PRIVATE uballoc::uballoc)
    set_target_properties(${exe} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endforeach()
