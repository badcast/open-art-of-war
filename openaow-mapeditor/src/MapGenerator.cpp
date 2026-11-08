#include "MapGenerator.hpp"
#include "MapNoise.hpp"
#include "TileCorners.hpp"
#include "core/Utility.hpp"
#include "game/TileCollision.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace OpenArtOfWar
{

// How the original maps are built (measured on map_mission_* / map_skirmish_*):
// - The ground is a corner tile set: every corner of a tile is light grass (G), dark grass (D), earth (E),
//   water (W) or, in tileset 1, sand (S), and neighbours share their corners (TileCorners.hpp). Materials
//   nest: water lies inside dark grass, dark grass, earth and sand inside light grass.
// - Everything impassable is an island of overlapping decorations: rock massifs and groves in the middle,
//   trees, groves and small stones on the rim. The collision mask is the union of their footprints; the
//   open ground carries no decorations at all. 5..60 islands cover 40..65 % of a map, a few of them large,
//   most touch the map edge. Under an island lies dark grass, water and jungle, hidden by the decorations.
// - A map keeps one look: forest-only islands, or islands that are 40..70 % rock.

namespace
{

using MapNoise::OctaveNoise2D;
using Rng = Utility::RandomGenerator;

constexpr float kPi = 3.14159265f;
constexpr int kBaseSize = 10;             // a base occupies 10x10 tiles
constexpr int kBaseMargin = 3;            // kept clear around a base
constexpr float kMinBaseDistance = 18.0f; // closest two bases of a ring may be (tiles, corner to corner)

// ---- Ground ----------------------------------------------------------------------------------------------------
enum Material : char
{
    G = 'G',
    D = 'D',
    E = 'E',
    W = 'W',
    S = 'S'
};

int Rank(char m)
{
    return m == W ? 2 : (m == G ? 0 : 1);
}
char Parent(char m)
{
    return m == W ? D : G;
}

bool PairAllowed(char a, char b)
{
    if (a == b)
        return true;
    if (a > b)
        std::swap(a, b);
    // sorted pairs: D-G, D-W, E-G, G-S
    return (a == D && b == G) || (a == D && b == W) || (a == E && b == G) || (a == G && b == S);
}

struct TileSet
{
    std::map<std::string, std::vector<int>> plain;    // corner code -> tiles of the basic ranges
    std::map<std::string, std::vector<int>> variants; // speckled variants of the uniform codes
    std::vector<int> jungle;                          // impassable undergrowth (under islands)
    std::vector<int> rubble;                          // open rough ground on grass
};

TileSet MakeTileSet(int tilesetId)
{
    TileSet set;
    const char *const *codes = tilesetId == 1 ? TileCorners::kTileset1 : TileCorners::kTileset0;
    // Basic ranges the game varies itself (TileMap::loadMap): plain material tiles
    const int basic[2][5][2]
        = {{{0, 9}, {10, 17}, {19, 25}, {26, 33}, {0, -1}}, {{0, 9}, {10, 17}, {18, 25}, {26, 32}, {33, 35}}};
    const auto isBasic = [&](int tile) {
        for (const auto &range : basic[tilesetId])
        {
            if (tile >= range[0] && tile <= range[1])
                return true;
        }
        return false;
    };
    for (int tile = 0; tile < TileCorners::kGroundTiles; ++tile)
    {
        if (tilesetId == 0 && tile == 18)
            continue; // a lone orange earth tile, out of place
        const std::string code = codes[tile];
        // Away from water every ground tile must stay open
        if (code.find(W) == std::string::npos && TileCollision::IsBlocked(tilesetId, tile))
            continue;
        const bool uniform = code[0] == code[1] && code[1] == code[2] && code[2] == code[3];
        (uniform && !isBasic(tile) ? set.variants : set.plain)[code].push_back(tile);
    }
    const int jungle[2] = {tilesetId == 1 ? 238 : 235, tilesetId == 1 ? 243 : 240};
    for (int tile = jungle[0]; tile <= jungle[1]; ++tile)
        set.jungle.push_back(tile);
    const int rubble[2] = {tilesetId == 1 ? 191 : 211, tilesetId == 1 ? 206 : 226}; // rubble on grass
    for (int tile = rubble[0]; tile <= rubble[1]; ++tile)
    {
        // Tileset 1 rubble lies inside the ground range: only the ones on plain grass fit anywhere on grass
        const bool onGrass = tile >= TileCorners::kGroundTiles || std::string(codes[tile]) == "GGGG";
        if (onGrass && !TileCollision::IsBlocked(tilesetId, tile))
            set.rubble.push_back(tile);
    }
    return set;
}

const TileSet &tileSet(int tilesetId)
{
    static const TileSet sets[2] = {MakeTileSet(0), MakeTileSet(1)};
    return sets[tilesetId == 1 ? 1 : 0];
}

// ---- Decorations -----------------------------------------------------------------------------------------------
// Footprints (width, height) of the decoration types 0..94; the same in both tilesets (buildings 31..132).
constexpr int kDecorTypes = 95;
constexpr int kDecorSize[kDecorTypes][2] = {
    {2, 2}, {2, 2}, {1, 2}, {1, 3}, {2, 2}, {2, 3}, {2, 3}, {1, 3}, {1, 3}, {2, 3}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
    {2, 3}, {2, 3}, {2, 3}, {2, 3}, {3, 2}, {3, 2}, {3, 2}, {3, 2}, {3, 3}, {2, 3}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
    {2, 1}, {2, 1}, {2, 2}, {3, 2}, {2, 1}, {2, 2}, {3, 2}, {3, 2}, {4, 2}, {3, 3}, {2, 2}, {2, 2}, {2, 2}, {2, 1},
    {1, 2}, {2, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
    {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {3, 3}, {3, 3}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {1, 1}, {1, 1}, {1, 1},
    {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1},
    {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1},
};

struct TypeRange
{
    int first;
    int last;
};
constexpr TypeRange kRocks = {0, 25};      // rock outcrops and cliff clusters
constexpr TypeRange kLowRocks = {26, 33};  // low rocks and rock ridges
constexpr TypeRange kRockPiles = {34, 37}; // big rock piles
constexpr TypeRange kStones = {38, 49};    // scattered stones
constexpr TypeRange kGroves = {50, 66};    // groups of trees and bushes
constexpr TypeRange kTrees = {67, 87};     // single trees (1x1)
constexpr TypeRange kDeadTrees = {88, 94}; // dead trees (1x1)

// ---- Symmetry --------------------------------------------------------------------------------------------------
struct Symmetry
{
    GeneratorSymmetry kind {GeneratorSymmetry::None};
    int w {0};
    int h {0};

    bool Active() const
    {
        return kind != GeneratorSymmetry::None;
    }
    // Objects keep their orientation under flips only, so a transposed map gets no mirrored decorations
    bool MirrorsObjects() const
    {
        return Active() && kind != GeneratorSymmetry::Diagonal;
    }

    void point(float x, float y, float &ox, float &oy) const
    {
        switch (kind)
        {
            case GeneratorSymmetry::Rotational180:
                ox = w - x;
                oy = h - y;
                break;
            case GeneratorSymmetry::Horizontal:
                ox = w - x;
                oy = y;
                break;
            case GeneratorSymmetry::Vertical:
                ox = x;
                oy = h - y;
                break;
            case GeneratorSymmetry::Diagonal:
                ox = y * w / h;
                oy = x * h / w;
                break;
            default:
                ox = x;
                oy = y;
                break;
        }
    }

    // Top-left corner of the mirror image of a rw x rh rectangle
    void rect(int x, int y, int rw, int rh, int &ox, int &oy) const
    {
        float cx, cy;
        point(x + rw * 0.5f, y + rh * 0.5f, cx, cy);
        ox = static_cast<int>(std::lround(cx - rw * 0.5f));
        oy = static_cast<int>(std::lround(cy - rh * 0.5f));
    }
};

class Builder
{
public:
    Builder(MapDocument &doc, const GeneratorParams &params, int w, int h, int tilesetId)
        : m_doc(doc)
        , m_params(params)
        , m_w(w)
        , m_h(h)
        , m_tilesetId(tilesetId)
        , m_tileset(tileSet(tilesetId))
        , m_rng(params.seed)
        , m_seed(params.seed)
    {
        m_players = std::clamp(params.players, 2, 8);
        // More than two bases sit on a ring: the terrain is not mirrored, the bases are alike by construction
        m_sym.kind = m_players > 2 ? GeneratorSymmetry::None : params.symmetry;
        m_sym.w = w;
        m_sym.h = h;
        const int tiles = w * h;
        m_reserved.assign(tiles, 0);
        m_island.assign(tiles, 0);
        m_depth.assign(tiles, 0);
        m_cover.assign(tiles, 0);
        m_corner.assign((w + 1) * (h + 1), G);
    }

    void run()
    {
        LayoutBases();
        CarveRoads();
        BuildIslands();
        BuildGround();
        ResolveTiles();
        SetStartPositions();
        FillIslands();
        Finalize();
    }

private:
    struct Point
    {
        int x;
        int y;
    };
    struct FPoint
    {
        float x;
        float y;
    };

    // ---- helpers ---------------------------------------------------------------------------------------------
    int Idx(int x, int y) const
    {
        return y * m_w + x;
    }
    int CIdx(int x, int y) const
    {
        return y * (m_w + 1) + x;
    }
    bool Inside(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < m_w && y < m_h;
    }
    int EdgeDistance(int x, int y) const
    {
        return std::min(std::min(x, y), std::min(m_w - 1 - x, m_h - 1 - y));
    }
    int Pick(int lo, int hi)
    {
        return m_rng.Range(lo, hi);
    }
    bool Chance(float p)
    {
        return m_rng.unit() < p;
    }
    template <typename T>
    const T &PickFrom(const std::vector<T> &items)
    {
        return items[Pick(0, static_cast<int>(items.size()) - 1)];
    }

    // Noise symmetric under the chosen symmetry (the original point and its mirror image are averaged)
    float field(float x, float y, float frequency, int octaves, std::uint32_t salt) const
    {
        float value = OctaveNoise2D(x * frequency, y * frequency, octaves, 0.5f, m_seed + salt);
        if (m_sym.Active())
        {
            float mx, my;
            m_sym.point(x, y, mx, my);
            value = (value + OctaveNoise2D(mx * frequency, my * frequency, octaves, 0.5f, m_seed + salt)) * 0.5f;
        }
        return value;
    }

    // Value above which `fraction` of the samples lie
    static float Quantile(std::vector<float> values, float fraction)
    {
        if (values.empty())
            return 2.0f;
        const std::size_t pos = static_cast<std::size_t>(std::clamp(1.0f - fraction, 0.0f, 1.0f) * (values.size() - 1));
        std::nth_element(values.begin(), values.begin() + pos, values.end());
        return values[pos];
    }

    void ReserveRect(int x0, int y0, int x1, int y1)
    {
        for (int y = std::max(0, y0); y <= std::min(m_h - 1, y1); ++y)
        {
            for (int x = std::max(0, x0); x <= std::min(m_w - 1, x1); ++x)
                m_reserved[Idx(x, y)] = 1;
        }
    }

    bool InBaseZone(int x, int y) const
    {
        for (const auto &b : m_bases)
        {
            if (x >= b.x - kBaseMargin && x < b.x + kBaseSize + kBaseMargin && y >= b.y - kBaseMargin
                && y < b.y + kBaseSize + kBaseMargin)
            {
                return true;
            }
        }
        return false;
    }

    // Labels the 4-connected components of the tiles where `inside` holds; returns the component count
    template <typename Pred>
    int Components(Pred inside, std::vector<int> &label) const
    {
        label.assign(m_w * m_h, -1);
        int count = 0;
        std::vector<Point> queue;
        for (int sy = 0; sy < m_h; ++sy)
        {
            for (int sx = 0; sx < m_w; ++sx)
            {
                if (!inside(sx, sy) || label[Idx(sx, sy)] >= 0)
                    continue;
                queue.assign(1, {sx, sy});
                label[Idx(sx, sy)] = count;
                for (std::size_t head = 0; head < queue.size(); ++head)
                {
                    const Point cur = queue[head];
                    static constexpr int kStep[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto &s : kStep)
                    {
                        const int nx = cur.x + s[0], ny = cur.y + s[1];
                        if (!Inside(nx, ny) || !inside(nx, ny) || label[Idx(nx, ny)] >= 0)
                            continue;
                        label[Idx(nx, ny)] = count;
                        queue.push_back({nx, ny});
                    }
                }
                ++count;
            }
        }
        return count;
    }

    // ---- 1. bases --------------------------------------------------------------------------------------------
    // Bases of three or more players: evenly on an ellipse around the centre, the first one at the bottom left. The
    // number shrinks while two bases would be closer than kMinBaseDistance (a small map takes fewer players).
    void LayoutRingBases()
    {
        const int maxX = m_w - kBaseSize - kBaseMargin - 1;
        const int maxY = m_h - kBaseSize - kBaseMargin - 1;
        const float cx = m_w * 0.5f, cy = m_h * 0.5f;
        const float rx = std::max(1.0f, cx - kBaseSize * 0.5f - kBaseMargin - 1.0f);
        const float ry = std::max(1.0f, cy - kBaseSize * 0.5f - kBaseMargin - 1.0f);
        for (; m_players >= 3; --m_players)
        {
            m_bases.clear();
            for (int i = 0; i < m_players; ++i)
            {
                const float angle = kPi * 0.75f + 2.0f * kPi * i / m_players;
                const int x = std::clamp(
                    static_cast<int>(std::lround(cx + rx * std::cos(angle) - kBaseSize * 0.5f)), kBaseMargin + 1, maxX);
                const int y = std::clamp(
                    static_cast<int>(std::lround(cy + ry * std::sin(angle) - kBaseSize * 0.5f)), kBaseMargin + 1, maxY);
                m_bases.push_back({x, y});
            }
            float nearest = 1e9f;
            for (std::size_t a = 0; a < m_bases.size(); ++a)
            {
                for (std::size_t b = a + 1; b < m_bases.size(); ++b)
                {
                    nearest = std::min(
                        nearest, std::hypot(float(m_bases[a].x - m_bases[b].x), float(m_bases[a].y - m_bases[b].y)));
                }
            }
            if (nearest >= kMinBaseDistance)
                break;
        }
        if (m_players < 3)
            m_bases.clear(); // too small for a ring: the two-base layout below
    }

    void LayoutBases()
    {
        if (m_players > 2)
            LayoutRingBases();
        if (m_bases.size() >= 3)
        {
            for (const auto &b : m_bases)
            {
                ReserveRect(
                    b.x - kBaseMargin,
                    b.y - kBaseMargin,
                    b.x + kBaseSize + kBaseMargin - 1,
                    b.y + kBaseSize + kBaseMargin - 1);
            }
            return;
        }
        const int maxX = m_w - kBaseSize - kBaseMargin - 1;
        const int maxY = m_h - kBaseSize - kBaseMargin - 1;
        int px = 6, py = m_h - 14;
        if (m_params.symmetry == GeneratorSymmetry::Horizontal)
        {
            px = 5;
            py = m_h / 2 - kBaseSize / 2;
        }
        else if (m_params.symmetry == GeneratorSymmetry::Vertical)
        {
            px = m_w / 2 - kBaseSize / 2;
            py = m_h - 14;
        }
        px = std::clamp(px, kBaseMargin + 1, maxX);
        py = std::clamp(py, kBaseMargin + 1, maxY);
        int ex = m_w - px - kBaseSize;
        int ey = m_h - py - kBaseSize;
        if (m_params.symmetry == GeneratorSymmetry::Horizontal)
            ey = py;
        if (m_params.symmetry == GeneratorSymmetry::Vertical)
            ex = px;
        m_bases = {{px, py}, {std::clamp(ex, kBaseMargin + 1, maxX), std::clamp(ey, kBaseMargin + 1, maxY)}};
        for (const auto &b : m_bases)
        {
            ReserveRect(
                b.x - kBaseMargin,
                b.y - kBaseMargin,
                b.x + kBaseSize + kBaseMargin - 1,
                b.y + kBaseSize + kBaseMargin - 1);
        }
    }

    // ---- 2. roads and clearings: kept free of islands --------------------------------------------------------
    void StampOne(float x, float y, float clearRadius)
    {
        for (int ty = static_cast<int>(y - clearRadius) - 1; ty <= static_cast<int>(y + clearRadius) + 1; ++ty)
        {
            for (int tx = static_cast<int>(x - clearRadius) - 1; tx <= static_cast<int>(x + clearRadius) + 1; ++tx)
            {
                if (!Inside(tx, ty))
                    continue;
                const float dx = tx + 0.5f - x, dy = ty + 0.5f - y;
                if (dx * dx + dy * dy <= clearRadius * clearRadius)
                    m_reserved[Idx(tx, ty)] = 1;
            }
        }
    }

    void Stamp(float x, float y, float clearRadius)
    {
        StampOne(x, y, clearRadius);
        m_roadPoints.push_back({x, y});
        if (m_sym.Active())
        {
            float mx, my;
            m_sym.point(x, y, mx, my);
            StampOne(mx, my, clearRadius);
            m_roadPoints.push_back({mx, my});
        }
    }

    // A winding strip between two points
    void Walk(float ax, float ay, float bx, float by, float clearRadius)
    {
        const float length = std::hypot(bx - ax, by - ay);
        if (length < 1.0f)
            return;
        const float nx = -(by - ay) / length, ny = (bx - ax) / length;
        const float phase = m_rng.Range(0.0f, 2.0f * kPi);
        const float amplitude = m_rng.Range(1.0f, 3.5f) * std::min(1.0f, length / 25.0f);
        const float waves = m_rng.Range(1.0f, 2.5f);
        const int steps = std::max(2, static_cast<int>(length / 0.6f));
        for (int i = 0; i <= steps; ++i)
        {
            const float t = static_cast<float>(i) / steps;
            const float wobble = std::sin(phase + t * waves * kPi) * amplitude * std::sin(t * kPi);
            Stamp(ax + (bx - ax) * t + nx * wobble, ay + (by - ay) * t + ny * wobble, clearRadius);
        }
    }

    // Bases of three or more players: every base gets a winding road to the centre
    void CarveRingRoads()
    {
        const float cx = m_w * 0.5f, cy = m_h * 0.5f;
        for (const auto &b : m_bases)
        {
            const float bx = b.x + kBaseSize * 0.5f, by = b.y + kBaseSize * 0.5f;
            const float wx = std::clamp((bx + cx) * 0.5f + m_rng.SignedUnit() * m_w * 0.08f, 8.0f, m_w - 8.0f);
            const float wy = std::clamp((by + cy) * 0.5f + m_rng.SignedUnit() * m_h * 0.08f, 8.0f, m_h - 8.0f);
            Walk(bx, by, wx, wy, 2.0f);
            Walk(wx, wy, cx, cy, 2.0f);
        }
        CarveClearings(cx, cy);
    }

    void CarveRoads()
    {
        if (m_bases.size() >= 3)
        {
            CarveRingRoads();
            return;
        }
        const float p0x = m_bases[0].x + kBaseSize * 0.5f, p0y = m_bases[0].y + kBaseSize * 0.5f;
        const float p1x = m_bases[1].x + kBaseSize * 0.5f, p1y = m_bases[1].y + kBaseSize * 0.5f;
        const float cx = m_w * 0.5f, cy = m_h * 0.5f;
        const float wx = std::clamp(cx + m_rng.SignedUnit() * m_w * 0.22f, 8.0f, m_w - 8.0f);
        const float wy = std::clamp(cy + m_rng.SignedUnit() * m_h * 0.22f, 8.0f, m_h - 8.0f);
        Walk(p0x, p0y, wx, wy, 2.0f);
        Walk(wx, wy, cx, cy, 2.0f);
        if (!m_sym.Active())
        {
            const float w2x = std::clamp(cx + m_rng.SignedUnit() * m_w * 0.22f, 8.0f, m_w - 8.0f);
            const float w2y = std::clamp(cy + m_rng.SignedUnit() * m_h * 0.22f, 8.0f, m_h - 8.0f);
            Walk(cx, cy, w2x, w2y, 2.0f);
            Walk(w2x, w2y, p1x, p1y, 2.0f);
        }

        CarveClearings(cx, cy);
    }

    // A few open clearings (room for expansions and flanking) joined to the road
    void CarveClearings(float cx, float cy)
    {
        const int clearings = std::clamp(m_w * m_h / 1500, 1, 6);
        std::vector<Point> placed;
        for (int attempt = 0; attempt < clearings * 12 && static_cast<int>(placed.size()) < clearings; ++attempt)
        {
            const int x = Pick(8, m_w - 9), y = Pick(8, m_h - 9);
            bool ok = true;
            for (const auto &b : m_bases)
            {
                if (std::hypot(x - (b.x + kBaseSize * 0.5f), y - (b.y + kBaseSize * 0.5f)) < 15.0f)
                    ok = false;
            }
            for (const auto &c : placed)
            {
                if (std::hypot(float(x - c.x), float(y - c.y)) < 14.0f)
                    ok = false;
            }
            if (!ok)
                continue;
            placed.push_back({x, y});
            float nearestX = cx, nearestY = cy, best = 1e9f;
            for (const auto &pt : m_roadPoints)
            {
                const float d = std::hypot(pt.x - x, pt.y - y);
                if (d < best)
                {
                    best = d;
                    nearestX = pt.x;
                    nearestY = pt.y;
                }
            }
            Walk(x, y, nearestX, nearestY, 1.5f);
            Stamp(static_cast<float>(x), static_cast<float>(y), m_rng.Range(2.5f, 4.0f));
        }
    }

    // ---- 3. islands ------------------------------------------------------------------------------------------
    static float Percent(int value)
    {
        return std::clamp(value, 0, 100) / 100.0f;
    }

    void MakeIslandMask(const std::vector<float> &value, float fraction)
    {
        std::vector<float> eligible;
        for (int i = 0; i < m_w * m_h; ++i)
            if (!m_reserved[i])
                eligible.push_back(value[i]);
        const float wanted = fraction * m_w * m_h;
        const float threshold = Quantile(eligible, std::min(0.98f, wanted / std::max<std::size_t>(1, eligible.size())));
        for (int i = 0; i < m_w * m_h; ++i)
            m_island[i] = !m_reserved[i] && value[i] > threshold ? 1 : 0;

        // Majority filter: smooth outlines
        std::vector<std::uint8_t> smoothed(m_island.size());
        for (int y = 0; y < m_h; ++y)
        {
            for (int x = 0; x < m_w; ++x)
            {
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = std::clamp(x + dx, 0, m_w - 1), ny = std::clamp(y + dy, 0, m_h - 1);
                        count += m_island[Idx(nx, ny)];
                    }
                }
                smoothed[Idx(x, y)] = !m_reserved[Idx(x, y)] && count >= 5 ? 1 : 0;
            }
        }
        m_island = smoothed;

        // Specks of one or two tiles go
        std::vector<int> label;
        const int islands = Components([&](int x, int y) { return m_island[Idx(x, y)] != 0; }, label);
        std::vector<int> size(islands, 0);
        for (int l : label)
            if (l >= 0)
                ++size[l];
        for (int i = 0; i < m_w * m_h; ++i)
            if (label[i] >= 0 && size[label[i]] < 3)
                m_island[i] = 0;

        ConnectPockets();
        ConnectBases();
    }

    // Every base must be reachable from the first one: while another base's open ground is cut off, a passage
    // (two tiles wide) is cut through the islands to it
    void ConnectBases()
    {
        if (m_bases.size() < 2)
            return;
        std::vector<int> label;
        for (int pass = 0; pass < 32; ++pass)
        {
            Components([&](int x, int y) { return m_island[Idx(x, y)] == 0; }, label);
            auto labelOf = [&](const Point &b) {
                return label[Idx(b.x + kBaseSize / 2, b.y + kBaseSize / 2)];
            };
            const int home = labelOf(m_bases[0]);
            int lost = -1;
            for (std::size_t i = 1; i < m_bases.size() && lost < 0; ++i)
            {
                if (labelOf(m_bases[i]) != home)
                    lost = labelOf(m_bases[i]);
            }
            if (lost < 0)
                return;
            // Shortest way from the first base's ground to the cut-off ground, through islands too
            std::vector<int> from(m_w * m_h, -2);
            std::vector<int> queue;
            for (int i = 0; i < m_w * m_h; ++i)
            {
                if (label[i] == home)
                {
                    from[i] = -1;
                    queue.push_back(i);
                }
            }
            int reached = -1;
            for (std::size_t head = 0; head < queue.size() && reached < 0; ++head)
            {
                const int cur = queue[head];
                static constexpr int kStep[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto &st : kStep)
                {
                    const int nx = cur % m_w + st[0], ny = cur / m_w + st[1];
                    if (!Inside(nx, ny) || from[Idx(nx, ny)] != -2)
                        continue;
                    from[Idx(nx, ny)] = cur;
                    if (label[Idx(nx, ny)] == lost)
                    {
                        reached = Idx(nx, ny);
                        break;
                    }
                    queue.push_back(Idx(nx, ny));
                }
            }
            if (reached < 0)
                return;
            for (int cur = from[reached]; cur >= 0; cur = from[cur])
            {
                const int x = cur % m_w, y = cur / m_w;
                for (int dy = 0; dy <= 1; ++dy)
                {
                    for (int dx = 0; dx <= 1; ++dx)
                    {
                        if (Inside(x + dx, y + dy))
                            m_island[Idx(x + dx, y + dy)] = 0;
                    }
                }
            }
        }
    }

    // Open ground cut off from both bases: small pockets join the islands, larger ones get a passage
    // (two tiles wide) cut through the islands to the open ground of the bases
    void ConnectPockets()
    {
        std::vector<int> label;
        for (int pass = 0; pass < 64; ++pass)
        {
            const int open = Components([&](int x, int y) { return m_island[Idx(x, y)] == 0; }, label);
            std::vector<char> keep(open, 0);
            std::vector<int> size(open, 0);
            for (int y = 0; y < m_h; ++y)
            {
                for (int x = 0; x < m_w; ++x)
                {
                    const int l = label[Idx(x, y)];
                    if (l < 0)
                        continue;
                    ++size[l];
                    if (InBaseZone(x, y))
                        keep[l] = 1;
                }
            }
            int pocket = -1;
            for (int l = 0; l < open && pocket < 0; ++l)
            {
                if (!keep[l] && size[l] >= 12)
                    pocket = l;
            }
            for (int i = 0; i < m_w * m_h; ++i)
            {
                if (label[i] >= 0 && !keep[label[i]] && size[label[i]] < 12)
                    m_island[i] = 1;
            }
            if (pocket < 0)
                return;

            // Shortest way from the pocket to kept open ground
            std::vector<int> from(m_w * m_h, -2);
            std::vector<int> queue;
            for (int i = 0; i < m_w * m_h; ++i)
            {
                if (label[i] == pocket)
                {
                    from[i] = -1;
                    queue.push_back(i);
                }
            }
            int reached = -1;
            for (std::size_t head = 0; head < queue.size() && reached < 0; ++head)
            {
                const int cur = queue[head];
                static constexpr int kStep[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto &st : kStep)
                {
                    const int nx = cur % m_w + st[0], ny = cur / m_w + st[1];
                    if (!Inside(nx, ny) || from[Idx(nx, ny)] != -2)
                        continue;
                    from[Idx(nx, ny)] = cur;
                    if (label[Idx(nx, ny)] >= 0 && keep[label[Idx(nx, ny)]])
                    {
                        reached = Idx(nx, ny);
                        break;
                    }
                    queue.push_back(Idx(nx, ny));
                }
            }
            if (reached < 0)
            { // nothing to join: the pocket becomes island
                for (int i = 0; i < m_w * m_h; ++i)
                    if (label[i] == pocket)
                        m_island[i] = 1;
                continue;
            }
            for (int cur = from[reached]; cur >= 0; cur = from[cur])
            {
                const int x = cur % m_w, y = cur / m_w;
                for (int dy = 0; dy <= 1; ++dy)
                {
                    for (int dx = 0; dx <= 1; ++dx)
                    {
                        if (Inside(x + dx, y + dy))
                            m_island[Idx(x + dx, y + dy)] = 0;
                    }
                }
            }
        }
    }

    void BuildIslands()
    {
        // Blobs plus ridges (bands along the noise's mid line): round massifs and long winding walls
        std::vector<float> shape(m_w * m_h), edgeField(m_w * m_h), edgeNoise;
        for (int y = 0; y < m_h; ++y)
        {
            for (int x = 0; x < m_w; ++x)
            {
                const float px = x + 0.5f, py = y + 0.5f;
                const float blob = field(px, py, 0.045f, 3, 303) * 0.65f + field(px, py, 0.13f, 2, 404) * 0.35f;
                const float ridge = 1.0f - std::fabs(field(px, py, 0.035f, 3, 606) - 0.5f) * 2.0f;
                shape[Idx(x, y)] = std::max(blob, ridge * 0.62f + field(px, py, 0.15f, 2, 707) * 0.18f);
                edgeField[Idx(x, y)] = field(px, py, 0.07f, 2, 1212);
                if (EdgeDistance(x, y) == 0)
                    edgeNoise.push_back(edgeField[Idx(x, y)]);
            }
        }

        // The edge is closed in long stretches with gaps: a share of the edge gets pulled into the islands, the
        // rest pushed out. Smoothing, the pocket fill and islands reaching the edge on their own shift both
        // shares, so the two thresholds are corrected until the measured shares match.
        const float target = Percent(m_params.islandPercent);
        const float edgeTarget = Percent(m_params.edgePercent);
        float fraction = target, edgeFraction = edgeTarget;
        float bestError = 1e9f, bestFraction = fraction, bestEdgeFraction = edgeFraction;
        std::vector<float> value(m_w * m_h);
        const auto buildValue = [&](float edgeShareWanted) {
            const float edgeThreshold = Quantile(edgeNoise, edgeShareWanted);
            for (int y = 0; y < m_h; ++y)
            {
                for (int x = 0; x < m_w; ++x)
                {
                    const float nearEdge = std::max(0.0f, 1.0f - EdgeDistance(x, y) / 3.0f);
                    const float pull = edgeField[Idx(x, y)] > edgeThreshold ? 0.4f : -0.3f;
                    value[Idx(x, y)] = shape[Idx(x, y)] + pull * nearEdge;
                }
            }
        };
        for (int round = 0; round < 8; ++round)
        {
            buildValue(edgeFraction);
            MakeIslandMask(value, fraction);
            int count = 0, edgeCount = 0, edgeTiles = 0;
            for (int y = 0; y < m_h; ++y)
            {
                for (int x = 0; x < m_w; ++x)
                {
                    count += m_island[Idx(x, y)];
                    if (EdgeDistance(x, y) == 0)
                    {
                        ++edgeTiles;
                        edgeCount += m_island[Idx(x, y)];
                    }
                }
            }
            const float share = static_cast<float>(count) / (m_w * m_h);
            const float edgeShare = static_cast<float>(edgeCount) / std::max(1, edgeTiles);
            const float error = std::fabs(share - target) + 0.3f * std::fabs(edgeShare - edgeTarget);
            if (error < bestError)
            {
                bestError = error;
                bestFraction = fraction;
                bestEdgeFraction = edgeFraction;
            }
            if (std::fabs(share - target) < 0.01f && std::fabs(edgeShare - edgeTarget) < 0.03f)
                break;
            fraction = std::clamp(fraction + (target - share) * 0.8f, 0.0f, 0.98f);
            edgeFraction = std::clamp(edgeFraction + (edgeTarget - edgeShare) * 0.8f, 0.0f, 1.0f);
        }
        if (bestFraction != fraction || bestEdgeFraction != edgeFraction)
        {
            buildValue(bestEdgeFraction);
            MakeIslandMask(value, bestFraction);
        }

        // Depth: distance (4-steps) from the open ground; 1 = rim
        std::vector<Point> queue;
        for (int y = 0; y < m_h; ++y)
        {
            for (int x = 0; x < m_w; ++x)
            {
                m_depth[Idx(x, y)] = m_island[Idx(x, y)] ? 1000 : 0;
                if (!m_island[Idx(x, y)])
                    queue.push_back({x, y});
            }
        }
        // The map edge does not count as open ground: islands touching it keep a deep core there
        for (std::size_t head = 0; head < queue.size(); ++head)
        {
            const Point cur = queue[head];
            static constexpr int kStep[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto &s : kStep)
            {
                const int nx = cur.x + s[0], ny = cur.y + s[1];
                if (!Inside(nx, ny) || m_depth[Idx(nx, ny)] <= m_depth[Idx(cur.x, cur.y)] + 1)
                    continue;
                m_depth[Idx(nx, ny)] = m_depth[Idx(cur.x, cur.y)] + 1;
                queue.push_back({nx, ny});
            }
        }
        for (auto &d : m_depth)
            if (d == 1000)
                d = 8; // an island without open ground (all-blocked map)
    }

    // ---- 4. ground materials on the corner grid --------------------------------------------------------------
    // Depth of the shallowest tile touching a corner (0 = the corner touches open ground)
    int CornerDepth(int cx, int cy) const
    {
        int depth = 1000;
        for (int dy = -1; dy <= 0; ++dy)
        {
            for (int dx = -1; dx <= 0; ++dx)
            {
                const int x = cx + dx, y = cy + dy;
                if (Inside(x, y))
                    depth = std::min(depth, m_depth[Idx(x, y)]);
            }
        }
        return depth == 1000 ? 0 : depth;
    }

    void BuildGround()
    {
        const int cw = m_w + 1, ch = m_h + 1;
        std::vector<int> depth(cw * ch);
        std::vector<float> earthOpen;
        for (int y = 0; y < ch; ++y)
        {
            for (int x = 0; x < cw; ++x)
            {
                depth[CIdx(x, y)] = CornerDepth(x, y);
                if (depth[CIdx(x, y)] == 0)
                    earthOpen.push_back(field(x, y, 0.07f, 3, 505));
            }
        }
        const float openCorners = static_cast<float>(std::max<std::size_t>(1, earthOpen.size()));
        std::fill(m_corner.begin(), m_corner.end(), G);

        // Earth (on tileset 1 also sand) patches on the open ground; a ring of light grass keeps them off the dark
        // grass
        const float earthThreshold = Quantile(earthOpen, Percent(m_params.earthPercent));
        const float sandThreshold = m_tilesetId == 1 ? Quantile(earthOpen, 0.5f) : 2.0f;
        std::vector<std::uint8_t> grassRing(cw * ch, 0);
        for (int y = 0; y < ch; ++y)
        {
            for (int x = 0; x < cw; ++x)
            {
                if (depth[CIdx(x, y)] != 0 || field(x, y, 0.07f, 3, 505) <= earthThreshold)
                    continue;
                m_corner[CIdx(x, y)] = field(x, y, 0.04f, 2, 808) > sandThreshold ? S : E;
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (x + dx >= 0 && y + dy >= 0 && x + dx < cw && y + dy < ch)
                            grassRing[CIdx(x + dx, y + dy)] = 1;
                    }
                }
            }
        }

        // Dark grass: the chosen share of the open ground, and the ground under the islands
        std::vector<float> darkOpen;
        for (int y = 0; y < ch; ++y)
        {
            for (int x = 0; x < cw; ++x)
            {
                if (depth[CIdx(x, y)] == 0 && !grassRing[CIdx(x, y)])
                    darkOpen.push_back(field(x, y, 0.05f, 3, 101));
            }
        }
        const float darkFraction
            = Percent(m_params.darkGrassPercent) * openCorners / std::max<std::size_t>(1, darkOpen.size());
        const float darkThreshold = Quantile(darkOpen, std::min(1.0f, darkFraction));
        for (int y = 0; y < ch; ++y)
        {
            for (int x = 0; x < cw; ++x)
            {
                const int i = CIdx(x, y);
                if (grassRing[i])
                    continue;
                if (depth[i] > 0 || field(x, y, 0.05f, 3, 101) > darkThreshold)
                    m_corner[i] = D;
            }
        }

        // Water hides under the island cores, always rimmed with dark grass
        std::vector<float> waterCore;
        for (int i = 0; i < cw * ch; ++i)
        {
            if (depth[i] >= 3)
                waterCore.push_back(field(static_cast<float>(i % cw), static_cast<float>(i / cw), 0.09f, 2, 202));
        }
        const float waterThreshold = Quantile(waterCore, Percent(m_params.coreWaterPercent));
        for (int y = 0; y < ch; ++y)
        {
            for (int x = 0; x < cw; ++x)
            {
                if (depth[CIdx(x, y)] < 3 || field(x, y, 0.09f, 2, 202) <= waterThreshold)
                    continue;
                m_corner[CIdx(x, y)] = W;
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < cw && ny < ch && m_corner[CIdx(nx, ny)] != W)
                            m_corner[CIdx(nx, ny)] = D;
                    }
                }
            }
        }
        FixCorners();
    }

    // Every tile must show a combination that has a picture: at most two materials that may touch, and no
    // diagonal pattern. Offending corners fall back to their parent material until the grid settles.
    std::string Code(int x, int y) const
    {
        return {m_corner[CIdx(x, y)], m_corner[CIdx(x + 1, y)], m_corner[CIdx(x, y + 1)], m_corner[CIdx(x + 1, y + 1)]};
    }

    bool HasPicture(const std::string &code) const
    {
        return m_tileset.plain.count(code) != 0 || m_tileset.variants.count(code) != 0;
    }

    void FixCorners()
    {
        for (int pass = 0; pass < 64; ++pass)
        {
            bool changed = false;
            for (int y = 0; y < m_h; ++y)
            {
                for (int x = 0; x < m_w; ++x)
                {
                    const std::string code = Code(x, y);
                    bool legal = HasPicture(code);
                    for (int a = 0; a < 4 && legal; ++a)
                    {
                        for (int b = a + 1; b < 4; ++b)
                            legal = legal && PairAllowed(code[a], code[b]);
                    }
                    if (legal)
                        continue;
                    // Shrink the highest-ranked material; between ranks the dark grass stays (it holds water)
                    int top = 0;
                    for (char m : code)
                        top = std::max(top, Rank(m));
                    char victim = 0;
                    for (char m : code)
                    {
                        if (Rank(m) == top && (victim == 0 || victim == D))
                            victim = m;
                    }
                    const int cx[4] = {x, x + 1, x, x + 1}, cy[4] = {y, y, y + 1, y + 1};
                    for (int q = 0; q < 4; ++q)
                    {
                        if (code[q] == victim)
                            m_corner[CIdx(cx[q], cy[q])] = Parent(victim);
                    }
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
    }

    // ---- 5. tiles --------------------------------------------------------------------------------------------
    void ResolveTiles()
    {
        // Speckled variants and rubble come in patches; their thresholds give the chosen shares
        // (counted over the open tiles they may go on, as on the original maps)
        std::vector<float> speckle, rough;
        for (int y = 0; y < m_h; ++y)
        {
            for (int x = 0; x < m_w; ++x)
            {
                if (m_island[Idx(x, y)])
                    continue;
                const std::string code = Code(x, y);
                if (code == "GGGG")
                    rough.push_back(field(x + 0.5f, y + 0.5f, 0.11f, 2, 909));
                if (code[0] == code[1] && code[1] == code[2] && code[2] == code[3] && m_tileset.variants.count(code))
                {
                    speckle.push_back(field(x + 0.5f, y + 0.5f, 0.08f, 2, 1001));
                }
            }
        }
        const float speckleThreshold = Quantile(speckle, Percent(m_params.variantPercent));
        const float roughThreshold = Quantile(rough, Percent(m_params.rubblePercent));
        // Jungle share of the core tiles, water tiles included (as measured on the original maps)
        const float water = Percent(m_params.coreWaterPercent);
        const float jungle = std::min(1.0f, Percent(m_params.coreJunglePercent) / std::max(0.2f, 1.0f - water));
        for (int y = 0; y < m_h; ++y)
        {
            for (int x = 0; x < m_w; ++x)
            {
                const std::string code = Code(x, y);
                const bool uniform = code[0] == code[1] && code[1] == code[2] && code[2] == code[3];
                const float fx = x + 0.5f, fy = y + 0.5f;
                int tile = 0;
                const auto plain = m_tileset.plain.find(code);
                const auto variant = m_tileset.variants.find(code);
                if (m_island[Idx(x, y)] && m_depth[Idx(x, y)] >= 2 && code[0] != W && uniform && Chance(jungle))
                {
                    tile = PickFrom(m_tileset.jungle); // undergrowth inside the islands
                }
                else if (
                    !m_island[Idx(x, y)] && code == "GGGG" && !m_tileset.rubble.empty()
                    && field(fx, fy, 0.11f, 2, 909) > roughThreshold)
                {
                    tile = PickFrom(m_tileset.rubble);
                }
                else if (
                    uniform && variant != m_tileset.variants.end() && field(fx, fy, 0.08f, 2, 1001) > speckleThreshold)
                {
                    tile = PickFrom(variant->second); // speckled patches
                }
                else if (plain != m_tileset.plain.end())
                {
                    tile = PickFrom(plain->second);
                }
                else if (variant != m_tileset.variants.end())
                {
                    tile = PickFrom(variant->second);
                }
                m_doc.SetTile(x, y, static_cast<std::uint8_t>(tile));
            }
        }
    }

    void SetStartPositions()
    {
        m_doc.setPlayerStart(m_bases[0].x + 1, m_bases[0].y + 2);
        m_doc.setEnemyStart(m_bases[1].x + 1, m_bases[1].y + 2);
        for (std::size_t i = 2; i < m_bases.size(); ++i)
            m_doc.AddStartPos(m_bases[i].x + 1, m_bases[i].y + 2);
    }

    // ---- 6. decorations: every island tile ends up under a footprint -----------------------------------------
    bool InsideIsland(int type, int x, int y) const
    {
        const int dw = kDecorSize[type][0], dh = kDecorSize[type][1];
        if (x < 0 || y < 0 || x + dw > m_w || y + dh > m_h)
            return false;
        for (int ty = y; ty < y + dh; ++ty)
        {
            for (int tx = x; tx < x + dw; ++tx)
            {
                if (!m_island[Idx(tx, ty)])
                    return false;
            }
        }
        return true;
    }

    int NewlyCovered(int type, int x, int y) const
    {
        int count = 0;
        for (int ty = y; ty < y + kDecorSize[type][1]; ++ty)
        {
            for (int tx = x; tx < x + kDecorSize[type][0]; ++tx)
                count += m_cover[Idx(tx, ty)] ? 0 : 1;
        }
        return count;
    }

    void Put(int type, int x, int y)
    {
        m_doc.AddDecoration(type, x, y);
        m_decorArea += kDecorSize[type][0] * kDecorSize[type][1];
        ++m_decorCount;
        m_rockCount += type <= kStones.last ? 1 : 0;
        for (int ty = y; ty < y + kDecorSize[type][1]; ++ty)
        {
            for (int tx = x; tx < x + kDecorSize[type][0]; ++tx)
                m_cover[Idx(tx, ty)] = 1;
        }
    }

    // Places a decoration and, on a symmetric map, its mirror image when that fits the island as well
    void Place(int type, int x, int y)
    {
        Put(type, x, y);
        if (!m_sym.MirrorsObjects())
            return;
        int mx, my;
        m_sym.rect(x, y, kDecorSize[type][0], kDecorSize[type][1], mx, my);
        if ((mx != x || my != y) && InsideIsland(type, mx, my))
            Put(type, mx, my);
    }

    int PickType(TypeRange range)
    {
        return Pick(range.first, range.last);
    }

    // Rocks lean to the island middle; the running count is steered toward the chosen share of rocks
    float RockChance(float islandRock, float depthWeight) const
    {
        const float share = m_decorCount ? static_cast<float>(m_rockCount) / m_decorCount : islandRock;
        return std::clamp(islandRock * depthWeight + 4.0f * (Percent(m_params.rockPercent) - share), 0.0f, 1.0f);
    }

    // What stands at a given depth of an island whose rock share is `rock`
    int ChooseType(int depth, float rock)
    {
        const float roll = m_rng.unit();
        if (depth >= 3)
        {
            if (roll < RockChance(rock, 1.3f))
                return PickType(Chance(0.75f) ? kRocks : kRockPiles);
            return PickType(kGroves);
        }
        if (depth == 2)
        {
            if (roll < RockChance(rock, 1.0f))
                return PickType(Chance(0.7f) ? kRocks : kLowRocks);
            return PickType(Chance(0.65f) ? kGroves : kTrees);
        }
        if (roll < RockChance(rock, 0.6f))
            return PickType(Chance(0.5f) ? kStones : (Chance(0.5f) ? kLowRocks : kRocks));
        const float kind = m_rng.unit();
        if (kind < 0.45f)
            return PickType(kTrees);
        if (kind < 0.9f)
            return PickType(kGroves);
        return PickType(kDeadTrees);
    }

    // Rock share of every island: one look per map, mirrored islands look alike
    std::vector<float> IslandRockShares(std::vector<int> &label)
    {
        const int count = Components([&](int x, int y) { return m_island[Idx(x, y)] != 0; }, label);
        std::vector<float> sumX(count, 0.0f), sumY(count, 0.0f), size(count, 0.0f), rock(count, 0.0f);
        for (int y = 0; y < m_h; ++y)
        {
            for (int x = 0; x < m_w; ++x)
            {
                const int l = label[Idx(x, y)];
                if (l < 0)
                    continue;
                sumX[l] += x + 0.5f;
                sumY[l] += y + 0.5f;
                size[l] += 1.0f;
            }
        }
        for (int l = 0; l < count; ++l)
        {
            const float spread = field(sumX[l] / size[l], sumY[l] / size[l], 0.03f, 1, 1111) - 0.5f;
            // Islands differ around the chosen share, mirrored islands alike
            const float base = Percent(m_params.rockPercent);
            rock[l] = std::clamp(base + spread * 1.2f * std::min(base, 1.0f - base), 0.0f, 1.0f);
        }
        return rock;
    }

    void FillIslands()
    {
        std::vector<int> label;
        const std::vector<float> rock = IslandRockShares(label);

        // Deep tiles first, so the big rocks and groves take the middle and the rim gets the small things
        std::vector<int> order;
        for (int i = 0; i < m_w * m_h; ++i)
            if (m_island[i])
                order.push_back(i);
        for (std::size_t i = order.size(); i > 1; --i)
            std::swap(order[i - 1], order[Pick(0, static_cast<int>(i) - 1)]);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return std::min(m_depth[a], 3) > std::min(m_depth[b], 3);
        });

        for (int i : order)
        {
            if (m_cover[i])
                continue;
            const int tx = i % m_w, ty = i / m_w;
            const int depth = m_depth[i];
            bool placed = false;
            for (int attempt = 0; attempt < 10 && !placed; ++attempt)
            {
                const int type = ChooseType(depth, rock[label[i]]);
                const int dw = kDecorSize[type][0], dh = kDecorSize[type][1];
                int bestX = 0, bestY = 0, best = 0, bestScore = -1000;
                for (int y = ty - dh + 1; y <= ty; ++y)
                {
                    for (int x = tx - dw + 1; x <= tx; ++x)
                    {
                        if (!InsideIsland(type, x, y))
                            continue;
                        const int gain = NewlyCovered(type, x, y);
                        const int score = 2 * gain - (dw * dh - gain); // prefer little overlap
                        if (gain > 0 && score > bestScore)
                        {
                            best = gain;
                            bestScore = score;
                            bestX = x;
                            bestY = y;
                        }
                    }
                }
                // The first tries only take a spot with at most one tile of overlap
                const int overlap = dw * dh - best;
                if (best > 0 && (attempt >= 6 || overlap <= 1))
                {
                    Place(type, bestX, bestY);
                    placed = true;
                }
            }
            // A single (1x1) tree always fits
            if (!placed)
                Place(PickType(Chance(0.85f) ? kTrees : kDeadTrees), tx, ty);
        }

        // Extra layer: the original islands are piled up; footprints add up to the chosen share of the island area
        const int wanted = static_cast<int>(std::clamp(m_params.coverPercent, 0, 300) / 100.0f * order.size());
        for (int attempts = 0; m_decorArea < wanted && attempts < static_cast<int>(order.size()) * 4; ++attempts)
        {
            const int i = PickFrom(order);
            const int type = ChooseType(m_depth[i], rock[label[i]]);
            if (InsideIsland(type, i % m_w, i / m_w))
                Place(type, i % m_w, i / m_w);
        }
    }

    // ---- 7. passability --------------------------------------------------------------------------------------
    void Finalize()
    {
        for (int y = 0; y < m_h; ++y)
        {
            for (int x = 0; x < m_w; ++x)
            {
                const bool blockedTile = TileCollision::IsBlocked(m_tilesetId, m_doc.GetTileMap().GetBaseTile(x, y));
                m_doc.SetPassable(x, y, !(blockedTile || m_cover[Idx(x, y)] || m_island[Idx(x, y)]));
            }
        }
        m_doc.SetModified(true);
    }

    MapDocument &m_doc;
    const GeneratorParams &m_params;
    int m_w;
    int m_h;
    int m_tilesetId;
    const TileSet &m_tileset;
    Rng m_rng;
    std::uint32_t m_seed;
    Symmetry m_sym;

    std::vector<Point> m_bases;
    int m_players {2};
    std::vector<FPoint> m_roadPoints;
    std::vector<std::uint8_t> m_reserved; // kept free: bases, roads, clearings
    std::vector<std::uint8_t> m_island;   // impassable island
    std::vector<int> m_depth;             // distance of an island tile from the open ground (0 = open)
    std::vector<std::uint8_t> m_cover;    // covered by a decoration footprint
    int m_decorArea {0};                  // sum of the footprint areas
    int m_decorCount {0};
    int m_rockCount {0};        // rocks and stones among the decorations
    std::vector<char> m_corner; // ground material of the (w+1) x (h+1) tile corners
};

} // namespace

void MapGenerator::Generate(MapDocument &doc, GeneratorParams params)
{
    if (params.randomSeed)
    {
        std::random_device rd;
        params.seed = rd();
    }
    const int w = std::clamp(params.width, 40, MAX_MAP_SIZE);
    const int h = std::clamp(params.height, 40, MAX_MAP_SIZE);
    const int tilesetId = std::clamp(params.tilesetId, 0, 1);
    params.players = std::clamp(params.players, 2, 8);

    doc.NewMap(w, h, tilesetId, 0);
    doc.SetName("generated_map_" + std::to_string(params.seed % 10000));

    Builder(doc, params, w, h, tilesetId).run();

    std::cout << "[MapGenerator] Generated map '" << doc.GetName() << "' (" << w << "x" << h
              << ", seed: " << params.seed << ")" << std::endl;
}

void MapGenerator::GenerateQuick(MapDocument &doc)
{
    GeneratorParams params;
    params.width = doc.GetWidth();
    params.height = doc.GetHeight();
    params.tilesetId = doc.GetTilesetId();
    OriginalDefaults(params, params.tilesetId);
    Generate(doc, params);
}

// Medians over map_mission_* and map_skirmish_* of each tileset (tools/derive_tile_corners.py gives the table
// the ground shares are counted with)
void MapGenerator::OriginalDefaults(GeneratorParams &params, int tilesetId)
{
    if (tilesetId == 1)
    {
        params.islandPercent = 48;
        params.edgePercent = 64;
        params.rockPercent = 55;
        params.coverPercent = 115;
        params.darkGrassPercent = 14;
        params.earthPercent = 40;
        params.variantPercent = 30;
        params.rubblePercent = 2;
        params.coreWaterPercent = 13;
        params.coreJunglePercent = 16;
    }
    else
    {
        params.islandPercent = 41;
        params.edgePercent = 74;
        params.rockPercent = 9;
        params.coverPercent = 105;
        params.darkGrassPercent = 53;
        params.earthPercent = 8;
        params.variantPercent = 46;
        params.rubblePercent = 2;
        params.coreWaterPercent = 30;
        params.coreJunglePercent = 27;
    }
}

const MapGenerator::Setting *MapGenerator::Settings(int &count)
{
    static const Setting settings[] = {
        {"Islands, % of the map",
         &GeneratorParams::islandPercent,
         10,
         70,
         "Impassable rock and forest islands. Original maps: 17..67 %, median 46 %."},
        {"Closed map edge, %",
         &GeneratorParams::edgePercent,
         0,
         100,
         "Share of the map edge taken by islands. Original maps: 23..100 %, median 69 %."},
        {"Rocks in islands, %",
         &GeneratorParams::rockPercent,
         0,
         100,
         "Rocks among the island decorations, the rest are trees and groves.\n"
         "Original maps: 0..82 %; jungle maps median 9 %, tileset 1 median 55 %."},
        {"Decoration cover, %",
         &GeneratorParams::coverPercent,
         60,
         200,
         "Footprints of the decorations added up, against the island area: over 100 % they overlap.\n"
         "Original maps: 68..152 %, median 113 %."},
        {"Dark grass, % of open ground",
         &GeneratorParams::darkGrassPercent,
         0,
         80,
         "Original maps: jungle 20..72 % (median 53 %), tileset 1 8..25 % (median 14 %)."},
        {"Earth / sand, % of open ground",
         &GeneratorParams::earthPercent,
         0,
         50,
         "Original maps: jungle 2..19 % (median 8 %), tileset 1 23..45 % (median 40 %)."},
        {"Speckled ground tiles, %",
         &GeneratorParams::variantPercent,
         0,
         90,
         "Speckled variants among the plain ground tiles. Original maps: 5..82 %, median 43 %."},
        {"Rubble, % of open ground",
         &GeneratorParams::rubblePercent,
         0,
         15,
         "Open rough ground on the grass. Original maps: 0..9 %, median 2 %."},
        {"Water under islands, %",
         &GeneratorParams::coreWaterPercent,
         0,
         100,
         "Water under the middle of the islands, mostly hidden by the decorations.\n"
         "Original maps: 0..96 %; jungle median 30 %, tileset 1 median 13 %."},
        {"Jungle under islands, %",
         &GeneratorParams::coreJunglePercent,
         0,
         60,
         "Jungle tiles under the middle of the islands. Original maps: 1..35 %, median 17 %."},
    };
    count = static_cast<int>(sizeof(settings) / sizeof(settings[0]));
    return settings;
}

} // namespace OpenArtOfWar
