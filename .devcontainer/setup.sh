#!/bin/sh
set -eu

WORKSPACE="${WORKSPACE:-$(pwd)}"

echo "=== ZMK workspace setup ==="
echo "Workspace: $WORKSPACE"
cd "$WORKSPACE"

if [ ! -d .west ]; then
    echo "--- west init ---"
    west init -l config
else
    echo "--- west already initialized, skipping init ---"
fi

# The dongle modules are in separate groups in west.yml and excluded from this
# initial update — fetched on demand by build.sh to avoid Kconfig conflicts.
echo "--- Disabling dongle modules for initial setup (fetched on demand by build.sh) ---"
west config manifest.group-filter -- -dongle

echo "--- west update ---"
west update

echo "--- west zephyr-export ---"
west zephyr-export

echo "=== Setup complete ==="
