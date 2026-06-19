# Worldgen tooling — historical note

> **Historical / superseded.** This file documented a worldgen-editor merge plan
> (`worldgen_tool.py` / `biome_tool.py` / `genmap_tool.py` / `worldgen_studio.py`)
> and a `--genmap` CLI mode that **no longer exist** — those tools and that mode
> were deleted, and the notes referenced `assets/biomes.yaml`, which the shipped
> code does not use (worldgen config is `assets/world.yaml`). The full write-up
> actively misled, so it was trimmed to this pointer.

For the **current** tooling and data model, see:

- [`docs/CODE_INDEX.md`](CODE_INDEX.md) — Python tools, CLI modes, and the
  *Documentation drift* section (which records exactly this removal).
- [`tools/README.md`](../tools/README.md) — the live editors
  (`feature_tool`, `structure_tool`, `recipe_tool`, `particle_tool`) launched by
  `tools/hub.py`.
- [`docs/WORLDGEN.md`](WORLDGEN.md) — worldgen design notes.

The original content remains in git history if the editor-merge design record is
ever wanted again.
