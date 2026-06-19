# `tools/` — Python data editors & model generators

In-repo authoring tools for the game's data files. They are **development aids**,
not part of the build: each reads/writes the same YAML/binary the engine loads,
with a browser-based live preview that mirrors the C++ behaviour.

> Part of the [**Code Index**](../docs/CODE_INDEX.md) (see [Python tools](../docs/CODE_INDEX.md#python-tools)).
> Index tooling (the code-index generator) lives in [`tools/codeindex/`](codeindex/).

## Editors (launch via `python tools/hub.py`, port 5005)

| Tool | Port | Authors |
|---|---|---|
| `hub.py` | 5005 | Launcher + read-only block/item/recipe browser. |
| `feature_tool.py` | 5004 | `assets/features/*.yaml` — procedural scatter (shape ops, randomization, noise fills). Preview mirrors `Feature::at`. |
| `recipe_tool.py` | 5003 | `assets/recipes.yaml` — crafting recipes (comment-preserving). |
| `particle_tool.py` | 5001 | `assets/particles/*.prtcl` — effect tuning; preview mirrors `Particles.cpp`. |
| `structure_tool.py` | 5007 | `assets/structures/*.yaml` — layer-by-layer voxel painter. |

## Generators (run once per design; commit the output)

| Tool | Output |
|---|---|
| `gen_tool_models.py` | hammer/sword/pickaxe/torch `.bbmodel` + shaded PNG skins → `assets/models/`. |
| `gen_critter_model.py` | quadruped critter rig + skin → `assets/models/critter/`. |

(Offline texture/colour/icon generators live in `scripts/`, not here.)

## `tools/codeindex/` — the code index toolchain

| File | Purpose |
|---|---|
| `codeindex.py` | `gen` regenerates `docs/index/*` (symbols.json/.tags, SYMBOLS.md, manifest.json); `check` fails when the index is stale (CI/hook guard). Stdlib only. |
| `verify_comments.py` | Proves a change is comment-only (strips comments, asserts the code is byte-identical vs HEAD) — the safety net for the Doxygen pass. |

See [`docs/CODE_INDEX_GUIDE.md`](../docs/CODE_INDEX_GUIDE.md) for the full
maintenance procedure, or run **`/skill code-index`**.

## Note on older docs

`docs/TOOLING.md` describes worldgen editors (`worldgen_tool.py`,
`biome_tool.py`, `genmap_tool.py`, `worldgen_studio.py`) and a `--genmap` mode
that **no longer exist** — they were deleted. The list above is the live set.
