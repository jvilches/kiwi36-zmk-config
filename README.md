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

All dependencies are declared in `config/west.yml` and fetched automatically. You do not need to clone anything manually.

| Dependency | Repository | Version |
|---|---|---|
| ZMK firmware | [zmkfirmware/zmk](https://github.com/zmkfirmware/zmk) | `v0.3.0` |
| prospector-zmk-module | [carrefinho/prospector-zmk-module](https://github.com/carrefinho/prospector-zmk-module) | `main` (dongle only) |
| zmk-dongle-screen | [janpfischer/zmk-dongle-screen](https://github.com/janpfischer/zmk-dongle-screen) | `main` (dongle only) |

#### Why v0.3.0 and not ZMK main?

ZMK `v0.3.0` is the latest stable release and internally uses **Zephyr 3.5** (`v3.5.0+zmk-fixes`). After this release, ZMK `main` migrated to Zephyr 4.1 — and both dongle modules (`prospector-zmk-module` and `zmk-dongle-screen`) have `main` branches that target Zephyr 3.5 only.

Using ZMK `main` would require switching both modules to their Zephyr 4.1-compatible branches (`core/zephyr-4.1` and `upgrade-4.1` respectively), which are still work-in-progress. Until those branches stabilise and merge to `main`, `v0.3.0` is the correct anchor point.

The devcontainer image `zmkfirmware/zmk-dev-arm:3.5-branch` ships the matching Zephyr 3.5 SDK.

The two dongle modules are in the `dongle` west group and are **not** fetched during initial setup — `build.sh` fetches them automatically the first time a dongle variant is built.

### Repository layout

```
zmk-config/
├── config/
│   ├── boards/shields/kiwi36/   ← shield files (keymaps, overlays, Kconfig)
│   ├── kiwi36.conf               ← base config for split halves
│   └── west.yml                  ← west manifest
├── build.sh                      ← build script
├── .devcontainer/
│   ├── devcontainer.json
│   └── setup.sh                  ← runs west init + west update on first start
├── zmk/                          ← fetched by west (gitignored)
├── zephyr/                       ← Docker volume (see below)
├── modules/                      ← Docker volume (see below)
└── tools/                        ← Docker volume (see below)
```

### Dev container internals

The devcontainer uses a mix of a bind mount and named Docker volumes:

| Path | How mounted | Contents |
|---|---|---|
| `/workspaces/zmk-config/` | bind mount | your repo, editable on the host |
| `.../zephyr/` | Docker volume `zmk-zephyr` | Zephyr RTOS source (fetched by west) |
| `.../modules/` | Docker volume `zmk-zephyr-modules` | Zephyr modules (fetched by west) |
| `.../tools/` | Docker volume `zmk-zephyr-tools` | Zephyr toolchain helpers |

The named volumes survive container rebuilds so Zephyr is only downloaded once. The bind mount means all files under `config/` are directly editable from the host.

**Important:** The Docker volumes completely shadow the `zephyr/`, `modules/`, and `tools/` directories inside the container. Never put your own files in those directories — they will be invisible to the build system. All user config belongs in `config/` (or the repo root).

### First-time setup

1. Clone this repo and open it in VSCode:
   ```bash
   git clone <repo-url>
   code zmk-config
   ```

2. VSCode will detect `.devcontainer/devcontainer.json` and prompt **"Reopen in Container"** — click it.

3. The container starts and runs `.devcontainer/setup.sh` automatically, which:
   - Runs `west init -l config` (idempotent — skipped if already initialised)
   - Disables the `dongle` west group so dongle modules are not fetched yet
   - Runs `west update` to fetch ZMK and Zephyr into the named volumes
   - Runs `west zephyr-export` to register Zephyr in the CMake package registry

   **This only happens once** — subsequent opens reuse the cached volumes.

### Build all variants

Inside the devcontainer terminal:

```bash
bash build.sh
```

Output `.uf2` files are written to `output/`. The first run also fetches the dongle modules (`prospector-zmk-module`, `zmk-dongle-screen`).

### Build a single variant manually

```bash
ZMK=$(west list zmk -f '{abspath}')
CONFIG=$(west list config -f '{abspath}')

# Left half with Nice!View
west build -p -s "$ZMK/app" -d build/kiwi36_left -b nice_nano_v2 -- \
  -DSHIELD="kiwi36_left nice_view_adapter nice_view" \
  -DZMK_CONFIG="$CONFIG"

# USB dongle (no screen) — requires dongle group enabled first
west config manifest.group-filter -- +dongle
west update prospector-zmk-module zmk-dongle-screen
PROSPECTOR=$(west list prospector-zmk-module -f '{abspath}')
west build -p -s "$ZMK/app" -d build/kiwi36_dongle -b nice_nano_v2 -- \
  -DSHIELD="kiwi36_dongle" \
  -DZMK_CONFIG="$CONFIG" \
  -DZMK_EXTRA_MODULES="$PROSPECTOR"
```

### Flashing

Put the Nice!Nano into bootloader mode by double-tapping the reset button. It mounts as a USB drive — drag the `.uf2` file onto it.

## Keymap

> **Legend:** `·` = tap · hold &nbsp;|&nbsp; `►` = to-layer (permanent switch) &nbsp;|&nbsp; `BOOT` = double-tap reset &nbsp;|&nbsp; `CAP` = Caps Word

### BASE — QWERTY + home row mods

```
╭─────┬─────┬─────┬─────┬─────╮   ╭─────┬─────┬─────┬─────┬─────╮
│  Q  │  W  │  E  │  R  │  T  │   │  Y  │  U  │  I  │  O  │  P  │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│A·GUI│S·ALT│D·CTL│F·SFT│  G  │   │  H  │J·SFT│K·CTL│L·ALT│'·GUI│
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│  Z  │  X  │  C  │  V  │  B  │   │  N  │  M  │  ,  │  .  │  /  │
╰─────┴─────┴──┬──┴──┬──┴──┬──╯   ╰──┬──┴──┬──┴──┬──┴─────┴─────╯
               │ ESC │ SPC │ TAB │   │ RET │ BSP │ DEL │
               │ MED │ NAV │ FUN │   │ SYM │ NUM │ FUN │
               ╰─────┴─────┴─────╯   ╰─────┴─────┴─────╯
```

### FUN — Function keys

```
╭─────┬─────┬─────┬─────┬─────╮   ╭─────┬─────┬─────┬─────┬─────╮
│ F12 │  F7 │  F8 │  F9 │ PSC │   │     │►BASE│►TAP │     │BOOT │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│ F11 │  F4 │  F5 │  F6 │ SLC │   │     │ SFT │ CTL │ ALT │ GUI │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│ F10 │  F1 │  F2 │  F3 │ PAU │   │     │►FUN │►MED │ ALT │     │
╰─────┴─────┴──┬──┴──┬──┴──┬──╯   ╰──┬──┴──┬──┴──┬──┴─────┴─────╯
               │ APP │ SPC │ TAB │   │     │     │     │
               ╰─────┴─────┴─────╯   ╰─────┴─────┴─────╯
```

### NAV — Navigation

```
╭─────┬─────┬─────┬─────┬─────╮   ╭─────┬─────┬─────┬─────┬─────╮
│BOOT │►TAP │     │►BASE│     │   │ RDO │ PST │ CPY │ CUT │ UND │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│ GUI │ ALT │ CTL │ SFT │     │   │ CAP │  ←  │  ↓  │  ↑  │  →  │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│     │ ALT │►NUM │►NAV │     │   │ INS │ HOM │ PDN │ PUP │ END │
╰─────┴─────┴──┬──┴──┬──┴──┬──╯   ╰──┬──┴──┬──┴──┬──┴─────┴─────╯
               │     │     │     │   │ RET │ BSP │ DEL │
               ╰─────┴─────┴─────╯   ╰─────┴─────┴─────╯
```

### MED — Media & Bluetooth

```
╭─────┬─────┬─────┬─────┬─────╮   ╭─────┬─────┬─────┬─────┬─────╮
│BOOT │►TAP │     │►BASE│BTCLR│   │     │ BT← │     │     │ BT→ │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│ GUI │ ALT │ CTL │ SFT │     │   │     │ PRV │ V-  │ V+  │ NXT │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│     │ ALT │►FUN │►MED │     │   │     │     │     │     │     │
╰─────┴─────┴──┬──┴──┬──┴──┬──╯   ╰──┬──┴──┬──┴──┬──┴─────┴─────╯
               │     │     │     │   │ STP │ PLY │ MUT │
               ╰─────┴─────┴─────╯   ╰─────┴─────┴─────╯
```

### NUM — Numeral

```
╭─────┬─────┬─────┬─────┬─────╮   ╭─────┬─────┬─────┬─────┬─────╮
│  [  │  7  │  8  │  9  │  ]  │   │     │►BASE│►TAP │     │BOOT │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│  ;  │  4  │  5  │  6  │  =  │   │     │ SFT │ CTL │ ALT │ GUI │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│  `  │  1  │  2  │  3  │  \  │   │     │►NUM │►NAV │ ALT │     │
╰─────┴─────┴──┬──┴──┬──┴──┬──╯   ╰──┬──┴──┬──┴──┬──┴─────┴─────╯
               │  .  │  0  │  -  │   │     │     │     │
               ╰─────┴─────┴─────╯   ╰─────┴─────┴─────╯
```

### SYM — Symbols

```
╭─────┬─────┬─────┬─────┬─────╮   ╭─────┬─────┬─────┬─────┬─────╮
│  {  │  &  │  *  │  (  │  }  │   │     │►BASE│►TAP │     │BOOT │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│  :  │  $  │  %  │  ^  │  +  │   │     │ SFT │ CTL │ ALT │ GUI │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│  ~  │  !  │  @  │  #  │  |  │   │     │►SYM │     │ ALT │     │
╰─────┴─────┴──┬──┴──┬──┴──┬──╯   ╰──┬──┴──┬──┴──┬──┴─────┴─────╯
               │  (  │  )  │  _  │   │     │     │     │
               ╰─────┴─────┴─────╯   ╰─────┴─────┴─────╯
```

### TAP — Gaming (no mods)

```
╭─────┬─────┬─────┬─────┬─────╮   ╭─────┬─────┬─────┬─────┬─────╮
│  Q  │  W  │  E  │  R  │  T  │   │  Y  │  U  │  I  │  O  │  P  │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│  A  │  S  │  D  │  F  │  G  │   │  H  │  J  │  K  │  L  │  '  │
├─────┼─────┼─────┼─────┼─────┤   ├─────┼─────┼─────┼─────┼─────┤
│  Z  │  X  │  C  │  V  │  B  │   │  N  │  M  │  ,  │  .  │  /  │
╰─────┴─────┴──┬──┴──┬──┴──┬──╯   ╰──┬──┴──┬──┴──┬──┴─────┴─────╯
               │ ESC │ SPC │ TAB │   │ RET │ BSP │ DEL │
               ╰─────┴─────┴─────╯   ╰─────┴─────┴─────╯
```

## Modules

Both modules are required to build the dongle variants. They are in the `dongle` west group and fetched on demand by `build.sh`.

| Module | Used for |
|---|---|
| [prospector-zmk-module](https://github.com/carrefinho/prospector-zmk-module) | Ambient light sensor support on the dongle |
| [zmk-dongle-screen](https://github.com/janpfischer/zmk-dongle-screen) | YADS dongle screen support |
