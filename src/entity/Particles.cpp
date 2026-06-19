/**
 * @file Particles.cpp
 * @brief ParticleEffect YAML loader, burst spawner, and per-frame physics update.
 * @see docs/CODE_INDEX.md
 */

#include "entity/Particles.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vg {

/// Parse a .prtcl YAML file into a ParticleEffect; throws std::runtime_error on failure.
ParticleEffect ParticleEffect::load(const std::string& path) {
    ParticleEffect fx;
    YAML::Node n;
    try {
        n = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("ParticleEffect: failed to load '" + path + "': " + e.what());
    }
    auto f = [&](const char* k, float d) { return n[k] ? n[k].as<float>() : d; };
    auto pair = [&](const char* k, float& lo, float& hi) {
        if (n[k] && n[k].IsSequence() && n[k].size() >= 2) {
            lo = n[k][0].as<float>();
            hi = n[k][1].as<float>();
        }
    };
    if (n["name"]) fx.name = n["name"].as<std::string>();
    if (n["texture"]) fx.texture = n["texture"].as<std::string>();
    if (n["count"]) fx.count = std::max(0, n["count"].as<int>());
    fx.gravity     = f("gravity", fx.gravity);
    fx.drag        = f("drag", fx.drag);
    fx.spawnRadius = f("spawn_radius", fx.spawnRadius);
    fx.upBias      = f("up_bias", fx.upBias);
    fx.sizeEnd     = f("size_end", fx.sizeEnd);
    pair("speed", fx.speedMin, fx.speedMax);
    pair("lifetime", fx.lifeMin, fx.lifeMax);
    pair("size", fx.sizeMin, fx.sizeMax);
    pair("spin", fx.spinMin, fx.spinMax);
    return fx;
}

float Particles::rand01() {
    // xorshift32 -> [0,1). Good enough for scatter; avoids a <random> dependency.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

/// Spawn one burst of `fx` centred at `center`, sampling texture-array `layer`.
/// Drops the oldest particle when the pool is at kMaxParticles.
void Particles::spawnEffect(const ParticleEffect& fx, const glm::vec3& center,
                            uint32_t layer) {
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    for (int i = 0; i < fx.count; ++i) {
        if (particles_.size() >= kMaxParticles) {
            particles_.erase(particles_.begin()); // drop the oldest
        }
        Particle p;
        p.layer = layer;
        p.pos = center + glm::vec3(rand01() - 0.5f, rand01() - 0.5f, rand01() - 0.5f) *
                             (fx.spawnRadius * 2.0f);
        // Outward direction in the XZ plane + a slight up, scaled by the speed range.
        const float ang = rand01() * 6.2831853f;
        const float spd = lerp(fx.speedMin, fx.speedMax, rand01());
        p.vel = glm::vec3(std::cos(ang) * spd, fx.upBias + rand01() * spd,
                          std::sin(ang) * spd);
        p.size0   = lerp(fx.sizeMin, fx.sizeMax, rand01());
        p.size    = p.size0;
        p.sizeEnd = fx.sizeEnd;
        p.maxLife = lerp(fx.lifeMin, fx.lifeMax, rand01());
        p.life    = p.maxLife;
        p.gravity = fx.gravity;
        p.drag    = fx.drag;
        p.spin    = rand01() * 6.2831853f;
        p.spinVel = lerp(fx.spinMin, fx.spinMax, rand01());
        p.uv0     = glm::vec2(rand01() * 0.75f, rand01() * 0.75f); // a 1/4 chip
        particles_.push_back(p);
    }
}

/// Advance all particles: gravity, drag, size interpolation, ground settle, then expire aged-out ones.
void Particles::update(float dt, const SolidFn& solid) {
    for (Particle& p : particles_) {
        p.life -= dt;
        p.spin += p.spinVel * dt;
        p.vel.y += p.gravity * dt;
        if (p.drag > 0.0f) {
            const float d = std::min(1.0f, p.drag * dt);
            p.vel.x -= p.vel.x * d;
            p.vel.z -= p.vel.z * d;
        }
        // Shrink over life (t: 0 at spawn -> 1 at death).
        const float t = p.maxLife > 0.0f ? 1.0f - p.life / p.maxLife : 1.0f;
        p.size = p.size0 * (1.0f + (p.sizeEnd - 1.0f) * std::clamp(t, 0.0f, 1.0f));

        glm::vec3 next = p.pos + p.vel * dt;
        if (solid && p.vel.y < 0.0f) {
            const int bx = static_cast<int>(std::floor(next.x));
            const int bz = static_cast<int>(std::floor(next.z));
            const int by = static_cast<int>(std::floor(next.y - p.size));
            if (solid(bx, by, bz)) {
                next.y = static_cast<float>(by) + 1.0f + p.size;
                p.vel.y = 0.0f;
                const float gd = std::min(1.0f, 8.0f * dt);
                p.vel.x -= p.vel.x * gd;
                p.vel.z -= p.vel.z * gd;
            }
        }
        p.pos = next;
    }
    particles_.erase(
        std::remove_if(particles_.begin(), particles_.end(),
                       [](const Particle& p) { return p.life <= 0.0f; }),
        particles_.end());
}

} // namespace vg
