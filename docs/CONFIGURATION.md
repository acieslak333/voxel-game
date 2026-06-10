# Configuration & Data Convention

**This is a project rule. Agents and contributors must follow it.**

> **Tunable values do not live as magic numbers in scripts or code. They live in
> a YAML file under `assets/`, each value described in place: *what it is*,
> *where it is used*, and *what changes if you alter it*.** Scripts and C++ read
> those files; they hold no bare constants whose meaning you'd have to reverse-
> engineer.

The goal: a designer (or a future agent) can re-tune behaviour and appearance by
editing well-commented data and re-running/relaunching — never by hunting for a
`0.80` buried in a loop and guessing what it does.

---

## What counts as a "tunable value"

Move it to a documented YAML when it is a **knob someone might reasonably want to
change** without rewriting logic:

- numeric parameters (texture size, noise frequencies, speckle amounts, shade
  factors, world dimensions, reach distance, speeds);
- named tables of data (block definitions, the texture table, the colour palette);
- feature toggles and defaults (lighting mode, pixelation).

**Leave it in code** when it is *structural*, not a knob:

- file paths and directory layout, struct field order, enum values;
- algorithm invariants where a different value would be a different algorithm
  (e.g. the 6 face directions of a cube);
- anything the type system or build depends on at compile time (`Chunk::kSize` is
  a compile-time constant by design — but its *value* is a candidate to surface).

When unsure, lean toward data, and **always add the explanatory comment** — an
uncommented YAML number is just a magic number in a different file.

---

## The config files

| File | Read by | Controls |
|------|---------|----------|
| `assets/blocks.yaml` | `vg::BlockRegistry` (runtime) | Block types: name, solid/opaque, light emission, per-face textures. Add a block here. |
| `assets/world.yaml` | `vg::WorldConfig` (runtime) | World size (view radius/height) + seed; terrain shaping (noise frequencies & octaves, base height, amplitude, dirt depth, rocky/beach thresholds, domain warp); the temporary island mask (falloff radii, coast warp); and whimsical scatter-feature densities (lantern-trees, cairns, geodes). |
| `assets/textures.yaml` | `scripts/gen_textures.py` (offline) | Placeholder texture generation: size, the per-texture table, and the pattern constants (speckle, bands, planks, streaks, rings). |
| `assets/colors.yaml` | `vg::Palette` (runtime) **and** `gen_textures.py` | The named colour palette. **Generated** — do not hand-edit. |
| `assets/colormap.png` + `scripts/color_names.txt` | `scripts/gen_colors.py` (offline) | Source of the palette: swatch image + the name for each swatch. The single source of truth for colour. |
| `assets/settings.yaml` | `vg::Settings` (runtime) | Player options, written back by the game (edit via the Esc menu): pixelation, light falloff (cave darkness / glow radius), FOV, sensitivity, fly speed, day length, time running, sky colour, font. |
| `assets/sky.yaml` | `vg::DayNight` (runtime) | The day-night cycle's look: start hour, the analytic atmosphere (Rayleigh `betaR`, Mie `betaM`/`turbidity`/`mieG`, `sunIntensity`, `exposure`, `sunsetStrength`, `useAnalyticSky` fallback flag), night/legacy gradient colours, sun & moon disc colour/size/glow, terrain light ambient floors, sunlight/moonlight colours, night intensity, per-day weather variation. |
| `assets/clouds.yaml` | `vg::CloudSystem` (runtime) | The volumetric cloud system: slab altitude/quality, voxelise cell size, noise tile sizes, erosion/density, light transport (extinction, HG g, ambient/powder, multi-scatter, anti-solar twilight tint), wind, the evolving weather (diurnal cumulus hours, calm/fair coverage, multi-day drift, spatial variation), and debug `forceCoverage`/`forceType` presets. |

The colour pipeline is layered on purpose: **`colormap.png` → `gen_colors.py` →
`colors.yaml` → (runtime `Palette`, and `gen_textures.py`)**. Re-paint the
colormap, re-run `gen_colors.py`, and both the game and the generated textures
re-tint from one source.

---

## How to add or change a value

1. **Find the right file** from the table above (or create a new one — then add a
   row here and a header comment in the file pointing back to this doc).
2. **Add the value with a comment** stating, in plain words:
   - *what* it is and its units/range,
   - *where* it is used (which script/function/system),
   - *what changes* when you alter it (and any constraints, e.g. "all texture
     layers must share `size`").
   See `assets/textures.yaml` for the style to match.
3. **Read it in the consumer**, don't duplicate it. Python loads YAML with
   PyYAML; C++ uses `yaml-cpp` (already a dependency) at startup.
4. **Re-run the relevant step**: `python scripts/gen_textures.py` for textures,
   `python scripts/gen_colors.py` for the palette; runtime configs just need a
   relaunch. Generated artefacts (the PNGs, `colors.yaml`) are committed so the
   build never needs Python.

---

## For agents specifically

- Introducing a constant in a script or `.cpp`? **Stop** — put it in the matching
  YAML with the what/where/effect comment, and read it from there instead.
- Touching `gen_textures.py` / `gen_colors.py`? They are thin readers of their
  config. Keep them that way: new knobs go in `assets/textures.yaml`, not in the
  script body.
- Changing a generated artefact's *inputs*? Re-run the generator and commit the
  regenerated output, so the committed PNGs/`colors.yaml` stay in sync with the
  config.
- Adding a new config file? Document it in the table above and give the file a
  header comment naming its reader and what it controls.

### Known follow-ups (same convention, not yet applied)

These still hold tunables in C++ and are good candidates to surface into a
documented YAML loaded at startup via `yaml-cpp`:

- `src/core/App.h` — window size (`kWidth`, `kHeight`) and edit reach
  (`kReach`). Window size could join `assets/settings.yaml` (it's a display
  preference); `kReach` is a gameplay tuneable.

Done already: terrain shaping + world size/seed now live in `assets/world.yaml`
(`vg::WorldConfig`); texture-generation knobs in `assets/textures.yaml`.
