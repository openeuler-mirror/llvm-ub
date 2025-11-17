//===-- test_component.cpp - Tests for the component implementation. ------===//
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
 * @file test_component.cpp
 * @brief Comprehensive test suite for component api.
 */

#include <MatrixCPP.h>
#include <iostream>
#include <string>
#include <vector>

/// class
class TestComponent {
public:
  int count;

  explicit TestComponent(int init) : count(init) {}
  /// static factory method
  static TestComponent *FactoryCreate(int init) {
    return new TestComponent(init);
  }

  /// non static function
  int Add(int x) {
    count += x;
    return count;
  }
};

/// Declare remote function
BISHENG_REMOTE(TestComponent::FactoryCreate, &TestComponent::Add);

int main(int argc, char **argv) {
  /// initialization
  bisheng::StartupShutdown::Init();

  bisheng::id_type id =
      bisheng::createNew<TestComponent>(TestComponent::FactoryCreate, 0);

  bisheng::future<int> f1 = bisheng::async(&TestComponent::Add, id, 3);
  bisheng::future<int> f2 = bisheng::async<&TestComponent::Add>(id, 3);
  bisheng::future<int> f3 =
      bisheng::async(&TestComponent::Add, "node0", 1, id, 3);
  bisheng::future<int> f4 =
      bisheng::async<&TestComponent::Add>("node0", 1, id, 3);
  int res = f1.get();
  std::cout << "test_result_1 = " << res << std::endl;
  res = f2.get();
  std::cout << "test_result_2 = " << res << std::endl;
  res = f3.get();
  std::cout << "test_result_3 = " << res << std::endl;
  res = f4.get();
  std::cout << "test_result_4 = " << res << std::endl;

  /// shutdown
  bisheng::StartupShutdown::Shutdown();
  return 0;
}
