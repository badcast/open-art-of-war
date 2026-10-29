#pragma once

#include "EditorCommon.hpp"

namespace CharEditor {

struct BattleBatchResult {
    int runs{0};
    int winsA{0};
    int winsB{0};
    int draws{0};
    float avgTime{0.0f};
    float avgAliveA{0.0f};
    float avgAliveB{0.0f};
};

/**
 * Battle on the real game simulation (EntityManager / Unit / Pathfinder) in a blank arena.
 * Side A (Resistance) starts on the left, side B (Confederation) on the right.
 */
class BattleSim {
public:
    struct Setup {
        int countA{5};
        int countB{5};
        int distance{12};     // tiles between the two groups
        bool bAdvances{true}; // false: B holds its position
        float timeLimit{180.0f};
    };

    struct Side {
        std::vector<int> ids;
        int total{0};
        int alive{0};
        int hpStart{0};
        int hpNow{0};
        int damageDealt{0};
    };

    bool start(const Setup& setup, const fs::path& assetsRoot, OpenArtOfWar::TextureManager* textures,
               YAML::Node docA, YAML::Node docB, std::string& error);

    void update(float dt);

    // Runs the battle to the end without rendering (batch statistics)
    void runToEnd();

    void render(OpenArtOfWar::Renderer& renderer, const OpenArtOfWar::Camera& camera, int animTick);

    void stop() { m_active = false; }

    bool active() const { return m_active; }

    bool finished() const { return m_finished; }

    int winner() const { return m_winner; }

    // 0 running, 1 A, 2 B, 3 draw
    float time() const { return m_time; }

    const Side& sideA() const { return m_a; }

    const Side& sideB() const { return m_b; }

    float arenaWidthPx() const { return static_cast<float>(m_width * OpenArtOfWar::TILE_WIDTH); }

    float arenaHeightPx() const { return static_cast<float>(m_height * OpenArtOfWar::TILE_HEIGHT); }

    // World rectangle around both starting groups (camera framing)
    SDL_FRect battleAreaPx() const { return m_battleArea; }

private:
    void spawnSide(YAML::Node doc, int count, OpenArtOfWar::Faction faction, int startX, int midY, int facing, Side& side);

    // Each side attack-moves toward the other one; repeated so idle units never stall the fight
    void issueOrders(bool initial);

    // Alive counts, remaining HP and damage dealt (HP lost by the other side since the last tick)
    void refreshStats();

    void checkFinished();
    Setup m_setup;
    std::unique_ptr<OpenArtOfWar::TileMap> m_map;
    std::unique_ptr<OpenArtOfWar::EntityManager> m_entities;
    std::unique_ptr<OpenArtOfWar::Pathfinder> m_pathfinder;
    std::unordered_map<int, int> m_lastHp;
    Side m_a;
    Side m_b;
    int m_width{40};
    int m_height{20};
    SDL_FRect m_battleArea{0.0f, 0.0f, 1.0f, 1.0f};
    float m_time{0.0f};
    float m_accumulator{0.0f};
    float m_orderTimer{0.0f};
    int m_winner{0};
    bool m_finished{false};
    bool m_active{false};
};

} // namespace CharEditor
