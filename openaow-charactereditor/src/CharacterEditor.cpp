#include "CharacterEditor.hpp"

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

bool CharacterEditor::init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        status = std::string(_("SDL error: ")) + SDL_GetError();
        return false;
    }

    assetsRoot = findAssetRoot();
    if (assetsRoot.empty()) {
        status = _("Could not find the assets folder with graphics data.");
        return false;
    }
    OpenArtOfWar::Localization::init((assetsRoot / "locale").string(), "en");
    charactersDir = assetsRoot / "characters";

    window = SDL_CreateWindow(_("Open Art Of War Character Archetype Editor"), 1500, 900, SDL_WINDOW_RESIZABLE);
    if (!window) {
        status = std::string(_("SDL window error: ")) + SDL_GetError();
        return false;
    }
    // The SDL GPU renderer first: the shader effects (army colours) need it
    SDL_SetHint(SDL_HINT_GPU_DRIVER, "vulkan");
    renderer = SDL_CreateRenderer(window, "gpu");
    if (!renderer) renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    }
    if (renderer) SDL_SetRenderVSync(renderer, 1);
    if (!renderer) {
        status = std::string(_("SDL renderer error: ")) + SDL_GetError();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    const fs::path fontPath = assetsRoot / "fonts" / "Roboto-Regular.ttf";
    if (fs::exists(fontPath)) {
        io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 15.0f, nullptr,
                                     io.Fonts->GetGlyphRangesCyrillic());
    }
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.086f, 0.09f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.115f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.18f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.14f, 0.32f, 0.28f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.43f, 0.36f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.35f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.46f, 0.38f, 1.0f);

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    textures = std::make_unique<OpenArtOfWar::TextureManager>(renderer);
    textures->loadAllAssets(assetsRoot.string());
    gameRenderer = std::make_unique<OpenArtOfWar::Renderer>(renderer, textures.get());
    shaderEffects = std::make_unique<OpenArtOfWar::ShaderEffects>();
    if (shaderEffects->init(renderer)) gameRenderer->setEffects(shaderEffects.get());

    camera = std::make_unique<OpenArtOfWar::Camera>(1500, 900);
    previewZoom = 2.0f;
    cameraPanX = 0.0f;
    cameraPanY = 0.0f;
    camera->setZoom(previewZoom);
    loadCatalog();
    if (files.empty()) {
        status = _("No YAML files found in assets/characters.");
    } else {
        loadFile(files.front().path);
    }
    return true;
}

int CharacterEditor::run() {
    using Clock = std::chrono::steady_clock;
    auto previousFrame = Clock::now();
    bool running = true;
    while (running) {
        const auto now = Clock::now();
        const float deltaTime = std::chrono::duration<float>(now - previousFrame).count();
        previousFrame = now;
        const float simDt = std::clamp(deltaTime, 0.0f, 0.1f) * playbackSpeed;
        previewTime += simDt;
        if (loopPreview && previewMode == PreviewMode::Die && previewTime >= 3.0f) {
            previewTime = 0.0f;
        }
        if (editorMode == EditorMode::Simulation) {
            if (!battlePaused) battle.update(simDt);
        } else {
            updateCombatSim(simDt);
            updateEffectsSim(simDt);
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && (event.key.mod & SDL_KMOD_CTRL) &&
                       event.key.key == SDLK_S) {
                if (editorMode == EditorMode::Research) saveResearch();
                else if (editorMode == EditorMode::Explosions || editorMode == EditorMode::Wrecks ||
                         editorMode == EditorMode::GameUi || editorMode == EditorMode::Environment ||
                         editorMode == EditorMode::AiDifficulty) saveActiveSettings();
                else saveFile();
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_G &&
                       !ImGui::GetIO().WantCaptureKeyboard && !ImGui::IsAnyItemActive()) {
                testSiegeMode = !testSiegeMode;
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && previewRect.w > 0 && previewRect.h > 0) {
                float mouseX = 0.0f;
                float mouseY = 0.0f;
                SDL_GetMouseState(&mouseX, &mouseY);
                const bool overPreview = mouseX >= previewRect.x && mouseX < previewRect.x + previewRect.w &&
                                         mouseY >= previewRect.y && mouseY < previewRect.y + previewRect.h;
                if (overPreview) {
                    const float wheelRaw = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                        ? -event.wheel.y : event.wheel.y;
                    const int wheelY = (wheelRaw > 0.0f) ? 1 : ((wheelRaw < 0.0f) ? -1 : 0);
                    const float oldZoom = previewZoom;
                    previewZoom = std::clamp(previewZoom * std::pow(1.12f, static_cast<float>(wheelY)),
                                             0.5f, 4.0f);
                    if (std::abs(previewZoom - oldZoom) > 0.0001f) {
                        float mouseWorldX = 0.0f, mouseWorldY = 0.0f;
                        camera->screenToWorld(static_cast<float>(mouseX), static_cast<float>(mouseY), mouseWorldX, mouseWorldY);
                        const float centerScreenX = static_cast<float>(previewRect.x) + static_cast<float>(previewRect.w) * 0.5f;
                        const float centerScreenY = static_cast<float>(previewRect.y) + static_cast<float>(previewRect.h) * 0.5f;
                        cameraPanX = mouseWorldX - (static_cast<float>(mouseX) - centerScreenX) / previewZoom;
                        cameraPanY = mouseWorldY - (static_cast<float>(mouseY) - centerScreenY) / previewZoom;
                    }
                }
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        drawEditor();
        ImGui::Render();

        int width = 0;
        int height = 0;
        SDL_GetRenderOutputSize(renderer, &width, &height);
        camera->setViewport(width, height);

        if (previewRect.w > 0 && previewRect.h > 0) {
            const float centerScreenX = static_cast<float>(previewRect.x) + static_cast<float>(previewRect.w) * 0.5f;
            const float centerScreenY = static_cast<float>(previewRect.y) + static_cast<float>(previewRect.h) * 0.5f;
            camera->setZoom(previewZoom);
            camera->setPosition(-centerScreenX / previewZoom + cameraPanX,
                                -centerScreenY / previewZoom + cameraPanY);
        }

        SDL_SetRenderDrawColor(renderer, 18, 22, 23, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        drawPreviewScene();
        SDL_RenderPresent(renderer);
    }
    return 0;
}

CharacterEditor::~CharacterEditor() {
    for (auto& [comp, tex] : wreckThumbs) {
        if (tex) SDL_DestroyTexture(tex);
    }
    for (auto& [id, tex] : animThumbs) {
        if (tex) SDL_DestroyTexture(tex);
    }
    for (auto& [key, tex] : fontPreviews) {
        if (tex) SDL_DestroyTexture(tex);
    }
    if (gameRenderer) gameRenderer.reset();
    if (shaderEffects) shaderEffects.reset();
    if (textures) textures.reset();
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}

void CharacterEditor::sortCatalogFiles() {
    auto factionRank = [](const std::string& f) -> int {
        switch (OpenArtOfWar::Utility::parseFaction(f, OpenArtOfWar::Faction::Neutral)) {
            case OpenArtOfWar::Faction::Confederation: return 0;
            case OpenArtOfWar::Faction::Resistance: return 1;
            default: return 2;
        }
    };
    auto typeRank = [](const std::string& t) -> int {
        if (t == "unit") return 0;
        if (t == "building") return 1;
        return 2;
    };

    std::sort(files.begin(), files.end(), [&](const CharacterFile& left, const CharacterFile& right) {
        int f1 = factionRank(left.faction), f2 = factionRank(right.faction);
        if (f1 != f2) return f1 < f2;
        int t1 = typeRank(left.type), t2 = typeRank(right.type);
        if (t1 != t2) return t1 < t2;
        if (left.numericId != right.numericId) return left.numericId < right.numericId;
        return left.name < right.name;
    });
}

void CharacterEditor::loadCatalog() {
    files.clear();
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(charactersDir, error)) {
        if (error || !entry.is_regular_file()) continue;
        const std::string extension = entry.path().extension().string();
        if (extension != ".yaml" && extension != ".yml") continue;
        if (entry.path().filename() == "difficulty.yaml" || entry.path().filename() == "ai_difficulty.yaml") continue;
        if (entry.path().filename().string().rfind("research_", 0) == 0) continue; // edited in the Research tab
        try {
            const YAML::Node root = YAML::LoadFile(entry.path().string());
            if (root["default_difficulty"].IsDefined()) continue;
            std::string fType = yamlString(root, "type", "?");
            int numId = -1;
            if (fType == "building" || root["building_index"].IsDefined()) {
                numId = yamlInt(root, "building_index", -1);
            } else {
                numId = yamlInt(root, "archetype_id", -1);
            }
            files.push_back({entry.path(),
                             yamlString(root, "name", entry.path().stem().string()),
                             OpenArtOfWar::Utility::factionName(docFaction(root)),
                             fType,
                             numId,
                             yamlString(root, "id", entry.path().stem().string())});
        } catch (const std::exception&) {
            files.push_back({entry.path(), entry.path().stem().string(), _("Invalid YAML"), "?", -1, entry.path().stem().string()});
        }
    }
    sortCatalogFiles();
}

bool CharacterEditor::loadFile(const fs::path& path) {
    try {
        document = YAML::LoadFile(path.string());
        if (!document.IsMap()) {
            status = _("The YAML root must be a map.");
            return false;
        }
        activePath = path;
        textBuffers.clear();
        dirty = false;
        testSiegeMode = false;
        std::strncpy(savePath.data(), path.filename().string().c_str(), savePath.size() - 1);
        savePath.back() = '\0';
        status = std::string(_("Loaded: ")) + path.filename().string();

        YAML::Node combat = document["combat"];
        float cfgBulletSpeed = yamlFloat(combat, "bullet_speed", yamlFloat(combat, "projectile_speed", 0.0f));
        std::string proj = yamlString(mainWeapon(combat), "projectile", "bullet");
        if (cfgBulletSpeed > 10.0f) {
            previewBulletSpeed = cfgBulletSpeed;
        } else {
            previewBulletSpeed = (proj == "rocket") ? 420.0f : ((proj == "flame") ? 240.0f : 620.0f);
        }
        previewFxSpeed = yamlFloat(combat, "fx_speed", 1.0f);

        const VisualOffsets offsets = readVisualOffsets(document);
        muzzleOffsetX = offsets.muzzleX;
        muzzleOffsetY = offsets.muzzleY;
        turretOffsetX = offsets.turretX;
        turretOffsetY = offsets.turretY;
        hasCustomTurretOffset = offsets.customTurret;

        simProjectiles.clear();
        simShotCooldown = 0.0f;
        simBurstIndex = 0;
        simBurstDelayTimer = 0.0f;
        simShotCooldown2 = 0.0f;
        simBurstIndex2 = 0;
        simBurstDelayTimer2 = 0.0f;
        muzzleFlashTimer = 0.0f;
        turretFacingDir = facing;
        linkTurretToHull = true;

        if (!targetCustomized) {
            float ang = static_cast<float>(facing) * (static_cast<float>(M_PI) / 4.0f);
            targetWorldX = std::cos(ang) * 120.0f;
            targetWorldY = std::sin(ang) * 120.0f;
        }
        return true;
    } catch (const std::exception& error) {
        status = std::string(_("Could not load YAML: ")) + error.what();
        return false;
    }
}

bool CharacterEditor::saveFile() {
    if (!document || activePath.empty()) return false;
    try {
        YAML::Emitter emitter;
        emitter.SetIndent(2);
        emitter << document;
        std::ofstream output(activePath);
        if (!output) {
            status = std::string(_("Could not open file for writing: ")) + activePath.string();
            return false;
        }
        output << emitter.c_str() << '\n';
        if (!output) {
            status = std::string(_("Failed to write file: ")) + activePath.string();
            return false;
        }
        dirty = false;
        status = std::string(_("Saved: ")) + activePath.filename().string();
        reloadCatalogSelection();
        return true;
    } catch (const std::exception& error) {
        status = std::string(_("YAML save error: ")) + error.what();
        return false;
    }
}

bool CharacterEditor::saveFileAs() {
    if (!document || savePath[0] == '\0') return false;
    fs::path target(savePath.data());
    if (target.is_relative()) target = charactersDir / target;
    if (target.extension().empty()) target.replace_extension(".yaml");
    activePath = target.lexically_normal();
    if (!saveFile()) return false;
    loadCatalog();
    return loadFile(activePath);
}

void CharacterEditor::reloadCatalogSelection() {
    for (auto& file : files) {
        if (file.path == activePath) {
            file.name = yamlString(document, "name", activePath.stem().string());
            file.faction = OpenArtOfWar::Utility::factionName(docFaction(document));
            file.type = yamlString(document, "type", "?");
            file.stringId = yamlString(document, "id", activePath.stem().string());
            if (file.type == "building" || document["building_index"].IsDefined()) {
                file.numericId = yamlInt(document, "building_index", -1);
            } else {
                file.numericId = yamlInt(document, "archetype_id", -1);
            }
        }
    }
    sortCatalogFiles();
}

YAML::Node CharacterEditor::section(const char* name) {
    if (!document[name] || !document[name].IsMap()) {
        document[name] = YAML::Node(YAML::NodeType::Map);
    }
    return document[name];
}

bool CharacterEditor::editString(YAML::Node parent, const char* key, const char* label, const std::string& scope,
                    bool multiline) {
    const std::string bufferKey = scope + "/" + key;
    auto found = textBuffers.find(bufferKey);
    if (found == textBuffers.end()) {
        std::array<char, 1024> initial{};
        std::string value;
        try { value = parent[key].as<std::string>(""); } catch (...) {}
        std::strncpy(initial.data(), value.c_str(), initial.size() - 1);
        found = textBuffers.emplace(bufferKey, initial).first;
    }

    bool changed = false;
    ImGui::PushID(bufferKey.c_str());
    if (multiline) {
        changed = ImGui::InputTextMultiline(_(label), found->second.data(), found->second.size(),
                                            ImVec2(-1.0f, 72.0f));
    } else {
        changed = ImGui::InputText(_(label), found->second.data(), found->second.size());
    }
    fieldHint(label);
    ImGui::PopID();
    if (changed) parent[key] = std::string(found->second.data());
    return changed;
}

bool CharacterEditor::editInt(YAML::Node parent, const char* key, const char* label, int fallback) {
    int value = yamlInt(parent, key, fallback);
    const bool edited = ImGui::InputInt(_(label), &value);
    fieldHint(label);
    if (!edited) return false;
    parent[key] = value;
    return true;
}

bool CharacterEditor::editFloat(YAML::Node parent, const char* key, const char* label, float fallback,
                   float step) {
    float value = 0.0f;
    try { value = parent[key].as<float>(fallback); } catch (...) { value = fallback; }
    const bool edited = ImGui::DragFloat(_(label), &value, step, 0.0f, 0.0f, "%.2f");
    fieldHint(label);
    if (!edited) return false;
    parent[key] = value;
    return true;
}

bool CharacterEditor::editBool(YAML::Node parent, const char* key, const char* label, bool fallback) {
    bool value = yamlBool(parent, key, fallback);
    const bool edited = ImGui::Checkbox(_(label), &value);
    fieldHint(label);
    if (!edited) return false;
    parent[key] = value;
    return true;
}

bool CharacterEditor::editWeaponDelay(YAML::Node w) {
    bool changed = false;
    int mode = weaponRandomDelay(w) ? 1 : 0;
    const char* modes[] = {_("Fixed"), _("Random (min - max)")};
    ImGui::SetNextItemWidth(170.0f);
    if (hinted(ImGui::Combo(_("Firing delay"), &mode, modes, 2), "Firing delay")) {
        if (mode == 1) {
            const float base = yamlFloat(w, "cooldown", 1.0f);
            w["cooldown_mode"] = "random";
            w["cooldown_min"] = yamlFloat(w, "cooldown_min", base);
            w["cooldown_max"] = yamlFloat(w, "cooldown_max", base * 1.5f);
            w.remove("cooldown");
        } else {
            w["cooldown"] = yamlFloat(w, "cooldown_min", yamlFloat(w, "cooldown", 1.0f));
            w.remove("cooldown_mode");
            w.remove("cooldown_min");
            w.remove("cooldown_max");
        }
        changed = true;
    }
    if (mode == 1) {
        float range[2] = {yamlFloat(w, "cooldown_min", 1.0f), yamlFloat(w, "cooldown_max", 1.5f)};
        if (hinted(ImGui::DragFloat2(_("Delay min / max (s)"), range, 0.01f, 0.0f, 30.0f, "%.2f"), "Delay min / max (s)")) {
            range[0] = std::max(0.0f, range[0]);
            range[1] = std::max(range[0], range[1]);
            w["cooldown_min"] = range[0];
            w["cooldown_max"] = range[1];
            changed = true;
        }
    } else {
        changed |= editFloat(w, "cooldown", "Cooldown (seconds)", 1.0f);
    }
    return changed;
}

bool CharacterEditor::editProjectileCombo(YAML::Node parent) {
    static const char* s_projectiles[] = {"bullet", "shell", "rocket", "flame", "none"};
    static const char* s_labels[] = {
        "bullet (Bullet / Rifle / MG)",
        "shell (Cannon shell / Artillery)",
        "rocket (Rocket / Missile / SAM)",
        "flame (Flamethrower stream)",
        "none (No projectile / Unarmed)"
    };
    std::string current = yamlString(parent, "projectile", "bullet");
    int currentIdx = 0;
    for (int i = 0; i < 5; ++i) {
        if (current == s_projectiles[i]) {
            currentIdx = i;
            break;
        }
    }

    bool changed = false;
    if (hinted(ImGui::Combo(_("Projectile"), &currentIdx, s_labels, 5), "Projectile")) {
        parent["projectile"] = std::string(s_projectiles[currentIdx]);
        changed = true;
    }
    return changed;
}

void CharacterEditor::setMode(PreviewMode mode) {
    previewMode = mode;
    previewTime = 0.0f;
    simProjectiles.clear();
    simShotCooldown = 0.0f;
    simBurstIndex = 0;
    simBurstDelayTimer = 0.0f;
    simShotCooldown2 = 0.0f;
    simBurstIndex2 = 0;
    simBurstDelayTimer2 = 0.0f;
    muzzleFlashTimer = 0.0f;
}

} // namespace CharEditor
