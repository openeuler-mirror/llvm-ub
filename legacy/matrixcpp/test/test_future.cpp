//==- test_future.cpp - Comprehensive tests for the future implementation. -==//
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
 * @file test_future.cpp
 * @brief Comprehensive test suite for bisheng::future
 *
 * This test file verifies all core functionality of the distributed future
 * pattern:
 * - Basic value retrieval
 * - Exception handling
 * - Performance characteristics
 * @note Requires Ray cluster initialization
 */

#include <MatrixCPP.h>
#include <chrono>
#include <iostream>
#include <thread>


// 1. Define simple remote functions
int SimpleCompute(int x) { return x + x; }

int ComputeWithException() {
  throw std::runtime_error("Intentional exception for testing");
  return 0;
}

// 2. Register remote functions
BISHENG_REMOTE(SimpleCompute);
BISHENG_REMOTE(ComputeWithException);

void TestBasicFunctionality() {
  std::cout << "=== Testing Basic Functionality ===" << std::endl;

  // Test construction and validity
  bisheng::future<int> empty_fut;
  std::cout << "Empty future is " << (empty_fut ? "valid ✘" : "invalid ✔")
            << std::endl;
  int test_ans = 20;

  // Submit remote task
  bisheng::future<int> fut = bisheng::async(SimpleCompute, 10);

  // Test validity
  std::cout << "Initialized future is " << (fut ? "valid ✔" : "invalid ✘")
            << std::endl;

  // Retrieve result with both methods
  try {
    // Standard get
    int result = fut.get();
    std::cout << "Got result (get): " << result
              << (result == test_ans ? " ✔" : " ✘") << std::endl;

    // Error-code version
    std::error_code ec;
    int result_ec = fut.get(ec);
    if (!ec) {
      std::cout << "Got result (get with ec): " << result_ec
                << (result_ec == test_ans ? " ✔" : " ✘") << std::endl;
    }

    // Shared pointer version
    auto ptr = fut.getPtr();
    std::cout << "Got result (ptr): " << *ptr
              << (*ptr == test_ans ? " ✔" : " ✘") << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "Unexpected error : " << e.what() << std::endl;
  }
}

void TestExceptionHandling() {
  std::cout << "\n=== Testing Exception Handling ===" << std::endl;

  // Test standard get() with exception
  {
    bisheng::future<int> fut = bisheng::async(ComputeWithException);

    try {
      int result = fut.get();
      std::cerr << "get() Unexpected success, got: " << result << " ✘"
                << std::endl;
    } catch (const std::runtime_error &e) {
      std::cout << "get() Caught expected exception: " << e.what() << " ✔"
                << std::endl;
    } catch (...) {
      std::cerr << "get() Caught unexpected exception type ✘" << std::endl;
    }
  }

  // Test error_code version
  {
    bisheng::future<int> fut = bisheng::async(ComputeWithException);

    std::error_code ec;
    int val = fut.get(ec);
    if (ec) {
      std::cout << "get(ec) Caught error via error_code: "
                << (ec == std::errc::operation_not_permitted ? "Expected ✔"
                                                             : "Unexpected ✘")
                << " | " << ec.message() << std::endl;
    } else {
      std::cerr << "get(ec) Failed to catch exception via error_code ✘"
                << std::endl;
    }
  }

  // Test pointer version
  {
    bisheng::future<int> fut = bisheng::async(ComputeWithException);

    try {
      auto ptr = fut.getPtr();
      std::cerr << "getPtr() Unexpected success, returned: "
                << (ptr ? std::to_string(*ptr) : "nullptr") << " ✘"
                << std::endl;
    } catch (const std::runtime_error &e) {
      std::cout << "getPtr() Caught expected exception: " << e.what() << " ✔"
                << std::endl;
    } catch (...) {
      std::cerr << "getPtr() Caught unexpected exception type ✘" << std::endl;
    }
  }
}

void TestPerformance(int num_tasks = 100) {
  std::cout << "\n=== Testing Performance (" << num_tasks
            << " tasks ) === " << std::endl;

  std::vector<bisheng::future<int>> futures;
  futures.reserve(num_tasks);

  auto start = std::chrono::steady_clock::now();

  // Submit batch
  for (int i = 0; i < num_tasks; ++i) {
    futures.emplace_back(bisheng::async(SimpleCompute, i));
  }

  // Validate
  int success_count = 0;
  for (auto &fut : futures) {
    std::error_code ec;
    int val = fut.get(ec);
    if (!ec && val == (success_count + success_count)) {
      ++success_count;
    }
  }

  auto end = std::chrono::steady_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  std::cout << "Completed " << success_count << "/" << num_tasks << " tasks in "
            << duration.count() << "ms" << std::endl;
}

int main() {
  bisheng::StartupShutdown::Init();

  TestBasicFunctionality();
  TestExceptionHandling();
  TestPerformance();

  bisheng::StartupShutdown::Shutdown();
  return 0;
}

/*
Expected output of test cases

=== Testing Basic Functionality ===
Empty future is invalid ✔
Initialized future is valid ✔
Got result (get): 20 ✔
Got result (get with ec): 20 ✔
Got result (ptr): 20 ✔

=== Testing Exception Handling ===
get() Caught expected exception: Invalid: An exception was thrown while
executing function(ComputeWithException):
        std::runtime_error: Intentional exception for testing ✔

get(ec) Caught error via error_code:
        Expected ✔ | Operation not permitted

getPtr() Caught expected exception: Invalid: An exception was thrown while
executing function(ComputeWithException):
        std::runtime_error: Intentional exception for testing ✔

=== Testing Performence (100 tasks) ===
Completed 100/100 tasks in 167ms

*/
