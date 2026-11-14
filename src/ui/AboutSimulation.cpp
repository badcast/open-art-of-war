#include "ui/AboutSimulation.hpp"
#include "audio/GameSounds.hpp"
#include "audio/SoundEngine.hpp"

#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"
#include "core/LoaderResources.hpp"
#include "core/Localization.hpp"
#include "core/Utility.hpp"
#include "game/Building.hpp"
#include "game/EntityManager.hpp"
#include "game/Pathfinder.hpp"
#include "game/Players.hpp"
#include "game/Research.hpp"
#include "game/Unit.hpp"
#include "game/TileMap.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/NightLighting.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "graphics/TextureManager.hpp"
#include "ui/UiKit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <tuple>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar
{
namespace
{
struct View
{
    float x, y, zoom;
};
struct Shot
{
    float duration;
    View from, to;
};
constexpr float kTick = 0.05f;

View ReadView(const nlohmann::json &v, View fallback)
{
    if (!v.is_object())
        return fallback;
    const View result {v.value("x", fallback.x), v.value("y", fallback.y), v.value("zoom", fallback.zoom)};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.zoom))
        return fallback;
    return result;
}
} // namespace

struct AboutSimulation::Impl
{
    TileMap map;
    EntityManager entities;
    Pathfinder pathfinder;
    Camera camera;
    std::array<PlayerSlot, kMaxPlayers> savedPlayers {};
    std::array<PlayerSlot, kMaxPlayers> demoPlayers {};
    PlayerId savedLocal {0};
    bool playersSaved {false};
    std::uint32_t savedRng {0};
    ResearchState savedResearch {};
    ResearchState demoResearch {};
    std::uint32_t demoRng {0x4f414f57u};
    float musicVolume {0.7f}, sfxVolume {0.5f};
    bool allTechnologies {true};
    float productionSpeed {10.0f}, minZoom {0.1f}, maxZoom {1.25f};
    int maxUnits {1200};
    int maxUnitsPerType {40};
    View startView {37.5f, 37.0f, 1.0f};
    std::string mapPath {"assets/maps/aboutmap.json"};
    std::string music {"Machinimasound - Nightshift"};
    nlohmann::json mapData;
    std::vector<Shot> shots;
    int creditsWidth {-1};
    std::vector<std::string> creditsSource, wrappedCredits;
    std::vector<std::pair<int, Faction>> orders;
    struct Factory
    {
        int id;
        float elapsed;
        int archetype {-1};
    };
    std::array<std::vector<Factory>, 2> factories;
    std::size_t nextOrder {0}, shot {0};
    std::array<int, 2> produced {};
    std::array<int, 2> initialUnits {};
    std::array<std::vector<std::tuple<int, int, int>>, 2> initialPositions;
    std::array<bool, 19> producedTypes {};
    float fadeSeconds {0.7f}, creditsDuration {20.0f}, elapsed {0.0f}, creditsTime {0.0f};
    float accumulator {0.0f}, animTime {0.0f};
    float rallyTimer {0.0f};
    NightLighting nightLighting;
    bool nightConfigured {false};
    bool startNight {true};
    float nightHoldSeconds {12.0f};
    float dawnTransitionSeconds {18.0f};
    float dayNightTime {0.0f};
    bool resetPending {false};
    unsigned worldGeneration {0};

    float NightStrength() const
    {
        if (!startNight)
            return 0.0f;
        if (dayNightTime <= nightHoldSeconds)
            return 1.0f;
        const float t = (dayNightTime - nightHoldSeconds) / std::max(0.01f, dawnTransitionSeconds);
        if (t >= 1.0f)
            return 0.0f;
        const float s = 1.0f - t;
        return s * s * (3.0f - 2.0f * s);
    }
    enum class Phase
    {
        Moving,
        FadeOut,
        FadeIn
    } phase {Phase::Moving};

    void Enter()
    {
        if (playersSaved)
            return;
        for (PlayerId p = 0; p < kMaxPlayers; ++p)
            savedPlayers[p] = Players::Slot(p);
        savedLocal = Players::Local();
        savedResearch = CaptureResearchState();
        savedRng = Utility::Rng().State();
        playersSaved = true;
        Players::Setup(demoPlayers, 0);
        RestoreResearchState(demoResearch);
        Utility::Rng().Reseed(demoRng);
    }
    void Leave()
    {
        if (!playersSaved)
            return;
        demoResearch = CaptureResearchState();
        demoRng = Utility::Rng().State();
        Players::Setup(savedPlayers, savedLocal);
        RestoreResearchState(savedResearch);
        Utility::Rng().Reseed(savedRng);
        playersSaved = false;
    }
    struct Scope
    {
        Impl &scene;
        explicit Scope(Impl &s)
            : scene(s)
        {
            scene.Enter();
        }
        ~Scope()
        {
            scene.Leave();
        }
    };
    ~Impl()
    {
        Leave();
    }

    bool LoadConfig(std::string_view configPath)
    {
        std::string text;
        nlohmann::json cfg;
        if (Assets::ReadText(configPath, text))
            cfg = JsonFile::ReadYamlFromString(text);
        if (cfg.is_object())
        {
            mapPath = cfg.value("map", mapPath);
            music = cfg.value("audio_name", music);
            if (cfg.contains("volume") && cfg["volume"].is_object())
            {
                const auto &volume = cfg["volume"];
                musicVolume = std::clamp(volume.value("music", musicVolume), 0.0f, 1.0f);
                sfxVolume = std::clamp(volume.value("sfx", sfxVolume), 0.0f, 1.0f);
            }
            allTechnologies = cfg.value("all_technologies", allTechnologies);
            productionSpeed = std::clamp(cfg.value("production_speed_multiplier", productionSpeed), 0.1f, 100.0f);
            maxUnits = std::clamp(cfg.value("max_units", maxUnits), 1, 100000);
            maxUnitsPerType = std::clamp(cfg.value("max_units_per_type", maxUnitsPerType), 1, 100000);
            if (cfg.contains("camera") && cfg["camera"].is_object())
            {
                const auto &cam = cfg["camera"];
                startView = ReadView(cam.value("start", nlohmann::json::object()), startView);
                minZoom = std::clamp(cam.value("min_zoom", minZoom), 0.1f, 1.25f);
                maxZoom = std::clamp(cam.value("max_zoom", maxZoom), minZoom, 1.25f);
            }
            fadeSeconds = std::clamp(cfg.value("fade_seconds", fadeSeconds), 0.1f, 5.0f);
            creditsDuration = std::clamp(cfg.value("credits_time", creditsDuration), 1.0f, 3600.0f);
            if (cfg.contains("night") && cfg["night"].is_object())
            {
                const auto &night = cfg["night"];
                startNight = night.value("enabled", startNight);
                nightHoldSeconds = std::max(0.0f, night.value("hold_seconds", nightHoldSeconds));
                dawnTransitionSeconds = std::max(0.1f, night.value("transition_seconds", dawnTransitionSeconds));
            }
            if (cfg.contains("shots") && cfg["shots"].is_array())
                for (const auto &item : cfg["shots"])
                {
                    if (!item.is_object())
                        continue;
                    const View start = ReadView(
                        item.value("from", nlohmann::json::object()), shots.empty() ? startView : shots.back().to);
                    const View end = ReadView(item.value("to", nlohmann::json::object()), start);
                    const float duration = item.value("duration", 18.0f);
                    if (std::isfinite(duration) && duration >= 1.0f && duration <= 300.0f)
                        shots.push_back({duration, start, end});
                }
        }
        if (shots.empty())
            shots
                = {{18, startView, {37.5f, 37, 0}},
                   {18, {22, 57, 1.25f}, {37, 42, 1.05f}},
                   {18, {38, 38, 1.25f}, {48, 28, 1.05f}},
                   {18, {56, 18, 1.25f}, {45, 36, 0.9f}},
                   {18, {37, 37, 0.8f}, {37.5f, 37, 0}}};
        return true;
    }

    bool ResetWorld()
    {
        if (!map.LoadMap(mapPath))
            return false;
        ++worldGeneration;
        resetPending = false;
        Utility::Rng().Reseed(Utility::VisualRng().Next());
        ResetResearch();
        map.SetFogEnabled(false);
        entities.Clear();
        const auto unitsCount = mapData.contains("units") && mapData["units"].is_array() ? mapData["units"].size() : 0;
        const auto buildingsCount
            = mapData.contains("buildings") && mapData["buildings"].is_array() ? mapData["buildings"].size() : 0;
        entities.SetEntityLimit(
            static_cast<int>(
                std::min<std::size_t>(std::max<std::size_t>(unitsCount, maxUnits) + buildingsCount, 200000)));
        pathfinder.SetMap(&map);
        pathfinder.SetEntityManager(&entities);
        orders.clear();
        for (auto &side : factories)
            side.clear();
        nextOrder = 0;
        rallyTimer = 0.0f;
        dayNightTime = 0.0f;
        produced = {};
        initialUnits = {};
        for (auto &positions : initialPositions)
            positions.clear();
        producedTypes = {};
        auto ownerOf = [](Faction faction) -> PlayerId {
            return faction == Faction::Confederation ? 1 : 0;
        };
        if (mapData.contains("buildings") && mapData["buildings"].is_array())
            for (const auto &entry : mapData["buildings"])
            {
                const Faction faction = Utility::ParseFaction(entry.value("faction", "resistance"));
                const int x = entry.value("x", 0), y = entry.value("y", 0);
                const int id = entities.SpawnBuildingByIndex(
                    entry.value("buildingIndex", 0), x, y, faction, 1.0f, ownerOf(faction));
                if (const Building *b = entities.GetBuilding(id))
                {
                    if (b->IsUnitProducer())
                        factories[faction == Faction::Confederation ? 1 : 0].push_back({id, 0.0f, -1});
                    for (int yy = y; yy < y + b->buildHeight && yy < map.GetHeight(); ++yy)
                        for (int xx = x; xx < x + b->buildWidth && xx < map.GetWidth(); ++xx)
                            if (xx >= 0 && yy >= 0)
                                map.SetPassable(xx, yy, false);
                }
            }
        if (mapData.contains("units") && mapData["units"].is_array())
            for (const auto &entry : mapData["units"])
            {
                const Faction faction = Utility::ParseFaction(entry.value("faction", "resistance"));
                if (faction != Faction::Resistance && faction != Faction::Confederation)
                    continue;
                const int side = faction == Faction::Confederation ? 1 : 0;
                const int x = entry.value("x", 0), y = entry.value("y", 0);
                const int id
                    = entities.SpawnUnitByArchetype(entry.value("archetypeId", 0), x, y, faction, ownerOf(faction));
                if (id > 0)
                {
                    orders.emplace_back(id, faction);
                    ++initialUnits[side];
                    initialPositions[side].emplace_back(id, x, y);
                }
            }
        if (allTechnologies)
            for (PlayerId player = 0; player < 2; ++player)
                for (int technology = 0; technology < kResearchCount; ++technology)
                    CompleteResearch(player, technology, entities);
        accumulator = 0.0f;
        animTime = 0.0f;
        return true;
    }

    void SetView(const View &a, const View &b, float t)
    {
        const float fit
            = std::min(
                  camera.GetViewportWidth() / static_cast<float>(std::max(1, map.GetWidth() * TILE_WIDTH)),
                  camera.GetViewportHeight() / static_cast<float>(std::max(1, map.GetHeight() * TILE_HEIGHT)))
              * 0.94f;
        const float za = a.zoom > 0 ? a.zoom : fit, zb = b.zoom > 0 ? b.zoom : fit;
        camera.SetZoom(za + (zb - za) * t);
        camera.CenterOn((a.x + (b.x - a.x) * t) * TILE_WIDTH, (a.y + (b.y - a.y) * t) * TILE_HEIGHT);
    }
};

AboutSimulation::AboutSimulation()
    : m_impl(std::make_unique<Impl>())
{}
AboutSimulation::~AboutSimulation() = default;

bool AboutSimulation::Start(TextureManager &textures, int width, int height, std::string_view configPath)
{
    auto &s = *m_impl;
    try
    {
        s.LoadConfig(configPath);
        std::string text;
        if (!Assets::ReadText(s.mapPath, text))
            return false;
        s.mapData = nlohmann::json::parse(text, nullptr, false);
        if (!s.mapData.is_object())
            return false;
        s.demoPlayers = {};
        s.demoPlayers[0] = {true, Faction::Resistance, 0, 0, false};
        s.demoPlayers[1] = {true, Faction::Confederation, 1, 0, false};
        Impl::Scope scope(s);
        s.entities.SetTextureManager(&textures);
        if (!s.entities.LoadCharacterConfigs() || !s.ResetWorld())
            return false;
        s.camera.SetViewport(width, height);
        s.camera.SetZoomLimits(s.minZoom, s.maxZoom);
        s.SetView(s.shots[0].from, s.shots[0].to, 0.0f);
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[About] Could not load showcase: " << e.what() << std::endl;
        return false;
    }
}

void AboutSimulation::Update(float deltaTime, SoundEngine *sound)
{
    auto &s = *m_impl;
    Impl::Scope scope(s);
    const float dt = std::clamp(deltaTime, 0.0f, 0.25f);
    s.creditsTime += dt;
    s.elapsed += dt;
    s.dayNightTime += dt;
    // Never catch up several expensive battle ticks in one menu frame on slower devices.
    s.accumulator = std::min(s.accumulator + dt, 2.0f * kTick);
    if (s.accumulator >= kTick)
    {
        s.entities.SnapshotRenderState();
        int livingUnits = 0;
        std::array<int, 19> livingByType {};
        for (const Entity &entity : s.entities.GetAllEntities())
            if (const Unit *unit = entity.AsUnit())
                if (unit->alive && unit->state != ActionState::Die)
                {
                    ++livingUnits;
                    if (unit->archetypeId >= 0 && unit->archetypeId < static_cast<int>(livingByType.size()))
                        ++livingByType[static_cast<std::size_t>(unit->archetypeId)];
                }
        auto underTypeLimit = [&](int archetypeId) {
            return archetypeId < 0 || archetypeId >= static_cast<int>(livingByType.size())
                   || livingByType[static_cast<std::size_t>(archetypeId)] < s.maxUnitsPerType;
        };
        for (auto &side : s.factories)
            for (auto &production : side)
            {
                Building *factory = s.entities.GetBuilding(production.id);
                if (!factory || !factory->alive)
                    continue;
                const Faction faction = factory->faction;
                const PlayerId owner = factory->owner;
                const bool vehicle = factory->type == UnitType::Factory;
                if (production.archetype < 0)
                {
                    std::vector<int> choices;
                    for (int candidate = 0; candidate <= 13; ++candidate)
                    {
                        const UnitConfig *option = s.entities.GetUnitConfig(candidate);
                        const int form = ProductionForm(owner, candidate);
                        if (option && option->faction == faction && !option->isSiegeMode
                            && option->producedIn == (vehicle ? "factory" : "barracks")
                            && IsUnitUnlocked(owner, candidate) && underTypeLimit(form))
                            choices.push_back(candidate);
                    }
                    if (choices.empty())
                    {
                        factory->isProducing = false;
                        continue;
                    }
                    production.archetype = choices[static_cast<std::size_t>(
                        Utility::Rng().Range(0, static_cast<int>(choices.size()) - 1))];
                }
                const int archetype = ProductionForm(owner, production.archetype);
                const UnitConfig *config = s.entities.GetUnitConfig(archetype);
                const float normalTime = config ? config->buildTime : 4.5f;
                const float buildTime = std::max(kTick, normalTime / s.productionSpeed);
                production.elapsed = std::min(buildTime, production.elapsed + kTick);
                const bool canSpawn = livingUnits < s.maxUnits && underTypeLimit(archetype);
                factory->isProducing = (production.elapsed < buildTime) || canSpawn;
                if (production.elapsed < buildTime)
                    continue;
                if (!canSpawn)
                    continue;
                int x = factory->tileX + factory->buildWidth, y = factory->tileY + factory->buildHeight;
                EntityManager::NearestOpenTile(s.map, x, y);
                const int spawned = s.entities.SpawnUnitByArchetype(archetype, x, y, faction, owner);
                if (Unit *unit = s.entities.GetUnit(spawned))
                {
                    ApplyCompletedResearch(*unit);
                    s.orders.emplace_back(spawned, faction);
                    if (owner < s.produced.size())
                        ++s.produced[owner];
                    if (archetype >= 0 && archetype < static_cast<int>(s.producedTypes.size()))
                        s.producedTypes[static_cast<std::size_t>(archetype)] = true;
                    if (archetype >= 0 && archetype < static_cast<int>(livingByType.size()))
                        ++livingByType[static_cast<std::size_t>(archetype)];
                    production.elapsed = 0.0f;
                    production.archetype = -1;
                    ++livingUnits;
                }
            }
        // Re-engage idle troops after they reach a position or lose their target.
        s.rallyTimer += kTick;
        if (s.nextOrder >= s.orders.size() && s.rallyTimer >= 5.0f)
        {
            s.orders.clear();
            s.nextOrder = 0;
            s.rallyTimer = 0.0f;
            for (const Entity &entity : s.entities.GetAllEntities())
                if (const Unit *unit = entity.AsUnit())
                    if (unit->alive && unit->state == ActionState::Idle && !unit->waitingForPath
                        && unit->targetEntityId == 0 && unit->owner < 2)
                        s.orders.emplace_back(unit->id, unit->faction);
        }
        // Orders are distributed over ticks so the showcase does not compute hundreds of routes at once.
        struct OrderGroup
        {
            int targetId;
            int x;
            int y;
            std::vector<int> units;
        };
        std::vector<OrderGroup> groups;
        for (int n = 0; n < 8 && s.nextOrder < s.orders.size(); ++n, ++s.nextOrder)
        {
            const int id = s.orders[s.nextOrder].first;
            const Unit *unit = s.entities.GetUnit(id);
            if (!unit || !unit->alive)
                continue;
            const Entity *target = nullptr;
            int bestDistance = std::numeric_limits<int>::max();
            for (const Entity &entity : s.entities.GetAllEntities())
            {
                if (!entity.alive || entity.state == ActionState::Die || (!entity.AsUnit() && !entity.AsBuilding())
                    || !Players::Hostile(unit->owner, entity.owner))
                    continue;
                const int dx = unit->tileX - entity.tileX, dy = unit->tileY - entity.tileY;
                const int distance = dx * dx + dy * dy;
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    target = &entity;
                }
            }
            if (target)
            {
                auto group = std::find_if(
                    groups.begin(), groups.end(), [target](const OrderGroup &g) { return g.targetId == target->id; });
                if (group == groups.end())
                {
                    groups.push_back({target->id, target->tileX, target->tileY, {id}});
                }
                else
                    group->units.push_back(id);
            }
        }
        for (const auto &group : groups)
            s.entities.IssueAttackMoveOrder(group.units, group.x, group.y, s.pathfinder);
        s.entities.Update(kTick, s.map, s.pathfinder);
        s.entities.UpdateElimination(s.map);
        s.animTime += kTick;
        s.accumulator -= kTick;
        if (!s.resetPending && (s.entities.IsEliminated(0) || s.entities.IsEliminated(1)))
        {
            s.resetPending = true;
            s.phase = Impl::Phase::FadeOut;
            s.elapsed = 0.0f;
            s.accumulator = 0.0f;
        }
    }
    s.entities.SetRenderAlpha(std::min(1.0f, s.accumulator / kTick));
    if (s.phase == Impl::Phase::Moving)
    {
        const Shot &shot = s.shots[s.shot];
        s.SetView(shot.from, shot.to, std::min(1.0f, s.elapsed / shot.duration));
        if (s.elapsed >= shot.duration)
        {
            s.phase = Impl::Phase::FadeOut;
            s.elapsed = 0.0f;
        }
    }
    else if (s.elapsed >= s.fadeSeconds)
    {
        s.elapsed = 0.0f;
        if (s.phase == Impl::Phase::FadeOut)
        {
            if (s.resetPending)
            {
                s.ResetWorld();
                s.shot = 0;
            }
            else
                s.shot = (s.shot + 1) % s.shots.size();
            s.SetView(s.shots[s.shot].from, s.shots[s.shot].to, 0.0f);
            s.phase = Impl::Phase::FadeIn;
        }
        else
            s.phase = Impl::Phase::Moving;
    }
    if (sound)
        GameSounds::PlayBattleCues(*sound, s.entities, s.camera, s.sfxVolume);
    else
        s.entities.TakeSoundCues();
}

void AboutSimulation::Render(
    Renderer &r,
    BitmapFontRenderer &font,
    const std::vector<std::string> &credits,
    int width,
    int height,
    bool showCredits)
{
    auto &s = *m_impl;
    Impl::Scope scope(s);
    s.camera.SetViewport(width, height);
    const int tick = static_cast<int>(s.animTime * 8.0f);
    for (PlayerId p = 0; p < kMaxPlayers; ++p)
    {
        const int color = Players::Slot(p).color;
        r.SetArmyColor(p, Players::IsActive(p) && color > 0, ArmyColor(color));
    }
    // A ground-coloured backing keeps subpixel tile seams from showing as black at the overview zoom.
    r.FillRectWorld(
        0,
        0,
        static_cast<float>(s.map.GetWidth() * TILE_WIDTH),
        static_cast<float>(s.map.GetHeight() * TILE_HEIGHT),
        s.camera,
        {44, 61, 29, 255});
    s.map.RenderTerrain(r, s.camera, false, tick);
    s.map.RenderWallRubble(r, s.camera, false);
    s.entities.RenderGroundRemnants(r, s.camera, s.map, false);
    s.map.RenderDecorations(r, s.camera, false, tick);
    s.entities.Render(r, s.camera, s.map, false, tick, false, false);
    r.EndWorldPass();
    const float nightStrength = s.NightStrength();
    if (nightStrength > 0.001f)
    {
        if (!s.nightConfigured)
        {
            NightConfig nightCfg;
            nightCfg.Load(LoaderResources::Nights());
            s.nightLighting.Configure(nightCfg);
            s.nightConfigured = true;
        }
        std::vector<EntityManager::LightSpot> &lights = s.nightLighting.Lights();
        s.entities.CollectLights(s.map, false, lights);
        s.nightLighting.Apply(
            r.GetSdlRenderer(), s.camera, s.map, false, false, lights, width, height, nightStrength);
    }
    float dark = 0.0f;
    if (s.phase == Impl::Phase::FadeOut)
        dark = std::min(1.0f, s.elapsed / s.fadeSeconds);
    if (s.phase == Impl::Phase::FadeIn)
        dark = 1.0f - std::min(1.0f, s.elapsed / s.fadeSeconds);
    if (dark > 0.0f)
        r.FillRectScreen(0, 0, width, height, {0, 0, 0, static_cast<Uint8>(dark * 255)});
    if (!showCredits)
        return;
    const int lineHeight = 25;
    const int maxWidth = std::max(40, width - 60);
    if (s.creditsWidth != maxWidth || s.creditsSource != credits)
    {
        s.creditsWidth = maxWidth;
        s.creditsSource = credits;
        s.wrappedCredits.clear();
        for (const auto &line : credits)
        {
            std::istringstream words(line);
            std::string word, current;
            while (words >> word)
            {
                const std::string candidate = current.empty() ? word : current + " " + word;
                if (!current.empty() && font.GetTextWidth(candidate, ui::FONT_BODY_BOLD) > maxWidth)
                {
                    s.wrappedCredits.push_back(current);
                    current = word;
                }
                else
                    current = candidate;
            }
            s.wrappedCredits.push_back(current);
        }
    }
    const auto &wrapped = s.wrappedCredits;
    const float rollLength = static_cast<float>(height + wrapped.size() * lineHeight + height / 3);
    const float scroll = std::fmod(s.creditsTime, s.creditsDuration) / s.creditsDuration * rollLength;
    for (std::size_t i = 0; i < wrapped.size(); ++i)
    {
        const int y = height + static_cast<int>(i) * lineHeight - static_cast<int>(scroll);
        if (y < -lineHeight || y > height)
            continue;
        font.DrawString(
            ui::FitText(font, wrapped[i], ui::FONT_BODY_BOLD, maxWidth),
            width / 2,
            y,
            ui::FONT_BODY_BOLD,
            {255, 255, 255, 255},
            true,
            Anchor::HCENTER | Anchor::VCENTER);
    }
    font.DrawString(
        _("BACK"), width - 24, 24, ui::FONT_BODY_BOLD, {255, 255, 255, 220}, true, Anchor::RIGHT | Anchor::TOP);
}

const std::string &AboutSimulation::MusicName() const
{
    return m_impl->music;
}
void AboutSimulation::RestartCredits()
{
    m_impl->creditsTime = 0.0f;
}
float AboutSimulation::MusicVolume() const
{
    return m_impl->musicVolume;
}
float AboutSimulation::SfxVolume() const
{
    return m_impl->sfxVolume;
}
bool AboutSimulation::AllResearchCompleted() const
{
    constexpr std::uint32_t all = (1u << kResearchCount) - 1u;
    return m_impl->demoResearch[0] == all && m_impl->demoResearch[1] == all;
}
float AboutSimulation::SimulationSeconds() const
{
    return m_impl->animTime;
}
float AboutSimulation::CameraZoom() const
{
    return m_impl->camera.GetZoom();
}
unsigned AboutSimulation::WorldGeneration() const
{
    return m_impl->worldGeneration;
}
int AboutSimulation::DistinctProducedTypes() const
{
    return static_cast<int>(std::count(m_impl->producedTypes.begin(), m_impl->producedTypes.end(), true));
}
int AboutSimulation::ProducedUnits(unsigned player) const
{
    return player < m_impl->produced.size() ? m_impl->produced[player] : 0;
}
int AboutSimulation::ProducingFactories(unsigned player) const
{
    if (player >= m_impl->factories.size())
        return 0;
    int count = 0;
    for (const auto &production : m_impl->factories[player])
    {
        const Building *factory = m_impl->entities.GetBuilding(production.id);
        if (factory && factory->alive && factory->isProducing)
            ++count;
    }
    return count;
}
int AboutSimulation::MaxLivingUnitsOfAnyType() const
{
    std::array<int, 19> counts {};
    int best = 0;
    for (const Entity &entity : m_impl->entities.GetAllEntities())
        if (const Unit *unit = entity.AsUnit())
            if (unit->alive && unit->state != ActionState::Die && unit->archetypeId >= 0
                && unit->archetypeId < static_cast<int>(counts.size()))
            {
                best = std::max(best, ++counts[static_cast<std::size_t>(unit->archetypeId)]);
            }
    return best;
}
int AboutSimulation::InitialUnits(unsigned player) const
{
    return player < m_impl->initialUnits.size() ? m_impl->initialUnits[player] : 0;
}
int AboutSimulation::MovedInitialUnits(unsigned player) const
{
    if (player >= m_impl->initialPositions.size())
        return 0;
    int moved = 0;
    for (const auto &[id, x, y] : m_impl->initialPositions[player])
        if (const Unit *unit = m_impl->entities.GetUnit(id))
            if (unit->tileX != x || unit->tileY != y || std::abs(unit->subX) > 0.01f || std::abs(unit->subY) > 0.01f)
                ++moved;
    return moved;
}
int AboutSimulation::DamagedInitialUnits(unsigned player) const
{
    if (player >= m_impl->initialPositions.size())
        return 0;
    int damaged = 0;
    for (const auto &[id, x, y] : m_impl->initialPositions[player])
    {
        (void) x;
        (void) y;
        const Unit *unit = m_impl->entities.GetUnit(id);
        if (!unit || !unit->alive || unit->currentHp < unit->maxHp)
            ++damaged;
    }
    return damaged;
}
} // namespace OpenArtOfWar
