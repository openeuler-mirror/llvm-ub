#  Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
#  See https://llvm.org/LICENSE.txt for license information.
#  SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#!/usr/bin/env python3

import ray
import json
import sys
import os

def init_ray():
    """Initialize Ray connection and suppress unnecessary output"""
    if not ray.is_initialized():
        try:
            # Suppress Ray logging output
            import logging
            ray_logger = logging.getLogger("ray")
            ray_logger.setLevel(logging.ERROR)

            # Initialize Ray connection
            address = os.getenv("RAY_ADDRESS", "auto")
            ray.init(
                address=address,
                log_to_driver=False,
                configure_logging=False
            )
            return True
        except Exception as e:
            return False
    return True

def get_locality_id():
    try:
        if not init_ray():
            return ''
        # Get current node ID
        return ray.get_runtime_context().get_node_id()
    except Exception as e:
        print(f"Error in get_locality_id: {e}", file=sys.stderr)
        return ''

def get_num_localities():
    try:
        if not init_ray():
            return 0
        nodes = ray.nodes()
        return len(nodes)
    except Exception:
        return 0

def get_all_nodes_json():
    """Return all node information in JSON format"""
    try:
        if not init_ray():
            return json.dumps({"success": False, "error": "Failed to connect to Ray"})

        nodes = ray.nodes()

        # Extract key information
        node_info = []
        for node in nodes:
            node_data = {
                'node_id': node.get('NodeID', ''),
                'alive': node.get('Alive', False),
                'node_manager_address': node.get('NodeManagerAddress', ''),
                'node_manager_hostname': node.get('NodeManagerHostname', ''),
                'node_manager_port': node.get('NodeManagerPort', 0),
                'resources': node.get('Resources', {}),
                'labels': node.get('Labels', {})
            }
            node_info.append(node_data)

        # Try to identify current node
        current_node_id = ""
        for node in node_info:
            if node['alive']:
                current_node_id = node['node_id']
                break

        result = {
            'current_node_id': current_node_id,
            'total_nodes': len(node_info),
            'nodes': node_info,
            'success': True
        }
        return json.dumps(result)

    except Exception as e:
        error_result = {
            'success': False,
            'error': str(e),
            'current_node_id': '',
            'total_nodes': 0,
            'nodes': []
        }
        return json.dumps(error_result)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python3 bisheng_nodes.py [get_locality_id|get_num_localities|get_all_nodes_json]")
        sys.exit(1)

    command = sys.argv[1]

    if command == "get_locality_id":
        print(get_locality_id())
    elif command == "get_num_localities":
        print(get_num_localities())
    elif command == "get_all_nodes_json":
        print(get_all_nodes_json())
    else:
        print(f"Unknown command: {command}")
        sys.exit(1)