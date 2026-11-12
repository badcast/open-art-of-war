#pragma once

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar
{

// The picture trembles when a building blows up. Visual only: never touches the simulation or its checksum.
// Settings: section "camera_shake" of assets/data/game_environment.yaml.
class CameraShake
{
public:
    void Configure(const nlohmann::json &section)
    {
        m_maxOffsetPx = section.value("max_offset_px", m_maxOffsetPx);
        m_traumaPerBuilding = section.value("trauma_per_building", m_traumaPerBuilding);
        m_attackPerSecond = section.value("attack_per_second", m_attackPerSecond);
        m_decayPerSecond = section.value("decay_per_second", m_decayPerSecond);
        m_frequencyHz = section.value("frequency_hz", m_frequencyHz);
        m_maxDistanceTiles = section.value("max_distance_tiles", m_maxDistanceTiles);
    }

    // An explosion `distanceTiles` from the middle of the screen; `size` ~ 1 for a medium building (footprint
    // tiles / 9 under a square root, so a big one adds more and a small one less)
    void AddBlast(float distanceTiles, float size)
    {
        if (m_maxDistanceTiles <= 0.0f || distanceTiles >= m_maxDistanceTiles)
            return;
        const float near = 1.0f - distanceTiles / m_maxDistanceTiles;
        m_target = std::min(1.0f, m_target + m_traumaPerBuilding * std::clamp(size, 0.4f, 1.6f) * near * near);
    }

    void Update(float dt)
    {
        dt = std::clamp(dt, 0.0f, 0.1f);
        m_time += dt;
        // The level climbs to the target smoothly, the target itself dies down
        if (m_level < m_target)
            m_level = std::min(m_target, m_level + m_attackPerSecond * dt);
        else
            m_level = m_target;
        m_target = std::max(0.0f, m_target - m_decayPerSecond * dt);
        if (m_level > m_target && m_target <= 0.0f)
            m_level = std::max(0.0f, m_level - m_decayPerSecond * dt);
    }

    void Reset()
    {
        m_level = 0.0f;
        m_target = 0.0f;
    }

    bool Active() const
    {
        return m_level > 0.001f;
    }

    // Shift of the picture in screen pixels
    void Offset(float &dx, float &dy) const
    {
        const float k = m_maxOffsetPx * m_level * m_level;
        const float w = 6.2831853f * m_frequencyHz * m_time;
        dx = k * (0.6f * std::sin(w) + 0.4f * std::sin(w * 2.3f + 1.7f));
        dy = k * (0.6f * std::sin(w * 1.27f + 0.9f) + 0.4f * std::sin(w * 2.9f + 4.1f));
    }

private:
    float m_maxOffsetPx {10.0f};
    float m_traumaPerBuilding {0.5f};
    float m_attackPerSecond {9.0f};
    float m_decayPerSecond {1.3f};
    float m_frequencyHz {9.0f};
    float m_maxDistanceTiles {26.0f};
    float m_level {0.0f};
    float m_target {0.0f};
    float m_time {0.0f};
};

} // namespace OpenArtOfWar
