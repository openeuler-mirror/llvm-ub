//===---internal_locality.cpp-Locality information implementation-*-C++-*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "internal_locality.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

constexpr int buffer_diam = 128;
constexpr int kilo = 1024;
constexpr int parts_size_two = 2;
constexpr int parts_size_three = 3;

namespace bisheng {

namespace {

// Find script path automatically
std::string find_script_path(const std::string &script_name) {
  const char *env_scripts_dir = std::getenv("BISHENG_SCRIPTS_DIR");
  if (env_scripts_dir) {
    std::filesystem::path candidate =
        std::filesystem::path(env_scripts_dir) / script_name;
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
  }

#ifdef BISHENG_DEFAULT_SCRIPTS_DIR
  std::filesystem::path default_candidate =
      std::filesystem::path(BISHENG_DEFAULT_SCRIPTS_DIR) / script_name;
  if (std::filesystem::exists(default_candidate)) {
    return default_candidate.string();
  }
#endif

  throw std::runtime_error(
      "Cannot find script: " + script_name +
      ". Please set BISHENG_SCRIPTS_DIR environment variable.");
}

// Safe string conversion functions
int safe_stoi(const std::string &str, int default_val = 0) {
  try {
    return std::stoi(str);
  } catch (...) {
    return default_val;
  }
}

double safe_stod(const std::string &str, double default_val = 0.0) {
  try {
    return std::stod(str);
  } catch (...) {
    return default_val;
  }
}

// Execute command and get output
std::string exec(const char *cmd) {
  std::array<char, buffer_diam> buffer;
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
  if (!pipe) {
    return "";
  }
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  return result;
}

// Simple string split function
std::vector<std::string> split(const std::string &str, char delimiter) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(str);
  while (std::getline(tokenStream, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

// Trim whitespace from both ends of string
std::string trim(const std::string &str) {
  size_t start = str.find_first_not_of(" \t\n\r");
  size_t end = str.find_last_not_of(" \t\n\r");
  if (start == std::string::npos) {
    return "";
  }
  return str.substr(start, end - start + 1);
}

} // namespace

std::string NodeResources::to_string() const {
  std::ostringstream oss;
  oss << "CPU: " << cpu << "\n";
  oss << "Memory: " << memory / (kilo * kilo * kilo) << " GB\n";
  oss << "Object Store Memory: " << object_store_memory / (kilo * kilo * kilo)
      << " GB\n";

  if (!custom_resources.empty()) {
    oss << "Custom Resources:\n";
    for (const auto &resource : custom_resources) {
      oss << "  " << resource.first << ": " << resource.second << "\n";
    }
  }

  return oss.str();
}

std::string NodeInfo::to_string() const {
  std::ostringstream oss;
  oss << "Node ID: " << node_id << "\n";
  oss << "Alive: " << (alive ? "Yes" : "No") << "\n";
  oss << "Address: " << node_manager_address << "\n";
  oss << "Hostname: " << node_manager_hostname << "\n";
  oss << "Port: " << node_manager_port << "\n";
  oss << "Resources:\n" << resources.to_string();

  if (!labels.empty()) {
    oss << "Labels:\n";
    for (const auto &label : labels) {
      oss << "  " << label.first << ": " << label.second << "\n";
    }
  }

  return oss.str();
}

std::string ClusterInfo::to_string() const {
  std::ostringstream oss;
  oss << "=== Cluster Information ===\n";
  oss << "Current Node ID: " << current_node_id << "\n";
  oss << "Total Nodes: " << total_nodes << "\n\n";

  for (size_t i = 0; i < nodes.size(); ++i) {
    oss << "Node " << (i + 1) << ":\n";
    oss << nodes[i].to_string() << "\n";
  }

  oss << "===============================";
  return oss.str();
}

const NodeInfo *ClusterInfo::find_node_by_id(const std::string &node_id) const {
  for (const auto &node : nodes) {
    if (node.node_id == node_id) {
      return &node;
    }
  }
  return nullptr;
}

std::vector<NodeInfo> ClusterInfo::get_alive_nodes() const {
  std::vector<NodeInfo> alive_nodes;
  for (const auto &node : nodes) {
    if (node.alive) {
      alive_nodes.push_back(node);
    }
  }
  return alive_nodes;
}

std::vector<std::string> ClusterInfo::get_all_node_ids() const {
  std::vector<std::string> node_ids;
  for (const auto &node : nodes) {
    node_ids.push_back(node.node_id);
  }
  return node_ids;
}

// Parse JSON formatted node information
ClusterInfo parse_nodes_json(const std::string &json_str) {
  ClusterInfo info;

  if (json_str.empty()) {
    info.success = false;
    info.error_message = "Empty response from Python script";
    return info;
  }

  // Check for error response
  if (json_str.find("\"success\": false") != std::string::npos ||
      json_str.find("Error:") != std::string::npos) {
    info.success = false;
    info.error_message = "Python script returned error";
    return info;
  }

  // Use separate Python parsing script
  std::string script_path = find_script_path("parse_nodes.py");
  std::string command =
      "echo '" + json_str + "' | python3 '" + script_path + "'";
  std::string output = exec(command.c_str());

  // Parse output
  std::istringstream iss(output);
  std::string line;

  NodeInfo current_node;
  bool in_node = false;

  while (std::getline(iss, line)) {
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    // Split line using | delimiter
    std::vector<std::string> parts = split(line, '|');
    if (parts.empty()) {
      continue;
    }

    std::string key = parts[0];

    if (key == "CURRENT_NODE") {
      if (parts.size() >= parts_size_two) {
        info.current_node_id = parts[1];
      }
    } else if (key == "TOTAL_NODES") {
      if (parts.size() >= parts_size_two) {
        info.total_nodes = safe_stoi(parts[1]);
      }
    } else if (key == "NODE_START") {
      in_node = true;
      current_node = NodeInfo(); // Reset current node
    } else if (key == "NODE_END") {
      in_node = false;
      info.nodes.push_back(current_node);
    } else if (key == "ID") {
      if (parts.size() >= parts_size_two) {
        current_node.node_id = parts[1];
      }
    } else if (key == "ALIVE") {
      if (parts.size() >= parts_size_two) {
        current_node.alive = (parts[1] == "True");
      }
    } else if (key == "ADDRESS") {
      if (parts.size() >= parts_size_two) {
        current_node.node_manager_address = parts[1];
      }
    } else if (key == "HOSTNAME") {
      if (parts.size() >= parts_size_two) {
        current_node.node_manager_hostname = parts[1];
      }
    } else if (key == "PORT") {
      if (parts.size() >= parts_size_two) {
        current_node.node_manager_port = safe_stoi(parts[1]);
      }
    } else if (key == "CPU") {
      if (parts.size() >= parts_size_two) {
        current_node.resources.cpu = safe_stod(parts[1]);
      }
    } else if (key == "MEMORY") {
      if (parts.size() >= parts_size_two) {
        current_node.resources.memory = safe_stod(parts[1]);
      }
    } else if (key == "OBJECT_STORE_MEMORY") {
      if (parts.size() >= parts_size_two) {
        current_node.resources.object_store_memory = safe_stod(parts[1]);
      }
    } else if (key == "CUSTOM_RESOURCE") {
      if (parts.size() >= parts_size_three) {
        std::string resource_key = parts[1];
        double value = safe_stod(parts[2]);
        current_node.resources.custom_resources[resource_key] = value;
      }
    } else if (key == "LABEL") {
      if (parts.size() >= parts_size_three) {
        std::string label_key = parts[1];
        std::string label_value = parts[2];
        current_node.labels[label_key] = label_value;
      }
    } else if (key == "SUCCESS") {
      info.success = true;
    } else if (key == "ERROR") {
      info.success = false;
      if (parts.size() >= parts_size_two) {
        info.error_message = parts[1];
      } else {
        info.error_message = "Unknown error";
      }
    }
  }

  return info;
}

// LocalityManager implementation
std::string LocalityManager::get_locality_id() {
  std::string script_path = find_script_path("nodes.py");
  std::string result = exec(
      ("python3 '" + script_path + "' get_locality_id 2>/dev/null").c_str());
  if (!result.empty() && result.back() == '\n') {
    result.pop_back();
  }
  return result;
}

int LocalityManager::get_num_localities() {
  std::string script_path = find_script_path("nodes.py");
  std::string result = exec(
      ("python3 '" + script_path + "' get_num_localities 2>/dev/null").c_str());
  return safe_stoi(result);
}

ClusterInfo LocalityManager::find_all_localities() {
  // Get JSON formatted node information
  std::string script_path = find_script_path("nodes.py");
  std::string json_result = exec(
      ("python3 '" + script_path + "' get_all_nodes_json 2>/dev/null").c_str());

  // Parse JSON using separate Python script
  return parse_nodes_json(json_result);
}

std::string LocalityManager::format_cluster_info(const ClusterInfo &info) {
  return info.to_string();
}

std::string LocalityManager::get_locality_resource() {
  std::string current_node_id = get_locality_id();
  if (current_node_id.empty()) {
    return "";
  }

  ClusterInfo cluster_info = find_all_localities();
  if (!cluster_info.success) {
    return "";
  }

  // Find current node
  for (const auto &node : cluster_info.nodes) {
    if (node.node_id == current_node_id) {
      // Extract simple node resource names (e.g., node0, node1)
      std::vector<std::string> simple_resources;
      for (const auto &resource : node.resources.custom_resources) {
        std::string resource_name = resource.first;
        // Keep only simple node names (node followed by number)
        if (resource_name.find("node") == 0 &&
            resource_name.find(":") == std::string::npos &&
            resource_name.find("__") == std::string::npos) {
          simple_resources.push_back(resource_name);
        }
      }

      // Return resource name
      if (!simple_resources.empty()) {
        return simple_resources[0];
      }
      return "";
    }
  }

  return "";
}

std::vector<std::string> LocalityManager::get_all_resource() {
  ClusterInfo cluster_info = find_all_localities();
  if (!cluster_info.success) {
    return {};
  }

  std::set<std::string> simple_resources;

  for (const auto &node : cluster_info.nodes) {
    // Collect simple node resource names only
    for (const auto &resource : node.resources.custom_resources) {
      std::string resource_name = resource.first;
      // Keep only simple node names (node followed by number)
      if (resource_name.find("node") == 0 &&
          resource_name.find(":") == std::string::npos &&
          resource_name.find("__") == std::string::npos) {
        simple_resources.insert(resource_name);
      }
    }
  }

  return std::vector<std::string>(simple_resources.begin(),
                                  simple_resources.end());
}

} // namespace bisheng