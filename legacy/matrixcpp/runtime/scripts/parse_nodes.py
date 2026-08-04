#  Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
#  See https://llvm.org/LICENSE.txt for license information.
#  SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#!/usr/bin/env python3

import json
import sys

def parse_nodes_json():
    """Parse JSON formatted node information and output easily parsable format (using | delimiter)"""
    try:
        # Read JSON data from stdin
        json_str = sys.stdin.read()
        data = json.loads(json_str)

        # Output basic information
        print(f"CURRENT_NODE|{data.get('current_node_id', '')}")
        print(f"TOTAL_NODES|{data.get('total_nodes', 0)}")

        # Output each node's information
        for i, node in enumerate(data.get('nodes', [])):
            print(f"NODE_START|{i}")
            print(f"ID|{node.get('node_id', '')}")
            print(f"ALIVE|{node.get('alive', False)}")
            print(f"ADDRESS|{node.get('node_manager_address', '')}")
            print(f"HOSTNAME|{node.get('node_manager_hostname', '')}")
            print(f"PORT|{node.get('node_manager_port', 0)}")

            # Resource information
            resources = node.get('resources', {})
            print(f"CPU|{resources.get('CPU', 0.0)}")
            print(f"MEMORY|{resources.get('memory', 0.0)}")
            print(f"OBJECT_STORE_MEMORY|{resources.get('object_store_memory', 0.0)}")

            # Custom resources
            for key, value in resources.items():
                if key not in ['CPU', 'memory', 'object_store_memory']:
                    print(f"CUSTOM_RESOURCE|{key}|{value}")

            # Labels
            labels = node.get('labels', {})
            for key, value in labels.items():
                print(f"LABEL|{key}|{value}")

            print(f"NODE_END|{i}")

        print("SUCCESS|true")

    except Exception as e:
        print(f"ERROR|{str(e)}")

if __name__ == "__main__":
    parse_nodes_json()