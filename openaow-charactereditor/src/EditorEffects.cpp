#include "CharacterEditor.hpp"

#include "game/Effects.hpp"

#include <algorithm>
#include <cmath>

namespace CharEditor {

namespace {

using OpenArtOfWar::EffectsConfig;
using OpenArtOfWar::TILE_HEIGHT;
using OpenArtOfWar::TILE_WIDTH;

constexpr int kAnimCapacity = 400;

struct WreckClass {
    const char* label;      // gettext msgid
    const char* key;        // key in effects.yaml
};

const WreckClass kWreckClasses[] = {
    {"Vehicle", "vehicle"},
    {"Light vehicle", "light"},
    {"Heavy vehicle", "heavy"},
    {"Rubble 2x2", "building_small"},
    {"Rubble 3x3", "building_medium"},
    {"Rubble 4x4", "building_large"},
};

struct BuildingPreset {
    int w, h;
};
const BuildingPreset kBuildingPresets[3] = {{2, 2}, {3, 3}, {4, 3}};

} // namespace

void CharacterEditor::EnsureEffects() {
    if (effectsTried) return;
    effectsTried = true;
    if (LoadYamlFile(effectsYaml, "effects.yaml")) {
        yamlAnimPaths.insert({"explosions.vehicle.main", "explosions.vehicle.second", "explosions.building.small",
                              "explosions.building.power", "explosions.building.large", "explosions.building.extra_anims",
                              "explosions.building.big_anim"});
        EffectsChanged();
        StartExplosionPreview(0);
    }
}

void CharacterEditor::EffectsChanged() {
    if (effectsYaml.Loaded()) EffectsConfig::Apply(effectsYaml.text());
}

SDL_Texture* CharacterEditor::AnimThumbnail(int animId) {
    auto it = animThumbs.find(animId);
    if (it != animThumbs.end()) return it->second;
    constexpr int kSize = 64;
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, kSize, kSize);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_Texture* previous = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, tex);
        SDL_SetRenderClipRect(renderer, nullptr);
        SDL_SetRenderDrawColor(renderer, 28, 30, 36, 255);
        SDL_RenderClear(renderer);
        OpenArtOfWar::Camera cam(kSize, kSize);
        cam.SetZoom(1.0f);
        cam.SetPosition(-kSize * 0.5f, -kSize * 0.5f);
        const OpenArtOfWar::IndexRange r = textures->GetAnimRange(animId);
        float ox = 0.0f, oy = 0.0f;
        OpenArtOfWar::ExplosionCentering(animId, ox, oy);
        const int count = std::max(1, r.end - r.start + 1);
        gameRenderer->DrawAnimSequenceWorld(animId, count / 3, ox, oy, cam);
        SDL_SetRenderTarget(renderer, previous);
    }
    animThumbs[animId] = tex;
    return tex;
}

void CharacterEditor::DrawAnimGallery(float height) {
    ImGui::TextUnformatted(_("Animations"));
    FieldHint("Animations");
    if (!animPickPath.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f), "%s", _("Click an animation to put it in the chosen field"));
    }
    ImGui::BeginChild("anim-gallery", ImVec2(0.0f, std::max(80.0f, height - ImGui::GetTextLineHeightWithSpacing())), true);
    const float cell = 66.0f;
    const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cell + 4.0f)));
    int k = 0;
    for (int id = 0; id < kAnimCapacity; ++id) {
        const OpenArtOfWar::IndexRange r = textures->GetAnimRange(id);
        if (r.end < r.start) continue;
        if (k++ % perRow != 0) ImGui::SameLine();
        ImGui::PushID(id);
        const bool visible = ImGui::IsRectVisible(ImVec2(cell, cell));
        SDL_Texture* thumb = visible ? AnimThumbnail(id) : nullptr;
        const bool selected = id == fxAnim && fxPreset == 4;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
        const bool clicked = thumb ? ImGui::ImageButton("a", reinterpret_cast<ImTextureID>(thumb), ImVec2(cell - 8, cell - 8))
                                   : ImGui::Button("...", ImVec2(cell, cell));
        if (selected) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(_("Animation %d: %d frames"), id, r.end - r.start + 1);
        }
        if (clicked) {
            if (!animPickPath.empty()) {
                // The field belongs to the file of the tab: projectiles.yaml or effects.yaml
                const bool projectiles = editorMode == EditorMode::Projectiles;
                CommentedYaml& y = projectiles ? projectilesYaml : effectsYaml;
                const YAML::Node target = y.At(animPickPath);
                bool ok = false;
                if (target.IsSequence()) {
                    std::string list = "[";
                    for (size_t i = 0; i < target.size(); ++i) list += target[i].Scalar() + ", ";
                    ok = y.Set(animPickPath, list + std::to_string(id) + "]");
                } else {
                    ok = y.Set(animPickPath, std::to_string(id));
                }
                if (ok) {
                    animPickPath.clear();
                    if (projectiles) {
                        ProjectilesChanged();
                        projShots.clear();
                    } else {
                        EffectsChanged();
                        StartExplosionPreview(fxPreset);
                    }
                }
            } else {
                fxAnim = id;
                StartExplosionPreview(4);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void CharacterEditor::StartExplosionPreview(int preset) {
    fxPreset = preset;
    fxEvents.clear();
    fxTime = 0.0f;
    fxRestartTimer = 0.0f;
    const EffectsConfig& fx = EffectsConfig::Get();
    auto pick = [](const std::vector<int>& list, int i) { return list.empty() ? -1 : list[static_cast<size_t>(i) % list.size()]; };
    static int seed = 0;
    ++seed;
    auto add = [&](float x, float y, int anim, float delay) {
        if (anim < 0) return;
        FxEvent e;
        e.x = x;
        e.y = y;
        e.anim = anim;
        e.delay = delay;
        fxEvents.push_back(e);
    };
    using OpenArtOfWar::Utility::RandomFloat;
    using OpenArtOfWar::Utility::RandomInt;
    if (preset == 0) {
        add(0.0f, 0.0f, pick(fx.vehicleMain, seed), 0.0f);
        add(RandomFloat(-fx.vehicleSecondSpreadX, fx.vehicleSecondSpreadX),
            RandomFloat(fx.vehicleSecondSpreadYMin, fx.vehicleSecondSpreadYMax), pick(fx.vehicleSecond, seed + 2),
            RandomFloat(fx.vehicleSecondDelayMin, fx.vehicleSecondDelayMax));
    } else if (preset >= 1 && preset <= 3) {
        const BuildingPreset& b = kBuildingPresets[preset - 1];
        const int main = preset == 1 ? fx.buildingSmallMain : preset == 2 ? fx.buildingPowerMain : fx.buildingLargeMain;
        add(0.0f, 0.0f, main, 0.0f);
        const int extra = std::clamp(b.w * b.h / 2, fx.buildingExtraMin, fx.buildingExtraMax);
        for (int i = 0; i < extra; ++i) {
            const float x = (RandomFloat(0.2f, b.w - 0.2f) - b.w * 0.5f) * TILE_WIDTH;
            const float y = (RandomFloat(0.2f, b.h - 0.2f) - b.h * 0.5f) * TILE_HEIGHT;
            const int anim = (i % fx.buildingBigEvery == 0) ? fx.buildingBigAnim : pick(fx.buildingExtraAnims, RandomInt(0, 3));
            add(x, y, anim,
                fx.buildingDelayStart + fx.buildingDelayStep * static_cast<float>(i) + RandomFloat(0.0f, fx.buildingDelayJitter));
        }
    } else {
        add(0.0f, 0.0f, fxAnim, 0.0f);
    }
}

void CharacterEditor::UpdateEffectsSim(float dt) {
    if (editorMode == EditorMode::Wrecks) {
        if (wreckPlay) {
            wreckClock += dt;
            if (wreckClock > wreckPlaySeconds + 1.0f) wreckClock = 0.0f;
        }
        return;
    }
    if (editorMode != EditorMode::Explosions) return;
    EnsureEffects();
    const float frameTime = EffectsConfig::Get().frameTime;
    fxTime += dt;
    bool allDone = true;
    for (FxEvent& e : fxEvents) {
        if (e.done) continue;
        allDone = false;
        if (e.delay > 0.0f) {
            e.delay -= dt;
            continue;
        }
        e.timer += dt;
        if (e.timer >= frameTime) {
            e.timer = 0.0f;
            const OpenArtOfWar::IndexRange r = textures->GetAnimRange(e.anim);
            if (++e.frame >= std::max(1, r.end - r.start + 1)) e.done = true;
        }
    }
    if (allDone && fxLoop) {
        fxRestartTimer += dt;
        if (fxRestartTimer > 0.9f) StartExplosionPreview(fxPreset);
    }
}

void CharacterEditor::DrawEffectsScene() {
    const EffectsConfig& fx = EffectsConfig::Get();
    const auto faction = wreckFaction == 0 ? OpenArtOfWar::Faction::Confederation : OpenArtOfWar::Faction::Resistance;
    if (editorMode == EditorMode::Wrecks) {
        EffectsConfig::Wreck w;
        switch (wreckClass) {
            case 0: w = fx.vehicle; break;
            case 1: w = fx.light; break;
            case 2: w = fx.heavy; break;
            case 3: w = fx.buildingSmall; break;
            case 4: w = fx.buildingMedium; break;
            default: w = fx.buildingLarge; break;
        }
        const int stages = std::max(1, w.stages);
        const float t = std::clamp(wreckClock / std::max(0.1f, wreckPlaySeconds), 0.0f, 0.999f);
        const int current = std::min(stages - 1, static_cast<int>(t * static_cast<float>(stages)));
        gameRenderer->DrawCompositeCentered(w.composite + current, 0.0f, -40.0f, *camera, faction);
        // Every stage in a row below, the current one marked
        const float step = 90.0f;
        for (int s = 0; s < stages; ++s) {
            const float cx = (static_cast<float>(s) - static_cast<float>(stages - 1) * 0.5f) * step;
            gameRenderer->DrawCompositeCentered(w.composite + s, cx, 100.0f, *camera, faction);
            if (s == current) {
                gameRenderer->FillRectWorld(cx - step * 0.5f + 2.0f, 112.0f, step - 4.0f, 3.0f, *camera,
                                            SDL_Color{232, 176, 74, 255});
            }
        }
        return;
    }
    // Explosions: the object that blows up, the wreck that appears from the middle of the blast, the blasts
    int w = 1, h = 1;
    if (fxPreset >= 1 && fxPreset <= 3) {
        w = kBuildingPresets[fxPreset - 1].w;
        h = kBuildingPresets[fxPreset - 1].h;
        gameRenderer->FillRectWorld(-w * TILE_WIDTH * 0.5f, -h * TILE_HEIGHT * 0.5f, static_cast<float>(w * TILE_WIDTH),
                                    static_cast<float>(h * TILE_HEIGHT), *camera, SDL_Color{90, 110, 130, 70});
    } else if (fxPreset == 0) {
        gameRenderer->FillRectWorld(-TILE_WIDTH * 0.5f, -TILE_HEIGHT * 0.5f, static_cast<float>(TILE_WIDTH),
                                    static_cast<float>(TILE_HEIGHT), *camera, SDL_Color{90, 110, 130, 70});
    }
    if (fxPreset <= 3 && fxTime >= 0.25f) {
        const EffectsConfig::Wreck wr = fxPreset == 0 ? fx.vehicle : fx.WreckOfBuilding(w, h);
        gameRenderer->DrawCompositeCentered(wr.composite, 0.0f, 0.0f, *camera, faction);
    }
    for (const FxEvent& e : fxEvents) {
        if (e.done || e.delay > 0.0f) continue;
        float ox = 0.0f, oy = 0.0f;
        OpenArtOfWar::ExplosionCentering(e.anim, ox, oy);
        gameRenderer->DrawAnimSequenceWorld(e.anim, e.frame, e.x + ox, e.y + oy, *camera, faction);
    }
}

void CharacterEditor::DrawExplosionControls() {
    EnsureEffects();
    static const char* const labels[4] = {"Vehicle death", "Small building", "Power building", "Large building"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        const bool active = fxPreset == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
        if (Hinted(ImGui::Button(_(labels[i])), labels[i])) StartExplosionPreview(i);
        if (active) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Replay")), "Replay")) StartExplosionPreview(fxPreset);
    ImGui::SameLine();
    Hinted(ImGui::Checkbox(_("Loop"), &fxLoop), "Loop effect");
    ImGui::SetNextItemWidth(160.0f);
    Hinted(ImGui::SliderFloat(_("Speed"), &playbackSpeed, 0.1f, 2.0f, "%.2fx"), "Playback speed");
    if (fxPreset == 4) {
        const OpenArtOfWar::IndexRange r = textures->GetAnimRange(fxAnim);
        ImGui::SameLine();
        ImGui::Text(_("Animation %d: %d frames"), fxAnim, r.end - r.start + 1);
    }
}

void CharacterEditor::DrawWreckControls() {
    EnsureEffects();
    for (int i = 0; i < 6; ++i) {
        if (i == 3) ImGui::NewLine();
        else if (i) ImGui::SameLine();
        if (Hinted(ImGui::RadioButton(_(kWreckClasses[i].label), wreckClass == i), kWreckClasses[i].label)) wreckClass = i;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    if (Hinted(ImGui::RadioButton(_("Confederation"), wreckFaction == 0), "Confederation")) wreckFaction = 0;
    ImGui::SameLine();
    if (Hinted(ImGui::RadioButton(_("Resistance"), wreckFaction == 1), "Resistance")) wreckFaction = 1;
    Hinted(ImGui::Checkbox(_("Play burial"), &wreckPlay), "Play burial");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    Hinted(ImGui::SliderFloat(_("Seconds"), &wreckPlaySeconds, 2.0f, 60.0f, "%.0f s"), "Burial preview seconds");
    if (!wreckPlay) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        Hinted(ImGui::SliderFloat("##wreck-clock", &wreckClock, 0.0f, wreckPlaySeconds, "%.1f s"), "Burial time");
    }
    ImGui::TextDisabled("%s", _("In the game the burial takes the time of 'Burial seconds' (the right panel)."));
}

void CharacterEditor::DrawExplosionProperties() {
    EnsureEffects();
    if (!effectsYaml.Loaded()) {
        ImGui::TextWrapped("%s", _("The effects file could not be loaded."));
        return;
    }
    DrawSettingsBar(effectsYaml, "effects.yaml");
    ImGui::Separator();
    bool changed = EditYamlScalar(effectsYaml, "explosions.frame_time", _("Frame time"));
    if (ImGui::CollapsingHeader(_("Vehicle death"), ImGuiTreeNodeFlags_DefaultOpen))
        changed |= EditYamlTree(effectsYaml, "explosions.vehicle", 1);
    if (ImGui::CollapsingHeader(_("Building death"), ImGuiTreeNodeFlags_DefaultOpen))
        changed |= EditYamlTree(effectsYaml, "explosions.building", 1);
    if (changed) {
        EffectsChanged();
        StartExplosionPreview(fxPreset);
    }
    ImGui::Separator();
    DrawAnimGallery(ImGui::GetContentRegionAvail().y);
}

void CharacterEditor::DrawWreckTabProperties() {
    EnsureEffects();
    if (!effectsYaml.Loaded()) {
        ImGui::TextWrapped("%s", _("The effects file could not be loaded."));
        return;
    }
    DrawSettingsBar(effectsYaml, "effects.yaml");
    ImGui::Separator();
    bool changed = EditYamlScalar(effectsYaml, "wrecks.burial_seconds", _("Burial seconds"));
    changed |= EditYamlScalar(effectsYaml, "wrecks.fade_seconds", _("Fade seconds"));
    for (int i = 0; i < 6; ++i) {
        const std::string base = std::string("wrecks.") + kWreckClasses[i].key;
        if (!ImGui::CollapsingHeader(_(kWreckClasses[i].label), i == wreckClass ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
        ImGui::PushID(i);
        changed |= EditYamlScalar(effectsYaml, base + ".composite", _("Composite"));
        changed |= EditYamlScalar(effectsYaml, base + ".stages", _("Stages"));
        // The known wreck families of the game: one click sets composite and stage count
        ImGui::TextUnformatted(_("Families"));
        FieldHint("Families");
        const int cur = static_cast<int>(effectsYaml.At(base + ".composite").as<int>(0));
        for (size_t f = 0; f < std::size(kWreckFamilies); ++f) {
            if (f) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(f));
            const bool selected = kWreckFamilies[f].composite == cur;
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
            SDL_Texture* thumb = CompositeThumbnail(kWreckFamilies[f].composite, true);
            const bool clicked = thumb ? ImGui::ImageButton("fam", reinterpret_cast<ImTextureID>(thumb), ImVec2(48, 32))
                                       : ImGui::Button(std::to_string(kWreckFamilies[f].composite).c_str());
            if (selected) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%d)", _(kWreckFamilies[f].label), kWreckFamilies[f].composite);
            if (clicked) {
                changed |= effectsYaml.Set(base + ".composite", std::to_string(kWreckFamilies[f].composite));
                changed |= effectsYaml.Set(base + ".stages", std::to_string(kWreckFamilies[f].stages));
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }

    // Which wreck every vehicle leaves
    if (ImGui::CollapsingHeader(_("Wreck of each vehicle"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const EffectsConfig& fx = EffectsConfig::Get();
        std::vector<int> light = fx.lightArchetypes, heavy = fx.heavyArchetypes;
        auto drop = [](std::vector<int>& v, int x) { v.erase(std::remove(v.begin(), v.end(), x), v.end()); };
        bool edited = false;
        for (int a = 0; a < OpenArtOfWar::Utility::kArchetypeCount; ++a) {
            if (OpenArtOfWar::Utility::IsInfantryArchetype(a)) continue;
            const auto* info = OpenArtOfWar::Utility::archetypeInfo(a);
            if (!info) continue;
            int cls = std::count(heavy.begin(), heavy.end(), a) ? 2 : std::count(light.begin(), light.end(), a) ? 1 : 0;
            ImGui::PushID(a);
            ImGui::Text("%2d %s", a, _(info->name));
            ImGui::SameLine(200.0f);
            int pick = cls;
            ImGui::RadioButton(_("Vehicle"), &pick, 0);
            ImGui::SameLine();
            ImGui::RadioButton(_("Light vehicle"), &pick, 1);
            ImGui::SameLine();
            ImGui::RadioButton(_("Heavy vehicle"), &pick, 2);
            ImGui::PopID();
            if (pick != cls) {
                drop(light, a);
                drop(heavy, a);
                if (pick == 1) light.push_back(a);
                if (pick == 2) heavy.push_back(a);
                edited = true;
            }
        }
        if (edited) {
            auto text = [](std::vector<int> v) {
                std::sort(v.begin(), v.end());
                std::string s = "[";
                for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + std::to_string(v[i]);
                return s + "]";
            };
            effectsYaml.Set("wrecks.light_archetypes", text(light));
            effectsYaml.Set("wrecks.heavy_archetypes", text(heavy));
            changed = true;
        }
    }
    if (changed) EffectsChanged();
}

} // namespace CharEditor
