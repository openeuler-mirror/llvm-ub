//===---locality.h - User-facing locality information interface -*- C++ -*-===//
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
 * @file locality.h
 * @brief User-facing interface for cluster locality information
 *
 * Provides simplified access to node information and cluster topology.
 * Enables resource-aware distributed computing.
 *
 * A locality in MatrixCPP is a single operating system process representing one
 * node in a distributed system.
 *
 */

#ifndef BISHENG_LOCALITY_H
#define BISHENG_LOCALITY_H

#include "internal_locality.h"
#include <memory>

namespace bisheng {

class Locality {
public:
  // Get current node ID
  static std::string get_locality_id() {
    return LocalityManager::get_locality_id();
  }

  // Get total number of nodes
  static int get_num_localities() {
    return LocalityManager::get_num_localities();
  }

  // Get all node information
  static ClusterInfo find_all_localities() {
    return LocalityManager::find_all_localities();
  }

  // Format cluster information
  static std::string format_cluster_info(const ClusterInfo &info) {
    return LocalityManager::format_cluster_info(info);
  }

  static std::string get_locality_resource() {
    return LocalityManager::get_locality_resource();
  }

  static std::vector<std::string> get_all_resource() {
    return LocalityManager::get_all_resource();
  }

private:
  Locality() = delete;
  Locality(const Locality &) = delete;
  Locality &operator=(const Locality &) = delete;
};

} // namespace bisheng

#endif // BISHENG_LOCALITY_H