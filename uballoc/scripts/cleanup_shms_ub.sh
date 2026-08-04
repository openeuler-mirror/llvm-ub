#!/bin/bash
# Clean up UBSE shm objects for a given heap_id after a crash (UBShmProvider).
#
# Discovery-based: queries `ubsectl display memory -t borrow_detail -bt share`
# ONCE on the local node (the UBSE daemon returns the cluster-wide view), then
# parses the table to find every existing shm whose name starts with
# `ub-${UBALLOC_HEAP_ID}-`.  For each shm it extracts the borrower (attacher) and
# lender (creator) hostnames directly from the table — no need for the user
# to pass node IPs or process count.
#
# Two-phase cleanup (matches the original teardown ordering constraint):
#   Phase 1 — detach:  every borrower node detaches from the shm
#       ubsectl detach memory -n <shm_name>
#   * implicit barrier *
#       Each `ssh host cmd` is synchronous, so all Phase 1 detaches finish
#       before any Phase 2 delete begins.
#   Phase 2 — delete:  the lender node deletes the shm it created
#       ubsectl delete memory -t share -n <shm_name>
#
# Shm naming (must match UBShmProvider::shm_name in
# include/uballoc/ubshm_provider.hpp):
#   Per-process regions: ub-<heap_id>-p<rank>-{m,ds,dl,dh}[-s<seg_idx>]
#   Bootstrap shm:       ub-<heap_id>-bootstrap   (created by rank 0, attached by all)
#
# Usage:
#   ./cleanup_shms_ub.sh                          # default UBALLOC_HEAP_ID=heap
#   UBALLOC_HEAP_ID=myheap ./cleanup_shms_ub.sh   # override heap_id
#
# Requires:
#   - ubsectl in PATH on the local node and every peer node
#   - passwordless SSH from this node to every peer hostname reported by ubsectl
#     (ubsectl reports cluster hostnames; configure ~/.ssh/config or /etc/hosts
#     to make them resolvable)
#
# Failures (unreachable node, shm already deleted, ssh timeout) are silently
# skipped with 2>/dev/null, since a crashed process may not have attached to
# every shm.

set -uo pipefail

UBALLOC_HEAP_ID="${UBALLOC_HEAP_ID:-heap}"
PREFIX="ub-${UBALLOC_HEAP_ID}-"

# ---------- detect local node identity ----------
# ubsectl reports cluster hostnames in the borrow_node/lend_node columns.
# If one of those hostnames happens to be THIS node, we must run ubsectl
# locally instead of SSHing to ourselves (which would prompt for a password
# if passwordless-loopback SSH isn't configured).
declare -A LOCAL_HOST_SET=()
LOCAL_HOST_SET["local"]=1
LOCAL_HOST_SET["-"]=1
LOCAL_HOST_SET["localhost"]=1
LOCAL_HOST_SET["127.0.0.1"]=1
LOCAL_HOST_SET["::1"]=1
_short=$(hostname 2>/dev/null || true)
_fqdn=$(timeout 5 hostname -f 2>/dev/null || true)
[ -n "$_short" ] && LOCAL_HOST_SET["$_short"]=1
[ -n "$_fqdn" ] && [ "$_fqdn" != "$_short" ] && LOCAL_HOST_SET["$_fqdn"]=1
# hostname -I prints a space-separated list of all local IPs
for _ip in $(hostname -I 2>/dev/null); do
    [ -n "$_ip" ] && LOCAL_HOST_SET["$_ip"]=1
done
unset _short _fqdn _ip

# ---------- helpers ----------

# is_local_host HOST → 0 if HOST is the launching node (or empty/localhost),
# 1 otherwise
is_local_host() {
    local h="$1"
    [ -z "$h" ] && return 0
    [ -n "${LOCAL_HOST_SET[$h]+x}" ]
}

# run_on_host HOST CMD [ARGS...]
# If HOST is the local node → run locally. Otherwise ssh -n (so the script's
# stdin is not consumed by ssh).
run_on_host() {
    local host="$1"
    shift
    if is_local_host "$host"; then
        "$@"
    else
        ssh -n "$host" "$(printf '%q ' "$@")"
    fi
}

# strip_slot "node01(0)" → "node01"   (hostname without the (slot_id) suffix)
#             "-(0)"      → ""         (no hostname available; treat as local)
#             ""          → ""
strip_slot() {
    local s="$1"
    s="${s%%(*}"          # drop "(...)" suffix
    s="${s%"${s##*[![:space:]]}"}"  # trim trailing whitespace
    if [ -z "$s" ] || [ "$s" = "-" ]; then
        echo ""
    else
        echo "$s"
    fi
}

# ---------- phase 0: discover existing shms from ubsectl ----------

echo "=== Phase 0: discover shms matching prefix '${PREFIX}' ==="
echo "  querying ubsectl display memory -t borrow_detail -bt share ..."

RAW_OUTPUT=$(ubsectl display memory -t borrow_detail -bt share 2>/dev/null)
if [ -z "$RAW_OUTPUT" ]; then
    echo "  ubsectl returned no output (no shms in cluster, or ubsectl unavailable)."
    echo "  Nothing to clean up."
    exit 0
fi

# Parse the table.  We mimic UBSE's own UbseCliTableParser
# (test/IT/infra/client/it_cli_table_parser.h):
#   1. Find the header row (contains "borrow_node" and "lend_node").
#   2. Record each column's start offset from the header token positions.
#   3. For each subsequent data line, slice the line at column boundaries to
#      extract cell values.  Skip lines where the name column is empty
#      (continuation lines for wrapped values like the handle UUID).
# Output: one line per shm, formatted as  name|borrow_node|lend_node
RECORDS=$(awk -v prefix="$PREFIX" '
    # Skip dash-separator lines (all dashes after trim)
    /^[[:space:]]*-+[[:space:]]*$/ { next }

    # Detect header: must contain both borrow_node and lend_node, and must
    # not start with whitespace-only (headers are non-empty tokens).
    !found_header && /borrow_node/ && /lend_node/ {
        line = $0
        pos = 1
        n = 0
        # Walk the line, extracting token positions (skip blanks, scan token)
        while (pos <= length(line)) {
            while (pos <= length(line) && (substr(line, pos, 1) == " " || substr(line, pos, 1) == "\t")) pos++
            if (pos > length(line)) break
            tok_start = pos
            while (pos <= length(line) && substr(line, pos, 1) != " " && substr(line, pos, 1) != "\t") pos++
            n++
            col_name[n] = substr(line, tok_start, pos - tok_start)
            col_start[n] = tok_start
        }
        found_header = 1
        next
    }

    # No header yet — skip (noise lines, [INFO]/[ERROR]/etc.)
    !found_header { next }

    # Data row: slice at column boundaries
    {
        name = ""; borrow = ""; lend = ""
        for (i = 1; i <= n; i++) {
            s = col_start[i]
            if (i < n) { e = col_start[i+1] - 1 } else { e = length($0) }
            if (s > length($0)) continue
            cell = substr($0, s, e - s + 1)
            # Trim leading/trailing whitespace
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", cell)
            if      (col_name[i] == "name")        name = cell
            else if (col_name[i] == "borrow_node")  borrow = cell
            else if (col_name[i] == "lend_node")    lend = cell
        }
        # Skip continuation lines (wrapped handle values, etc.) where name is empty
        if (name == "") next
        # Filter by our heap_id prefix
        if (index(name, prefix) != 1) next
        print name "|" borrow "|" lend
    }
' <<< "$RAW_OUTPUT")

if [ -z "$RECORDS" ]; then
    echo "  No shms matching prefix '${PREFIX}' found in the cluster."
    echo "  Nothing to clean up."
    # Still show what's in the cluster for debugging
    echo
    echo "--- remaining ub- shms in cluster (any heap_id) ---"
    echo "$RAW_OUTPUT" | grep "ub-" || echo "(none)"
    exit 0
fi

echo "  Found $(echo "$RECORDS" | wc -l) record(s):"
echo "$RECORDS" | awk -F'|' '{
    bn = $2 ? $2 : "(unattached)"
    printf "    %-40s  borrower=%-20s  lender=%s\n", $1, bn, $3
}'
echo

# ---------- group by shm name ----------

# Build two associative arrays:
#   LENDER[name]   = lender hostname (a shm has exactly one lender)
#   BORROWERS[name]= comma-separated list of borrower hostnames (dedup)
declare -A LENDER BORROWERS
while IFS='|' read -r name borrow lend; do
    [ -z "$name" ] && continue
    borrow_h=$(strip_slot "$borrow")
    lend_h=$(strip_slot "$lend")
    if [ -z "${LENDER[$name]+x}" ]; then
        LENDER[$name]="$lend_h"
        BORROWERS[$name]="$borrow_h"
    else
        # Append borrower if not already present
        local_borrowers="${BORROWERS[$name]}"
        if [ -n "$borrow_h" ] && [[ ",$local_borrowers," != *",$borrow_h,"* ]]; then
            BORROWERS[$name]="${local_borrowers:+$local_borrowers,}$borrow_h"
        fi
    fi
done <<< "$RECORDS"

NAMES=("${!LENDER[@]}")

# ---------- phase 1: detach on every borrower node ----------

detached=0
echo "=== Phase 1: detach (each borrower node detaches from the shm) ==="
for name in "${NAMES[@]}"; do
    borrowers="${BORROWERS[$name]}"
    if [ -z "$borrowers" ]; then
        echo "  ${name}: no borrowers (unattached shm) — skipping detach"
        continue
    fi
    IFS=',' read -ra borrower_list <<< "$borrowers"
    for host in "${borrower_list[@]}"; do
        printf "  %-40s  " "${name} ← ${host:-local}"
        if run_on_host "$host" ubsectl detach memory -n "$name" 2>/dev/null; then
            echo "detached OK"
            detached=$((detached + 1))
        else
            echo "detach failed/skipped"
        fi
    done
done
echo "Phase 1 complete: ${detached} detach operation(s) succeeded."
echo

# ---------- * implicit barrier * ----------
# All ssh calls above are synchronous, so every Phase 1 detach has finished
# before any Phase 2 delete below begins.  Deletes therefore cannot race
# with outstanding detaches on remote nodes.

# ---------- phase 2: delete on the lender node ----------

deleted=0
echo "=== Phase 2: delete (lender node deletes the shm it created) ==="
for name in "${NAMES[@]}"; do
    host="${LENDER[$name]}"
    printf "  %-40s  " "${name} @ ${host:-local}"
    if run_on_host "$host" ubsectl delete memory -t share -n "$name" 2>/dev/null; then
        echo "deleted OK"
        deleted=$((deleted + 1))
    else
        echo "delete failed/skipped"
    fi
done
echo "Phase 2 complete: ${deleted} delete operation(s) succeeded."
echo

# ---------- summary ----------

echo "=== Summary: ${detached} detached, ${deleted} deleted ==="
echo
echo "--- verification: remaining ${PREFIX} shms (should be empty) ---"
ubsectl display memory -t borrow_detail -bt share 2>/dev/null | grep "${PREFIX}" || echo "(none)"
