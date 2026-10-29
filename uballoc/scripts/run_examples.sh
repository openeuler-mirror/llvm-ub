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
#                      LD_LIBRARY_PATH=$REMOTE_BUILD_DIR is exported on the
#                      remote command since the binary's RPATH is the local
#                      build dir (not portable across install paths).

# set -e  # disabled: cleanup commands may return non-zero (no shms to clean)

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
                     LD_LIBRARY_PATH=$REMOTE_BUILD_DIR is exported on the
                     remote command so P1 finds libuballoc.so.

Prerequisites:
  - UBSE backend built on both nodes (UBALLOC_USE_UBSE=ON)
  - Passwordless SSH from this node to remote_host (ssh-copy-id)
  - ubsectl in PATH on both nodes
  - For fault_handler_example: userfaultfd must be available on BOTH nodes
    (sysctl vm.unprivileged_userfaultfd=1, or run as root). Otherwise the
    example is auto-SKIPped with a reason printed in the summary.
  - cleanup_shms_ub.sh may ssh to other peers reported by ubsectl; those
    peers must also have passwordless SSH configured from THIS node.

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

# Reject hostnames starting with '-' to prevent ssh option injection.
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

if [ ! -f "$CLEANUP_SCRIPT" ]; then
    echo "Error: cleanup script not found: $CLEANUP_SCRIPT"
    exit 1
fi

# Array form prevents word-splitting/option injection on $REMOTE_HOST.
# ConnectTimeout/BatchMode make probes fail fast instead of stalling ~2min.
SSH_CMD=(ssh -o ConnectTimeout=10 -o BatchMode=yes -- "$REMOTE_HOST")

# ─── Pre-flight checks ───

# 1. SSH connectivity (local → remote)
echo "Checking SSH connectivity to $REMOTE_HOST..."
if ! "${SSH_CMD[@]}" "echo ok" >/dev/null 2>&1; then
    echo "Error: cannot connect to $REMOTE_HOST via SSH."
    echo "Ensure SSH key-based auth is configured (ssh-copy-id $REMOTE_HOST)."
    exit 1
fi
echo "  SSH OK (local -> $REMOTE_HOST)."

# 2. ubsectl availability + permission on local node.
#    ubsectl is the management CLI used by cleanup_shms_ub.sh to discover
#    residual shms. RBAC may deny it (UBS_ERR_PERMISSION_DENIED=30) while the
#    SDK data path (shm_create/attach) still works — examples themselves do
#    NOT depend on ubsectl. So a permission failure is a WARNING, not fatal:
#    cleanup will be counted as failed in the summary, but examples still run.
#    ubsectl reports errors by printing "ERROR: ..." while STILL exiting 0,
#    so we check the output content, not the exit code. Use a POSITIVE check
#    (table header present) rather than grep "ERROR" — the latter would
#    misfire on a shm whose name happens to contain "ERROR".
echo "Checking ubsectl on local node..."
if ! command -v ubsectl >/dev/null 2>&1; then
    echo "Error: ubsectl not found in PATH on local node."
    echo "cleanup_shms_ub.sh requires ubsectl to discover residual shms."
    exit 1
fi
if ! ubsectl display memory -t borrow_detail -bt share 2>&1 \
        | grep -q "borrow_node"; then
    echo "Warning: ubsectl on local node did not return a valid table"
    echo "  (likely UBS_ERR_PERMISSION_DENIED=30 under the documented RBAC roles)."
    echo "  cleanup_shms_ub.sh will fail; residual shms will accumulate and be"
    echo "  counted in the final summary. Examples themselves do not require"
    echo "  ubsectl permission and will still run. Continuing..."
fi

# 3. ubsectl on remote node (same positive header check as local)
echo "Checking ubsectl on remote node ($REMOTE_HOST)..."
if ! "${SSH_CMD[@]}" "command -v ubsectl >/dev/null 2>&1"; then
    echo "Warning: ubsectl not found in PATH on $REMOTE_HOST."
    echo "  Remote cleanup will fail; residual shms on $REMOTE_HOST will accumulate"
    echo "  and be counted in the final summary. Examples still run. Continuing..."
elif ! "${SSH_CMD[@]}" "ubsectl display memory -t borrow_detail -bt share 2>&1 \
        | grep -q borrow_node"; then
    echo "Warning: ubsectl on $REMOTE_HOST did not return a valid table"
    echo "  (likely permission denied). Remote cleanup will fail; residual shms"
    echo "  on $REMOTE_HOST will accumulate and be counted in the final summary."
    echo "  Examples themselves do not require ubsectl permission. Continuing..."
fi

# 4. uffd availability probe (required by fault_handler_example).
#    On hardened kernels (openEuler/RHEL default), vm.unprivileged_userfaultfd=0
#    makes userfaultfd(2) fail with EPERM for non-root users; the library then
#    degrades to a SIGSEGV handler that cannot discover+attach previously-unseen
#    segments. Root is never restricted by this sysctl. This carries the SKIP
#    decision basis for fault_handler_example below.
probe_uffd_local() {
    local v
    v=$(cat /proc/sys/vm/unprivileged_userfaultfd 2>/dev/null || echo "?")
    [ "$v" = "0" ] && [ "$EUID" -ne 0 ] && return 1
    return 0
}
probe_uffd_remote() {
    local v
    v=$("${SSH_CMD[@]}" "cat /proc/sys/vm/unprivileged_userfaultfd 2>/dev/null || echo ?")
    [ "$v" = "0" ] && [ "$EUID" -ne 0 ] && return 1
    return 0
}
echo "Checking userfaultfd availability..."
UFFD_OK_LOCAL=1
UFFD_OK_REMOTE=1
if ! probe_uffd_local; then
    UFFD_OK_LOCAL=0
    echo "  Warning: vm.unprivileged_userfaultfd=0 on local node (non-root) — uffd unavailable."
fi
if ! probe_uffd_remote; then
    UFFD_OK_REMOTE=0
    echo "  Warning: vm.unprivileged_userfaultfd=0 on $REMOTE_HOST (non-root) — uffd unavailable."
fi
if [ $UFFD_OK_LOCAL -eq 1 ] && [ $UFFD_OK_REMOTE -eq 1 ]; then
    echo "  uffd OK on both nodes."
    UFFD_AVAILABLE=1
else
    UFFD_AVAILABLE=0
    echo "  fault_handler_example will be SKIPped (requires uffd on both nodes)."
    echo "  Fix: sudo sysctl -w vm.unprivileged_userfaultfd=1 (or run as root)."
fi

# cleanup_shms_ub.sh may ssh from the remote node to OTHER peers reported by
# ubsectl; those must also be reachable via passwordless SSH from this node.
echo "Note: if cleanup_shms_ub.sh reports peers other than {$REMOTE_HOST,localhost},"
echo "      passwordless SSH from this node to those peers is also required."

echo "=== uballoc examples (UBSE backend, remote=$REMOTE_HOST, timeout=90s, verbose=$VERBOSE) ==="
echo ""

separator() {
    echo "────────────────────────────────────────────────────────────────"
}

# Clean up residual shms (local + remote). cleanup_shms_ub.sh exits non-zero
# (exit 2) when ubsectl does not return a valid table (e.g. permission denied),
# so failures are caught here and counted in CLEANUP_FAILURES for the final
# summary. Output is suppressed for readability; failures are non-fatal —
# examples do not depend on ubsectl permission.
CLEANUP_FAILURES=0
cleanup() {
    local heap_id="$1"
    local rc_local=0 rc_remote=0
    export UBALLOC_HEAP_ID="$heap_id"
    "$CLEANUP_SCRIPT" >/dev/null 2>&1 || rc_local=$?
    "${SSH_CMD[@]}" "UBALLOC_HEAP_ID=$heap_id bash -s" < "$CLEANUP_SCRIPT" >/dev/null 2>&1 || rc_remote=$?
    if [ $rc_local -ne 0 ] || [ $rc_remote -ne 0 ]; then
        CLEANUP_FAILURES=$((CLEANUP_FAILURES + 1))
    fi
    return 0
}

# Run a two-process example: P0 local, P1 remote via SSH.
# Args: <name> <heap_id> <binary> [extra_args...]
# P0: <binary> <extra_args> 0   |   P1: <binary> <extra_args> 1
# extra_args are %-quoted for the remote shell so spaces/metachars survive.
PASS=0
FAIL=0
SKIP=0
FAILED_EXAMPLES=""
SKIPPED_EXAMPLES=""

run_two_proc_example() {
    local name="$1"
    local heap_id="$2"
    shift 2
    local binary="$1"
    shift
    local -a extra_args=("$@")
    local remote_binary="${binary/$BUILD_DIR/$REMOTE_BUILD_DIR}"
    local -a quoted_args=()
    local a
    for a in "${extra_args[@]}"; do
        quoted_args+=("$(printf '%q' "$a")")
    done
    local remote_extra="${quoted_args[*]}"

    separator
    echo "[$name] starting..."
    cleanup "$heap_id"

    # P0 in background; LD_LIBRARY_PATH for parity with P1.
    if [ "$VERBOSE" -eq 1 ]; then
        timeout 90 env UBALLOC_HEAP_ID="$heap_id" UBALLOC_LOG_LEVEL=error \
            LD_LIBRARY_PATH="$BUILD_DIR" \
            "$binary" 0 "${extra_args[@]}" 2>&1 &
    else
        timeout 90 env UBALLOC_HEAP_ID="$heap_id" UBALLOC_LOG_LEVEL=error \
            LD_LIBRARY_PATH="$BUILD_DIR" \
            "$binary" 0 "${extra_args[@]}" >/dev/null 2>&1 &
    fi
    p0_pid=$!
    # Let P0 finish bootstrap_join + create_own_regions so P1 can attach.
    # (P1 retries discovery internally if 2s is too short under load.)
    sleep 2

    # P1 remote. LD_LIBRARY_PATH lets P1 find libuballoc.so.0 (RPATH is the
    # local build dir, not portable to different REMOTE_BUILD_DIR paths).
    if [ "$VERBOSE" -eq 1 ]; then
        timeout 90 "${SSH_CMD[@]}" \
            "UBALLOC_HEAP_ID=$heap_id UBALLOC_LOG_LEVEL=error LD_LIBRARY_PATH='$REMOTE_BUILD_DIR' '$remote_binary' 1 $remote_extra" 2>&1
    else
        timeout 90 "${SSH_CMD[@]}" \
            "UBALLOC_HEAP_ID=$heap_id UBALLOC_LOG_LEVEL=error LD_LIBRARY_PATH='$REMOTE_BUILD_DIR' '$remote_binary' 1 $remote_extra" >/dev/null 2>&1
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

skip_example() {
    local name="$1"
    local reason="$2"
    separator
    echo "[$name] SKIP: $reason"
    SKIP=$((SKIP + 1))
    SKIPPED_EXAMPLES="$SKIPPED_EXAMPLES $name"
}

# ─── Examples ───
run_two_proc_example "ub_example"                   "ubex"    "$BUILD_DIR/ub_example"
run_two_proc_example "distributed_example"         "distex"  "$BUILD_DIR/distributed_example"
run_two_proc_example "published_example"           "pubex"   "$BUILD_DIR/published_example"
run_two_proc_example "dynamic_discovery_example"   "dynex"   "$BUILD_DIR/dynamic_discovery_example"
run_two_proc_example "stl_full_shm_example"        "stlex"   "$BUILD_DIR/stl_full_shm_example"
run_two_proc_example "stl_data_only_example"       "stldoex" "$BUILD_DIR/stl_data_only_example"
# fault_handler_example hard-requires uffd (the SIGSEGV fallback cannot
# discover+attach previously-unseen segments — see example header).
if [ "$UFFD_AVAILABLE" -eq 1 ]; then
    run_two_proc_example "fault_handler_example"   "fhex"    "$BUILD_DIR/fault_handler_example"
else
    skip_example "fault_handler_example" \
        "uffd unavailable (vm.unprivileged_userfaultfd=0 on local or $REMOTE_HOST, non-root)"
fi
# uffd_example only validates setup/teardown + coexistence with eager attach;
# uffd never fires (the eager path attaches first), so it does NOT require uffd.
run_two_proc_example "uffd_example"                "uffdex"  "$BUILD_DIR/uffd_example"
run_two_proc_example "reclaim_example (sync)"      "reclaim" "$BUILD_DIR/reclaim_example" sync
run_two_proc_example "reclaim_example (async)"     "reclaim" "$BUILD_DIR/reclaim_example" async
run_two_proc_example "cross_node_2gb_example"      "cross2g" "$BUILD_DIR/cross_node_2gb_example"

# ─── Summary ───
separator
echo ""
echo "=== Summary: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ $SKIP -gt 0 ] && echo "Skipped:$SKIPPED_EXAMPLES"
[ $FAIL -gt 0 ] && echo "Failed:$FAILED_EXAMPLES"
if [ $CLEANUP_FAILURES -gt 0 ]; then
    echo "Cleanup failures: $CLEANUP_FAILURES (residual shms may have accumulated;"
    echo "  check 'ubsectl display memory -t borrow_detail -bt share' on both nodes)."
fi
if [ $FAIL -gt 0 ]; then
    exit 1
fi
echo "All non-skipped examples passed!"
exit 0
