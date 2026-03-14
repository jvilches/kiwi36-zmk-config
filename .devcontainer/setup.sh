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

echo "--- west update ---"
west update

echo "=== Setup complete ==="
