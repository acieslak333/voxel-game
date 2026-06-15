# Color palettes (post-process)

Each `.hex` file here is a selectable **retro colour palette** for the composite
post pass. When one is picked in the in-game *Retro* settings tab, the whole
frame is remapped to the nearest colour in the palette (with the same ordered
Bayer dither the rest of the retro FX use, so gradients break up into a stipple
instead of hard banding). Selecting **Off** falls back to the per-channel
"Colour bits" quantiser.

## File format (Lospec `.hex`)

One colour per line, six hex digits `RRGGBB` (an optional leading `#` is allowed).
Blank lines and lines starting with `;`, `#` (when not 6 hex digits) or `//` are
comments. Colours are read in sRGB. Up to `kMaxPaletteColors` (64) entries are
used; extras are ignored.

```
; my-palette.hex
1a1c2c
5d275d
b13e53
```

Drop a `.hex` exported from https://lospec.com/palette-list straight in here and
it shows up in the selector — the filename (minus `.hex`) is the menu label.

## How it's wired

- Loaded by `vg::loadColorPalette` / listed by `vg::listColorPalettes`
  (`src/core/ColorPalette.{h,cpp}`).
- The selected name is saved as `retroPalette` in `settings.yaml`
  (empty string = Off).
- Uploaded into the composite `Post` UBO and applied in `shaders/composite.frag`
  (see `CompositeRenderer::setPalette`).
