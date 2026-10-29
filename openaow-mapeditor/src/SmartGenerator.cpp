#include "SmartGenerator.hpp"
#include "MapNoise.hpp"
#include "core/Utility.hpp"
#include "game/TileCollision.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

namespace OpenArtOfWar {

namespace {

using MapNoise::octaveNoise2D;
using Rng = Utility::Random;

constexpr float kPi = 3.14159265f;
constexpr int kBaseSize = 10;      // a base occupies 10x10 tiles
constexpr int kBaseMargin = 3;     // kept clear around a base

// ---- Ground tiles --------------------------------------------------------------------------------------------
// Passability follows the tile sprite (TileCollision): bare earth is impassable plateau, jungle tiles are
// impassable undergrowth, grass, transition and rubble tiles are open. A tile holds four quadrants (NW, NE, SW,
// SE) that are either grass or earth; the transition pools below are indexed by those quadrants as bits
// (bit0 = NW, bit1 = NE, bit2 = SW, bit3 = SE, set = earth). They were read from the tile pictures of both tilesets.
struct TileSet {
    int grass[2];                       // inclusive tile range of the plain grass variants
    int meadow[2];
    int altGrass[2];
    int earth[2];                       // bare earth, impassable
    std::vector<int> jungle;            // impassable undergrowth
    std::vector<int> rubble;            // rough ground, open
    std::array<std::vector<int>, 16> transition;
};

std::vector<int> tilesIn(int tilesetId, int first, int last, bool blocked) {
    std::vector<int> tiles;
    for (int tile = first; tile <= last; ++tile) {
        if (TileCollision::isBlocked(tilesetId, tile) == blocked) tiles.push_back(tile);
    }
    return tiles;
}

TileSet makeTileSet(int tilesetId) {
    TileSet set;
    std::array<std::vector<int>, 16> transition;
    if (tilesetId == 1) {
        set = {{0, 9}, {10, 17}, {18, 25}, {33, 35}, tilesIn(1, 238, 243, true), tilesIn(1, 191, 227, false), {}};
        transition = {{{}, {64, 86, 109}, {65, 85, 108}, {57, 58, 59, 80, 81, 82, 103, 104, 105},
                       {67, 84, 87, 107, 110}, {51, 52, 53, 74, 76, 97, 99}, {}, {60},
                       {66, 83, 106}, {}, {49, 50, 71, 72, 73, 94, 95, 96}, {61, 88, 111},
                       {54, 55, 56, 77, 78, 79, 100, 101, 102}, {63, 90, 113}, {62, 89, 112}, {}}};
    } else {
        set = {{0, 9}, {10, 17}, {19, 25}, {26, 33}, tilesIn(0, 235, 240, true), tilesIn(0, 211, 234, false), {}};
        transition = {{{}, {67, 88, 150, 199, 208}, {68, 87, 151, 203, 204}, {60, 61, 62, 82, 83, 84, 144, 145, 195},
                       {70, 86, 89, 153, 200}, {54, 55, 56, 77, 78, 139, 140, 141, 197}, {}, {63, 146},
                       {69, 85, 152, 207}, {}, {52, 53, 74, 75, 76, 137, 138, 198}, {64, 90, 147},
                       {57, 58, 59, 79, 80, 81, 142, 143, 196}, {66, 92, 149}, {65, 91, 148}, {}}};
    }
    // Edge tiles stay open: only the pure earth tiles are plateau
    for (size_t bits = 0; bits < transition.size(); ++bits) {
        for (int tile : transition[bits]) {
            if (!TileCollision::isBlocked(tilesetId, tile)) set.transition[bits].push_back(tile);
        }
        if (set.transition[bits].empty() && !transition[bits].empty()) set.transition[bits] = transition[bits];
    }
    return set;
}

const TileSet& tileSet(int tilesetId) {
    static const TileSet sets[2] = {makeTileSet(0), makeTileSet(1)};
    return sets[tilesetId == 1 ? 1 : 0];
}

// ---- Decorations ---------------------------------------------------------------------------------------------
// Footprints (width, height) of the decoration types 0..94; the same in both tilesets.
constexpr int kDecorTypes = 95;
constexpr int kDecorSize[kDecorTypes][2] = {
    {2, 2}, {2, 2}, {1, 2}, {1, 3}, {2, 2}, {2, 3}, {2, 3}, {1, 3},
    {1, 3}, {2, 3}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 3}, {2, 3},
    {2, 3}, {2, 3}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 3}, {2, 3},
    {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 1}, {2, 1}, {2, 2}, {3, 2},
    {2, 1}, {2, 2}, {3, 2}, {3, 2}, {4, 2}, {3, 3}, {2, 2}, {2, 2},
    {2, 2}, {2, 2}, {1, 2}, {2, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1},
    {2, 1}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
    {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {3, 3}, {3, 3}, {2, 2},
    {2, 2}, {2, 2}, {2, 2}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1},
    {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1},
    {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1},
    {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1},
};

struct TypeRange { int first; int last; };
constexpr TypeRange kBoulders = {0, 11};     // tall rock outcrops
constexpr TypeRange kCliffs = {12, 25};      // big cliff clusters
constexpr TypeRange kRubble = {26, 37};      // low stones on bare earth
constexpr TypeRange kFerns = {38, 49};       // undergrowth
constexpr TypeRange kTrees = {50, 66};
constexpr TypeRange kSmall = {67, 94};       // 1x1 bushes and stones

enum class Context { Grass, Dirt, Blocked };

struct Weighted { TypeRange range; int weight; };

// What the original maps put on each kind of ground
const std::vector<Weighted>& typePool(Context ctx) {
    static const std::vector<Weighted> grass = {{kTrees, 45}, {kFerns, 15}, {kSmall, 40}};
    static const std::vector<Weighted> dirt = {{kBoulders, 15}, {kCliffs, 25}, {kRubble, 30}, {kSmall, 30}};
    static const std::vector<Weighted> blocked = {{kTrees, 50}, {kFerns, 30}, {kCliffs, 20}};
    return ctx == Context::Grass ? grass : (ctx == Context::Dirt ? dirt : blocked);
}

// ---- Symmetry ------------------------------------------------------------------------------------------------
struct Symmetry {
    GeneratorSymmetry kind{GeneratorSymmetry::None};
    int w{0};
    int h{0};

    bool active() const { return kind != GeneratorSymmetry::None; }
    // Objects keep their orientation under flips only, so a transposed map gets no mirrored decorations
    bool mirrorsObjects() const { return active() && kind != GeneratorSymmetry::Diagonal; }

    void point(float x, float y, float& ox, float& oy) const {
        switch (kind) {
            case GeneratorSymmetry::Rotational180: ox = w - x; oy = h - y; break;
            case GeneratorSymmetry::Horizontal: ox = w - x; oy = y; break;
            case GeneratorSymmetry::Vertical: ox = x; oy = h - y; break;
            case GeneratorSymmetry::Diagonal: ox = y * w / h; oy = x * h / w; break;
            default: ox = x; oy = y; break;
        }
    }

    // Top-left corner of the mirror image of a rw x rh rectangle
    void rect(int x, int y, int rw, int rh, int& ox, int& oy) const {
        float cx, cy;
        point(x + rw * 0.5f, y + rh * 0.5f, cx, cy);
        ox = static_cast<int>(std::lround(cx - rw * 0.5f));
        oy = static_cast<int>(std::lround(cy - rh * 0.5f));
    }
};

class Builder {
public:
    Builder(MapDocument& doc, const GeneratorParams& params, int w, int h, int tilesetId)
        : m_doc(doc), m_params(params), m_w(w), m_h(h), m_tilesetId(tilesetId), m_tileset(tileSet(tilesetId)),
          m_rng(params.seed), m_seed(params.seed) {
        m_sym.kind = params.symmetry;
        m_sym.w = w;
        m_sym.h = h;
        const int tiles = w * h;
        m_reserved.assign(tiles, 0);
        m_obstacle.assign(tiles, 0);
        m_blocked.assign(tiles, 0);
        m_decorCover.assign(tiles, 0);
        m_dirt.assign(tiles * 4, 0);
    }

    void run() {
        layoutBases();
        carveRoads();
        buildDirt();
        buildObstacles();
        resolveTiles();
        setStartPositions();
        placeDecorations();
        finalize();
    }

private:
    // ---- helpers ---------------------------------------------------------------------------------------------
    int idx(int x, int y) const { return y * m_w + x; }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < m_w && y < m_h; }
    int edgeDistance(int x, int y) const { return std::min(std::min(x, y), std::min(m_w - 1 - x, m_h - 1 - y)); }
    int pick(int lo, int hi) { return m_rng.range(lo, hi); }
    int pick(const int (&range)[2]) { return m_rng.range(range[0], range[1]); }
    bool chance(float p) { return m_rng.unit() < p; }

    // Noise symmetric under the chosen symmetry (the original point and its mirror image are averaged)
    float field(float x, float y, float frequency, int octaves, uint32_t salt) const {
        float value = octaveNoise2D(x * frequency, y * frequency, octaves, 0.5f, m_seed + salt);
        if (m_sym.active()) {
            float mx, my;
            m_sym.point(x, y, mx, my);
            value = (value + octaveNoise2D(mx * frequency, my * frequency, octaves, 0.5f, m_seed + salt)) * 0.5f;
        }
        return value;
    }

    // Value above which `fraction` of the samples lie
    static float quantile(std::vector<float> values, float fraction) {
        if (values.empty()) return 2.0f;
        const size_t pos = static_cast<size_t>(std::clamp(1.0f - fraction, 0.0f, 1.0f) * (values.size() - 1));
        std::nth_element(values.begin(), values.begin() + pos, values.end());
        return values[pos];
    }

    void reserveRect(int x0, int y0, int x1, int y1) {
        for (int y = std::max(0, y0); y <= std::min(m_h - 1, y1); ++y) {
            for (int x = std::max(0, x0); x <= std::min(m_w - 1, x1); ++x) m_reserved[idx(x, y)] = 1;
        }
    }

    bool inBaseZone(int x, int y) const {
        for (const auto& b : m_bases) {
            if (x >= b.x - kBaseMargin && x < b.x + kBaseSize + kBaseMargin &&
                y >= b.y - kBaseMargin && y < b.y + kBaseSize + kBaseMargin) {
                return true;
            }
        }
        return false;
    }

    // ---- 1. bases --------------------------------------------------------------------------------------------
    struct Point { int x; int y; };

    void layoutBases() {
        const int maxX = m_w - kBaseSize - kBaseMargin - 1;
        const int maxY = m_h - kBaseSize - kBaseMargin - 1;
        int px = 6, py = m_h - 14;
        if (m_params.symmetry == GeneratorSymmetry::Horizontal) {
            px = 5;
            py = m_h / 2 - kBaseSize / 2;
        } else if (m_params.symmetry == GeneratorSymmetry::Vertical) {
            px = m_w / 2 - kBaseSize / 2;
            py = m_h - 14;
        }
        px = std::clamp(px, kBaseMargin + 1, maxX);
        py = std::clamp(py, kBaseMargin + 1, maxY);
        int ex = m_w - px - kBaseSize;
        int ey = m_h - py - kBaseSize;
        if (m_params.symmetry == GeneratorSymmetry::Horizontal) ey = py;
        if (m_params.symmetry == GeneratorSymmetry::Vertical) ex = px;
        m_bases = {{px, py}, {std::clamp(ex, kBaseMargin + 1, maxX), std::clamp(ey, kBaseMargin + 1, maxY)}};
        for (const auto& b : m_bases) {
            reserveRect(b.x - kBaseMargin, b.y - kBaseMargin, b.x + kBaseSize + kBaseMargin - 1,
                        b.y + kBaseSize + kBaseMargin - 1);
        }
    }

    // ---- 2. roads and clearings ------------------------------------------------------------------------------
    // Keeps a corridor free of obstacles, plateau and decorations
    void stampOne(float x, float y, float clearRadius) {
        for (int ty = static_cast<int>(y - clearRadius) - 1; ty <= static_cast<int>(y + clearRadius) + 1; ++ty) {
            for (int tx = static_cast<int>(x - clearRadius) - 1; tx <= static_cast<int>(x + clearRadius) + 1; ++tx) {
                if (!inside(tx, ty)) continue;
                const float dx = tx + 0.5f - x, dy = ty + 0.5f - y;
                if (dx * dx + dy * dy <= clearRadius * clearRadius) m_reserved[idx(tx, ty)] = 1;
            }
        }
    }

    void stamp(float x, float y, float clearRadius) {
        stampOne(x, y, clearRadius);
        m_roadPoints.push_back({x, y});
        if (m_sym.active()) {
            float mx, my;
            m_sym.point(x, y, mx, my);
            stampOne(mx, my, clearRadius);
            m_roadPoints.push_back({mx, my});
        }
    }

    // A winding strip between two points
    void walk(float ax, float ay, float bx, float by, float clearRadius) {
        const float length = std::hypot(bx - ax, by - ay);
        if (length < 1.0f) return;
        const float nx = -(by - ay) / length, ny = (bx - ax) / length;
        const float phase = m_rng.range(0.0f, 2.0f * kPi);
        const float amplitude = m_rng.range(1.0f, 3.5f) * std::min(1.0f, length / 25.0f);
        const float waves = m_rng.range(1.0f, 2.5f);
        const int steps = std::max(2, static_cast<int>(length / 0.6f));
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / steps;
            const float wobble = std::sin(phase + t * waves * kPi) * amplitude * std::sin(t * kPi);
            stamp(ax + (bx - ax) * t + nx * wobble, ay + (by - ay) * t + ny * wobble, clearRadius);
        }
    }

    void carveRoads() {
        const float p0x = m_bases[0].x + kBaseSize * 0.5f, p0y = m_bases[0].y + kBaseSize * 0.5f;
        const float p1x = m_bases[1].x + kBaseSize * 0.5f, p1y = m_bases[1].y + kBaseSize * 0.5f;
        const float cx = m_w * 0.5f, cy = m_h * 0.5f;
        const float wx = std::clamp(cx + m_rng.signedUnit() * m_w * 0.22f, 8.0f, m_w - 8.0f);
        const float wy = std::clamp(cy + m_rng.signedUnit() * m_h * 0.22f, 8.0f, m_h - 8.0f);
        walk(p0x, p0y, wx, wy, 2.3f);
        walk(wx, wy, cx, cy, 2.3f);
        if (!m_sym.active()) {
            const float w2x = std::clamp(cx + m_rng.signedUnit() * m_w * 0.22f, 8.0f, m_w - 8.0f);
            const float w2y = std::clamp(cy + m_rng.signedUnit() * m_h * 0.22f, 8.0f, m_h - 8.0f);
            walk(cx, cy, w2x, w2y, 2.3f);
            walk(w2x, w2y, p1x, p1y, 2.3f);
        }

        // Open clearings (room for expansions and flanking) joined to the road
        const int clearings = std::clamp(m_w * m_h / 1100, 2, 8);
        std::vector<Point> placed;
        for (int attempt = 0; attempt < clearings * 12 && static_cast<int>(placed.size()) < clearings; ++attempt) {
            const int x = pick(8, m_w - 9), y = pick(8, m_h - 9);
            bool ok = true;
            for (const auto& b : m_bases) {
                if (std::hypot(x - (b.x + kBaseSize * 0.5f), y - (b.y + kBaseSize * 0.5f)) < 15.0f) ok = false;
            }
            for (const auto& c : placed) {
                if (std::hypot(float(x - c.x), float(y - c.y)) < 12.0f) ok = false;
            }
            if (!ok) continue;
            placed.push_back({x, y});
            const float radius = m_rng.range(3.0f, 5.0f);
            float nearestX = cx, nearestY = cy, best = 1e9f;
            for (const auto& pt : m_roadPoints) {
                const float d = std::hypot(pt.x - x, pt.y - y);
                if (d < best) { best = d; nearestX = pt.x; nearestY = pt.y; }
            }
            walk(x, y, nearestX, nearestY, 1.7f);
            stamp(static_cast<float>(x), static_cast<float>(y), radius);
        }
    }

    // ---- 3. ground (grass / dirt on a half-tile grid) --------------------------------------------------------
    void buildDirt() {
        const int sw = m_w * 2, sh = m_h * 2;
        std::vector<float> value(sw * sh);
        for (int sy = 0; sy < sh; ++sy) {
            for (int sx = 0; sx < sw; ++sx) {
                value[sy * sw + sx] = field((sx + 0.5f) * 0.5f, (sy + 0.5f) * 0.5f, 0.06f, 3, 101);
            }
        }
        static constexpr float kDirtShare[3] = {0.08f, 0.16f, 0.26f};
        const float threshold = quantile(value, kDirtShare[std::clamp(m_params.dirtLevel, 0, 2)]);
        for (int i = 0; i < sw * sh; ++i) m_dirt[i] = value[i] > threshold ? 1 : 0;

        // Majority filter: drops specks so patches read like the hand-made ones
        std::vector<uint8_t> smoothed(m_dirt.size());
        for (int sy = 0; sy < sh; ++sy) {
            for (int sx = 0; sx < sw; ++sx) {
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = std::clamp(sx + dx, 0, sw - 1), ny = std::clamp(sy + dy, 0, sh - 1);
                        count += m_dirt[ny * sw + nx];
                    }
                }
                smoothed[sy * sw + sx] = count >= 5 ? 1 : 0;
            }
        }
        m_dirt = smoothed;

        for (int sy = 0; sy < sh; ++sy) {
            for (int sx = 0; sx < sw; ++sx) {
                if (m_reserved[idx(sx / 2, sy / 2)]) m_dirt[sy * sw + sx] = 0;
            }
        }

        // No tile shows a grass/dirt checkerboard: those two quadrant patterns have no picture, merge them into dirt
        for (int y = 0; y < m_h; ++y) {
            for (int x = 0; x < m_w; ++x) {
                const int bits = quadrants(x, y);
                if (bits == 6 || bits == 9) {
                    for (int q = 0; q < 4; ++q) m_dirt[(y * 2 + q / 2) * sw + x * 2 + q % 2] = 1;
                }
            }
        }
    }

    int quadrants(int x, int y) const {
        const int sw = m_w * 2;
        return m_dirt[(y * 2) * sw + x * 2] | (m_dirt[(y * 2) * sw + x * 2 + 1] << 1) |
               (m_dirt[(y * 2 + 1) * sw + x * 2] << 2) | (m_dirt[(y * 2 + 1) * sw + x * 2 + 1] << 3);
    }

    // ---- 4. blocked terrain ----------------------------------------------------------------------------------
    void buildObstacles() {
        std::vector<float> value(m_w * m_h);
        std::vector<float> eligible;
        eligible.reserve(value.size());
        for (int y = 0; y < m_h; ++y) {
            for (int x = 0; x < m_w; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                value[idx(x, y)] = field(px, py, 0.07f, 3, 303) * 0.7f + field(px, py, 0.18f, 2, 404) * 0.3f;
                if (!m_reserved[idx(x, y)]) eligible.push_back(value[idx(x, y)]);
            }
        }
        static constexpr float kShare[3] = {0.05f, 0.10f, 0.16f};
        const float threshold = quantile(eligible, kShare[std::clamp(m_params.obstacleLevel, 0, 2)]);
        for (int y = 0; y < m_h; ++y) {
            for (int x = 0; x < m_w; ++x) {
                const bool wall = m_params.borderForest && edgeDistance(x, y) < 2;
                if (!m_reserved[idx(x, y)] && (wall || value[idx(x, y)] > threshold)) m_obstacle[idx(x, y)] = 1;
            }
        }
    }

    // ---- 5. tiles --------------------------------------------------------------------------------------------
    int dirtQuadrants(int x, int y) const {
        const int bits = quadrants(x, y);
        return (bits & 1) + ((bits >> 1) & 1) + ((bits >> 2) & 1) + ((bits >> 3) & 1);
    }

    int groundTile(int x, int y) {
        const int bits = quadrants(x, y);
        if (bits == 15) return pick(m_tileset.earth);
        if (bits != 0) {
            const auto& pool = m_tileset.transition[bits];
            return pool.empty() ? pick(m_tileset.earth) : pool[pick(0, static_cast<int>(pool.size()) - 1)];
        }
        if (inBaseZone(x, y)) return pick(m_tileset.meadow);
        // Rough ground (open) in patches
        if (!m_tileset.rubble.empty() && field(x + 0.5f, y + 0.5f, 0.09f, 2, 505) > m_roughThreshold) {
            return m_tileset.rubble[pick(0, static_cast<int>(m_tileset.rubble.size()) - 1)];
        }
        const float meadow = field(x + 0.5f, y + 0.5f, 0.05f, 2, 202);
        if (meadow > 0.60f) return pick(m_tileset.meadow);
        if (meadow < 0.32f) return pick(m_tileset.altGrass);
        return pick(m_tileset.grass);
    }

    int jungleTile() { return m_tileset.jungle[pick(0, static_cast<int>(m_tileset.jungle.size()) - 1)]; }

    void resolveTiles() {
        // About a tenth of the open ground is rough
        std::vector<float> rough;
        for (int y = 0; y < m_h; ++y) {
            for (int x = 0; x < m_w; ++x) rough.push_back(field(x + 0.5f, y + 0.5f, 0.09f, 2, 505));
        }
        m_roughThreshold = quantile(rough, 0.10f);

        for (int y = 0; y < m_h; ++y) {
            for (int x = 0; x < m_w; ++x) {
                const int tile = m_obstacle[idx(x, y)] ? jungleTile() : groundTile(x, y);
                const bool blocked = TileCollision::isBlocked(m_tilesetId, tile);
                m_doc.setTile(x, y, static_cast<uint8_t>(tile));
                m_doc.setPassable(x, y, !blocked);
                m_blocked[idx(x, y)] = blocked ? 1 : 0;
            }
        }
    }

    void setStartPositions() {
        const Point& p = m_bases[0];
        const Point& e = m_bases[1];
        m_doc.setPlayerStart(p.x + 1, p.y + 2);
        m_doc.setEnemyStart(e.x + 1, e.y + 2);
    }

    // ---- 6. decorations --------------------------------------------------------------------------------------
    bool fits(int type, int x, int y, bool onBlocked, bool onReserved = false) const {
        const int dw = kDecorSize[type][0], dh = kDecorSize[type][1];
        if (x < 0 || y < 0 || x + dw > m_w || y + dh > m_h) return false;
        for (int ty = y; ty < y + dh; ++ty) {
            for (int tx = x; tx < x + dw; ++tx) {
                const int i = idx(tx, ty);
                if (m_decorCover[i] || (!onReserved && m_reserved[i])) return false;
                if (!onBlocked && m_blocked[i]) return false;
            }
        }
        return true;
    }

    void put(int type, int x, int y) {
        const int dw = kDecorSize[type][0], dh = kDecorSize[type][1];
        m_doc.addDecoration(type, x, y);
        for (int ty = y; ty < y + dh; ++ty) {
            for (int tx = x; tx < x + dw; ++tx) m_decorCover[idx(tx, ty)] = 1;
        }
        m_decorCells += dw * dh;
    }

    // Places a decoration and, on a symmetric map, its mirror image
    bool place(int type, int x, int y, bool onBlocked) {
        if (!fits(type, x, y, onBlocked)) return false;
        const int dw = kDecorSize[type][0], dh = kDecorSize[type][1];
        if (!m_sym.mirrorsObjects()) {
            put(type, x, y);
            return true;
        }
        int mx, my;
        m_sym.rect(x, y, dw, dh, mx, my);
        const bool overlaps = mx < x + dw && x < mx + dw && my < y + dh && y < my + dh;
        if (overlaps || !fits(type, mx, my, onBlocked)) return false;
        put(type, x, y);
        put(type, mx, my);
        return true;
    }

    // Smaller footprints are more common, as in the original maps
    int pickType(const std::vector<Weighted>& pool) {
        float total = 0.0f;
        std::vector<float> weights;
        for (const auto& entry : pool) {
            for (int t = entry.range.first; t <= entry.range.last; ++t) {
                const float area = static_cast<float>(kDecorSize[t][0] * kDecorSize[t][1]);
                weights.push_back(entry.weight / std::pow(area, 0.7f) / (entry.range.last - entry.range.first + 1));
                total += weights.back();
            }
        }
        float roll = m_rng.range(0.0f, total);
        size_t i = 0;
        for (const auto& entry : pool) {
            for (int t = entry.range.first; t <= entry.range.last; ++t, ++i) {
                roll -= weights[i];
                if (roll <= 0.0f) return t;
            }
        }
        return kSmall.first;
    }

    int pickType(TypeRange range) { return pickType(std::vector<Weighted>{{range, 1}}); }

    Context contextAt(int x, int y) const {
        if (dirtQuadrants(x, y) >= 2) return Context::Dirt;          // plateau: rocks
        return m_blocked[idx(x, y)] ? Context::Blocked : Context::Grass;
    }

    void placeDecorations() {
        static constexpr float kCoverage[3] = {0.16f, 0.27f, 0.38f};
        const int budget = static_cast<int>(kCoverage[std::clamp(m_params.decorDensity, 0, 2)] * m_w * m_h);

        // Cliff ridges with gaps: natural chokepoints; the reserved roads never get closed
        const int ridges = m_w * m_h / 1500 + 1;
        for (int r = 0; r < ridges; ++r) {
            float x = static_cast<float>(pick(6, m_w - 7)), y = static_cast<float>(pick(6, m_h - 7));
            const float angle = m_rng.range(0.0f, 2.0f * kPi);
            const int length = pick(4, 9);
            for (int i = 0; i < length; ++i) {
                if (!chance(0.75f)) { x += std::cos(angle) * 3.0f; y += std::sin(angle) * 3.0f; continue; }
                place(pickType(kCliffs), static_cast<int>(x), static_cast<int>(y), false);
                x += std::cos(angle) * 3.0f + m_rng.signedUnit();
                y += std::sin(angle) * 3.0f + m_rng.signedUnit();
            }
        }

        // Dense belt of trees and rocks along the edge
        if (m_params.borderForest) {
            for (int y = 0; y < m_h; ++y) {
                for (int x = 0; x < m_w; ++x) {
                    if (edgeDistance(x, y) > 2 || !chance(0.8f)) continue;
                    const Context ctx = contextAt(x, y);
                    place(pickType(ctx == Context::Dirt ? kCliffs : kTrees), x, y, true);
                }
            }
        }

        // Undergrowth over blocked terrain
        for (int y = 0; y < m_h; ++y) {
            for (int x = 0; x < m_w; ++x) {
                if (m_blocked[idx(x, y)] && !m_decorCover[idx(x, y)] && chance(0.14f)) {
                    place(pickType(typePool(contextAt(x, y))), x, y, true);
                }
            }
        }

        // Groups of trees, bushes and stones on open ground, until the budget is spent
        for (int attempt = 0; attempt < m_w * m_h && m_decorCells < budget; ++attempt) {
            const int cx = pick(1, m_w - 2), cy = pick(1, m_h - 2);
            if (m_reserved[idx(cx, cy)] || m_blocked[idx(cx, cy)]) continue;
            const int members = pick(2, 6);
            for (int i = 0; i < members; ++i) {
                const int x = cx + pick(-3, 3), y = cy + pick(-2, 2);
                if (!inside(x, y) || m_blocked[idx(x, y)]) continue;
                place(pickType(typePool(contextAt(x, y))), x, y, false);
            }
        }
    }

    // ---- 7. passability --------------------------------------------------------------------------------------
    void finalize() {
        for (int i = 0; i < m_w * m_h; ++i) {
            if (m_decorCover[i]) m_blocked[i] = 1;
        }
        sealPockets();
        for (int y = 0; y < m_h; ++y) {
            for (int x = 0; x < m_w; ++x) m_doc.setPassable(x, y, !m_blocked[idx(x, y)]);
        }
        m_doc.setModified(true);
    }

    // Open ground cut off from both bases (by decorations or ridges) turns into undergrowth
    void sealPockets() {
        std::vector<int> component(m_w * m_h, -1);
        std::vector<char> keep;
        int count = 0;
        for (int sy = 0; sy < m_h; ++sy) {
            for (int sx = 0; sx < m_w; ++sx) {
                if (m_blocked[idx(sx, sy)] || component[idx(sx, sy)] >= 0) continue;
                bool hasBase = false;
                std::vector<Point> queue{{sx, sy}};
                component[idx(sx, sy)] = count;
                for (size_t head = 0; head < queue.size(); ++head) {
                    const Point cur = queue[head];
                    hasBase = hasBase || inBaseZone(cur.x, cur.y);
                    static constexpr int kStep[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& s : kStep) {
                        const int nx = cur.x + s[0], ny = cur.y + s[1];
                        if (!inside(nx, ny) || m_blocked[idx(nx, ny)] || component[idx(nx, ny)] >= 0) continue;
                        component[idx(nx, ny)] = count;
                        queue.push_back({nx, ny});
                    }
                }
                keep.push_back(hasBase ? 1 : 0);
                ++count;
            }
        }
        for (int y = 0; y < m_h; ++y) {
            for (int x = 0; x < m_w; ++x) {
                const int c = component[idx(x, y)];
                if (c < 0 || keep[c]) continue;
                m_doc.setTile(x, y, static_cast<uint8_t>(jungleTile()));
                m_blocked[idx(x, y)] = 1;
            }
        }
    }

    MapDocument& m_doc;
    const GeneratorParams& m_params;
    int m_w;
    int m_h;
    int m_tilesetId;
    const TileSet& m_tileset;
    float m_roughThreshold{2.0f};
    Rng m_rng;
    uint32_t m_seed;
    Symmetry m_sym;

    std::vector<Point> m_bases;
    struct FPoint { float x; float y; };
    std::vector<FPoint> m_roadPoints;
    std::vector<uint8_t> m_reserved;     // kept free: bases, roads, clearings
    std::vector<uint8_t> m_obstacle;     // blocked terrain tile
    std::vector<uint8_t> m_blocked;      // impassable for any reason
    std::vector<uint8_t> m_decorCover;   // covered by a decoration footprint
    std::vector<uint8_t> m_dirt;         // quadrants (2x2 per tile): 1 = bare earth (impassable plateau)
    int m_decorCells{0};
};

} // namespace

void SmartGenerator::generate(MapDocument& doc, GeneratorParams params) {
    if (params.randomSeed) {
        std::random_device rd;
        params.seed = rd();
    }
    const int w = std::clamp(params.width, 40, MAX_MAP_SIZE);
    const int h = std::clamp(params.height, 40, MAX_MAP_SIZE);
    const int tilesetId = std::clamp(params.tilesetId, 0, 1);

    doc.newMap(w, h, tilesetId, 0);
    doc.setName("smart_map_" + std::to_string(params.seed % 10000));

    Builder(doc, params, w, h, tilesetId).run();

    std::cout << "[SmartGenerator] Generated map '" << doc.getName() << "' (" << w << "x" << h
              << ", seed: " << params.seed << ")" << std::endl;
}

} // namespace OpenArtOfWar
