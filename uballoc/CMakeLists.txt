cmake_minimum_required(VERSION 3.16)
project(uballoc VERSION 0.6.0 LANGUAGES CXX
        HOMEPAGE_URL "https://gitcode.com/openeuler/llvm-ub")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(UBALLOC_LSE "Enable ARMv8.1 LSE atomic instructions" ON)
option(UBALLOC_USE_UBSE "Use UBShmProvider as default (requires ubse-client)" ON)

add_compile_options(-Wall -Wextra -Wpedantic -g -O2)

if(UBALLOC_LSE)
    add_compile_options(-march=armv8-a+lse)
endif()

option(UBALLOC_BUILD_TESTS "Build tests" ON)
option(UBALLOC_RECOVER_FLUSH "Enable cache flushing for recovery" OFF)
option(UBALLOC_RECOVER_CAS "Enable detectable CAS for recovery" ON)
option(UBALLOC_RECOVER_LOG "Enable state logging for recovery" ON)
option(UBALLOC_LOG "Enable distributed path logging" ON)

add_library(uballoc SHARED
    src/uballoc.cpp
    src/uballoc_instances.cpp
)

target_include_directories(uballoc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Propagate C++17 requirement to downstream consumers via the imported target.
# Without this, a downstream project that doesn't set CMAKE_CXX_STANDARD will
# fall back to the compiler default (e.g. C++14 for GCC 12), and the uballoc
# headers (which use C++17 features) will fail to compile.
set_target_properties(uballoc PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    INTERFACE_CXX_STANDARD 17
    INTERFACE_CXX_STANDARD_REQUIRED ON
)

target_link_libraries(uballoc PRIVATE rt)
if(UBALLOC_USE_UBSE)
    target_link_libraries(uballoc PUBLIC ubse-client)
endif()

target_compile_definitions(uballoc PUBLIC
    $<$<BOOL:UBALLOC_RECOVER_FLUSH>:UBALLOC_RECOVER_FLUSH>
    $<$<BOOL:UBALLOC_RECOVER_CAS>:UBALLOC_RECOVER_CAS>
    $<$<BOOL:UBALLOC_RECOVER_LOG>:UBALLOC_RECOVER_LOG>
    $<$<BOOL:${UBALLOC_LOG}>:UBALLOC_LOG>
    $<$<BOOL:${UBALLOC_USE_UBSE}>:UBALLOC_USE_UBSE>
    UBALLOC_VALIDATE
)

if(UBALLOC_BUILD_TESTS)
    enable_testing()
    add_executable(uballoc_tests tests/test_main.cpp)
    target_include_directories(uballoc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(uballoc_tests PRIVATE uballoc rt pthread)
    target_compile_definitions(uballoc_tests PRIVATE
        UBALLOC_NO_EXTERN_TEMPLATE
        $<$<BOOL:${UBALLOC_RECOVER_FLUSH}>:UBALLOC_RECOVER_FLUSH>
        $<$<BOOL:${UBALLOC_RECOVER_CAS}>:UBALLOC_RECOVER_CAS>
        $<$<BOOL:${UBALLOC_RECOVER_LOG}>:UBALLOC_RECOVER_LOG>
        $<$<BOOL:${UBALLOC_LOG}>:UBALLOC_LOG>
        UBALLOC_VALIDATE
    )
    add_test(NAME uballoc_tests COMMAND uballoc_tests)
    set_tests_properties(uballoc_tests PROPERTIES TIMEOUT 120)
    set_tests_properties(uballoc_tests PROPERTIES
        ENVIRONMENT "UBALLOC_LOG_LEVEL=error")
    add_subdirectory(tests/unit)
endif()

option(UBALLOC_BUILD_EXAMPLES "Build examples" ON)

if(UBALLOC_BUILD_EXAMPLES)
    add_executable(distributed_example example/distributed_example.cpp)
    target_link_libraries(distributed_example PRIVATE uballoc rt pthread)

    add_executable(ub_example example/ub_example.cpp)
    target_link_libraries(ub_example PRIVATE uballoc rt pthread)

    add_executable(stl_full_shm_example example/stl_full_shm_example.cpp)
    target_link_libraries(stl_full_shm_example PRIVATE uballoc rt pthread)

    add_executable(stl_data_only_example example/stl_data_only_example.cpp)
    target_link_libraries(stl_data_only_example PRIVATE uballoc rt pthread)

    add_executable(published_example example/published_example.cpp)
    target_link_libraries(published_example PRIVATE uballoc rt pthread)

    add_executable(dynamic_discovery_example example/dynamic_discovery_example.cpp)
    target_link_libraries(dynamic_discovery_example PRIVATE uballoc rt pthread)

    add_executable(fault_handler_example example/fault_handler_example.cpp)
    target_link_libraries(fault_handler_example PRIVATE uballoc rt pthread)

    add_executable(uffd_example example/uffd_example.cpp)
    target_link_libraries(uffd_example PRIVATE uballoc rt pthread)

    add_executable(reclaim_example example/reclaim_example.cpp)
    target_link_libraries(reclaim_example PRIVATE uballoc rt pthread)

endif()

install(TARGETS uballoc
    EXPORT uballocTargets
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h*"
)

# ---------- Examples ----------
# Install example sources into the tarball so end users can compile them
# against the installed library. Examples use #include <uballoc.hpp> (angle
# brackets) so they work both in-tree (via target_link_libraries) and when
# installed (via -I<prefix>/include).
install(DIRECTORY example/
    DESTINATION share/uballoc/examples
    FILES_MATCHING PATTERN "*.cpp" PATTERN "CMakeLists.txt"
)

# ---------- Cleanup scripts ----------
install(PROGRAMS
    scripts/cleanup_shms.sh
    scripts/cleanup_shms_ub.sh
    DESTINATION share/uballoc/scripts
)

# ---------- ABI versioning ----------
# Produces:
#   libuballoc.so.0      -> SONAME, what apps link against at runtime
#   libuballoc.so.0.6.0  -> actual file
#   libuballoc.so        -> dev symlink (for -luballoc)
set_target_properties(uballoc PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0  # ABI major version; bump on incompatible change
)

# ---------- pkg-config file ----------
# Generates lib/pkgconfig/uballoc.pc with correct cflags/libs for end users:
#   pkg-config --cflags --libs uballoc
# Reflects whether the build was UBSE or POSIX so downstream gets the right
# private deps (ubse-client) automatically.
#
# Note: for the POSIX build we do NOT pass -DUBALLOC_USE_UBSE=OFF — that would
# define the macro (to OFF), making #ifdef UBALLOC_USE_UBSE true and pulling in
# the ubse headers. The macro must be simply undefined for the POSIX path.
#
# For the UBSE build, we add -lubse-client to Libs (not Requires) because
# ubse-client is installed as a system library without a .pc file.
if(UBALLOC_USE_UBSE)
    set(UBALLOC_PC_LIBS " -lubse-client")
    set(UBALLOC_PC_CFLAGS " -DUBALLOC_USE_UBSE")
else()
    set(UBALLOC_PC_LIBS "")
    set(UBALLOC_PC_CFLAGS "")
endif()
configure_file(cmake/uballoc.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/uballoc.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/uballoc.pc
    DESTINATION lib/pkgconfig
)

# ---------- CMake package config (find_package(uballoc)) ----------
# Generates lib/cmake/uballoc/uballoc-config.cmake so downstream CMake projects
# can do:  find_package(uballoc CONFIG REQUIRED)
#          target_link_libraries(my_app PRIVATE uballoc::uballoc)
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/uballoc-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(EXPORT uballocTargets
    FILE uballoc-targets.cmake
    NAMESPACE uballoc::
    DESTINATION lib/cmake/uballoc
)
install(FILES
    cmake/uballoc-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/uballoc-config-version.cmake
    DESTINATION lib/cmake/uballoc
)

# ---------- CPack: tarball only ----------
# Generates a portable .tar.gz distribution that users can extract anywhere:
#   cpack -G TGZ
#   -> uballoc-0.1.0-linux-aarch64.tar.gz
# RPM generation is intentionally NOT enabled; users who want system install
# can extract the tarball to /usr/local or run cmake --install with a prefix.
set(CPACK_GENERATOR "TGZ")
set(CPACK_SOURCE_GENERATOR "")  # no source package
set(CPACK_PACKAGE_NAME "uballoc")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Distributed shared memory allocator for aarch64 Linux")
set(CPACK_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_PACKAGE_VENDOR "uballoc contributors")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://gitcode.com/openeuler/llvm-ub")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
endif()
# Encode target arch + OS in the archive name so multiple platforms can coexist
set(CPACK_SYSTEM_NAME "linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_SYSTEM_NAME}")
# Strip debug symbols from the installed .so for smaller archives
set(CPACK_STRIP_FILES TRUE)
include(CPack)
