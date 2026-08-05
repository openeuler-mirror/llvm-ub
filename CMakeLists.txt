cmake_minimum_required(VERSION 3.16)
project(llvm-ub LANGUAGES CXX)

enable_testing()

# Active sub-projects
add_subdirectory(uballoc)

# Legacy sub-projects (unmaintained, not built by default)
# To build: add_subdirectory(legacy/matrixcpp)
