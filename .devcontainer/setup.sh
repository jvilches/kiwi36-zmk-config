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

# The dongle modules (prospector-zmk-module, zmk-dongle-screen) are tagged with
# groups: [dongle] in west.yml and are excluded from this initial update.
# They are fetched on demand by build.sh.
echo "--- Disabling dongle modules for initial setup (fetched on demand by build.sh) ---"
west config manifest.group-filter -- -dongle

echo "--- west update ---"
west update

echo "--- west zephyr-export ---"
west zephyr-export

echo "=== Setup complete ==="
