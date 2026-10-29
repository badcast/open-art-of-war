#pragma once

#include <cmath>
#include <cstdint>

namespace OpenArtOfWar::MapNoise {

// Lightweight coherent gradient noise
inline float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float lerp(float a, float b, float t) {
    return a + t * (b - a);
}

inline float grad(int hash, float x, float y) {
    int h = hash & 7;
    float u = h < 4 ? x : y;
    float v = h < 4 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
}

inline float sampleNoise2D(float x, float y, uint32_t seed) {
    int X = static_cast<int>(std::floor(x)) & 255;
    int Y = static_cast<int>(std::floor(y)) & 255;

    x -= std::floor(x);
    y -= std::floor(y);

    float u = fade(x);
    float v = fade(y);

    auto hash = [seed](int ix, int iy) -> int {
        uint32_t n = static_cast<uint32_t>(ix + iy * 57) + seed * 1337u;
        n = (n << 13) ^ n;
        return (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff;
    };

    int aa = hash(X, Y);
    int ab = hash(X, Y + 1);
    int ba = hash(X + 1, Y);
    int bb = hash(X + 1, Y + 1);

    float res = lerp(lerp(grad(aa, x, y), grad(ba, x - 1, y), u),
                     lerp(grad(ab, x, y - 1), grad(bb, x - 1, y - 1), u), v);
    return (res + 1.0f) * 0.5f; // Normalize to [0, 1]
}

inline float octaveNoise2D(float x, float y, int octaves, float persistence, uint32_t seed) {
    float total = 0.0f;
    float frequency = 1.0f;
    float amplitude = 1.0f;
    float maxValue = 0.0f;

    for (int i = 0; i < octaves; ++i) {
        total += sampleNoise2D(x * frequency, y * frequency, seed + i * 31) * amplitude;
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
    }
    return total / maxValue;
}

} // namespace OpenArtOfWar::MapNoise
