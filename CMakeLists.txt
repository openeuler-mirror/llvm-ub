cmake_minimum_required(VERSION 3.12)
project(MatrixCPP VERSION 1.0.0 LANGUAGES CXX)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

# Override default install directories to use build directory
set(MATRIXCPP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(MATRIXCPP_BINARY_DIR ${MATRIXCPP_SOURCE_DIR}/build)
set(CMAKE_SKIP_RPATH TRUE)
add_compile_options(-fstack-protector-strong)
add_compile_options(-D_FORTIFY_SOURCE=2 -O2)
add_compile_options(-ftrapv)
add_link_options(-s)

# Basic configuration
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-omit-frame-pointer -fPIC -D_GLIBCXX_USE_CXX11_ABI=0")

# Check for required environment variables
if(NOT DEFINED RAY_CPP_PATH)
    message(FATAL_ERROR "RAY_CPP_PATH not set. Please input by -DRAY_CPP_PATH=/path/to/ray")
endif()
get_filename_component(RAY_API_LIB_PATH "${RAY_CPP_PATH}/lib/libray_api.so" ABSOLUTE)
get_filename_component(RAY_INCLUDE_PATH "${RAY_CPP_PATH}/include" ABSOLUTE)

# Import pre-built ray_api library
add_library(ray_api SHARED IMPORTED GLOBAL)
set_target_properties(ray_api PROPERTIES
    IMPORTED_LOCATION "${RAY_API_LIB_PATH}"
    INTERFACE_INCLUDE_DIRECTORIES "${RAY_INCLUDE_PATH}"
    IMPORTED_NO_SONAME TRUE
)

set(BISHENG_SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime/scripts")

# Main MatrixCPP library configuration
file(GLOB MatrixCPP_SRC "runtime/*.cpp")

add_library(MatrixCPP SHARED ${MatrixCPP_SRC})
target_include_directories(MatrixCPP PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    ${RAY_INCLUDE_PATH}
)
target_compile_definitions(MatrixCPP PRIVATE
    BISHENG_DEFAULT_SCRIPTS_DIR="${BISHENG_SCRIPTS_DIR}"
)
target_link_libraries(MatrixCPP PUBLIC
    ray_api
    pthread
)
target_link_options(MatrixCPP PRIVATE
    "-Wl,-z,relro,-z,now"
    "-Wl,--disable-new-dtags"
)

export(TARGETS MatrixCPP
    NAMESPACE MatrixCPP::
    FILE "${MATRIXCPP_BINARY_DIR}/MatrixCPPTargets.cmake"
)

# Create config file template
set(CONFIG_TEMPLATE "${MATRIXCPP_BINARY_DIR}/MatrixCPPConfig.cmake.in")
file(WRITE "${CONFIG_TEMPLATE}" [=[
@PACKAGE_INIT@

include(CMakeFindDependencyMacro)

# Find dependencies
find_dependency(Threads REQUIRED)

# Re-import ray_api (if not already defined by user)
if(NOT TARGET ray_api)
    set(RAY_API_LIB_PATH "@RAY_API_LIB_PATH@")
    set(RAY_INCLUDE_PATH "@RAY_INCLUDE_PATH@")

    add_library(ray_api SHARED IMPORTED)
    set_target_properties(ray_api PROPERTIES
        IMPORTED_LOCATION "${RAY_API_LIB_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${RAY_INCLUDE_PATH}"
        IMPORTED_NO_SONAME TRUE
    )
endif()

# Include the targets file
include("${CMAKE_CURRENT_LIST_DIR}/MatrixCPPTargets.cmake")

# Set variables for users
set(MatrixCPP_INCLUDE_DIRS "@MATRIXCPP_SOURCE_DIR@/include" "@RAY_INCLUDE_PATH@")
set(MatrixCPP_LIBRARIES MatrixCPP::MatrixCPP)

check_required_components(MatrixCPP)
]=])

# Generate and install package config files
configure_package_config_file(
    "${CONFIG_TEMPLATE}"
   "${MATRIXCPP_BINARY_DIR}/MatrixCPPConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MatrixCPP
    PATH_VARS RAY_API_LIB_PATH RAY_INCLUDE_PATH MATRIXCPP_SOURCE_DIR
)

write_basic_package_version_file(
    "${MATRIXCPP_BINARY_DIR}/MatrixCPPConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
