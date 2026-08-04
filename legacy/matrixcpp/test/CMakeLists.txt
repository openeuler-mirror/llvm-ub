# Minimum CMake version requirement
cmake_minimum_required(VERSION 3.12)

# Project definition
project(example)

# Basic compiler settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-omit-frame-pointer -fPIC -D_GLIBCXX_USE_CXX11_ABI=0")

# Find MatrixCPP package
if(NOT DEFINED MATRIXCPP_SOURCE_PATH)
    message(FATAL_ERROR "MATRIXCPP_SOURCE_PATH not set. Please input by -DMATRIXCPP_SOURCE_PATH=/path/to/MatrixCPP")
endif()
set(MatrixCPP_DIR "${MATRIXCPP_SOURCE_PATH}/build")
find_package(MatrixCPP 1.0 REQUIRED)

file(GLOB SOURCE_FILES "*.cpp")

# Create executables and shared libraries for each source file
foreach(SRC_FILE ${SOURCE_FILES})
    # Get base name without extension
    get_filename_component(TARGET_BASE ${SRC_FILE} NAME_WE)

    # set target dir
    set(TEST_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_BASE}")

    # Define executable target
    add_executable(${TARGET_BASE} ${SRC_FILE})
    target_include_directories(${TARGET_BASE} PRIVATE ${MatrixCPP_INCLUDE_DIRS})
    target_link_libraries(${TARGET_BASE} PRIVATE
        MatrixCPP::MatrixCPP
    )
    target_link_options(${TARGET_BASE} PRIVATE
        "-Wl,--no-as-needed"
    )

    set_target_properties(${TARGET_BASE} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
    )

    # Define shared library target
    add_library(${TARGET_BASE}_shared SHARED ${SRC_FILE})
    target_include_directories(${TARGET_BASE}_shared PRIVATE ${MatrixCPP_INCLUDE_DIRS})
    target_link_libraries(${TARGET_BASE}_shared PRIVATE
        MatrixCPP::MatrixCPP
    )
    set_target_properties(${TARGET_BASE}_shared PROPERTIES
        OUTPUT_NAME "${TARGET_BASE}"
        POSITION_INDEPENDENT_CODE ON
    )
    target_link_options(${TARGET_BASE}_shared PRIVATE
        "-Wl,-rpath,\$ORIGIN"
    )

    set_target_properties(${TARGET_BASE}_shared PROPERTIES
        OUTPUT_NAME "${TARGET_BASE}"
        LIBRARY_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
        ARCHIVE_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
        POSITION_INDEPENDENT_CODE ON
    )
endforeach()