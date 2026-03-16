#!/bin/bash
# Build all Kiwi36 firmware variants.
# Run this script inside the ZMK devcontainer from any directory.

set -e

ZMK_APP_PATH="$(west list zmk -f '{abspath}')/app"
CONFIG_PATH="$(west list config -f '{abspath}')"
MODULES_PATH="$(dirname "$CONFIG_PATH")"
YADS_MODULE_PATH="$(west list zmk-dongle-screen -f '{abspath}')"
OUTPUT_DIR="$MODULES_PATH/output"
BOARD="nice_nano//zmk"

mkdir -p "$OUTPUT_DIR"
rm -f "$OUTPUT_DIR"/*.uf2

# Build a split half with Nice!View
build_shield() {
    local shield=$1 side=$2
    local build_name="${shield}_${side}"
    echo "--- Building: $build_name ---"
    west build -p -s "$ZMK_APP_PATH" -d "build/$build_name" -b "$BOARD" -- \
        -DSHIELD="${shield}_${side} nice_view_adapter nice_view" \
        -DZMK_CONFIG="$CONFIG_PATH"
    cp "build/$build_name/zephyr/zmk.uf2" "$OUTPUT_DIR/${build_name}.uf2"
}

# Build a split half as dongle-mode peripheral (no central role)
build_shield_with_dongle() {
    local shield=$1 side=$2
    local build_name="${shield}_${side}_dongle"
    echo "--- Building: $build_name ---"
    west build -p -s "$ZMK_APP_PATH" -d "build/$build_name" -b "$BOARD" -- \
        -DSHIELD="${shield}_${side} nice_view_adapter nice_view" \
        -DZMK_CONFIG="$CONFIG_PATH" \
        -DCONFIG_ZMK_SPLIT=y \
        -DCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n
    cp "build/$build_name/zephyr/zmk.uf2" "$OUTPUT_DIR/${build_name}.uf2"
}

# Sync zmk-dongle-screen to the manifest revision before dongle builds.
# Excluded from the initial "west update" in setup.sh — see setup.sh for context.
ensure_dongle_module() {
    echo "--- Syncing dongle module ---"
    west config manifest.group-filter -- +dongle
    west update zmk-dongle-screen
}

# Build USB dongle with YADS screen
build_dongle_yads() {
    local shield=$1
    local build_name="${shield}_yads_dongle"
    echo "--- Building: $build_name ---"
    west build -p -s "$ZMK_APP_PATH" -d "build/$build_name" -b "$BOARD" -- \
        -DSHIELD="${shield}_dongle_yads dongle_screen" \
        -DZMK_CONFIG="$CONFIG_PATH" \
        -DZMK_EXTRA_MODULES="$YADS_MODULE_PATH"
    cp "build/$build_name/zephyr/zmk.uf2" "$OUTPUT_DIR/${build_name}.uf2"
}

# Build settings reset firmware for split halves
build_reset() {
    echo "--- Building: settings_reset ---"
    west build -p -s "$ZMK_APP_PATH" -d "build/reset" -b "$BOARD" -- \
        -DSHIELD="settings_reset" \
        -DZMK_CONFIG="$CONFIG_PATH"
    cp "build/reset/zephyr/zmk.uf2" "$OUTPUT_DIR/settings_reset.uf2"
}

# Build settings reset firmware for dongle
build_reset_dongle() {
    echo "--- Building: settings_reset_dongle ---"
    west build -p -s "$ZMK_APP_PATH" -d "build/reset_dongle" -b "$BOARD" -- \
        -DSHIELD="settings_reset" \
        -DZMK_CONFIG="$CONFIG_PATH"
    cp "build/reset_dongle/zephyr/zmk.uf2" "$OUTPUT_DIR/settings_reset_dongle.uf2"
}

#build_shield "kiwi36" "left"
#build_shield "kiwi36" "right"
#build_shield_with_dongle "kiwi36" "left"
#build_shield_with_dongle "kiwi36" "right"
ensure_dongle_module
build_dongle_yads "kiwi36"
#build_reset
#build_reset_dongle

echo ""
echo "Done! Output files:"
ls -lh "$OUTPUT_DIR"
