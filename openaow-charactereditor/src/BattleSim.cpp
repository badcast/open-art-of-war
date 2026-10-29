#include "BattleSim.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace CharEditor {

bool BattleSim::start(const Setup& setup, const fs::path& assetsRoot, OpenArtOfWar::TextureManager* textures,
               YAML::Node docA, YAML::Node docB, std::string& error) {
    m_setup = setup;
    m_time = 0.0f;
    m_accumulator = 0.0f;
    m_orderTimer = 0.0f;
    m_winner = 0;
    m_finished = false;
    m_lastHp.clear();
    m_a = Side{};
    m_b = Side{};

    const int rowsA = std::max(1, std::min(setup.countA, 5));
    const int rowsB = std::max(1, std::min(setup.countB, 5));
    const int colsA = (setup.countA + 4) / 5;
    const int colsB = (setup.countB + 4) / 5;
    const int margin = 8;
    m_width = margin * 2 + colsA + setup.distance + colsB + 4;
    m_height = std::max(18, std::max(rowsA, rowsB) * 4 + 10);

    m_map = std::make_unique<OpenArtOfWar::TileMap>();
    m_map->initBlankArena(m_width, m_height, 0);
    m_map->setFogEnabled(false);
    m_entities = std::make_unique<OpenArtOfWar::EntityManager>();
    m_entities->setTextureManager(textures);
    m_entities->setUnlimitedPower(true);
    if (!m_entities->loadCharacterConfigs(assetsRoot.string())) {
        error = "Could not load character configs";
        return false;
    }
    // Apply the editor's current (possibly unsaved) values
    m_entities->loadCharacterYaml(YAML::Clone(docA));
    m_entities->loadCharacterYaml(YAML::Clone(docB));
    m_entities->rebuildBlueprints();

    m_pathfinder = std::make_unique<OpenArtOfWar::Pathfinder>(m_map.get());
    m_pathfinder->setEntityManager(m_entities.get());

    const int midY = m_height / 2;
    const int bStartX = margin + colsA + setup.distance;
    spawnSide(docA, setup.countA, OpenArtOfWar::Faction::Resistance, margin, midY, 0, m_a);
    spawnSide(docB, setup.countB, OpenArtOfWar::Faction::Confederation, bStartX, midY, 4, m_b);
    const int rows = std::max(rowsA, rowsB);
    const float tw = static_cast<float>(OpenArtOfWar::TILE_WIDTH);
    const float th = static_cast<float>(OpenArtOfWar::TILE_HEIGHT);
    m_battleArea = {(margin - 3) * tw, (midY - rows - 3) * th,
                    (bStartX + colsB + 6 - margin) * tw, (rows * 2 + 6) * th};
    if (m_a.ids.empty() || m_b.ids.empty()) {
        error = "Could not spawn both sides (check archetype_id / building_index)";
        return false;
    }
    refreshStats();
    m_a.hpStart = m_a.hpNow;
    m_b.hpStart = m_b.hpNow;
    issueOrders(true);
    m_active = true;
    return true;
}

void BattleSim::update(float dt) {
    if (!m_active) return;
    m_accumulator += dt;
    const float step = 1.0f / 30.0f;
    int guard = 0;
    while (m_accumulator >= step && guard++ < 60) {
        m_accumulator -= step;
        m_entities->update(step, *m_map, *m_pathfinder);
        if (m_finished) continue; // keep death animations running, results stay frozen
        m_time += step;
        refreshStats();
        m_orderTimer += step;
        if (m_orderTimer >= 1.5f) {
            m_orderTimer = 0.0f;
            issueOrders(false);
        }
        checkFinished();
    }
}

void BattleSim::runToEnd() {
    const float step = 1.0f / 30.0f;
    while (m_active && !m_finished) update(step);
}

void BattleSim::render(OpenArtOfWar::Renderer& renderer, const OpenArtOfWar::Camera& camera, int animTick) {
    if (!m_map) return;
    m_map->renderTerrain(renderer, camera, false, animTick);
    m_entities->renderGroundRemnants(renderer, camera, *m_map, false);
    m_entities->render(renderer, camera, *m_map, false, animTick, false);
    renderer.drawRectWorld(0.0f, 0.0f, static_cast<float>(m_width * OpenArtOfWar::TILE_WIDTH),
                           static_cast<float>(m_height * OpenArtOfWar::TILE_HEIGHT), camera, SDL_Color{85, 140, 105, 200});
}

void BattleSim::spawnSide(YAML::Node doc, int count, OpenArtOfWar::Faction faction, int startX, int midY, int facing, Side& side) {
    const bool building = yamlString(doc, "type") == "building";
    if (building) {
        const int index = yamlInt(doc, "building_index", -1);
        const int w = std::max(1, yamlInt(doc["size"], "width", 1));
        const int h = std::max(1, yamlInt(doc["size"], "height", 1));
        const int perColumn = std::max(1, (m_height - 4) / (h + 1));
        for (int i = 0; i < count; ++i) {
            int x = startX + (i / perColumn) * (w + 1);
            int y = midY - (std::min(count, perColumn) * (h + 1)) / 2 + (i % perColumn) * (h + 1);
            int id = m_entities->spawnBuildingByIndex(index, x, y, faction, 1.0f);
            if (id <= 0) continue;
            for (int ty = y; ty < y + h; ++ty) {
                for (int tx = x; tx < x + w; ++tx) m_map->setPassable(tx, ty, false);
            }
            side.ids.push_back(id);
        }
    } else {
        const int arch = yamlInt(doc, "archetype_id", 0);
        const int rows = std::min(count, 5);
        for (int i = 0; i < count; ++i) {
            int x = startX + i / 5;
            int y = midY - rows + (i % 5) * 2;
            int id = m_entities->spawnUnitByArchetype(arch, x, y, faction);
            if (id <= 0) continue;
            if (OpenArtOfWar::Unit* u = m_entities->getUnit(id)) {
                u->facingDirection = facing;
                u->turretFacing = facing;
            }
            side.ids.push_back(id);
        }
    }
    side.total = static_cast<int>(side.ids.size());
}

void BattleSim::issueOrders(bool initial) {
    auto centroid = [&](const Side& s, int& outX, int& outY) -> bool {
        int sx = 0, sy = 0, n = 0;
        for (int id : s.ids) {
            const OpenArtOfWar::Entity* e = m_entities->getEntity(id);
            if (!e || !e->alive || e->state == OpenArtOfWar::ActionState::Die) continue;
            sx += e->tileX;
            sy += e->tileY;
            ++n;
        }
        if (n == 0) return false;
        outX = sx / n;
        outY = sy / n;
        return true;
    };
    auto order = [&](const Side& own, const Side& enemy) {
        int tx, ty;
        if (!centroid(enemy, tx, ty)) return;
        std::vector<int> idle;
        for (int id : own.ids) {
            const OpenArtOfWar::Unit* u = m_entities->getUnit(id);
            if (!u || !u->alive || u->state == OpenArtOfWar::ActionState::Die || u->moveSpeed <= 0.0f) continue;
            if (initial || (u->state == OpenArtOfWar::ActionState::Idle && u->targetEntityId == 0 &&
                            u->path.empty() && !u->waitingForPath)) {
                idle.push_back(id);
            }
        }
        if (!idle.empty()) m_entities->issueAttackMoveOrder(idle, tx, ty, *m_pathfinder);
    };
    order(m_a, m_b);
    if (m_setup.bAdvances) order(m_b, m_a);
}

void BattleSim::refreshStats() {
    auto account = [&](Side& s, Side& enemy) {
        s.alive = 0;
        s.hpNow = 0;
        for (int id : s.ids) {
            const OpenArtOfWar::Entity* e = m_entities->getEntity(id);
            int hp = (e && e->alive && e->state != OpenArtOfWar::ActionState::Die) ? std::max(0, e->currentHp) : 0;
            auto it = m_lastHp.find(id);
            if (it != m_lastHp.end() && it->second > hp) enemy.damageDealt += it->second - hp;
            m_lastHp[id] = hp;
            if (hp > 0) {
                ++s.alive;
                s.hpNow += hp;
            }
        }
    };
    account(m_a, m_b);
    account(m_b, m_a);
}

void BattleSim::checkFinished() {
    if (m_a.alive == 0 || m_b.alive == 0) {
        m_winner = (m_a.alive == 0 && m_b.alive == 0) ? 3 : (m_a.alive == 0 ? 2 : 1);
        m_finished = true;
    } else if (m_time >= m_setup.timeLimit) {
        // Time out: the side that kept the larger share of its HP wins
        float ra = static_cast<float>(m_a.hpNow) / std::max(1, m_a.hpStart);
        float rb = static_cast<float>(m_b.hpNow) / std::max(1, m_b.hpStart);
        m_winner = (std::abs(ra - rb) < 0.02f) ? 3 : (ra > rb ? 1 : 2);
        m_finished = true;
    }
}

} // namespace CharEditor
