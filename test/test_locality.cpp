// - test_locality.cpp - Comprehensive tests for the locality implementation.-//
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
 * @file test_locality.cpp
 * @brief Comprehensive test suite for bisheng::Locality
 *
 * This test file verifies all core functionally of the distributed locality
 * information system:
 * - Node identification and discovery
 * - Cluster topology information
 * - Resource availability querying
 * - Error handling and edge cases
 * @note Requires Ray cluster initialization
 */

#include <MatrixCPP.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Test basic locality functionality
void TestBasicFunctionality() {
  std::cout << "Testing Basic Locality Functionality" << std::endl;

  std::string locality_id = bisheng::Locality::get_locality_id();
  assert(!locality_id.empty() && "Locality ID should not be empty");
  std::cout << "✓ get_locality_id(): " << locality_id << std::endl;

  int num_localities = bisheng::Locality::get_num_localities();
  assert(num_localities > 0 && "Should have at least one locality");
  std::cout << "✓ get_num_localities(): " << num_localities << std::endl;
}

// Test structured data access
void TestStructuredData() {
  std::cout << "\nTesting Structured Data Access" << std::endl;

  bisheng::ClusterInfo cluster_info = bisheng::Locality::find_all_localities();
  assert(cluster_info.success && "Should successfully retrieve cluster info");
  assert(cluster_info.total_nodes > 0 && "Should have positive node count");
  assert(!cluster_info.nodes.empty() && "Should have at least one node");

  std::cout << "✓ Cluster info retrieved successfully" << std::endl;
  std::cout << "  - Current node: " << cluster_info.current_node_id
            << std::endl;
  std::cout << "  - Total nodes: " << cluster_info.total_nodes << std::endl;
  std::cout << "  - Nodes found: " << cluster_info.nodes.size() << std::endl;

  // Test node information completeness
  for (const auto &node : cluster_info.nodes) {
    assert(!node.node_id.empty() && "Node ID should not be empty");
    assert(!node.node_manager_address.empty() &&
           "Node address should not be empty");
  }

  // Test helper methods
  std::vector<bisheng::NodeInfo> alive_nodes = cluster_info.get_alive_nodes();
  assert(!alive_nodes.empty() && "Should have at least one alive node");
  std::cout << "✓ Alive nodes: " << alive_nodes.size() << std::endl;

  // Test formatting
  std::string formatted = bisheng::Locality::format_cluster_info(cluster_info);
  assert(!formatted.empty() && "Formatted output should not be empty");
  std::cout << "✓ Formatting produces valid output" << std::endl;
}

// Test error conditions
void TestErrorConditions() {
  std::cout << "\nTesting Error Conditions" << std::endl;

  bisheng::ClusterInfo cluster_info = bisheng::Locality::find_all_localities();
  const bisheng::NodeInfo *not_found =
      cluster_info.find_node_by_id("nonexistent_node_id");
  assert(not_found == nullptr && "Should return null for nonexistent node");
  std::cout << "✓ Nonexistent node lookup returns null" << std::endl;
}

// Test resource functions
void TestResourceFunctions() {
  std::cout << "\nTesting Resource Functions" << std::endl;

  // Test get_locality_resource
  std::string locality_resource = bisheng::Locality::get_locality_resource();
  if (!locality_resource.empty()) {
    std::cout << "✓ get_locality_resource(): " << locality_resource
              << std::endl;
  }

  // Test get_all_resource
  std::vector<std::string> all_resources =
      bisheng::Locality::get_all_resource();
  if (!all_resources.empty()) {
    std::cout << "✓ get_all_resource() found " << all_resources.size()
              << " resources" << std::endl;
  }

  std::cout << "✓ Resource function tests completed" << std::endl;
}

int main() {
  std::cout << "Starting Locality Test Suite\n" << std::endl;

  try {
    TestBasicFunctionality();
    TestStructuredData();
    TestErrorConditions();
    TestResourceFunctions();

    std::cout << "\n=== All Tests Completed Successfully ===" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Test failed with exception: " << e.what() << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "Test failed with unknown exception" << std::endl;
    return 1;
  }
}