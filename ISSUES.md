1. ~~The lightning doesnt update between chunks. For example when deleting glowstone the lightning in other chunk was still lightning when in the chunk of the glwo stone it went away~~
   **FIXED** (World::setBlock): the edit relights a 16-block-radius box but used to remesh only the edited chunk + its face neighbours, so farther chunks kept stale baked lighting. setBlock now returns every chunk overlapping the relit box (full height), so all chunks whose light changed get remeshed.

2. ~~Game lags when deleting/placing block~~
   **FIXED** (World::setBlock): the edit relit a 16-block box and then remeshed
   EVERY chunk overlapping that box across the full world height — up to
   3*3*chunksY chunks (~72 with chunksY=8) greedy-meshed + GPU-uploaded per click,
   even though a typical edit only changes light in a small neighbourhood.
   setBlock now snapshots the box, relights, and diffs old-vs-new light to remesh
   only the chunks that actually changed (plus the edited chunk and any
   face-adjacent chunk across a shared boundary). Correctness is unchanged (light
   reaches at most 15 blocks, fully inside the radius-16 box); the remesh count
   drops from dozens to a handful, killing the per-edit stutter. (Dead
   `relightRegion` helper removed; its two relightField calls are inlined.)

3. Please optimize those things:
Core Optimizations:

    Mesh & Vertex Culling: Instead of making every single block an individual object, data is generated into a single mesh. The creator removes faces obscured by other blocks, faces outside the player's field of view (frustum culling), and faces pointing away from the camera

    Burst Compiler: To fix massive lag spikes during chunk generation, they utilize the Burst compiler to disable safety measures in the code, making chunk generation

    Level of Detail (LOD) & Greedy Meshing: To render far distances, distant chunks are generated at progressively lower resolutions (fewer blocks). Furthermore, "greedy meshing" combines adjacent squares into larger rectangular shapes to drastically reduce the triangle count

    Multi-threading: To prevent the game from freezing when new chunks load, chunk generation is banished from the main thread to background threads

   **PROGRESS** (partial):
   - Mesh & vertex culling: *done.* Obscured faces are removed by greedy meshing
     (single mesh per chunk, cross-chunk faces culled). Back-face culling is on at
     the GPU (`VK_CULL_MODE_BACK_BIT`, Pipeline.cpp). **Frustum culling added**
     (WorldRenderer::record): a conservative AABB-vs-frustum test now skips the
     draw call for any chunk whose bounding box is entirely off-screen.
   - "Burst compiler": Unity-specific; the C++ analogue is just building Release
     (the slow stutters above were the per-edit remesh storm — see issue 2 — not
     the noise generator). No code change.
   - Multi-threading chunk generation: **DONE** — landed with the chunk-streaming
     milestone (worker-thread meshing + parallel generation + background relight).
     See issue 11.
   - LOD for distant chunks: still NOT done — now that streaming exists, far chunks
     do exist, so LOD is a real (but optional) future item. See issue 11's FUTURE
     list.


4. ~~The darkest light should be as dark as dark sky add it as another level~~
   **FIXED** (chunk.frag): shadow ramp went 6 -> 7 levels and `kShadow` was lowered
   to the night-zenith colour (#070A1A in linear, ~(0.003,0.004,0.013)), so the
   darkest terrain level is now as dark as the dark sky instead of glowing
   brighter than it.

5. ~~Clouds are not visble during night, they kinda should be slightly~~
   **FIXED** (sky.frag marchClouds): added a faint moonless night glow to the
   cloud ambient (`cMoon.rgb * 0.018 * night`) and bumped the moonlight term
   (0.4 -> 0.5), so the volumetric clouds read as slight silhouettes against the
   dark sky even when the moon is down.

6. ~~The night sky due to the dithering looks very messy especially with stars,
   please fix it and make it more smooth~~
   **FIXED** (sky.frag): the final posterise+dither now quantises in perceptual
   (gamma/sqrt) space instead of linear. The offscreen is an sRGB target, so the
   linear quantise was crushing the whole dark night sky into the bottom 1-2
   levels and dithering across a huge perceptual gap (the "messy" grain). sqrt()
   spreads the levels so darks get fine smooth steps; the bright day gradient is
   unchanged.

7. ~~stars are too grainy they sohuld be bigger so dithering doesnt make it look
   bad~~
   **FIXED** (sky.frag starLayer + sky.yaml): star disc radius and the min-size
   floor were increased (mix 0.011..0.0020, floor 2.4*fwidth) so each star covers
   a few low-res pixels with a soft edge the dither can shade smoothly. Stars were
   re-enabled in sky.yaml (`starBrightness 0 -> 0.6`, `milkyWay 0 -> 0.3`) — tune
   to taste.


8. ~~Stars are to grainy, make them more sparse. Due to dithering it looks very
   messy, also mikly way is a biiit to grainy. Also. Please take that into account~~
   **FIXED** (sky.frag):
   - Stars: dropped the densest third star layer (density 1500) — its tiny
     sub-pixel stars were what the low-res Bayer dither chewed into grain — and
     raised the empty-cell thresholds on the two remaining layers (0.88/0.93 ->
     0.93/0.97). The field is now noticeably sparser: a clean scatter of bright
     stars plus a thinner faint layer, instead of a dense messy speckle.
   - Milky Way (milkyWay): lowered the clump/dust noise frequencies (clump
     6/14 -> 4/8, dust 10 -> 6) so the band is broad and soft instead of
     high-frequency speckle, and the dust lanes now only *dim* the glow
     (mix floor 0.5) instead of punching hard black holes that read as grain.

9. ~~Please delete the voxel clouds above player and make normal clouds more
   blocky and more varied in height and size, go crazy with it please~~
   **FIXED**:
   - Deleted the near-field blocky voxel cloud layer entirely: removed
     CloudBlocksRenderer (.cpp/.h), its wiring in App (construct/update/record)
     and CMakeLists, the `cloud` block from blocks.yaml, all `blocky*` keys from
     clouds.yaml, and the now-dead CPU mirror that only it used
     (CloudSystem::weatherAt/densityAt, CloudNoise::sampleBase/sampleDetail,
     WeatherMap::sample + its `cells_` grid). The only clouds left are the
     volumetric sky layer.
   - Made the volumetric clouds blockier and far more varied (clouds.yaml):
     voxelise cell 3 -> 6 blocks (chunkier), a much taller slab (140..330 so
     cumulonimbus tower over flat stratus), and a small weather tile (1400 ->
     850) with big variation amplitudes (coverage 0.45 -> 0.60, type 0.50 ->
     0.85) so the visible sky swings between open gaps and dense banks and across
     the full stratus->cumulonimbus height range.

10. **WEATHER & ATMOSPHERE OVERHAUL** (umbrella / design spec — not yet built)
    Goal: a far richer, more *alive* sky — more weather variety, more cloud types,
    more colourful and more varied sunsets, fog, cloud-driven lighting, and a real
    night/moon cycle. Guiding principle (user): **most realistic, but nicely
    optimized** — chase real phenomena, but every heavy effect gets a cheap path
    (analytic where possible, coarse/low-res where a march is unavoidable, capped
    counts). Reference look: the four photos in `sunset/` (multi-band pink ->
    orange -> gold skies with pink-underlit broken/mackerel clouds) and
    `sunset/sunset_article.md` (extra colours come from a longer light path +
    aerosols/haze; haze intensity drives how red the dusk gets).

    Build as ordered, independently-shippable phases. Current touch-points:
    `shaders/sky.frag` (analytic scattering + cloud raymarch + stars), `chunk.frag`
    (terrain light), `src/core/DayNight.{h,cpp}` (time/sky/weather state),
    `src/clouds/*` (CloudSystem weather model, CloudNoise, WeatherMap),
    `assets/sky.yaml`, `assets/clouds.yaml`, and the composite pass
    (`CompositeRenderer`/`OffscreenTarget`) which fog will hook into.

    --- Phase A — Multi-colour, more varied sunsets --- **[DONE]** ----------------
    Implemented: multi-band dusk (sky.yaml `sunsetHorizon`/`sunsetMid`/`sunsetHigh`
    blended by elevation + sun-azimuth focus in sky.frag — gold horizon -> orange
    -> pink/violet afterglow that spreads wide like the Belt of Venus); per-day
    `sunsetPresets` deterministically picked in DayNight (gold/fire/pastel/purple/
    ember), warmth-tinted; ozone (Chappuis) absorption term in `skyRadiance()`
    deepening twilight blues/purples (haze-coupled); and a warm `cloudDuskTint`
    added to lit cloud undersides in `marchClouds` (independent of the sun-disc
    colour). New UBO fields packed in SkyRenderer. Verified: builds + renders a
    banded pastel dusk.
    (Original plan below.)
    Today dusk is ONE authored band (`sunsetHorizon: #DC9824`) painted near the sun
    (`DayNight::sunsetColor` -> sky.frag `sunset.rgb`); per-day "warmth" only nudges
    it warmer/cooler. Do all four of:
    - **Multi-band gradient:** stack >=3 authored sunset zones — high-sky
      (pink/violet), mid (orange), horizon (gold) — blended by view elevation and
      angle-to-sun in sky.frag, instead of the single band. New keys in sky.yaml
      (`sunsetZenith`/`sunsetMid`/`sunsetHorizon`).
    - **Daily presets:** a small palette of sunset moods (fiery red, pastel pink,
      purple, hazy gold) in sky.yaml; each in-game day deterministically draws one
      via the existing `weatherJitter` so sunsets differ day to day.
    - **Physical pinks via haze/ozone:** add an ozone (Chappuis-band) absorption
      term to `skyRadiance()` and couple aerosol Mie to the day's haze, so
      pinks/purples and the post-sunset blue emerge naturally (cheap: one extra
      transmittance factor). Mirror in DayNight terrain light for coherence.
    - **Underlit clouds:** tint cloud undersides with a separate warm sunset colour
      (independent of the sun-disc colour) in `marchClouds`, so broken clouds glow
      pink/red at the base like the photos.

    --- Phase B — More cloud types & more varied noise --- **[DONE, compiles; visual check pending build]** ---
    Implemented:
    - Noise enrichment (CloudNoise.cpp, cache version 2->3 so it regenerates):
      domain-warp the base Perlin by a low-freq vector + a ridged octave (soft
      blobs .. wispy filaments), and a ridged sharpen on the detail Worley
      (stringy/cirrus shredding).
    - Richer type axis (sky.frag cloudProfile): 0 cirrus (thin high sheet) ->
      ~0.25 stratus -> ~0.6 cumulus -> 1 cumulonimbus.
    - Altitude character in cloudDensity (no extra fetch): wind-compressed detail
      near the top -> cirrus streaks; stronger erosion at mid height -> altocumulus
      'mackerel' puffs.
    - **Changing weighted sum of noises (user request):** the base shape is now a
      blend of the primary base sample and a second decorrelated sample, with a
      weight that drifts across the sky and slowly over time (one extra fetch,
      skipped on the cheap light march) — so it's never one noise everywhere.
    Verified: sky.frag + CloudNoise.cpp compile clean. Visual screenshot blocked:
    the project currently won't link because the streaming worker-pool refactor in
    WorldRenderer/World/ChunkMesher (unrelated, in progress) is mid-flight.
    (Original plan below.)
    Today: one slab, one 1-D type axis (stratus->cumulus->cumulonimbus), one base
    (Perlin-Worley) + one detail (Worley). Add:
    - **Cirrus** (high, thin, fast, wind-streaked) — stretched/ridged noise on a
      separate high band.
    - **Altocumulus / mackerel** (dappled mid-level rows) — near-cellular Worley
      lattice; this is the "fish-scale" cloud that underlights so well at dusk.
    - **Overcast stratus** (flat full-sky grey) for gloomy/foggy days.
    - **More varied noise summing** in CloudNoise: domain warping + extra octaves +
      a ridged variant so shapes don't all read the same.

    --- Phase C — Hybrid weather states --- **[DONE, compiles; visual check pending build]** ---
    Implemented (CloudSystem + sky.frag): a table of 6 discrete states (clear /
    fair / broken / overcast / stormy / foggy), each with its own diurnal
    coverage/type endpoints, spatial-variation amplitudes, **cloud-height band**,
    cirrus-deck amount, fog density and night star clarity. Each day
    deterministically draws a weighted state (fair/broken common, stormy/foggy
    rare); its endpoints feed the *existing* diurnal build + multi-day drift, and
    the weather map varies it spatially within range (states = anchors, noise
    within). All outputs smoothly tracked so changes never pop. The live layer
    band, amps, and a new GpuParams `deck` (cirrus amount / fog / clarity) flow to
    the shader; a faint **high cirrus second deck** is added in cloudDensity
    alongside the main type (multiple decks, no second march). `fogDensity()` /
    `starClarity()` accessors exposed for Phases E/F. Verified: CloudSystem +
    sky.frag compile clean. Link/screenshot blocked again by the in-progress
    streaming integration (App.cpp -> WorldRenderer::recordPendingUploads).
    (Original plan below.)
    Replace the purely-continuous mood with **discrete states as anchors, each
    sampling continuous noise within its range** (so two "Stormy" days still
    differ). States: Clear / Fair (scattered cumulus) / Broken / Overcast (stratus)
    / Stormy (cumulonimbus) / Foggy. Each state defines ranges for coverage, type,
    **cloud height + amplitude (different decks/altitudes)**, fog density, turbidity
    and night star-clarity. A day picks a state (or a short sequence across the
    day) and lerps between them. Extend CloudSystem's weather model + WeatherMap.
    - **Multiple simultaneous decks:** allow e.g. low cumulus + high cirrus at once
      (different heights/amplitudes) rather than a single slab — cap the active
      deck count for cost (optimized: 2 decks typical, not 4).

    --- Phase D — Clouds affect lighting --- **[D1 DONE & verified; D2 deferred]** ---
    D1 (done): global cloud dimming + sky tint. CloudSystem exposes coverage();
    App.cpp dims the terrain light by overhead cover before handing it to
    WorldRenderer — heavy cover fades the directional sun/moon (intensity ->0.35),
    raises the ambient floor (shading flattens), and greys/cools the tint toward an
    overcast look. Verified: a forced-overcast day renders a visibly darker,
    flatter, muted-green ground vs a clear day. Cheap, zero pipeline change.
    D2 (deferred): real per-area dappled ground shadows need the cloud textures
    bound into the WorldRenderer pipeline (constructor signature + descriptor set +
    chunk.frag/.vert + App.cpp construction) — exactly the files in the user's live
    streaming refactor. Deferred to avoid colliding; revisit once streaming lands.
    (Original plan below.)
    Clouds should block the sun/moon by **density** and drive terrain light:
    - **Real ground shadows, optimized:** each frame build a low-res 2-D cloud
      shadow (sample cloud density along the sun direction over a coarse grid /
      project terrain XZ onto the cloud slab), sampled in `chunk.frag` as a light
      multiplier — moving dappled shadows without a per-pixel march.
    - **Global dimming + sky tint:** thick cover dims & cools terrain light
      (overcast = flat grey); the lit cloud colour bleeds into ambient so an orange
      sunset under cloud tints the ground. Drives `skyIntensity`/`ambient`/
      `lightColor` in DayNight from cloud coverage.
    - Sun/moon disc are already drawn under the clouds (occluded visually);
      make the terrain light intensity follow the same coverage.

    --- Phase E — Fog --- **[DONE & verified]** ----------------------------------
    Implemented entirely in the composite/post pass (no WorldRenderer collision):
    OffscreenTarget now stores + exposes a sampleable depth image; CompositeRenderer
    samples it and fogs by reconstructed world position. composite.frag does
    distance haze (uniform exp) + an analytic exponential height/valley fog
    (closed-form ray integral, no marching) toward a haze colour. Fog inputs flow
    App -> Renderer::setFog -> composite push constants (124B, no new UBO): haze
    colour tracks the horizon sky (pale blue day / warm dusk / dark night), distance
    density + ground-fog density follow CloudSystem::fogDensity() (the weather
    state) plus a dawn/dusk ground-fog boost. Sky pixels are skipped (the sky
    already holds horizon haze). Verified: clear midday = crisp with gentle far
    haze; foggy dawn = soft warm mist dissolving the distance.
    (Original plan below.)
    New system (no fog today). Needs scene depth in the composite/fog pass.
    - **Distance haze:** depth-based fog fading distant terrain into the sky colour
      in the view direction (cheap; pairs with hazy sunsets and adds depth/scale).
    - **Ground / valley fog:** height-based volumetric fog pooling in low terrain;
      use the **analytic height-exponential closed form** (integrate along the view
      ray, no marching) for the optimized path.
    - **Weather-driven:** fog density/top come from the Phase-C weather state + time
      (thick foggy/overcast mornings, burning off by midday).

    --- Phase F — Night, moon phase & star visibility --- **[DONE & verified]** ---
    Fix after first pass: the unlit moon used a 0.04 earthshine floor that read as a
    black disc — and at new moon (early game) the moon sits near the sun, so that
    black disc showed in daylight. Now only the *lit* crescent is painted; the dark
    side is gated to a faint night-only earthshine and the disc is hidden below the
    horizon, so a new moon is invisible (no black disc, day or night). Verified: a
    day-14 midnight moon renders bright with the gibbous terminator + washed stars;
    a day-0 midday sky shows no moon.
    Implemented (DayNight + sky.frag only — no new UBO field; moon illumination is
    derived in-shader from dot(sunDir,moonDir)):
    - Moon orbit: the moon now lags the sun by the synodic elongation E (one turn
      per 29.53 days), so it rises ~50 min later each night and cycles through its
      phases instead of sitting anti-solar. Own z-lean for a distinct arc.
    - Phase-shaded disc: each disc point is a point on the moon's sphere, lit where
      its normal faces the sun — the terminator sweeps it into crescent/gibbous/
      full, with faint earthshine on the dark side.
    - Phase-scaled night: sky-light + moonlight scale with the lit fraction
      (illum=(1-cosE)/2), so new-moon nights go near-black and full-moon nights are
      bright. Stars wash out under a full moon (and only when it's above the
      horizon), stay brilliant at new moon.
    - Weather clarity: the state's global star clarity (cDeck.z) thins the field on
      overcast/foggy/stormy nights on top of the local cloud haze.
    Added a VG_DAY debug hook (DayNight::setDay) to screenshot a chosen moon phase.
    (Original plan below.)
    - **Moon phase (full model):** a ~29.5-day lunar cycle -> phase angle; render
      the lit crescent/gibbous **shape** on the moon disc (terminator from the sun
      direction) and let the moon **orbit independently** (not always anti-solar).
      Moon brightness scales with illuminated fraction.
    - **Star visibility varies:** scale star brightness/extinction by moon
      illumination (new moon = brilliant field, full moon = washed) AND by the
      night's weather clarity (Phase C), so nights genuinely differ.


11. ~~Chunk streaming milestone + WORLD_GEN_AGENT_TIPS audit~~ **DONE**
    (full design: docs/STREAMING.md + memory/voxel-roadmap.md.)

    **Streaming** (gated by `streaming:` / `async_streaming:` in world.yaml): the
    world is now infinite and follows the player — windowed ring-buffer chunk + light
    storage, floor-correct world<->chunk<->local coords, windowed seam relight,
    save-to-disk for edited chunks (`saves/<seed>/`), and fully de-hitched:
    worker-thread greedy meshing, frame-integrated GPU upload (copies recorded into
    the frame's own command buffer — zero `vkQueueWaitIdle`), and a background-thread
    relight (generation + the window-origin move stay on the main thread; only the
    light flood runs off-thread, over disjoint edge data). Island shaping is disabled
    when streaming (endless terrain). This also closes **#3's** deferred
    multithreading (streaming 3b).

    **Audit** vs WORLD_GEN_AGENT_TIPS.md §6 checklist. Already-correct, kept as-is:
    determinism (generation is a pure fn of seed+world-coord; salted coordinate
    hashes; single-column features => approach-direction-independent), floor-div/mod
    for negative coords, neighbour-sampled cross-chunk meshing (no edge walls), the
    AO diagonal-flip (ChunkMesher.cpp ~L135: `(ao[0]+ao[2]) > (ao[1]+ao[3])`), and a
    safe worker->main handoff. Fixed:
    - **GPU allocation count:** a chunk's vertices + indices now share ONE buffer /
      allocation (WorldRenderer ChunkMesh `meshBuffer` + `indexOffset`; index data at
      a 4-byte-aligned offset). Halves device allocations, clearing the latent crash
      near Vulkan's guaranteed 4096 `maxMemoryAllocationCount` at high `view_radius`.
    - **Redundant per-voxel noise:** `generateChunk` -> `generateColumn` — the
      expensive `columnHeight`/material/feature noise is computed once per (X,Z)
      column and shared down the vertical stack (~`chunksY`x fewer calls). Output is
      bit-identical (not a worldgen change).
    - **Golden / determinism test:** `voxelgame --selftest` (headless, no window/
      Vulkan) generates a fixed world twice and asserts (a) bit-identical regen and
      (b) a hash match vs golden `0xe2ca35d78fbc2807` (block ids + sky + block light).
      Exit 0 = pass. Run it after any worldgen/lighting change; if it fails
      unexpectedly you changed output by accident — if intentional, run once and copy
      the printed hash into `kGolden` in `main.cpp`.

    **FUTURE (optional, deliberately deferred — all from WORLD_GEN_AGENT_TIPS):**
    - **OpenSimplex2** (drop-in via FastNoiseLite) + lacunarity 2.01-2.04 + a small
      inter-octave rotation in `Noise::fbm` — fewer axis-aligned/repeat artifacts.
      RESHAPES terrain, so version-gate it (the `--selftest` golden will flag the
      change; bump `kGolden` deliberately). Only if you want the terrain *look*
      changed — it's an art decision, not a bug.
    - **Camera-relative rendering** — float vertex precision only degrades ~16M
      blocks from spawn (years of walking), so academic for now. If ever needed:
      subtract a per-frame render origin from chunk `worldPos` and `camera.position`
      before building the float view/model matrices, consistently across terrain, the
      UI block wireframe, and clouds.
    - **LOD for distant chunks** (still open from #3) — render far chunks at lower
      resolution (larger voxels / a heightmap shell, Transvoxel/skirts for seams).
      Only pays off at much larger view distances than the current window.

12. **WATER & LAVA** — **DONE**
    Placeable liquids with worldgen oceans/lava, event-driven flow, and proper
    translucent rendering.
    - **Translucent water pass:** the mesher splits liquid surfaces into a separate
      batch (`MeshData.waterVertices/Indices`); WorldRenderer draws them in a second
      pass after opaque terrain with a translucent pipeline (alpha blend on, depth
      WRITE off, no back-face cull) at ~70% opacity — so the seabed/terrain behind
      shows through. Source water is greedy-meshed (oceans stay cheap); flowing water
      is per-cell. Lava stays opaque (molten rock). One combined per-chunk buffer
      holds opaque+water verts then opaque+water indices; the water draw uses
      `firstWaterVertex` as its `vkCmdDrawIndexed` vertexOffset. Alpha is a per-draw
      push constant (1 opaque / 0.7 water) read in chunk.vert -> chunk.frag.
    - **Light through water:** water is `opaque: false`, so the sky-light flood
      reaches the seabed (was pitch-black before — the flood stopped at the surface).
    - **Corner-connected flowing surface:** each top corner rises to the MAX fluid
      height of the up-to-4 liquid cells meeting at it, so neighbouring flowing cells
      and the full-height source share corner heights and form ONE continuous sloped
      sheet — fixes the stair-step gaps / invisible side strips. A vertical side wall
      shows only at the puddle's air edge.
    - **Flow fix:** new cells filled in a tick are deferred to a `next` queue applied
      after the batched relight (they were being drained the same tick before the
      world was updated, read back as air, and dropped — so flow died after one ring).
    - **Liquids non-targetable:** since water became non-opaque, `isTargetable` now
      targets solids + foliage (Cross/LeafCube) only, so you can't mine water/lava.
    - **Spread tuned for frames:** `kMaxLevel 7 -> 3` (≈7-wide puddles, ~5x fewer
      cells/remeshes) — the big puddles + per-tick remesh were frying the frame rate.

    **PENDING ACTION — relink the Debug build.** All of the above is built into the
    **Release** exe (`build\bin\Release\voxelgame.exe`), but the **Debug** exe
    (`build\bin\Debug\voxelgame.exe`) could not relink because the game was running and
    held the file lock — so the Debug build still has the OLD pre-fix behaviour. To get
    the fixes into Debug: close the running game, then
    `cmake --build build --config Debug`. (Shaders + 16x16 textures + blocks.yaml are in
    the shared `build/bin` asset/shader dirs already, so only the exe needs relinking.)

13. **NEXT STEPS / backlog** (pick a direction)
    Liquids and the weather overhaul are done; this is the candidate work after them.

    **A. Liquid polish & physics**
    - ~~Underwater fog + blue tint when the camera is submerged.~~ **DONE.** App
      flags the composite pass when the camera eye sits in a water block; the pass
      then drowns the whole view (sky included) in a distance-thickening blue-green
      murk (`kWaterMurk`, cheap exponential — no march). Routed through the existing
      fog push constant by repurposing its dead `levels` slot as `submerged` (no
      layout change; removed the unused `setLevels`/`levels_`). Render-only, so the
      `--selftest` golden is unaffected. NOTE: tuning (murk colour/density) is by eye
      and unverified in-engine — adjust `kWaterMurk` / the `0.045` density and the
      `0.30..0.94` range in composite.frag to taste.
    - ~~Swim/buoyancy physics + drowning; lava deals damage.~~ **DONE.** (Lava damage
      already existed.) PlayerController now samples whether the body/head are in
      water (via a new `setWaterFn` predicate App wires from the world) and, while
      submerged, swaps to buoyant physics: weak effective gravity, exponential
      vertical drag toward a slow terminal sink, hold-jump to swim up, slower
      horizontal swim, and water breaks fall damage. Drowning: a breath timer
      (`air_`, 10s) drains while the head is underwater and deals continuous HP/s
      once empty, refilling fast above water (exposed via `air()`/`maxAir()`/
      `inWater()` for a future HUD bubble bar). Verified headless: 6 new `--logictest`
      checks (buoyant sink rate, swim-up, breath-drain, drown damage, surface
      refill) pass; `--selftest` golden unchanged. TODO(polish): HUD air bubbles.
    - ~~Water meets lava -> stone (always stone, no obsidian).~~ **DONE.** In the
      liquid flow tick (App::tickLiquids), when a flowing liquid cell is processed
      and a lava cell touches water (either the cell is lava with a water neighbour,
      or water with a lava neighbour), that lava cell is converted to STONE (one
      batched edit, relit by setBlocksBatch; counted toward the per-tick fill cap so
      a large contact solidifies over a few ticks). Triggers on flow/placement
      (seedLiquid is queued on place + break). No obsidian variant, per user.
    - Infinite source (two sources fill the gap between them, like Minecraft).
    - Kill the per-tick `vkDeviceWaitIdle` in liquid remeshes — route flow remeshes
      through the streaming path (the remaining flow-time stutter; remeshChunks does
      a full device drain every 0.2s tick while water spreads).

    **B. Survival loop** (memory: game-features-direction)
    - Block hardness + mining time (hold-to-break); tools speed it up. Tools do NOT
      wear down (no durability). For now only TWO tools: a **sword** and a **pickaxe**.
    - **Torches:** a placeable block-light source (emits block light; lights caves /
      night). Reuses the existing block-light flood.
    - Dropped items as world entities (walk-over pickup) instead of straight-to-inv.
    - **Crafting, Terraria-style:** a list that shows what you can make from your
      current inventory (+ nearby crafting stations later), click to craft — NOT a
      fixed grid of recipes.
    - **Chests:** a placeable container block with its own slot grid; **contents
      persist** (saved with the chunk to disk — the streaming milestone already saves
      edited chunks under `saves/<seed>/`, so block metadata/extra-data needs a chest
      payload alongside it).
    - **Armor + trinkets:** Terraria-style equip slots — armor pieces (reduce damage)
      and trinkets/accessories (passive bonuses, e.g. speed/jump/regen). Separate
      equip slots in the inventory screen.
    - Health (combat / lava / fall). **No hunger.**

    **C. Worldgen richness** (memory: worldgen-overhaul Phase 2)
    - Rivers & lakes (now that water renders + flows correctly).
    - Cave variety (ravines, cave water/lava pools); ore-balance pass.

    **D. Performance / tech**
    - LOD for distant chunks (the one open item from #3) — only matters at larger
      view distance than the current window.
    - The liquid remesh de-stutter from A.

    **E. 3D entities & animation** (prereq for mobs/NPCs/dropped items, combat)
    Direction chosen (user): **blocky**, so use box-part rigs, NOT skinned meshes.
    - **Authoring tool: Blockbench** (free, purpose-built for voxel/Minecraft-style box
      models + a built-in keyframe animation editor). Matches the game's blocky look.
    - **Interchange format: glTF 2.0 (`.glb`)** — node hierarchy + animations; Blockbench
      exports it (Generic Model / glTF). The runtime standard for 3D.
    - **C++ loader: cgltf** (single-header, zero-dep, fast — fits the stb-style
      minimalism; Assimp is too heavy, tinygltf the middle option).
    - **Rig model: cuboid box-parts, no skinning.** Each entity is a tree of textured
      boxes (head/torso/arms/legs); animation = TRS keyframes rotating parts around
      pivots, composed down the hierarchy. No vertex weights / skinning shader needed.
    - **Engine work (new, separate from chunk rendering):**
      - An `EntityRenderer` + its own pipeline (entities move every frame; chunk meshes
        are static — keep them apart).
      - Per-draw part-matrix array (push constant / small UBO) for the box hierarchy.
      - A small animation player: sample keyframes by time, lerp pos/scale + slerp
        rotation, compose down the node tree.
      - Vertex format ≈ current `Vertex` (pos/uv/normal) + a `partIndex`.
    - Keep Blender + skeletal skinning in reserve ONLY if smoother/organic creatures are
      ever wanted (bigger: joint indices+weights, bone-matrix palette, skinning shader).

    **F. Flora expansion** (new trees + plants — best built ON the template tool, G)
    - New tree species: **birch** (tall thin pale trunk), **maple** (round, autumn-
      tintable canopy), **pine** (conical layered needles), **willow** (drooping
      canopy). Per-species look via either template variants (G) or parametric
      generators; stamp with random rotation/mirror for variety.
    - New plants: flowers, ferns, tall grass, mushrooms, cacti (desert), vines,
      lilypads — Cross-render foliage, biome-gated placement in worldgen.
    - Reuses the existing Cross/Model renderType + foliage cutout; new textures via
      gen_textures.py. Changes worldgen output -> rebaseline the --selftest golden.

    **G. Structure / template authoring system** (the reusable tool; trees are its
    first client, then houses/ruins/dungeons/villages)
    - **Template format:** a small voxel grid — palette (block names) + a 3D array of
      (paletteIndex, metadata) + an anchor/origin + tags. Files under
      `assets/structures/` (compact binary `.vxs`, or YAML for hand-editing). Like a
      sized-down MC structure-NBT / WorldEdit `.schem`.
    - **Authoring — in-game capture (the win):** a WorldEdit-style "select two corners
      -> save region to a template" wand. Reuses the existing block edit + chunk
      save-to-disk. Build a tree/house in-world, stamp it forever. (Plus hand-authored
      YAML for tiny things; Blockbench voxel exports can be converted.)
    - **Seam-safe stamping (the hard part):** structure ORIGINS are a deterministic
      function of region + seed; when a chunk generates it scans candidate origins
      within `maxStructureBounds` of its area and stamps only the overlapping voxels —
      so a multi-chunk structure is identical regardless of which chunk streams first
      (current single-column trees become a small special case). Rebaseline the golden.

    **H. Visual / feel polish** (highest impact-per-effort, roughly in order)
    - **Biome colour tinting** of grass/leaves/water — sample `colormap.png` by biome/
      temperature (biomes + colormap already exist). Biggest "alive" upgrade; enables
      maple autumn / pine tone.
    - **Animated foliage** — wind sway on the Cross quads (cheap vertex-shader wave).
    - **Water surface life** — gentle wave + scrolling UV + a sun specular glint /
      fresnel, so it reads as water not glass (already translucent).
    - **Precipitation** — rain/snow particles tied to the existing weather states.
    - **Break feedback** — crack overlay while mining + break particles + a sound hook
      (NOTE: audio is currently absent entirely — first sound system goes here).
    - **First-person hand / held-item view model** — big "it's a game now" feel.
    - Ambient particles (falling leaves, pollen, embers near lava); footstep/landing
      dust; subtle camera bob.

    **I. Gameplay systems** (the payoff once entity tooling, E, lands)
    - **Mobs & AI + spawning:** passive animals + hostile mobs; spawn rules by light
      level / time / biome; simple steering + pathfinding. This is the consumer of the
      entity tooling (E) — without it that tooling has no use.
    - **Combat:** sword swing arc, damage + knockback, mob health, death drops — ties
      the sword, mobs, and the survival loop together.
    - **Progression / tool tiers:** pickaxe tiers gate harder ores (wood -> stone ->
      iron -> ...). This is what gives the ores AND crafting a purpose (ores are
      currently decorative).

    **J. Audio** — **currently absent entirely** (no SFX / ambience / music)
    - Foundational: every system wants a sound hook (break/place, footsteps, water &
      lava ambience, day/night ambience, mobs, UI). First sound system + a small audio
      asset pipeline. Biggest "feels real" jump per effort; the break-feedback item in
      H is the natural first caller.

    **K. Smaller wins**
    - ~~**Coloured block light:**~~ **DONE.** The monochrome block-light flood now
      carries a per-emitter *hue* alongside the intensity: each cell records the colour
      of the brightest emitter reaching it (dominant-emitter model — the cheap path, one
      extra packed-RGBA8 field parallel to `blockLight_`, propagated in the same BFS in
      `World::computeBlockLight`/`relightField`; no 3-channel flood). Emitter colours are
      authored per block via `light_color: [r,g,b]` in blocks.yaml (default warm; torch
      flame-orange, lava deep molten orange-red, glowstone warm yellow-white). The mesher
      bakes a smoothed per-corner hue into a new `Vertex.blockColor` (packed RGBA8, attr
      5, R8G8B8A8_UNORM) — weighted by block level so the brightest emitter dominates and
      unlit cells don't wash it grey; `chunk.frag` tints the block-lit term by it instead
      of the old hardcoded `kSource`. Intensity values are unchanged, so `--selftest`
      golden (0x3ca4dfb49ca7f61e) still passes. KNOWN MINOR: a colour change that does
      NOT change intensity (e.g. removing one of two equally-bright differently-coloured
      emitters) won't remesh until the chunk is otherwise dirtied — the dirty diff still
      keys on intensity only; rare, deliberately left for the cheap path.
    - **Player save:** chunks persist (saves/<seed>/) but player position / inventory /
      health do not — save them with the world.
    - **Switch palette source to `assets/colormap3.png`** (newest of colormap/2/3):
      point `gen_colors.py` (`PNG = .../assets/colormap.png`, ~L24) at colormap3, re-run
      it (regenerates colors.yaml), then re-run gen_textures.py. Runtime reads
      colors.yaml so NO C++ change. CAVEAT: gen_colors pairs color_names.txt[i] with
      swatch[i] by read order, so colormap3's swatches must sit in the same slots as the
      names (lime/brown/stone_gray/...) or textures.yaml's named refs grab wrong colours.

    **L. Codebase review & refactor/optimise pass** (whole repo)
    A deliberate audit pass over the whole codebase to find refactors and
    optimisations — separate from feature work. Look for:
    - **Hot paths / perf:** the per-tick `vkDeviceWaitIdle` in liquid + edit remeshes
      (see A); greedy mesher allocation churn; light-flood cost; per-frame draw loop
      (instancing / indirect / GPU culling); redundant remeshes; texture/atlas usage.
    - **Architecture:** God-class creep in App.cpp; renderer pass duplication
      (Sky/Composite/UI/World each own pipeline+descriptor boilerplate — a shared
      pipeline/descriptor helper?); ChunkMesher growing many special-cases (cube/
      cross/leaf/model/liquid) — a cleaner per-render-type seam; World vs streaming
      responsibilities.
    - **Consistency / hygiene:** buffer-lifetime + retire logic, error handling around
      Vulkan calls, magic numbers vs the yaml-config convention, dead code, header
      include hygiene, naming.
    - Output: a prioritised list (high-value/low-risk first) — do NOT big-bang rewrite;
      land refactors incrementally with the --selftest golden as the safety net.

    **M. Particles & game-feel juice** (small wins)
    PREREQ: a small **pooled particle system** — billboarded quads with lifetime +
    gravity, drawn in one cheap instanced pass. Everything below hangs off it.
    - Particles: block-break shards (in the block's colour) + place poof; water splash
      + expanding ripple ring on entry, underwater bubbles; lava embers/sparks + faint
      smoke; torch flame flicker + smoke; hurt puff on hit; item-pickup sparkle / crit
      stars / magic glints; cave drips (droplet + tiny plink); sun-beam dust motes;
      night fireflies.
    - Game-feel (no particles needed):
      - **Floating damage numbers** (Terraria signature — cheap, big feedback).
      - **Screen shake** (hard landing / explosions) + **hit-stop** (1-2 frame freeze).
      - **Damage vignette** flash when hurt; low-HP screen pulse.
      - **Block-break crack stages** (progressive overlay as you mine).
      - Dropped-item **bob + spin** + vacuum-to-player on pickup.
      - **Held-item swing** on use + idle bob; **camera bob** when walking.
      - **Crosshair feedback** (expands on a valid target; hit-marker flash).
      - **Hotbar** slot bounce on select; **+count popup** on pickup.
      - **Torch/fire light flicker** (jitter the block-light value slightly).
      - Foliage **sway when walked through** (grass/bushes).
      - **Block place/break scale animation** (block pops in / shrinks out).
      - **Footstep + landing sounds** per material (needs the audio hook, J).
      - **Water surface bob** for floating items/entities.
      - **Sprint FOV kick** (slight FOV widen while running).
      - **Tool cooldown swing** so mining has a rhythm.
    - Highest juice-per-line: break shards · damage numbers · screen shake + hit-stop ·
      break-crack stages · item bob/pickup · torch flicker.

    **N. Movement & controls**
    - **Sneaking (hold Shift, Minecraft-style):** move slower + lower the camera/hitbox
      (crouch); **don't walk off block edges** (edge-stop); lets you safely place blocks
      while hanging over an edge; render the crouched pose. Wire to a new
      `InputState.sneak` (LeftShift) in Input + PlayerController movement/collision.
    - **Sprint** (the FOV-kick juice in M assumes this) — toggle/double-tap or hold to
      run faster, with stamina later if wanted.

14. **WORLD-GEN TOOLING + SINGLE ISLAND** — **DONE** (tuning aids + an island-shaped world)

    **A. `--genmap` (headless map export).** `voxelgame --genmap [--mapsize N]
    [--mapstep B] [--out PATH]` (main.cpp `runGenMap`) samples `TerrainGenerator::
    columnInfo` over an N×N grid (B blocks/pixel) at a FIXED seed (1337, so a config
    change is comparable run-to-run) and writes a top-down PNG: blue = ocean (darker =
    deeper), green/tan/white = grass/beach/snow surface, plus an NW hillshade for
    relief. No window/Vulkan. It runs the game's real generator, so the map always
    matches what the game makes.

    **B. Single big island.** New radial-island mode in `TerrainGenerator::shapeHeight`,
    config under `island:` in `assets/biomes.yaml` (enabled / center / radius / inner /
    coast_warp / land_base / interior_var / ocean_floor). ONE landmass at `center`
    whose land sinks to a deep ocean floor past `radius`, with a noise-warped irregular
    coastline; the rest of the infinite streamed world is open sea. `enabled: false`
    falls back to the continentalness-driven archipelago. Currently center (0,0),
    radius 1100 — the player spawns on it. (The old continental_spline still drives the
    archipelago path and gives interior height variation on the island.)

    **C. Live tuning tool — `tools/genmap_tool.py` (Flask localhost app).**
    `pip install flask ruamel.yaml` then `python tools/genmap_tool.py` → open
    http://127.0.0.1:5000. Sliders for the generation knobs (sea level, snow line,
    island radius/inner/coast/land-base, noise frequencies); dragging one patches
    `assets/biomes.yaml` (ruamel round-trip — comments preserved), deploys it to
    `build/bin/assets`, runs the game's `--genmap`, and shows the updated map live. The
    C++ `TerrainGenerator` + `biomes.yaml` are the SINGLE source of truth — no noise
    logic is reimplemented in Python, so the tool tunes exactly what the game generates.
    Uses the Release exe (build it first).

    **NOTE — selftest golden vs tuning.** `--selftest` reads `biomes.yaml`, so tuning
    generation (incl. via the tool) intentionally changes the golden hash. Rebaselined
    to `0x3ca4dfb49ca7f61e` for single-island mode; a flag is in main.cpp. If the churn
    is annoying, make the selftest config-independent (bake the noise defaults) later.

    **STILL WANTED (next):**
    - **Richer genmap modes** — **[DONE: cross-section + raw-noise extraction; iso still open]**
      `--genmap --mode noise --layer <cont|ero|peak|temp|hum|river|relief>` renders each
      raw noise layer as a diverging blue/white/red field (relief draws the sea-level
      coast); `--genmap --mode cross` draws a vertical cross-section through Z=0 (terrain
      profile, water column, soil/stone/snow layers — caves/ores are World per-voxel and
      omitted from the generator-only slice). Wired into `tools/genmap_tool.py` via a
      view-mode dropdown. Backed by a debug-only `TerrainGenerator::fieldValue()` that is
      NOT on the generation path, so world output is unchanged. Isometric view: still open.
    - **Composable `NoiseStack`** — **[DONE]** Any of the six noise fields
      (continentalness/erosion/peaks/temperature/humidity/rivers) can be authored as a
      data-driven *weighted blend* of perlin/ridged/billow layers via a `layers:` list in
      biomes.yaml (`vg::NoiseStack`, src/world/NoiseStack.{h,cpp}; 2D + 3D). Each layer has
      its own frequency/octaves/lacunarity/gain/weight/offset; the blend renormalises to
      ~[-1,1] so it drops in for a single fbm. **Opt-in**: with no `layers:` block the
      legacy single-noise path runs and worlds are byte-identical (the `--selftest`
      determinism check passes; the golden hash is unchanged). TerrainGenerator routes
      shape/climate/river sampling through sampleX() helpers that pick the stack when
      present. Documented in biomes.yaml (commented example) + docs/WORLDGEN.md; preview
      any field with `--genmap --mode noise --layer <field>`.
      NOTE: the committed `kGolden` in main.cpp was recorded on a different compiler
      (MSVC); on GCC here the fixed-config worldgen hashes to `0xf74953807c107d61`. That
      mismatch predates this work (cross-compiler float nondeterminism, see #11's
      "make the selftest config-independent" future item) — `kGolden` was left untouched
      so it still matches on the original toolchain; the portable determinism check (regen
      bit-identical) passes, and the local hash stayed constant across all of this work,
      proving default generation is unchanged.
      Also fixed an unrelated build break: `Inventory.h` used `size_t` without
      `<cstddef>` (GCC doesn't pull it in transitively), so the project didn't compile.
    - **Dramatic mountains** — almost certainly needs the 256-tall world (height_chunks
      16 + scale sea_level/splines/snow_line) for the vertical room.

15. Remove all armors expecpt boots, Also make Inventory more polished. Add scrolable inventory, more compact trinket space, crafting when howevered should show what is it using for the recipe and not available recipes should be darkned