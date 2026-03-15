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

# west update can fail intermittently on the first run with fresh Docker volumes:
#
# 1. inflate: data stream error — the zmkfirmware/zephyr shallow clone (~200 MB pack)
#    occasionally gets a corrupted packet over the network. west then aborts and
#    reports all subsequent unprocessed projects as failed (misleading error messages).
#
# 2. rm: cannot remove '.../nanopb/spm_headers/nanopb': Directory not empty — the
#    zmkfirmware/nanopb repo has git-tracked symlinks in spm_headers/nanopb/; a
#    subsequent module's processing calls rm on that directory while it is occupied.
#
# Both failures are intermittent and only occur on initial setup with empty volumes.
# Retrying is sufficient: partial progress is preserved between attempts, so each
# retry only needs to re-fetch what actually failed.
#
# Additionally: the dongle modules (prospector-zmk-module, zmk-dongle-screen) are
# tagged with groups: [dongle] in west.yml and are excluded from this initial update.
# They are fetched on demand by build.sh. This reduces the surface area for the
# above failures and keeps the initial setup faster.
echo "--- Disabling dongle modules for initial setup (fetched on demand by build.sh) ---"
west config manifest.group-filter -- -dongle

echo "--- west update (with retry) ---"
i=0
while [ $i -lt 3 ]; do
    i=$((i + 1))
    if west update; then
        break
    fi
    if [ $i -lt 3 ]; then
        echo "--- west update failed (attempt $i/3), retrying in 10s... ---"
        sleep 10
    else
        echo "--- west update failed after 3 attempts ---"
        exit 1
    fi
done

echo "--- west zephyr-export ---"
west zephyr-export

echo "=== Setup complete ==="
