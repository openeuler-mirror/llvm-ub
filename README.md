# MatrixCPP

1. Matrix C++ offers native support for distributed concurrent programming:
  The native supernode programming model builds upon ISO C++'s inherent
concurrency capabilities, enhancing supernode-level concurrency and
heterogeneous computing. It delivers a seamless development experience where
code written for a single machine can run directly on supernodes, eliminating
the need for third-party libraries or frameworks to achieve distributed
concurrency. This approach significantly improves programming efficiency and
cross-platform portability.

2. Required before building: Install Ray using the recommended command:
  pip install -U ray[cpp]== 2.48.0

3. Building method:
  Add the -M option when building the LLVM command.

4. To build the MatrixCPP library from source:
  mkdir build && cd build
  cmake -DRAY_CPP_PATH=path/to/ray ..
  make -j

5. For projects that depend on MatrixCPP, refer to the test directory for
compilation examples:
  cd test
  cmake -DMATRIXCPP_SOURCE_PATH=/path/to/MatrixCPP ..
  make -j$(nproc)