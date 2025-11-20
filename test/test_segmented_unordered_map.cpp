//===- test_segmented_unordered_map.cpp - Tests for segmented unordered map ==//
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
 * @file test_segmented_unordered_map.cpp
 * @brief Comprehensive test suite for bisheng::segmented_unordered_map
 *
 * This test file verifies all core functionality of segmented_unordered_map
 * pattern:
 * - Basic value get
 * - Basic value set
 * @note Requires Ray cluster initialization with --resources='{"node0" : 2}'
 */

#include <MatrixCPP.h>

#include <iostream>
#include <vector>
#include <string>

struct Test {
  int a;
  int b;
  BISHENG_PACK_DEFINE(a, b);
};

BISHENG_DECLARE_REMOTE_SEGMENTED_UNORDERED_MAP(std::string, Test);

int main()
{
  bisheng::StartupShutdown::Init();
  const size_t size = 10;
  bisheng::segmented_unordered_map<std::string, int> segmented_map(size);
  std::vector<bisheng::future<bool>> futures;
  futures.push_back(segmented_map.set_value_async("a", 1));
  futures.push_back(segmented_map.set_value_async("b", 2));
  futures.push_back(segmented_map.set_value_async("c", 3));
  std::cout << segmented_map.get_value_async("a").get() << std::endl;
  std::cout << segmented_map.get_value_async("b").get() << std::endl;
  std::cout << segmented_map.get_value_async("c").get() << std::endl;

  bisheng::segmented_unordered_map<std::string, Test> test_map(size);
  test_map.set_value("a", {1, 1});
  test_map.set_value("b", {2, 2});
  test_map.set_value("c", {3, 3});
  std::cout << test_map["a"].a << " " << test_map["a"].b << std::endl;
  std::cout << test_map["b"].a << " " << test_map["b"].b << std::endl;
  std::cout << test_map["c"].a << " " << test_map["c"].b << std::endl;

  bisheng::StartupShutdown::Shutdown();
  return 0;
}