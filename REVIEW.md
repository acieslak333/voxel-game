# Optimization backlog — voxel-game

The 2026-06-13 code review (findings **R1–R12**: streaming/threading correctness and
the remaining frame-hitch debt) has been **fully resolved** — see git history for the
commits. What remains here is the optimization backlog from that pass: follow-up
performance work, none of it felt slowness today.

Profiled context: startup ~4s (generate dominates), steady state 250–800 fps, worst
frame ~20–30 ms at a chunk-boundary crossing. These are about the remaining generate
cost, RAM, spike tails, and render-distance headroom. Ordered by value.

## Done

- **O1 + O2 — per-column worldgen memo + cliff-probe height cache.** Call-local memos
  in `generateColumnInto` (`columnInfo` / `surfaceY` / `height`, keyed on `(x,z)`)
  dedup the repeated evals across the tree/structure/feature scatter passes and the
  cliff probe. Byte-identical output (selftest hash unchanged, in-process `h1==h2`
  holds); same-environment generate time 1.37s → 0.92s (~33%).
- **O7 — render micro-costs.** Done as part of R8: `record()` iterates a compact
  `drawList_` instead of all ~17k slots twice per frame, and the water pass is skipped
  wholesale when no chunk has water.

## Remaining (future)

**O3 — Pack/sparsify `blockLightColor_` (memory).**
4 bytes per cell ≈ 285 MB at 33×33×256, almost all storing "no colored light"
(sky/block light add ~140 MB more). Emitter colors come from a handful of blocks.yaml
entries — a small palette index (or sparse storage) saves a couple hundred MB of the
~0.7–1 GB footprint. Matters on a machine that multitasks.

**O4 — Smooth the residual 20–30 ms boundary frame.**
Spread the strip apply over 2–3 frames (move a third of the columns per frame — still
exact), and/or lower mesh-worker thread priority so 8 workers chewing the new edge
don't deschedule the main thread.

**O5 — GPU buffer pool / VMA instead of one allocation per chunk.**
The per-chunk-buffer design sits near Vulkan's ~4096 allocation ceiling (why upload
batching needs slicing) and churns allocations during streaming. A pooled
suballocator removes the ceiling — the prerequisite for raising view_radius
meaningfully. Structural, medium effort.

**O6 — Interpolated density sampling (approximate; only for view_radius 24–32).**
Sample the terrain3d density stack on a coarse lattice (Minecraft uses 4×8×4) and
trilinearly interpolate: 10–100× on the most expensive function in the game, at the
cost of smoothing sub-4-block density detail. Hold until a bigger window is actually
wanted.

Measured and deliberately NOT worth touching: greedy mesher (0.19 ms/chunk), sky-light
flood (0.38s), per-frame entity baking, the liquid tick.

## What's solid (keep doing this)

- **Version-stamped worker results** (`meshVersion_`) make "newest request wins"
  trivially correct and let every other system re-request meshes without ceremony.
- **Deferred buffer retirement** (`retired_`, frames-in-flight aging) + frame-integrated
  uploads (`recordPendingUploads`) — the right Vulkan lifetime pattern, now used by
  streaming, liquids, AND edit/falloff remeshes (R2/R4).
- **Worldgen as a pure function of (seed, coords)** is what made strip pregeneration a
  safe afternoon refactor instead of a rewrite, and what made the O1/O2 memo a
  byte-identical change; `generateColumnInto` writing through caller pointers keeps it
  that way. Protect this property in review.
- The gated `VG_*` instrumentation (`VG_MESH_TIME`, `VG_FRAME_TIME`, `VG_AUTOWALK`)
  made every perf investigation measurement-driven; cheap to keep, worth using.
- **Streaming orchestration lives in one place** (`App::streamWindow`, R9) with the
  four-actor threading invariants stated in its header — keep new streaming logic
  there, not back inline in the frame loop.
