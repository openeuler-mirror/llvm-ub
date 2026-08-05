//===- test_async.cpp - Comprehensive tests for the async implementation. -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

/**
 * @file test_async.cpp
 * @brief Comprehensive test suite for bisheng::async and bisheng::async_auto
 * implementation
 *
 * This test file verifies all core functionality of the asynchronous functions:
 * - Manual scheduling using async function with name and value of a node
 * - Automatic scheduling using async_auto
 */

#include <MatrixCPP.h>
#include <iostream>
#include <string>
#include <vector>

int Myfunc(int x) { return x * x; }

BISHENG_REMOTE(Myfunc);

int main() {
  bisheng::StartupShutdown::Init();

  std::vector<bisheng::future<int>> futures;
  int num_tasks = 5;
  // assign tasks automatically with the runtime scheduling policy
  for (int i = 1; i < num_tasks; i++) {
    futures.push_back(bisheng::async(Myfunc, i));
    futures.push_back(bisheng::async<Myfunc>(i));
  }

  for (auto &future : futures) {
    std::cout << future.get() << std::endl;
  }

  // schedule tasks to a node with name "node0" and value 1.0
  std::string node = "node0";
  double value = 1.0;
  bisheng::future<int> res0 = bisheng::async<Myfunc>(node, value, 13);
  std::cout << (res0.get()) << std::endl;

  bisheng::future<int> res1 = bisheng::async(Myfunc, node, value, 14);
  std::cout << (res1.get()) << std::endl;

  bisheng::future<int> res2 = bisheng::async<Myfunc>("node0", 1.0, 13);
  std::cout << (res2.get()) << std::endl;

  bisheng::future<int> res3 = bisheng::async(Myfunc, "node0", 1.0, 14);
  std::cout << (res3.get()) << std::endl;

  bisheng::StartupShutdown::Shutdown();
  return 0;
}
