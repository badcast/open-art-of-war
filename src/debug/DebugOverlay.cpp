#if defined(AOW_DEBUG)

#include "debug/DebugOverlay.hpp"
#include "game/Players.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/MissionController.hpp"
#include "game/AIController.hpp"
#include "game/Research.hpp"
#include "game/Sides.hpp"
#include "audio/SoundEngine.hpp"
#include "graphics/Camera.hpp"

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <iostream>
#include <algorithm>

namespace OpenArtOfWar
{

DebugOverlay::DebugOverlay(SDL_Window *window, SDL_Renderer *renderer)
    : m_window(window)
    , m_renderer(renderer)
{}

DebugOverlay::~DebugOverlay() {}

void DebugOverlay::ProcessEvent(const SDL_Event &event)
{
    (void) event;
}

void DebugOverlay::Render(
    TileMap &map,
    EntityManager &entities,
    Economy &economy,
    MissionController &mission,
    SoundEngine &sound,
    Camera &camera,
    AIController &ai,
    float fps)
{
    if (!m_visible)
        return;

    ImGui::SetNextWindowSize(ImVec2(480, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Open Art Of War - RTS Engine Debug Inspector", &m_visible))
    {
        ImGui::Text(
            "Engine Status: FPS: %.1f | Camera: (%.1f, %.1f) Zoom: %.2fx",
            fps,
            camera.GetX(),
            camera.GetY(),
            camera.GetZoom());
        ImGui::Separator();

        // 1. Cheats & Resources
        if (ImGui::CollapsingHeader("Economy & Cheats", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "Credits: $%d | Power: %d / %d (Net: %d)",
                economy.GetCredits(),
                economy.GetPowerConsumed(),
                economy.GetPowerProduced(),
                economy.GetPowerProduced() - economy.GetPowerConsumed());

            if (ImGui::Button("+10,000 Credits"))
            {
                economy.AddCredits(10000);
            }
            ImGui::SameLine();
            if (ImGui::Button("Spawn Player Tank"))
            {
                entities.SpawnUnitByArchetype(8, 10, 10, Faction::Resistance);
            }
            ImGui::SameLine();
            if (ImGui::Button("Spawn Enemy Tank"))
            {
                entities.SpawnUnitByArchetype(18, 12, 12, Faction::Confederation);
            }

            ImGui::Checkbox("Disable Fog of War (Reveal All)", &m_disableFog);
        }

        // 2. AI Director: runtime difficulty override and visualization
        if (ImGui::CollapsingHeader("AI Director", ImGuiTreeNodeFlags_DefaultOpen))
        {
            AIDebugConfig cfg = ai.GetDebugConfig();
            bool changed = false;
            changed |= ImGui::Checkbox("Override difficulty", &cfg.forceDebugOverride);
            ImGui::SameLine();
            static const char *kLevels[] = {"Easy", "Normal", "Hard", "Hardcore"};
            ImGui::SetNextItemWidth(140);
            changed |= ImGui::Combo("##aiLevel", &cfg.debugDifficultyLevel, kLevels, 4);
            changed |= ImGui::Checkbox("Show groups & leash lines", &cfg.visualizeSquadLeash);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Show chokepoints", &cfg.visualizeChokepoints);
            changed |= ImGui::Checkbox("AI ignores fog of war", &cfg.disableFogOfWarForAi);
            ImGui::SetNextItemWidth(200);
            changed |= ImGui::SliderFloat("Wave size multiplier", &cfg.waveSpawnMultiplier, 0.25f, 4.0f, "%.2fx");
            if (changed)
                ai.SetDebugConfig(cfg);

            const AITacticsConfig &t = ai.GetTactics();
            ImGui::Text(
                "In effect: %s | leash %.1f tiles | chokepoint gather: %s | slowest leads: %s | retreat: %s",
                DifficultyToString(ai.GetEffectiveDifficulty()).c_str(),
                t.leashTiles,
                t.waitAtChokepoints ? "yes" : "no",
                t.syncToSlowest ? "yes" : "no",
                t.retreatLossRatio > 0.0f ? "yes" : "no");
            ImGui::Text(
                "Credits: %d | Waves sent: %d | Next wave: %d units, ready %d, cooldown %.0fs",
                ai.GetEnemyCredits(),
                ai.GetWaveCount(),
                static_cast<int>(ai.GetCurrentWavePlan().archetypes.size()),
                ai.GetReadyUnitCount(),
                std::max(0.0f, ai.GetWaveCooldown()));
            if (ai.GetResearchIndex() < kResearchCount)
            {
                ImGui::Text(
                    "Research %d/%d: %s %s",
                    ai.GetResearchIndex() + 1,
                    kResearchCount,
                    researchDef(AiFaction(), ai.GetResearchIndex()).name,
                    ai.IsResearchActive() ? "(in progress)" : "(waiting for lab / credits)");
            }
            else
            {
                ImGui::Text("Research: all technologies complete");
            }
            for (const auto &sq : ai.GetSquadController().GetSquads())
            {
                ImGui::BulletText(
                    "G%d (wave %d): %s, %zu/%d units, route %d/%zu, spread %.1f",
                    sq.id,
                    sq.waveNumber,
                    SquadStateName(sq.state),
                    sq.unitIds.size(),
                    sq.initialSize,
                    sq.progress,
                    sq.route.size(),
                    sq.maxSpread);
            }
        }

        // 2b. AI control: treasury, research, reasoning log
        if (ImGui::CollapsingHeader("AI Control & Thoughts", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RenderAiPanel(ai, entities);
        }

        // 3. Mission & Levels
        if (ImGui::CollapsingHeader("Mission Controller", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Current Mission: %d (Map: m%d)", mission.GetCurrentMissionId(), map.GetWidth());
            ImGui::Text("Objective: %s", mission.GetObjectiveText().c_str());
            ImGui::Text("Player Kills: %d", entities.GetPlayerKillCount());

            for (int m = 0; m <= 7; ++m)
            {
                if (m > 0)
                    ImGui::SameLine();
                char label[16];
                snprintf(label, sizeof(label), "M%d", m);
                if (ImGui::Button(label))
                {
                    // Level change
                }
            }
        }

        // 4. Audio & Music
        if (ImGui::CollapsingHeader("Sound & Audio Tracks"))
        {
            ImGui::Text("Current Music Track: %d", sound.GetCurrentTrack());
            if (ImGui::Button("Menu Theme"))
            {
                sound.PlayMenuMusic();
            }
            ImGui::SameLine();
            if (ImGui::Button("Random Game Track"))
            {
                sound.PlayRandomGameTrack();
            }
            ImGui::SameLine();
            if (ImGui::Button(sound.IsMuted() ? "Unmute" : "Mute"))
            {
                sound.ToggleMute();
            }
        }

        // 5. Entity Inspector
        if (ImGui::CollapsingHeader("Entity Inspector"))
        {
            ImGui::Text("Alive Entities Count: %zu", entities.GetAllEntities().size());
            ImGui::BeginChild("EntitiesList", ImVec2(0, 160), true);
            for (auto &e : entities.GetAllEntities())
            {
                if (!e.alive)
                    continue;
                const char *factionStr = (e.faction == Faction::Resistance) ? "[Red/Res]" : "[Blue/Conf]";
                const char *typeStr = e.isBuilding ? "Building" : "Unit";
                int arch = e.AsUnit() ? e.AsUnit()->archetypeId : -1;
                ImGui::Text(
                    "#%d %s %s (arch:%d) HP: %d/%d Pos:(%d,%d)",
                    e.id,
                    factionStr,
                    typeStr,
                    arch,
                    e.currentHp,
                    e.maxHp,
                    e.tileX,
                    e.tileY);
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

void DebugOverlay::RenderAiPanel(AIController &ai, EntityManager &entities)
{
    (void) entities;
    // --- Treasury: free edit field plus quick buttons ---
    if (!m_aiCreditsEditing)
        m_aiCreditsEdit = ai.GetEnemyCredits();
    ImGui::Text("AI credits: %d", ai.GetEnemyCredits());
    ImGui::SetNextItemWidth(160);
    // InputInt with step buttons does not support EnterReturnsTrue: take Enter from the deactivation instead
    ImGui::InputInt("##aiCredits", &m_aiCreditsEdit, 100, 1000);
    const bool entered = ImGui::IsItemDeactivatedAfterEdit()
                         && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
    m_aiCreditsEditing = ImGui::IsItemActive();
    ImGui::SameLine();
    if (ImGui::Button("Set") || entered)
    {
        ai.SetEnemyCredits(std::max(0, m_aiCreditsEdit));
        m_aiCreditsEditing = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("+500"))
        ai.SetEnemyCredits(ai.GetEnemyCredits() + 500);
    ImGui::SameLine();
    if (ImGui::Button("+2000"))
        ai.SetEnemyCredits(ai.GetEnemyCredits() + 2000);
    ImGui::SameLine();
    if (ImGui::Button("Bankrupt (0)"))
        ai.SetEnemyCredits(0);

    // --- Research: what the AI has, what it is on, what comes next ---
    ImGui::Separator();
    ImGui::Text(
        "Research (%s, %s):",
        AiFaction() == Faction::Confederation ? "Confederation / Tech Center" : "Resistance / Laboratory",
        HasResearchBuilding(Players::OwnerOf(AiFaction()), entities) ? "building ready" : "NO BUILDING - paused");
    if (ImGui::BeginTable(
            "aiResearch", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Technology");
        ImGui::TableSetupColumn("Cost");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        for (int i = 0; i < kResearchCount; ++i)
        {
            const ResearchDef &r = researchDef(AiFaction(), i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name);
            ImGui::TableNextColumn();
            ImGui::Text("$%d", r.cost);
            ImGui::TableNextColumn();
            if (r.playerOnly)
            {
                ImGui::TextDisabled("player only");
            }
            else if (ai.IsTechnologyResearched(i))
            {
                ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.45f, 1.0f), "researched");
            }
            else if (i == ai.GetResearchIndex() && ai.IsResearchActive())
            {
                ImGui::ProgressBar(ai.GetResearchProgress(), ImVec2(120, 0));
            }
            else if (i == ai.GetResearchIndex())
            {
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "next (waiting)");
            }
            else
            {
                ImGui::TextDisabled("queued");
            }
        }
        ImGui::EndTable();
    }

    // --- What the AI is doing right now ---
    ImGui::Separator();
    ImGui::TextUnformatted("Right now:");
    for (const std::string &line : ai.CurrentIntentions())
        ImGui::BulletText("%s", line.c_str());

    // --- Reasoning log ---
    ImGui::Separator();
    static const char *const kCategories[] = {"All", "WAVE", "ATTACK", "DEFENSE", "RESEARCH", "BUILD", "ECONOMY"};
    ImGui::TextUnformatted("Thoughts:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##thoughtFilter", &m_thoughtFilter, kCategories, IM_ARRAYSIZE(kCategories));
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_thoughtAutoScroll);
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        ai.ClearThoughts();

    auto categoryColor = [](const std::string &c) {
        if (c == "ATTACK")
            return ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
        if (c == "DEFENSE")
            return ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
        if (c == "RESEARCH")
            return ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
        if (c == "BUILD")
            return ImVec4(0.6f, 0.9f, 0.5f, 1.0f);
        if (c == "ECONOMY")
            return ImVec4(0.95f, 0.85f, 0.4f, 1.0f);
        return ImVec4(0.8f, 0.8f, 0.85f, 1.0f); // WAVE
    };
    const auto &thoughts = ai.GetThoughts();
    if (ImGui::BeginChild("aiThoughts", ImVec2(0, 220), ImGuiChildFlags_Borders))
    {
        for (const AIThought &t : thoughts)
        {
            if (m_thoughtFilter > 0 && t.category != kCategories[m_thoughtFilter])
                continue;
            const int sec = static_cast<int>(t.time);
            ImGui::TextDisabled("%02d:%02d", sec / 60, sec % 60);
            ImGui::SameLine();
            ImGui::TextColored(categoryColor(t.category), "%-8s", t.category.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", t.text.c_str());
        }
        // Follow new lines while the user has not scrolled up
        if (m_thoughtAutoScroll && ai.GetThoughtSerial() != m_thoughtsSeen
            && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f)
        {
            ImGui::SetScrollHereY(1.0f);
        }
        m_thoughtsSeen = ai.GetThoughtSerial();
    }
    ImGui::EndChild();
}

} // namespace OpenArtOfWar
#endif // AOW_DEBUG