# llvm-ub

**[中文](README.md)** | **English**

llvm-ub is a collection of UB (Unified Bus) based distributed programming projects under the openEuler community, providing infrastructure for multi-process, multi-node shared memory scenarios.

## Sub-projects

| Project | Description | Status |
|---------|-------------|--------|
| [uballoc](uballoc/) | C++17 shared memory allocator for multi-process, multi-node deployments on ARMv8 | Active |

## Quick Start

Using uballoc as an example:

```bash
# POSIX build (standard Linux, no UBSE dependency)
cd uballoc
cmake -B build -DUBALLOC_USE_UBSE=OFF && cmake --build build -j

# Run tests
cd build && ctest --output-on-failure
```

See each sub-project directory for detailed documentation.

## Legacy

The following projects are no longer maintained and are preserved for reference only:

- [matrixcpp](legacy/matrixcpp/) — Ray-based distributed concurrent programming library

## License

Apache-2.0. See [LICENSE](LICENSE) for the full text.
