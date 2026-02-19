#!/usr/bin/env bash
# run_instances.sh — build dui and launch N isolated instances in parallel.
# Usage: ./run_instances.sh [N]   (default N=2)
set -euo pipefail

N=${1:-2}
if ! [[ "$N" =~ ^[1-9][0-9]*$ ]]; then
    echo "Usage: $0 [N]  (N must be a positive integer)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# ---- build ----------------------------------------------------------------
echo "==> Configuring..."
cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR" -DDUI_PLATFORM=desktop -DCMAKE_BUILD_TYPE=Release

echo "==> Building..."
cmake --build "$BUILD_DIR" --parallel

BINARY="$BUILD_DIR/dui"

# ---- instance management --------------------------------------------------
INSTANCE_DIRS=()
PIDS=()

cleanup() {
    if [[ ${#INSTANCE_DIRS[@]} -gt 0 ]]; then
        echo ""
        echo "==> Removing ${#INSTANCE_DIRS[@]} instance director$(
            [[ ${#INSTANCE_DIRS[@]} -eq 1 ]] && echo "y" || echo "ies"
        )..."
        rm -rf "${INSTANCE_DIRS[@]}"
    fi
}
trap cleanup EXIT

# ---- launch instances -----------------------------------------------------
for i in $(seq 1 "$N"); do
    dir=$(mktemp -d "/tmp/dui_instance_XXXXXX")
    INSTANCE_DIRS+=("$dir")
    mkdir -p "$dir/chats"
    # Must be a copy, not a symlink: readlink(/proc/self/exe) would resolve
    # a symlink back to the original path, making basedir point at build/.
    cp "$BINARY" "$dir/dui"
    echo "==> Instance $i -> $dir"
    # Each instance runs in its own directory (isolated messages.db and
    # registry.db). Peer discovery is handled automatically by the multicast
    # discovery thread in sync.c — no cross-seeding needed.
    (cd "$dir" && DCOMMS_HOST=127.0.0.1 exec "$dir/dui") &
    PIDS+=($!)
done

echo ""
echo "==> $N instance(s) running (PIDs: ${PIDS[*]}). Close all windows to continue."
echo ""

for pid in "${PIDS[@]}"; do
    wait "$pid" || true
done

echo "==> All instances closed."
