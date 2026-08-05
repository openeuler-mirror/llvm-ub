//==- test_segmented_vector.cpp - Comprehensive tests for segmented vector. ==//
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
 * @file test_segmented_vector.cpp
 * @brief Comprehensive test suite for bisheng::segmented_vector
 *
 * This test file verifies all core functionality of the segmented_vector
 * pattern:
 * - Basic value set
 * - Basic value get
 * @note Requires Ray cluster initialization with --resources='{"node0": 2}'
 */

#include <MatrixCPP.h>

#include <iostream>
#include <vector>

struct Test {
  int a;
  int b;
  BISHENG_PACK_DEFINE(a, b);
};

BISHENG_DECLARE_REMOTE_SEGMENTED_VECTOR(Test);

int main()
{
  bisheng::StartupShutdown::Init();

  const size_t size = 100;
  bisheng::segmented_vector<int> sv(size);

  std::vector<size_t> pos(size);
  std::vector<int> val(size);
  for (size_t i = 0; i < size; i++) {
    pos[i] = i;
    val[i] = i;
  }

  const size_t num = 10;
  auto future = sv.set_values_async(pos, val);
  if (future.get()) {
    for (size_t i = 0; i < num; i++) {
      std::cout << sv[i] << std::endl;
    }
    pos.resize(num);
    for (size_t i = size - num; i < size; i++) {
      pos[i - (size - num)] = i;
    }
    auto res = sv.get_values_async(pos);
    for (auto n : res.get()) {
      std::cout << n << std::endl;
    }
  }

  bisheng::segmented_vector<Test> test_sv(size);
  std::vector<Test> test_val(size);
  for (size_t i = 0; i < size; i++) {
    pos[i] = i;
    test_val[i].a = i;
    test_val[i].b = i;
  }
  test_sv.set_values(pos, test_val);
  for (size_t i = 0; i < num; i++) {
    std::cout << test_sv[i].a << " " << test_sv[i].b << std::endl;
  }

  bisheng::StartupShutdown::Shutdown();
  return 0;
}