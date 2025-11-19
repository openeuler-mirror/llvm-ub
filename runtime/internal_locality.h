//===---internal_locality.h -Locality information interface  -*- C++  -*---===//
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
 * @file internal_locality.h
 * @brief Internal interface for node locality information management
 *
 * Provides low-level access to cluster node information and resource details.
 * Handles the interaction with distributed computing runtime.
 */

#ifndef INTERNAL_LOCALITY_H
#define INTERNAL_LOCALITY_H

#include <map>
#include <string>
#include <vector>

namespace bisheng {

// Node resource information
struct NodeResources {
  double cpu;
  double memory;
  double object_store_memory;
  std::map<std::string, double> custom_resources;

  std::string to_string() const;
};

// Node information
struct NodeInfo {
  std::string node_id;
  bool alive;
  std::string node_manager_address;
  std::string node_manager_hostname;
  int node_manager_port;
  NodeResources resources;
  std::map<std::string, std::string> labels;

  std::string to_string() const;
};

// Cluster information
struct ClusterInfo {
  std::string current_node_id;
  int total_nodes;
  std::vector<NodeInfo> nodes;
  bool success;
  std::string error_message;

  std::string to_string() const;
  const NodeInfo *find_node_by_id(const std::string &node_id) const;
  std::vector<NodeInfo> get_alive_nodes() const;
  std::vector<std::string> get_all_node_ids() const;
};

class LocalityManager {
public:
  static std::string get_locality_id();
  static int get_num_localities();
  static ClusterInfo find_all_localities();
  static std::string format_cluster_info(const ClusterInfo &info);
  static std::string get_locality_resource();
  static std::vector<std::string> get_all_resource();

  LocalityManager() = delete;
  LocalityManager(const LocalityManager &) = delete;
  LocalityManager &operator=(const LocalityManager &) = delete;
};

} // namespace bisheng

#endif // INTERNAL_LOCALITY_H