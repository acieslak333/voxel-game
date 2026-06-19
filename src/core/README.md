# `src/core/` — app lifecycle, input, UI, settings

The application shell: the `App` that owns every subsystem and drives the main
loop, plus windowing, input sampling, the immediate-mode UI, settings, the
day/night model, and colour palettes.

> Part of the [**Code Index**](../../docs/CODE_INDEX.md) (see the *core/* subsystem
> map). Exhaustive per-symbol listing: [`docs/index/SYMBOLS.md`](../../docs/index/SYMBOLS.md).

## Files

| File | Responsibility | Key symbols |
|---|---|---|
| `App.{h,cpp}` | Owns all subsystems; runs the loop: poll → input → update → stream → UI → render. | `App::run`, `updateSurvival`, `editBlocks`, `streamWindow`, `flushPendingEdits`, `savePlayer`/`loadPlayer`, `saveChests`/`loadChests`, `buildModels` |
| `AppUi.cpp` | Builds the HUD and every menu (inventory, crafting, chest, equipment, palette, options). | `App::buildUi` |
| `Input.{h,cpp}` | Samples GLFW keyboard/mouse once per frame into an edge-detected snapshot. | `Input::poll() → InputState`, `struct InputState` |
| `Settings.{h,cpp}` | Player options; loads/saves the runtime `build/bin/assets/settings.yaml`. | `Settings::load`, `Settings::save` |
| `DayNight.{h,cpp}` | Time-of-day + analytic atmosphere; snapshots `SkyState` for the sky/world shaders. | `DayNight::advance`, `DayNight::state() → SkyState` |
| `Ui.{h,cpp}` | Immediate-mode UI primitives (rects/text/sprites) the `UiRenderer` consumes. | `Ui` builder API |
| `Window.{h,cpp}` | GLFW window + Vulkan surface + resize/fullscreen. | `Window::pollEvents`, `framebufferSize` |
| `Palette.{h,cpp}` | Named colour palette from `assets/colors.yaml`. | `Palette` |
| `ColorPalette.{h,cpp}` | Retro/quantised colour palette swatches for the composite pass. | `ColorPalette` |
| `ShapePicker.h` | UI helper for choosing block shapes (slab/stairs/post/wall). | `ShapePicker` |

## Notes & cross-refs

- The **main loop** is the spine of the game; start in `App::run` (App.cpp).
  Per-frame: input → player physics → block edits (deferred while a background
  relight is in flight, REVIEW R3) → survival upkeep → entities/particles →
  `streamWindow` → `buildUi` → render.
- `Settings` reads/writes the **player-facing** `build/bin/assets/settings.yaml`
  at runtime — *not* the repo's `assets/settings.yaml`. World shape/seed live in
  `assets/world.yaml` (loaded via `WorldConfig`).
- Tunables belong in documented YAML (`docs/CONFIGURATION.md`); `App.h` still
  holds a few constants (`kWidth`/`kHeight`/`kReach`) — REVIEW R7.

## Read first

`App.h` (ownership + loop shape) → `App::run` in `App.cpp` → `Input.h` →
`DayNight.h`.
