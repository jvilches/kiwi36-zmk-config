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

echo "--- Disabling optional dongle modules (fetched on demand by build.sh) ---"
west config manifest.group-filter -- -dongle

echo "--- west update ---"
west update

echo "--- west zephyr-export ---"
west zephyr-export

echo "=== Setup complete ==="
