# `assets/` — game data

All game data: YAML configs, generated textures, Blockbench models, fonts,
particles, and colour palettes. The engine loads these at runtime; a copy is
placed next to the executable at build time (`build/bin/assets/`).

> **Project rule:** tunable values live in **documented YAML here**, never as
> magic numbers in code — read [`docs/CONFIGURATION.md`](../docs/CONFIGURATION.md)
> before adding a knob. Full catalog: [Code Index → Data & asset catalog](../docs/CODE_INDEX.md#data--asset-catalog).

## Config files

| File / dir | Read by | Controls |
|---|---|---|
| `blocks.yaml` | `BlockRegistry` | Block types: name, solid/opaque, emission, per-face textures, mining/tool/armor. |
| `world.yaml` | `WorldConfig` | **The worldgen config**: world size, seed, terrain shaping, `stream_tuning`. (Not `biomes.yaml` — that file does not exist.) |
| `world1.yaml` | (alt profile) | A second world profile. |
| `items.yaml` | registry/UI | Non-block item definitions. |
| `recipes.yaml` | `Crafting` | Crafting recipes. Edit via `tools/recipe_tool.py`. |
| `sky.yaml` | `DayNight` | Day/night look: atmosphere, sun/moon, stars, terrain light. |
| `clouds.yaml` | `CloudSystem` | Volumetric clouds: altitude, noise, density, light transport, weather. |
| `colors.yaml` (+ `colormap.png`) | `Palette` + `gen_textures.py` | Named colour palette. **Generated** via `scripts/gen_colors.py` — don't hand-edit. |
| `textures.yaml` | `scripts/gen_textures.py` | Placeholder texture generation knobs. |
| `settings.yaml` | `Settings` | Player options (read/written at `build/bin/assets/` at runtime). |
| `features/*.yaml` | `Feature` | Procedural scatter (trees). Edit via `tools/feature_tool.py`. |
| `structures/*.yaml` | `Structure` | Hand-authored voxel templates. Edit via `tools/structure_tool.py`. |
| `particles/*.prtcl` | `Particles` | Particle effects. Edit via `tools/particle_tool.py`. |

## Directories

| Dir | Contents |
|---|---|
| `models/<name>/` | Blockbench `.bbmodel` + skin PNG (hand, hammer, pickaxe, sword, torch, critter). |
| `fonts/<name>/` | TTF fonts for the UI. |
| `textures/` | Generated block textures + `icons/`. |
| `colorpalettes/` | Retro colour palettes for the composite pass. |

The colour pipeline is layered on purpose:
`colormap.png` → `gen_colors.py` → `colors.yaml` → (runtime `Palette` + textures).
Re-paint the colormap, re-run the generator, and game + textures re-tint from one
source.
