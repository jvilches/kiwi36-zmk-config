# Kiwi36 ZMK Firmware

ZMK firmware configuration for the [Kiwi36](https://github.com/klouderone/zmk-config-kiwi36split) — a 36-key wireless split keyboard.

## Hardware

| Part | Component |
|---|---|
| Controllers | 2× Nice!Nano v2 |
| Display | Nice!View (each half) |
| Optional dongle | Nice!Nano v2 (USB central) |

## Firmware variants

| File | Purpose |
|---|---|
| `kiwi36_left.uf2` | Left half (acts as BLE central when used without dongle) |
| `kiwi36_right.uf2` | Right half |
| `kiwi36_left_dongle.uf2` | Left half as pure BLE peripheral (for use with dongle) |
| `kiwi36_right_dongle.uf2` | Right half as pure BLE peripheral (for use with dongle) |
| `kiwi36_prospector_dongle.uf2` | USB dongle, no screen |
| `kiwi36_yads_dongle.uf2` | USB dongle with YADS screen |
| `settings_reset.uf2` | Reset BLE bonds on a split half |
| `settings_reset_dongle.uf2` | Reset BLE bonds on the dongle |

## Building

### Prerequisites

- [Docker](https://www.docker.com/products/docker-desktop)
- [VSCode](https://code.visualstudio.com) with the [Dev Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) extension

### Dependencies

All dependencies are declared in `config/west.yml` and fetched automatically by `west update`. You do not need to clone anything manually.

| Dependency | Repository | Version |
|---|---|---|
| ZMK firmware | [zmkfirmware/zmk](https://github.com/zmkfirmware/zmk) | `v3.5.0+zmk-fixes` |
| prospector-zmk-module | [carrefinho/prospector-zmk-module](https://github.com/carrefinho/prospector-zmk-module) | `main` |
| zmk-dongle-screen | [janpfischer/zmk-dongle-screen](https://github.com/janpfischer/zmk-dongle-screen) | `main` |

After `west update`, the modules are cloned into the repo root alongside the `config/` directory:
```
zmk-config/
├── config/               ← this repo
├── zmk/                  ← fetched by west
├── prospector-zmk-module/← fetched by west
└── zmk-dongle-screen/    ← fetched by west
```

### First-time setup

1. Clone this repo and open it in VSCode:
   ```bash
   git clone <repo-url>
   code zmk-config
   ```

2. VSCode will detect `.devcontainer/devcontainer.json` and prompt **"Reopen in Container"** — click it.

3. The container starts and runs `west init && west update` automatically. This fetches ZMK, the two modules, and all Zephyr toolchain dependencies into Docker volumes. **This only happens once** — subsequent opens reuse the cached volumes.

### Build all variants

Inside the devcontainer terminal:

```bash
bash build.sh
```

Output `.uf2` files are written to `output/`.

### Build a single variant manually

```bash
# Left half
west build -p -s /workspaces/zmk/app -d build/kiwi36_left -b nice_nano_v2 -- \
  -DSHIELD="kiwi36_left nice_view_adapter nice_view" \
  -DZMK_CONFIG="$ZMK_CONFIG" \
  -DZMK_EXTRA_MODULES="/workspaces/zmk-config"

# USB dongle (no screen)
west build -p -s /workspaces/zmk/app -d build/kiwi36_dongle -b nice_nano_v2 -- \
  -DSHIELD="kiwi36_dongle" \
  -DZMK_EXTRA_MODULES="/workspaces/zmk-config;/workspaces/zmk-config/prospector-zmk-module"
```

### Flashing

Put the Nice!Nano into bootloader mode by double-tapping the reset button. It mounts as a USB drive — drag the `.uf2` file onto it.

## Keymap

7 layers accessed via thumb cluster hold-taps:

| Layer | Access | Purpose |
|---|---|---|
| BASE | — | QWERTY with home row mods |
| NAV | Left thumb middle | Arrows, editing, undo/redo |
| NUM | Right thumb middle | Numpad, brackets |
| SYM | Right thumb inner | Symbols |
| MED | Left thumb inner | Media, Bluetooth |
| FUN | Either thumb outer | F-keys, layer switches |
| TAP | FUN → TAP | Gaming — plain keys, no mods |

Home row mods (left: GUI/ALT/CTL/SFT, right: SFT/CTL/ALT/GUI) use a 180 ms tapping term with balanced flavor.

## Modules

Both modules are required to build the dongle variants. They are fetched automatically via `west update` — see [Dependencies](#dependencies) above.

| Module | Used for |
|---|---|
| [prospector-zmk-module](https://github.com/carrefinho/prospector-zmk-module) | Ambient light sensor support on the dongle |
| [zmk-dongle-screen](https://github.com/janpfischer/zmk-dongle-screen) | YADS dongle screen support |
