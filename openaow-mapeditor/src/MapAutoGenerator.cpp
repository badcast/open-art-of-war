#include "MapAutoGenerator.hpp"
#include "MapNoise.hpp"
#include "core/Utility.hpp"
#include "game/TileCollision.hpp"
#include <random>
#include <cmath>
#include <algorithm>
#include <iostream>

namespace OpenArtOfWar {

using MapNoise::octaveNoise2D;

void MapAutoGenerator::generate(MapDocument& doc, GeneratorParams params) {
    if (params.randomSeed) {
        std::random_device rd;
        params.seed = rd();
    }

    int w = std::clamp(params.width, 20, MAX_MAP_SIZE);
    int h = std::clamp(params.height, 20, MAX_MAP_SIZE);
    int tilesetId = (params.biome == GeneratorBiome::DesertCanyon) ? 1 : params.tilesetId;

    // Reset document
    doc.newMap(w, h, tilesetId, 0);
    doc.setName("gen_map_" + std::to_string(params.seed % 10000));

    // 1. Generate multi-octave heightmap & moisture map
    std::vector<float> heightmap(w * h, 0.0f);
    std::vector<float> moisture(w * h, 0.0f);

    float freq = (params.biome == GeneratorBiome::DenseJungle) ? 0.07f : 0.045f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float nx = static_cast<float>(x) * freq;
            float ny = static_cast<float>(y) * freq;
            float val = octaveNoise2D(nx, ny, 4, 0.5f, params.seed);
            float moist = octaveNoise2D(nx + 100.0f, ny + 100.0f, 3, 0.5f, params.seed + 999);
            heightmap[y * w + x] = val;
            moisture[y * w + x] = moist;
        }
    }

    // 2. Apply symmetry if requested
    if (params.symmetry == GeneratorSymmetry::Rotational180) {
        for (int y = 0; y < h / 2; ++y) {
            for (int x = 0; x < w; ++x) {
                int oppX = w - 1 - x;
                int oppY = h - 1 - y;
                float avgH = (heightmap[y * w + x] + heightmap[oppY * w + oppX]) * 0.5f;
                float avgM = (moisture[y * w + x] + moisture[oppY * w + oppX]) * 0.5f;
                heightmap[y * w + x] = heightmap[oppY * w + oppX] = avgH;
                moisture[y * w + x] = moisture[oppY * w + oppX] = avgM;
            }
        }
    } else if (params.symmetry == GeneratorSymmetry::Diagonal) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int diagX = (y * w) / h;
                int diagY = (x * h) / w;
                if (diagX < w && diagY < h) {
                    float avgH = (heightmap[y * w + x] + heightmap[diagY * w + diagX]) * 0.5f;
                    heightmap[y * w + x] = heightmap[diagY * w + diagX] = avgH;
                }
            }
        }
    }

    // 3. Winding River generation
    std::vector<bool> isRiver(w * h, false);
    std::vector<bool> isBridge(w * h, false);

    if (params.addRiver && params.biome != GeneratorBiome::DesertCanyon) {
        float riverMidX = w * 0.5f;
        for (int y = 0; y < h; ++y) {
            float angle = (static_cast<float>(y) / h) * 3.14159f * 2.5f;
            float rx = riverMidX + std::sin(angle) * (w * 0.18f);
            int irx = static_cast<int>(rx);
            for (int dx = -1; dx <= 2; ++dx) {
                int tx = irx + dx;
                if (tx >= 0 && tx < w) {
                    isRiver[y * w + tx] = true;
                }
            }
        }

        // Place bridges across river
        int numBridges = std::clamp(params.riverBridges, 1, 4);
        int spacing = h / (numBridges + 1);
        for (int b = 1; b <= numBridges; ++b) {
            int bridgeY = b * spacing;
            for (int x = 0; x < w; ++x) {
                if (isRiver[bridgeY * w + x]) {
                    isBridge[bridgeY * w + x] = true;
                    if (bridgeY + 1 < h && isRiver[(bridgeY + 1) * w + x]) {
                        isBridge[(bridgeY + 1) * w + x] = true;
                    }
                }
            }
        }
    }

    // 4. Map height and moisture into authentic terrain tiles
    int waterTile = (tilesetId == 1) ? 238 : 235;     // "Water": impassable undergrowth tiles
    int shallowWater = 240;
    int baseGrass = 0;
    int meadowGrass = 10;
    int dirtPath = 18;
    int rockTile = 26;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = y * w + x;
            if (isBridge[idx]) {
                doc.setTile(x, y, dirtPath);
                doc.setPassable(x, y, true);
                continue;
            }
            if (isRiver[idx]) {
                doc.setTile(x, y, waterTile);
                doc.setPassable(x, y, false);
                continue;
            }

            float hval = heightmap[idx];
            float mval = moisture[idx];

            if (params.biome == GeneratorBiome::Archipelago && hval < 0.42f) {
                doc.setTile(x, y, (hval < 0.35f) ? waterTile : shallowWater);
                doc.setPassable(x, y, false);
            } else if (tilesetId == 1) { // Desert
                if (hval < 0.28f && mval > 0.65f) { // Oasis
                    doc.setTile(x, y, shallowWater);
                    doc.setPassable(x, y, false);
                } else if (hval < 0.40f) {
                    doc.setTile(x, y, baseGrass); // Flat sand
                } else if (hval < 0.65f) {
                    doc.setTile(x, y, meadowGrass); // Dunes
                } else if (hval < 0.82f) {
                    doc.setTile(x, y, dirtPath); // Cracked earth
                } else {
                    doc.setTile(x, y, rockTile); // Rocky canyon
                }
            } else { // Jungle / Plains
                if (hval < 0.25f) {
                    doc.setTile(x, y, shallowWater);
                    doc.setPassable(x, y, false);
                } else if (hval < 0.55f) {
                    doc.setTile(x, y, (mval > 0.5f) ? meadowGrass : baseGrass);
                } else if (hval < 0.78f) {
                    doc.setTile(x, y, dirtPath);
                } else {
                    doc.setTile(x, y, rockTile);
                }
            }
        }
    }

    // 5. Strategic Starting Bases
    int pBaseX = 6, pBaseY = h - 14;
    int eBaseX = w - 16, eBaseY = 6;

    if (params.generateBases) {
        doc.setPlayerStart(pBaseX + 1, pBaseY + 2);
        doc.setEnemyStart(eBaseX + 1, eBaseY + 2);

        auto clearBasePlateau = [&](int bx, int by, int bw, int bh) {
            for (int y = by - 2; y < by + bh + 2; ++y) {
                for (int x = bx - 2; x < bx + bw + 2; ++x) {
                    if (x >= 0 && x < w && y >= 0 && y < h) {
                        doc.setTile(x, y, meadowGrass);
                        doc.setPassable(x, y, true);
                        isRiver[y * w + x] = false;
                        isBridge[y * w + x] = false;
                    }
                }
            }
        };

        // Clear bases
        clearBasePlateau(pBaseX, pBaseY, 10, 10);
        clearBasePlateau(eBaseX, eBaseY, 10, 10);

    }

    // 6. Natural Road / Path connecting bases and bridges
    auto carveRoadSegment = [&](int x0, int y0, int x1, int y1) {
        int cx = x0, cy = y0;
        while (cx != x1 || cy != y1) {
            if (cx >= 0 && cx < w && cy >= 0 && cy < h) {
                if (!isRiver[cy * w + cx] || isBridge[cy * w + cx]) {
                    doc.setTile(cx, cy, dirtPath);
                    doc.setPassable(cx, cy, true);
                }
            }
            if (std::abs(x1 - cx) > std::abs(y1 - cy)) {
                cx += (x1 > cx) ? 1 : -1;
            } else {
                cy += (y1 > cy) ? 1 : -1;
            }
        }
    };

    if (params.generateBases) {
        // Connect player base to center / bridge, and center / bridge to enemy base
        int midBridgeX = w / 2;
        int midBridgeY = h / 2;
        for (int y = 0; y < h; ++y) {
            if (isBridge[y * w + midBridgeX]) {
                midBridgeY = y;
                break;
            }
        }
        carveRoadSegment(pBaseX + 5, pBaseY - 1, midBridgeX, midBridgeY);
        carveRoadSegment(midBridgeX, midBridgeY, eBaseX + 5, eBaseY + 10);
    }

    // 7. Decorations Placement (Trees, Rocks, Bushes)
    int decorDensityPercent = (params.decorDensity == 0) ? 6 : (params.decorDensity == 1 ? 14 : 24);
    OpenArtOfWar::Utility::Random rng(params.seed);
    auto distPct = [&](OpenArtOfWar::Utility::Random& r) { return r.range(0, 99); };
    const int maxTree = (tilesetId == 1) ? 20 : 35;
    auto treeDist = [&](OpenArtOfWar::Utility::Random& r) { return r.range(0, maxTree); };

    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            int idx = y * w + x;
            if (isRiver[idx] || isBridge[idx]) continue;

            // Keep clear of bases
            if (params.generateBases) {
                if (x >= pBaseX - 3 && x <= pBaseX + 13 && y >= pBaseY - 3 && y <= pBaseY + 13) continue;
                if (x >= eBaseX - 3 && x <= eBaseX + 13 && y >= eBaseY - 3 && y <= eBaseY + 13) continue;
            }

            uint8_t t = doc.getTileMap().getBaseTile(x, y);
            if (TileCollision::isBlocked(tilesetId, t)) continue; // Water / blocked ground

            float m = moisture[idx];
            float n = octaveNoise2D(x * 0.12f, y * 0.12f, 2, 0.5f, params.seed + 555);

            if (n > 0.52f && distPct(rng) < decorDensityPercent) {
                int decorType = treeDist(rng);
                if (tilesetId == 1) { // Desert rocks / palms
                    decorType = (m > 0.5f) ? (14 + (distPct(rng) % 6)) : (distPct(rng) % 12);
                } else {
                    decorType = (distPct(rng) % 32);
                }
                doc.addDecoration(decorType, x, y);
            }
        }
    }

    // 8. Auto-calculate complete passability map
    doc.autoRecalculatePassability();
    doc.setModified(true);
    std::cout << "[MapAutoGenerator] Generated procedural map '" << doc.getName()
              << "' (" << w << "x" << h << ", seed: " << params.seed << ") successfully!" << std::endl;
}

void MapAutoGenerator::generateQuickRandom(MapDocument& doc) {
    std::random_device rd;
    GeneratorParams p;
    p.seed = rd();
    p.randomSeed = true;
    p.width = doc.getWidth();
    p.height = doc.getHeight();
    p.tilesetId = doc.getTilesetId();
    p.biome = (p.tilesetId == 1) ? GeneratorBiome::DesertCanyon : GeneratorBiome::RiverValley;
    p.symmetry = GeneratorSymmetry::Rotational180;
    p.addRiver = (p.biome != GeneratorBiome::DesertCanyon);
    p.generateBases = true;
    p.decorDensity = 1;
    generate(doc, p);
}

} // namespace OpenArtOfWar
