#pragma once

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar
{

// Radar alarm: an enemy shows up on the radar -> the radar flashes (red edges), red circular waves spread
// from the contact point and a red dot marks it. After an alarm the radar stays quiet until no enemy was
// on it for `rearmDelaySeconds`; only then does the next contact set it off again.
// Settings: section "radar_alert" of assets/data/game_environment.json.
struct RadarAlertConfig
{
    bool enabled {true};
    bool includeBuildings {true};    // enemy structures count as contacts too (units always do)
    float blinkSeconds {3.0f};       // how long the radar flashes
    float blinkHz {3.0f};            // flashes per second of the edges and the dot
    float rearmDelaySeconds {15.0f}; // quiet time (no enemy on the radar) before the next alarm
    SDL_Color color {255, 40, 40, 255};
    int waveCount {3};                // circles spreading from the contact
    float waveSeconds {1.2f};         // life of one circle, from the dot to its full radius
    float waveIntervalSeconds {0.4f}; // delay between two circles
    float waveMaxRadiusPx {36.0f};    // full radius on the screen (a true circle: the radar is flat)
    int waveThicknessPx {2};
    float dotRadiusPx {3.0f};
    int edgeWidthPx {4}; // red band on the radar's sides
    int edgeAlpha {190}; // opacity of that band at the outer edge (it fades inward)

    void load(const nlohmann::json &section)
    {
        if (!section.is_object())
            return;
        enabled = section.value("enabled", enabled);
        includeBuildings = section.value("include_buildings", includeBuildings);
        blinkSeconds = std::max(0.0f, section.value("blink_duration_s", blinkSeconds));
        blinkHz = std::clamp(section.value("blink_hz", blinkHz), 0.1f, 30.0f);
        rearmDelaySeconds = std::max(0.0f, section.value("rearm_delay_s", rearmDelaySeconds));
        if (section.contains("color") && section["color"].is_array() && section["color"].size() >= 3)
        {
            const auto &c = section["color"];
            auto channel = [&](size_t i, int fallback) {
                return static_cast<Uint8>(
                    std::clamp(i < c.size() && c[i].is_number() ? c[i].get<int>() : fallback, 0, 255));
            };
            color = {channel(0, 255), channel(1, 40), channel(2, 40), channel(3, 255)};
        }
        waveCount = std::clamp(section.value("wave_count", waveCount), 0, 16);
        waveSeconds = std::max(0.05f, section.value("wave_duration_s", waveSeconds));
        waveIntervalSeconds = std::max(0.0f, section.value("wave_interval_s", waveIntervalSeconds));
        waveMaxRadiusPx = std::max(1.0f, section.value("wave_max_radius_px", waveMaxRadiusPx));
        waveThicknessPx = std::clamp(section.value("wave_thickness_px", waveThicknessPx), 1, 8);
        dotRadiusPx = std::max(0.0f, section.value("dot_radius_px", dotRadiusPx));
        edgeWidthPx = std::clamp(section.value("edge_width_px", edgeWidthPx), 0, 32);
        edgeAlpha = std::clamp(section.value("edge_alpha", edgeAlpha), 0, 255);
    }
};

class RadarAlert
{
public:
    RadarAlertConfig config;

    // Once per radar frame: is an enemy on the radar now, and where (map tiles)
    void Update(uint64_t nowMs, bool enemyOnRadar, float tileX, float tileY)
    {
        if (enemyOnRadar)
        {
            if (m_armed && config.enabled)
            {
                m_startMs = nowMs;
                m_active = true;
                m_tileX = tileX;
                m_tileY = tileY;
                m_armed = false;
            }
            m_lastEnemyMs = nowMs;
            m_seenEnemy = true;
        }
        if (m_active && Elapsed(nowMs) >= Duration())
            m_active = false;
        // Quiet long enough (and the last alarm is over): the next contact sets it off again
        if (!m_armed && !m_active
            && (!m_seenEnemy || static_cast<float>(nowMs - m_lastEnemyMs) >= config.rearmDelaySeconds * 1000.0f))
            m_armed = true;
    }

    void Reset()
    {
        m_active = false;
        m_armed = true;
        m_seenEnemy = false;
    }

    // When the current / last alarm went off (0: never)
    uint64_t StartMs() const
    {
        return m_startMs;
    }
    bool Active() const
    {
        return m_active;
    }
    bool Armed() const
    {
        return m_armed;
    }
    float TileX() const
    {
        return m_tileX;
    }
    float TileY() const
    {
        return m_tileY;
    }
    // Seconds since the alarm went off
    float Elapsed(uint64_t nowMs) const
    {
        return static_cast<float>(nowMs - m_startMs) / 1000.0f;
    }
    // The alarm lasts while it flashes and until the last wave has spread
    float Duration() const
    {
        const float waves
            = config.waveCount > 0 ? (config.waveCount - 1) * config.waveIntervalSeconds + config.waveSeconds : 0.0f;
        return std::max(config.blinkSeconds, waves);
    }
    // Flash phase: true in the "on" half of each blink, while the radar flashes
    bool BlinkOn(uint64_t nowMs) const
    {
        const float t = Elapsed(nowMs);
        if (!m_active || t >= config.blinkSeconds)
            return false;
        const float cycles = t * config.blinkHz;
        return cycles - static_cast<float>(static_cast<int>(cycles)) < 0.5f;
    }
    // Wave `i`: false when it has not started yet or is over; else its radius and opacity (0..1)
    bool Wave(int i, uint64_t nowMs, float &radiusPx, float &opacity) const
    {
        if (!m_active || i < 0 || i >= config.waveCount)
            return false;
        const float t = Elapsed(nowMs) - static_cast<float>(i) * config.waveIntervalSeconds;
        if (t < 0.0f || t >= config.waveSeconds)
            return false;
        const float k = t / config.waveSeconds;
        radiusPx = config.dotRadiusPx + (config.waveMaxRadiusPx - config.dotRadiusPx) * k;
        opacity = 1.0f - k;
        return true;
    }

private:
    bool m_active {false};
    bool m_armed {true};
    bool m_seenEnemy {false};
    uint64_t m_startMs {0};
    uint64_t m_lastEnemyMs {0};
    float m_tileX {0.0f};
    float m_tileY {0.0f};
};

} // namespace OpenArtOfWar
