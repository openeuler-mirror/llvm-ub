#!/bin/bash
# Run all uballoc examples in two-node mode (UBSE backend only).
#
# P0 runs locally, P1 runs on a remote node via SSH.
# Both nodes must have uballoc built (UBSE backend).
#
# Usage: ./scripts/run_examples.sh [-v] <remote_host>
#   -v            Verbose: print P0/P1 stdout/stderr (default: suppressed,
#                 only starting/PASS/FAIL lines shown). Can appear anywhere.
#   remote_host:  SSH hostname/IP for P1 (rank 1). Required.
#
# Environment:
#   BUILD_DIR          Local build directory  (default: $PROJECT_DIR/build)
#   REMOTE_BUILD_DIR   Remote build directory (default: same as BUILD_DIR).
#                      Set this when the two nodes have different install
#                      paths so P1 can find the binaries.
#
# Examples:
#   ./scripts/run_examples.sh 192.168.x.x
#   ./scripts/run_examples.sh -v 192.168.x.x
#   ./scripts/run_examples.sh 192.168.x.x -v
#   ./scripts/run_examples.sh node2
#   REMOTE_BUILD_DIR=/home/foo/llvm-ub/uballoc/build ./scripts/run_examples.sh node2

# set -e  # disabled: cleanup commands may return non-zero (no shms to clean)

# ─── Help ───
print_help() {
    cat << 'HELP'
Run all uballoc examples in two-node mode (UBSE backend only).

P0 runs locally, P1 runs on a remote node via SSH. Each example is
preceded/followed by silent shm cleanup (local + remote).

Usage: ./scripts/run_examples.sh [-v] [-h] <remote_host>

Options:
  -v, --verbose    Print P0/P1 stdout/stderr (default: only starting/PASS/FAIL).
                   Can appear anywhere on the command line.
  -h, --help       Show this help and exit.

Arguments:
  remote_host      SSH hostname/IP for P1 (rank 1). Required.

Environment:
  BUILD_DIR          Local build directory  (default: <project>/build)
  REMOTE_BUILD_DIR   Remote build directory (default: same as BUILD_DIR).
                     Set when the two nodes have different install paths.

Examples:
  ./scripts/run_examples.sh node2
  ./scripts/run_examples.sh -v node2
  ./scripts/run_examples.sh node2 -v
  ./scripts/run_examples.sh 192.168.x.x
  REMOTE_BUILD_DIR=/home/foo/llvm-ub/uballoc/build ./scripts/run_examples.sh node2
HELP
}

# ─── Parse args: -v/--verbose and -h/--help anywhere, rest are positional ───
VERBOSE=0
POSITIONAL=()
for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)    print_help; exit 0 ;;
        *) POSITIONAL+=("$arg") ;;
    esac
done
set -- "${POSITIONAL[@]}"

# ─── Require remote host ───
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build}"
REMOTE_BUILD_DIR="${REMOTE_BUILD_DIR:-$BUILD_DIR}"
REMOTE_HOST="${1:-}"

if [ -z "$REMOTE_HOST" ]; then
    echo "Error: remote host is required (two-node mode only)."
    echo "Usage: $0 [-v] <remote_host>"
    echo "Example: $0 node2"
    echo "         $0 -v node2"
    echo "         $0 -v 192.168.x.x"
    exit 1
fi

# Reject hostnames starting with '-' to prevent ssh option injection
# (e.g., '-oProxyCommand=...' would be parsed as an ssh option).
if [[ "$REMOTE_HOST" == -* ]]; then
    echo "Error: remote host must not start with '-': $REMOTE_HOST"
    echo "Usage: $0 [-v] <remote_host>"
    exit 1
fi

# ─── Detect backend (must be UBSE for cross-node) ───
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "Error: build directory $BUILD_DIR does not exist or is not configured."
    echo "Run: cmake -B $BUILD_DIR -DUBALLOC_USE_UBSE=ON && cmake --build $BUILD_DIR"
    exit 1
fi
if ! grep -q "UBALLOC_USE_UBSE:BOOL=ON" "$BUILD_DIR/CMakeCache.txt"; then
    echo "Error: build at $BUILD_DIR is not UBSE backend."
    echo "POSIX backend cannot run cross-node examples."
    echo "Rebuild with: cmake -B $BUILD_DIR -DUBALLOC_USE_UBSE=ON && cmake --build $BUILD_DIR"
    exit 1
fi
CLEANUP_SCRIPT="$SCRIPT_DIR/cleanup_shms_ub.sh"

# Verify cleanup script exists locally
if [ ! -f "$CLEANUP_SCRIPT" ]; then
    echo "Error: cleanup script not found: $CLEANUP_SCRIPT"
    exit 1
fi

# Use an array for SSH_CMD so $REMOTE_HOST is passed as a single argument
# to ssh, preventing word-splitting and option injection.
SSH_CMD=(ssh -- "$REMOTE_HOST")

# Verify SSH connectivity early (fail fast instead of per-example timeout)
echo "Checking SSH connectivity to $REMOTE_HOST..."
if ! "${SSH_CMD[@]}" "echo ok" >/dev/null 2>&1; then
    echo "Error: cannot connect to $REMOTE_HOST via SSH."
    echo "Ensure SSH key-based auth is configured (ssh-copy-id $REMOTE_HOST)."
    exit 1
fi
echo "SSH OK."
echo "=== uballoc examples (UBSE backend, remote=$REMOTE_HOST, timeout=90s, verbose=$VERBOSE) ==="
echo ""

# ─── Helper: print a separator between examples ───
separator() {
    echo "────────────────────────────────────────────────────────────────"
}

# ─── Helper: clean up residual shms (local + remote, silent) ───
cleanup() {
    local heap_id="$1"
    export UBALLOC_HEAP_ID="$heap_id"
    "$CLEANUP_SCRIPT" >/dev/null 2>&1 || true
    "${SSH_CMD[@]}" "UBALLOC_HEAP_ID=$heap_id bash -s" < "$CLEANUP_SCRIPT" >/dev/null 2>&1 || true
}

# ─── Helper: run a two-process example (P0 local, P1 remote) ───
# Args: <name> <heap_id> <binary> [extra_args...]
# P0 is launched as: <binary> <extra_args> 0
# P1 is launched as: <binary> <extra_args> 1
# shift 2 drops <name> and <heap_id> from $@, leaving <binary> + extra_args
# in $@. extra_args is stored in an array to preserve word boundaries.
PASS=0
FAIL=0
FAILED_EXAMPLES=""

run_two_proc_example() {
    local name="$1"
    local heap_id="$2"
    shift 2
    local binary="$1"
    shift
    local -a extra_args=("$@")
    # Remote binary path: replace local BUILD_DIR prefix with REMOTE_BUILD_DIR
    local remote_binary="${binary/$BUILD_DIR/$REMOTE_BUILD_DIR}"

    separator
    echo "[$name] starting..."
    cleanup "$heap_id"

    # Launch P0 locally in background (P0 creates bootstrap shm).
    # In verbose mode output goes to terminal; otherwise discarded.
    if [ "$VERBOSE" -eq 1 ]; then
        timeout 90 env UBALLOC_HEAP_ID="$heap_id" UBALLOC_LOG_LEVEL=error \
            "$binary" 0 "${extra_args[@]}" 2>&1 &
    else
        timeout 90 env UBALLOC_HEAP_ID="$heap_id" UBALLOC_LOG_LEVEL=error \
            "$binary" 0 "${extra_args[@]}" >/dev/null 2>&1 &
    fi
    p0_pid=$!
    # Wait for P0 to finish bootstrap_join + create_own_regions so P1
    # can attach. 2s is empirically sufficient on UBSE hardware; if P0
    # is slow (cold cache, heavy load), P1 may fail to discover the
    # bootstrap shm on first poll and retry until its internal timeout.
    sleep 2

    # Launch P1 on remote host (P1 discovers P0's bootstrap).
    # remote_binary uses REMOTE_BUILD_DIR so P1 finds the binary even if
    # the two nodes have different install paths.
    if [ "$VERBOSE" -eq 1 ]; then
        timeout 90 "${SSH_CMD[@]}" \
            "UBALLOC_HEAP_ID=$heap_id UBALLOC_LOG_LEVEL=error '$remote_binary' 1 ${extra_args[*]}" 2>&1
    else
        timeout 90 "${SSH_CMD[@]}" \
            "UBALLOC_HEAP_ID=$heap_id UBALLOC_LOG_LEVEL=error '$remote_binary' 1 ${extra_args[*]}" >/dev/null 2>&1
    fi
    p1_rc=$?
    wait $p0_pid 2>/dev/null
    p0_rc=$?

    if [ $p0_rc -eq 0 ] && [ $p1_rc -eq 0 ]; then
        echo "[$name] PASS"
        PASS=$((PASS + 1))
    else
        echo "[$name] FAIL (p0=$p0_rc p1=$p1_rc)"
        FAIL=$((FAIL + 1))
        FAILED_EXAMPLES="$FAILED_EXAMPLES $name"
    fi
    cleanup "$heap_id"
}

# ─── Two-process examples (P0 publishes, P1 discovers) ───
run_two_proc_example "ub_example"                   "ubex"    "$BUILD_DIR/ub_example"
run_two_proc_example "distributed_example"         "distex"  "$BUILD_DIR/distributed_example"
run_two_proc_example "published_example"           "pubex"   "$BUILD_DIR/published_example"
run_two_proc_example "dynamic_discovery_example"   "dynex"   "$BUILD_DIR/dynamic_discovery_example"
run_two_proc_example "stl_full_shm_example"        "stlex"   "$BUILD_DIR/stl_full_shm_example"
run_two_proc_example "stl_data_only_example"       "stldoex" "$BUILD_DIR/stl_data_only_example"
run_two_proc_example "fault_handler_example"       "fhex"    "$BUILD_DIR/fault_handler_example"
run_two_proc_example "uffd_example"                "uffdex"  "$BUILD_DIR/uffd_example"
run_two_proc_example "reclaim_example (sync)"      "reclaim" "$BUILD_DIR/reclaim_example" sync
run_two_proc_example "reclaim_example (async)"     "reclaim" "$BUILD_DIR/reclaim_example" async
run_two_proc_example "cross_node_2gb_example"      "cross2g" "$BUILD_DIR/cross_node_2gb_example"

# ─── Summary ───
separator
echo ""
echo "=== Summary: $PASS passed, $FAIL failed ==="
if [ $FAIL -gt 0 ]; then
    echo "Failed:$FAILED_EXAMPLES"
    exit 1
fi
echo "All examples passed!"
exit 0
