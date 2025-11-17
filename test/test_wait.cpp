//===-- test_wait.cpp - Comprehensive tests for the wait implementation. --===//
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
 * @file test_wait.cpp
 * @brief Comprehensive test suite for bisheng::wait wait_all and wait_some/any
 * implementation This test file verifies all core functionality of the
 * distributed future pattern:
 * - Basic value retrieval
 * @note Requires Ray cluster initialization
 */

#include <MatrixCPP.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

constexpr int milliseconds1000 = 1000;
constexpr int seconds1 = 1;
constexpr int seconds2 = 2;
constexpr int seconds3 = 3;

int ready_future_after(int n) {
  std::this_thread::sleep_for(std::chrono::seconds(n));
  return n;
}

BISHENG_REMOTE(ready_future_after);

void TestWait() {
  auto start = std::chrono::high_resolution_clock::now();

  bisheng::future<int> f_wait = bisheng::async(ready_future_after, seconds2);
  f_wait.wait();

  auto end = std::chrono::high_resolution_clock::now();

  std::chrono::duration<double, std::milli> elapsed = end - start;
  std::cout << "ready future after " << f_wait.get() << std::endl;
  std::cout << "wait elapsed time " << (elapsed.count() / milliseconds1000)
            << " s" << std::endl;
}

void TestWaitUntil() {
  bisheng::future<int> f_wait_until =
                            bisheng::async(ready_future_after, seconds2);
  auto after_1_second =
      std::chrono::system_clock::now() + std::chrono::seconds(seconds1);
  auto after_3_second =
      std::chrono::system_clock::now() + std::chrono::seconds(seconds3);
  auto status_until_1 = f_wait_until.wait_until(after_1_second);
  auto status_until_3 = f_wait_until.wait_until(after_3_second);

  std::cout << "wait until after 1 status " << int(status_until_1)
            << " after 3 status " << int(status_until_3) << std::endl;
}

void TestWaitFor() {
  bisheng::future<int> f_wait_for =
                            bisheng::async(ready_future_after, seconds2);
  auto status_for_1 = f_wait_for.wait_for(std::chrono::seconds(seconds1));
  auto status_for_3 = f_wait_for.wait_for(std::chrono::seconds(seconds3));
  std::cout << "wait for after 1 status " << int(status_for_1)
            << " after 3 status " << int(status_for_3) << std::endl;
}

void TestWaitAll(int n) {
  std::vector<bisheng::future<int>> fs;
  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 1; i <= n; i++) {
    fs.push_back(bisheng::async(ready_future_after, i));
  }
  wait_all(fs);

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  std::cout << "wait all elapsed time " << (elapsed.count() / milliseconds1000)
            << " s" << std::endl;
}

void TestWaitAllIter(int n) {
  std::vector<bisheng::future<int>> fs;
  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 1; i <= n; i++) {
    fs.push_back(bisheng::async(ready_future_after, i));
  }
  wait_all(fs.begin(), fs.end());

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  std::cout << "wait all elapsed time " << (elapsed.count() / milliseconds1000)
            << " s" << std::endl;
}

void TestWaitAllVA(int n) {
  std::vector<bisheng::future<int>> fs;
  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 1; i <= n; i++) {
    fs.push_back(bisheng::async(ready_future_after, i));
  }
  wait_all(fs[0], fs[n - 1]);

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  std::cout << "wait all elapsed time " << (elapsed.count() / milliseconds1000)
            << " s" << std::endl;
}

void TestWaitAny(int n) {
  std::vector<bisheng::future<int>> fs;
  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 1; i <= n; i++) {
    fs.push_back(bisheng::async(ready_future_after, i));
  }
  wait_any(fs);

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  std::cout << "wait any elapsed time " << (elapsed.count() / milliseconds1000)
            << " s" << std::endl;
}

void TestWaitSome(int n) {
  std::vector<bisheng::future<int>> fs;
  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 1; i <= n; i++) {
    fs.push_back(bisheng::async(ready_future_after, i));
  }
  wait_some(1, fs);

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  std::cout << "wait some elapsed time " << (elapsed.count() / milliseconds1000)
            << " s" << std::endl;
}

int main(int argc, char **argv) {
  /// initialization
  bisheng::StartupShutdown::Init();

  TestWait();
  TestWaitUntil();
  TestWaitFor();

  int n_futures = 10;
  TestWaitAll(n_futures);
  TestWaitAllIter(n_futures);
  TestWaitAllVA(n_futures);
  TestWaitAny(n_futures);
  TestWaitSome(n_futures);

  /// shutdown
  bisheng::StartupShutdown::Shutdown();
  return 0;
}
