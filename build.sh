#!/bin/bash
# Build all Kiwi36 firmware variants.
# Run this script inside the ZMK devcontainer from any directory.

set -e

ZMK_APP_PATH="$(west list zmk -f '{abspath}')/app"
CONFIG_PATH="$(west list config -f '{abspath}')"
MODULES_PATH="$(dirname "$CONFIG_PATH")"
PROSPECTOR_MODULE_PATH="$(west list prospector-zmk-module -f '{abspath}')"
YADS_MODULE_PATH="$(west list zmk-dongle-screen -f '{abspath}')"
OUTPUT_DIR="$MODULES_PATH/output"
BOARD="nice_nano_v2"
DONGLE_BOARD="nice_nano_v2"

mkdir -p "$OUTPUT_DIR"
rm -f "$OUTPUT_DIR"/*.uf2

# Build a split half with Nice!View
build_shield() {
    local shield=$1 side=$2
    local build_name="${shield}_${side}"
    echo "--- Building: $build_name ---"
    west build -p -s "$ZMK_APP_PATH" -d "build/$build_name" -b "$BOARD" -- \
        -DSHIELD="${shield}_${side} nice_view_adapter nice_view" \
        -DZMK_CONFIG="$CONFIG_PATH" \
        -DZMK_EXTRA_MODULES="$MODULES_PATH"
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
        -DZMK_EXTRA_MODULES="$MODULES_PATH" \
        -DCONFIG_ZMK_SPLIT=y \
        -DCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n
    cp "build/$build_name/zephyr/zmk.uf2" "$OUTPUT_DIR/${build_name}.uf2"
}

# Enable the dongle west group and fetch the optional modules if not yet present
ensure_dongle_modules() {
    if [ -d "$PROSPECTOR_MODULE_PATH" ] && [ -d "$YADS_MODULE_PATH" ]; then
        return 0
    fi
    echo "--- Fetching dongle modules ---"
    west config manifest.group-filter -- +dongle
    west update prospector-zmk-module zmk-dongle-screen
}

# Build USB dongle (no screen, uses prospector module)
build_dongle() {
    local shield=$1
    local build_name="${shield}_prospector_dongle"
    echo "--- Building: $build_name ---"
    west build -p -s "$ZMK_APP_PATH" -d "build/$build_name" -b "$DONGLE_BOARD" -- \
        -DSHIELD="${shield}_dongle" \
        -DZMK_EXTRA_MODULES="$MODULES_PATH;$PROSPECTOR_MODULE_PATH"
    cp "build/$build_name/zephyr/zmk.uf2" "$OUTPUT_DIR/${build_name}.uf2"
}

# Build USB dongle with YADS screen
build_dongle_yads() {
    local shield=$1
    local build_name="${shield}_yads_dongle"
    echo "--- Building: $build_name ---"
    west build -p -s "$ZMK_APP_PATH" -d "build/$build_name" -b "$DONGLE_BOARD" -- \
        -DSHIELD="${shield}_dongle_yads dongle_screen" \
        -DZMK_EXTRA_MODULES="$MODULES_PATH;$YADS_MODULE_PATH"
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
    west build -p -s "$ZMK_APP_PATH" -d "build/reset_dongle" -b "$DONGLE_BOARD" -- \
        -DSHIELD="settings_reset" \
        -DZMK_CONFIG="$CONFIG_PATH"
    cp "build/reset_dongle/zephyr/zmk.uf2" "$OUTPUT_DIR/settings_reset_dongle.uf2"
}

build_shield "kiwi36" "left"
build_shield "kiwi36" "right"
build_shield_with_dongle "kiwi36" "left"
build_shield_with_dongle "kiwi36" "right"
ensure_dongle_modules
build_dongle "kiwi36"
build_dongle_yads "kiwi36"
build_reset
build_reset_dongle

echo ""
echo "Done! Output files:"
ls -lh "$OUTPUT_DIR"
