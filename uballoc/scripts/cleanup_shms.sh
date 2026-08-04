#!/bin/bash
# Clean up distributed shm objects left behind after a crash.
#
# For deployments with has_reliable_unreferenced_check=false (POSIX),
# crashed processes leave shm objects in /dev/shm. This script removes
# them on the local node and optionally on remote nodes via ssh.
#
# Usage:
#   ./cleanup_shms.sh [num_processes] [node0_host] [node1_host] ...
#   ./cleanup_shms.sh -f nodefile
#
#   num_processes:  number of processes (default: 2)
#   nodeN_host:     ssh hostname for process N (skip for local)
#   -f nodefile:    file with one hostname per line (process_id = line index)
#                   local node = empty line or "local"
#
# Examples:
#   # Local-only cleanup (single machine deployment)
#   ./cleanup_shms.sh 2
#
#   # 2-node deployment: P0 local, P1 at node1
#   ./cleanup_shms.sh 2 "" node1
#
#   # 3-node deployment using config file
#   ./cleanup_shms.sh -f nodes.txt
#     nodes.txt contents:
#       local
#       10.0.0.2
#       10.0.0.3

SUFFIXES=("-m" "-ds" "-dl" "-dh")
# Bootstrap shm is per-heap_id (not per-process). Default heap_id is "heap";
# override via UBALLOC_HEAP_ID env var. Use glob *-bootstrap to catch all heap_ids.
# Shm naming: /<heap_id>-p<pid>-{m,ds,dl,dh}[-s<seg_idx>]
UBALLOC_HEAP_ID="${UBALLOC_HEAP_ID:-heap}"
MAX_SEGS=8

cleanup_node() {
    local pid=$1
    local host=$2
    local cleaned=0

    for suffix in "${SUFFIXES[@]}"; do
        local name="/${UBALLOC_HEAP_ID}-p${pid}${suffix}"
        if [ -z "$host" ] || [ "$host" = "local" ]; then
            local shm_path="/dev/shm${name}"
            if [ -e "$shm_path" ]; then
                rm -f "$shm_path"
                echo "  removed $shm_path"
                cleaned=$((cleaned + 1))
            fi
            # Also clean up segment shms (-s1, -s2, ...), data regions only
            if [ "$suffix" != "-m" ]; then
                for seg in $(seq 1 $MAX_SEGS); do
                    local seg_path="/dev/shm${name}-s${seg}"
                    if [ -e "$seg_path" ]; then
                        rm -f "$seg_path"
                        echo "  removed $seg_path"
                        cleaned=$((cleaned + 1))
                    fi
                done
            fi
        else
            local result=$(ssh "$host" "if [ -e '/dev/shm${name}' ]; then rm -f '/dev/shm${name}' && echo 'ok'; else echo 'absent'; fi" 2>/dev/null)
            if [ "$result" = "ok" ]; then
                echo "  removed ${name} on ${host}"
                cleaned=$((cleaned + 1))
            fi
            if [ "$suffix" != "-m" ]; then
                for seg in $(seq 1 $MAX_SEGS); do
                    result=$(ssh "$host" "if [ -e '/dev/shm${name}-s${seg}' ]; then rm -f '/dev/shm${name}-s${seg}' && echo 'ok'; else echo 'absent'; fi" 2>/dev/null)
                    if [ "$result" = "ok" ]; then
                        echo "  removed ${name}-s${seg} on ${host}"
                        cleaned=$((cleaned + 1))
                    fi
                done
            fi
        fi
    done
    return $cleaned
}

cleanup_bootstrap() {
    local host=$1
    local cleaned=0
    local name="/${UBALLOC_HEAP_ID}-bootstrap"
    if [ -z "$host" ] || [ "$host" = "local" ]; then
        # Also glob-clean any other heap_id bootstrap shms left from prior runs
        for shm_path in /dev/shm/*-bootstrap; do
            if [ -e "$shm_path" ]; then
                rm -f "$shm_path"
                echo "  removed $shm_path"
                cleaned=$((cleaned + 1))
            fi
        done
    else
        local result=$(ssh "$host" "for f in /dev/shm/*-bootstrap; do [ -e \"\$f\" ] && rm -f \"\$f\" && echo \"ok\"; done" 2>/dev/null)
        if [ "$result" = "ok" ]; then
            echo "  removed bootstrap shms on ${host}"
            cleaned=$((cleaned + 1))
        fi
    fi
    return $cleaned
}

if [ "$1" = "-f" ]; then
    if [ -z "$2" ] || [ ! -f "$2" ]; then
        echo "Error: nodefile required after -f"
        exit 1
    fi
    NODEFILE="$2"
    NUM_PROCS=0
    HOSTS=()
    while IFS= read -r line || [ -n "$line" ]; do
        line=$(echo "$line" | xargs)
        if [ -z "$line" ] || [ "$line" = "local" ]; then
            HOSTS+=("")
        else
            HOSTS+=("$line")
        fi
        NUM_PROCS=$((NUM_PROCS + 1))
    done < "$NODEFILE"
else
    NUM_PROCS=${1:-2}
    shift
    HOSTS=()
    for pid in $(seq 0 $((NUM_PROCS - 1))); do
        if [ $# -gt 0 ]; then
            HOSTS+=("$1")
            shift
        else
            HOSTS+=("")
        fi
    done
fi

total=0
echo "Cleaning up shm objects for $NUM_PROCS processes..."
for pid in $(seq 0 $((NUM_PROCS - 1))); do
    host="${HOSTS[$pid]}"
    if [ -z "$host" ]; then
        echo "P${pid} (local):"
    else
        echo "P${pid} (${host}):"
    fi
    cleanup_node "$pid" "$host"
    n=$?
    total=$((total + n))
done

# Bootstrap shms are per-heap_id (created by P0 only). Clean up on every node.
echo "Bootstrap shms:"
for pid in $(seq 0 $((NUM_PROCS - 1))); do
    host="${HOSTS[$pid]}"
    if [ -z "$host" ]; then
        echo "  (local):"
    else
        echo "  (${host}):"
    fi
    cleanup_bootstrap "$host"
    n=$?
    total=$((total + n))
done

if [ $total -eq 0 ]; then
    echo "No shm objects found to clean up."
else
    echo "Cleaned up $total shm objects across $NUM_PROCS processes."
fi
