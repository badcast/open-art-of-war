#pragma once

#include "MapDocument.hpp"
#include <cstdint>

namespace OpenArtOfWar {

enum class GeneratorBiome : int {
    Plains = 0,         // Зеленые равнины и луга
    RiverValley = 1,    // Речная долина с мостами
    DenseJungle = 2,    // Густые джунгли Перу
    DesertCanyon = 3,   // Пустынный каньон и дюны
    Archipelago = 4     // Островной архипелаг
};

enum class GeneratorSymmetry : int {
    None = 0,           // Естественная асимметрия
    Rotational180 = 1,  // Ротационная 180° (турнирная симметрия)
    Diagonal = 2,       // Диагональная
    Horizontal = 3,     // Зеркальная по горизонтали
    Vertical = 4        // Зеркальная по вертикали
};

struct GeneratorParams {
    int width{60};
    int height{40};
    int tilesetId{0};                   // 0 = Джунгли, 1 = Пустыня
    GeneratorBiome biome{GeneratorBiome::RiverValley};
    GeneratorSymmetry symmetry{GeneratorSymmetry::Rotational180};

    bool addRiver{true};
    int riverBridges{2};
    bool generateBases{true};           // clear the start areas of both players (always done by SmartGenerator)
    int decorDensity{1};                // 0 = мало, 1 = средне, 2 = много
    uint32_t seed{1337};
    bool randomSeed{true};

    // Used by SmartGenerator only
    int obstacleLevel{1};               // blocked terrain (jungle / rubble): 0 = little, 1 = medium, 2 = a lot
    int dirtLevel{1};                   // share of bare earth between the grass: 0 = little, 1 = medium, 2 = a lot
    bool borderForest{true};            // wall the map edge with trees and cliffs
};

class MapAutoGenerator {
public:
    static void generate(MapDocument& doc, GeneratorParams params);
    static void generateQuickRandom(MapDocument& doc);
};

} // namespace OpenArtOfWar
