#pragma once

#include "world/Feature.h"
#include "world/Hash.h"
#include "world/Noise.h"

#include <algorithm>
#include <cstdint>

namespace vg {

// -----------------------------------------------------------------------------
//  scatterFeaturesColumn — the ONE seam-safe feature placement pass for a single
//  world column. For every feature whose footprint can cover (wx,wz), gather the
//  covering origin cells, apply the placement gates (density · noise/mask dist ·
//  elevation · biome · slope · near-water), and emit each stamped block. A pure
//  function of (seed, coords), so the same column always grows the same flora no
//  matter which chunk streams in first.
//
//  Shared by World::generateColumn (emit -> write into the chunk stack) and the
//  headless voxel preview (emit -> paint into the preview grid), so the preview can
//  NEVER drift from the real placement. The column-info / surface-Y lookups are
//  passed as callables so each caller supplies its own memoised version.
//    colInfoAt(x,z) -> const ColumnInfo&   surfaceYAt(x,z) -> int
//    emit(int wy, uint16_t blockId, bool force)
//
//  Lives in its OWN header (not Feature.h) so that Hash.h — whose hash01/floordiv
//  would otherwise collide with the render files' local copies via World.h — stays
//  out of the widely-included graph. Only World.cpp and the genmap include this.
// -----------------------------------------------------------------------------
template <class ColInfoFn, class SurfFn, class EmitFn>
inline void scatterFeaturesColumn(const FeatureSet& feats, int wx, int wz, uint32_t seed,
                                  const Noise& featNoise, ColInfoFn colInfoAt,
                                  SurfFn surfaceYAt, EmitFn emit) {
    const auto& list = feats.all();
    for (std::size_t fi = 0; fi < list.size(); ++fi) {
        const Feature& ft = list[fi];
        if (ft.scatter.density <= 0.0f) continue;
        const int S = std::max(4, ft.scatter.spacing);
        const int reach = std::max({ft.anchor.x, ft.size.x - 1 - ft.anchor.x,
                                    ft.anchor.z, ft.size.z - 1 - ft.anchor.z});
        const int cellR = reach / S + 1;
        const uint32_t fsalt = seed ^ (0xF0000000u + static_cast<uint32_t>(fi) * 0x9e3779b9u);
        const int gx = floordiv(wx, S), gz = floordiv(wz, S);
        for (int cgz = gz - cellR; cgz <= gz + cellR; ++cgz)
            for (int cgx = gx - cellR; cgx <= gx + cellR; ++cgx) {
                if (hash01(cgx, cgz, fsalt ^ 0x1u) >= ft.scatter.density) continue;
                const int ox = cgx * S + static_cast<int>(hash01(cgx, cgz, fsalt ^ 0x2u) * (S - 1));
                const int oz = cgz * S + static_cast<int>(hash01(cgx, cgz, fsalt ^ 0x3u) * (S - 1));
                if (ft.scatter.dist == FeatureScatter::Dist::Noise &&
                    featNoise.perlin(static_cast<float>(ox) * ft.scatter.noiseFreq,
                                     static_cast<float>(oz) * ft.scatter.noiseFreq)
                        <= ft.scatter.noiseThresh) continue;
                if (!ft.scatter.mask.empty() &&
                    hash01(cgx, cgz, fsalt ^ 0x6du) >=
                        ft.scatter.mask.weight(static_cast<float>(ox), static_cast<float>(oz))) continue;
                const int slx = (wx - ox) + ft.anchor.x;
                const int slz = (wz - oz) + ft.anchor.z;
                if (slx < 0 || slz < 0 || slx >= ft.size.x || slz >= ft.size.z) continue;
                const auto& oc = colInfoAt(ox, oz);
                const bool submerged = oc.height < oc.waterLevel;
                if (ft.scatter.onWater) { if (!submerged) continue; }
                else if (ft.scatter.surface && submerged) continue;
                const int relEl = oc.height - oc.waterLevel;
                if (relEl < ft.scatter.minElevation || relEl > ft.scatter.maxElevation) continue;
                if (!ft.scatter.biomeIds.empty() &&
                    std::find(ft.scatter.biomeIds.begin(), ft.scatter.biomeIds.end(),
                              oc.biome) == ft.scatter.biomeIds.end()) continue;
                if (ft.scatter.nearWater > 0) {
                    const int nw = ft.scatter.nearWater;
                    const int dxs[4] = {nw, -nw, 0, 0}, dzs[4] = {0, 0, nw, -nw};
                    bool found = false;
                    for (int k = 0; k < 4 && !found; ++k) {
                        const auto& nc = colInfoAt(ox + dxs[k], oz + dzs[k]);
                        if (nc.height < nc.waterLevel) found = true;
                    }
                    if (!found) continue;
                }
                const int oh = ft.scatter.onWater ? oc.waterLevel : surfaceYAt(ox, oz);
                if (ft.scatter.minSlope > 0 || ft.scatter.maxSlope < 100000) {
                    int lo = oh, hi = oh;
                    const int dxs[4] = {3, -3, 0, 0}, dzs[4] = {0, 0, 3, -3};
                    for (int k = 0; k < 4; ++k) {
                        const int s = surfaceYAt(ox + dxs[k], oz + dzs[k]);
                        lo = std::min(lo, s); hi = std::max(hi, s);
                    }
                    const int slope = hi - lo;
                    if (slope < ft.scatter.minSlope || slope > ft.scatter.maxSlope) continue;
                }
                const uint32_t oseed = static_cast<uint32_t>(cgx) * 73856093u ^
                                       static_cast<uint32_t>(cgz) * 19349663u ^
                                       (fsalt * 2654435761u);
                for (int ly = 0; ly < ft.size.y; ++ly) {
                    const int wy = oh + (ly - ft.anchor.y);
                    const Feature::Cell c = ft.at(oseed, slx, ly, slz, featNoise, wx, wy, wz);
                    if (c.id != Feature::kSkip) emit(wy, c.id, c.force);
                }
            }
    }
}

} // namespace vg
