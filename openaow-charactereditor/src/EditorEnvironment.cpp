#include "CharacterEditor.hpp"

#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace CharEditor {

namespace {

using Json = nlohmann::ordered_json;

// Called by SDL (possibly from another thread) when the file dialog closes
void SDLCALL onFontChosen(void* userdata, const char* const* list, int /*filter*/) {
    auto* pick = static_cast<FolderPick*>(userdata);
    std::lock_guard<std::mutex> lock(pick->mutex);
    if (list && list[0]) {
        pick->path = list[0];
        pick->ready = true;
    }
}

struct EnvHint {
    const char* path;
    const char* text;
};

// Tooltips of the keys of game_environment.json (msgids translated in po/*.po)
const EnvHint kEnvHints[] = {
    {"construction", "Rules of placing buildings."},
    {"construction.enemy_block_tiles", "How many tiles around an enemy structure stay closed for building."},
    {"fog", "Look of the fog of war."},
    {"fog.color", "Color of the fog."},
    {"fog.explored_alpha", "Darkness (0..255) of the places you have already seen but cannot see now."},
    {"fog.hidden_alpha", "Darkness (0..255) of the places you have never seen."},
    {"fog.rounding", "How round the edge of the fog is (0 square, 1 smooth)."},
    {"fog.texels_per_tile", "Resolution of the fog picture: pixels per map tile."},
    {"network", "Network game settings."},
    {"network.hash_interval_ticks", "Every this many game ticks the players compare their game state to detect a desync."},
    {"network.input_delay_max_turns", "The largest delay (in turns) between giving an order and carrying it out."},
    {"network.input_delay_min_turns", "The smallest delay (in turns) between giving an order and carrying it out."},
    {"network.lan_probe_ports", "How many ports after the first one are searched for games in the local network."},
    {"network.master_host", "Address of the server that lists the public games."},
    {"network.master_port", "Port of that server."},
    {"network.max_map_transfer_kb", "The biggest map (in kilobytes) a host sends to the players."},
    {"network.peer_timeout_ms", "A player who is silent for this long (milliseconds) is dropped."},
    {"network.punch_timeout_ms", "How long (milliseconds) the game tries to connect two players directly."},
    {"network.reconnect_window_s", "A player who lost the connection can come back during this time (seconds)."},
    {"network.tcp_port", "TCP port of the game."},
    {"network.turn_ms", "Length of a network turn in milliseconds."},
    {"network.udp_port", "UDP port of the game."},
    {"overlay", "Order lines and bars shown while Alt is held."},
    {"overlay.line_opacity", "Opacity of the order lines (0..1)."},
    {"overlay.move_line", "Line to the place a unit is moving to."},
    {"overlay.attack_line", "Line to the target a unit attacks."},
    {"overlay.wall_line", "Line of the wall being built."},
    {"overlay.move_line.color", "Color of the line (#RRGGBB)."},
    {"overlay.attack_line.color", "Color of the line (#RRGGBB)."},
    {"overlay.wall_line.color", "Color of the line (#RRGGBB)."},
    {"overlay.move_line.width", "Thickness of the line in pixels."},
    {"overlay.attack_line.width", "Thickness of the line in pixels."},
    {"overlay.wall_line.width", "Thickness of the line in pixels."},
    {"render", "Which graphics API the game starts with."},
    {"render.api", "Preferred graphics API (vulkan, opengl, opengles2, software)."},
    {"render.fallback", "API used when the preferred one does not work."},
};

const char* envHint(const std::string& path) {
    for (const EnvHint& h : kEnvHints) if (path == h.path) return h.text;
    return nullptr;
}

bool hiddenKey(const std::string& path) {
    return path == "font" || path == "render.probed" || path == "render.probe_key" || path == "render.init_pending" ||
           path == "render.confirm_from";
}

std::string pretty(const std::string& key) {
    std::string s = key;
    for (char& c : s) if (c == '_') c = ' ';
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

} // namespace

void CharacterEditor::ensureEnvironment() {
    if (envTried) return;
    envTried = true;
    envPath = assetsRoot / "data" / "game_environment.json";
    std::ifstream in(envPath);
    if (!in) {
        status = std::string(_("Could not load YAML: ")) + envPath.string();
        return;
    }
    envJson = Json::parse(in, nullptr, false, true);
    envLoaded = envJson.is_object();
    if (!envLoaded) status = std::string(_("Could not load YAML: ")) + envPath.string();
    envDirty = false;
}

bool CharacterEditor::saveEnvironment() {
    if (!envLoaded) return false;
    std::ofstream out(envPath);
    if (!out) {
        status = std::string(_("Could not open file for writing: ")) + envPath.string();
        return false;
    }
    out << envJson.dump(2) << '\n';
    if (!out) {
        status = std::string(_("Failed to write file: ")) + envPath.string();
        return false;
    }
    envDirty = false;
    status = std::string(_("Saved: ")) + envPath.filename().string();
    return true;
}

bool CharacterEditor::editEnvJson(Json& node, const std::string& path, int depth) {
    bool changed = false;
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string key = it.key();
        const std::string sub = path.empty() ? key : path + "." + key;
        if (hiddenKey(sub)) continue;
        Json& v = it.value();
        const char* hint = envHint(sub);
        auto tip = [&]() {
            if (hint && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                ImGui::TextUnformatted(_(hint));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        };
        ImGui::PushID(sub.c_str());
        const std::string label = pretty(key);
        if (v.is_object()) {
            ImGui::SetNextItemOpen(depth < 1, ImGuiCond_Once);
            const bool open = ImGui::TreeNodeEx((label + "##" + sub).c_str(), ImGuiTreeNodeFlags_Framed);
            tip();
            if (open) {
                changed |= editEnvJson(v, sub, depth + 1);
                ImGui::TreePop();
            }
        } else if (v.is_boolean()) {
            bool b = v.get<bool>();
            if (ImGui::Checkbox(label.c_str(), &b)) { v = b; changed = true; }
            tip();
        } else if (v.is_number_integer()) {
            int n = v.get<int>();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::DragInt(label.c_str(), &n, 1.0f)) { v = n; changed = true; }
            tip();
        } else if (v.is_number_float()) {
            float f = v.get<float>();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::DragFloat(label.c_str(), &f, 0.01f, 0.0f, 0.0f, "%.3g")) { v = f; changed = true; }
            tip();
        } else if (v.is_array() && v.size() == 3 && v[0].is_number_integer() && key == "color") {
            float c[3] = {v[0].get<int>() / 255.0f, v[1].get<int>() / 255.0f, v[2].get<int>() / 255.0f};
            if (ImGui::ColorEdit3(label.c_str(), c)) {
                for (int i = 0; i < 3; ++i) v[i] = static_cast<int>(c[i] * 255.0f + 0.5f);
                changed = true;
            }
            tip();
        } else if (v.is_string()) {
            const std::string s = v.get<std::string>();
            const bool hex = s.size() >= 7 && s[0] == '#';
            ImGui::SetNextItemWidth(220.0f);
            if (hex) {
                unsigned r = 255, g = 255, b = 255, a = 255;
                std::sscanf(s.c_str() + 1, "%2x%2x%2x", &r, &g, &b);
                if (s.size() >= 9) std::sscanf(s.c_str() + 7, "%2x", &a);
                float c[4] = {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
                if (ImGui::ColorEdit4(label.c_str(), c, ImGuiColorEditFlags_AlphaBar)) {
                    char buf[16];
                    if (s.size() >= 9)
                        std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", int(c[0] * 255 + 0.5f), int(c[1] * 255 + 0.5f), int(c[2] * 255 + 0.5f), int(c[3] * 255 + 0.5f));
                    else
                        std::snprintf(buf, sizeof buf, "#%02X%02X%02X", int(c[0] * 255 + 0.5f), int(c[1] * 255 + 0.5f), int(c[2] * 255 + 0.5f));
                    v = std::string(buf);
                    changed = true;
                }
            } else {
                auto& buf = envTextBuffers[sub];
                if (!ImGui::IsAnyItemActive()) std::snprintf(buf.data(), buf.size(), "%s", s.c_str());
                if (ImGui::InputText(label.c_str(), buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    v = std::string(buf.data());
                    changed = true;
                }
            }
            tip();
        } else {
            ImGui::TextDisabled("%s", label.c_str());
        }
        ImGui::PopID();
    }
    return changed;
}

SDL_Texture* CharacterEditor::fontPreview(const std::string& path, float size, const std::string& text) {
    const std::string key = path + "|" + std::to_string(size) + "|" + text;
    auto it = fontPreviews.find(key);
    if (it != fontPreviews.end()) return it->second;
    SDL_Texture* tex = nullptr;
    fs::path resolved(path);
    if (resolved.is_relative()) resolved = assetsRoot.parent_path() / resolved;
    if (TTF_WasInit() || TTF_Init()) {
        if (TTF_Font* font = TTF_OpenFont(resolved.string().c_str(), size)) {
            if (SDL_Surface* surf = TTF_RenderText_Blended(font, text.c_str(), 0, SDL_Color{255, 255, 255, 255})) {
                tex = SDL_CreateTextureFromSurface(renderer, surf);
                SDL_DestroySurface(surf);
            }
            TTF_CloseFont(font);
        }
    }
    fontPreviews[key] = tex;
    return tex;
}

void CharacterEditor::drawEnvironment() {
    ensureEnvironment();
    if (!envLoaded) {
        ImGui::TextWrapped("%s", _("The environment file could not be loaded."));
        return;
    }
    Json& font = envJson["font"];
    if (!font.is_object()) font = Json::object();
    ImGui::TextUnformatted(_("Game font"));
    fieldHint("Game font");
    ImGui::TextWrapped("%s", _("The game reads the font files and sizes from here. A path may point outside the assets folder; a font that cannot be opened is replaced by a system one."));
    bool changed = false;
    static const char* const kKeys[2] = {"regular", "bold"};
    static const char* const kLabels[2] = {"Regular font file", "Bold font file"};
    for (int i = 0; i < 2; ++i) {
        FolderPick& pick = fontPick[i];
        {
            std::lock_guard<std::mutex> lock(pick.mutex);
            if (pick.ready) {
                font[kKeys[i]] = pick.path;
                pick.ready = false;
                changed = true;
            }
        }
        const std::string cur = font.value(kKeys[i], std::string());
        auto& buf = envTextBuffers[std::string("font.") + kKeys[i]];
        if (!ImGui::IsAnyItemActive()) std::snprintf(buf.data(), buf.size(), "%s", cur.c_str());
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(std::max(200.0f, ImGui::GetContentRegionAvail().x - 190.0f));
        if (hinted(ImGui::InputText(_(kLabels[i]), buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue), kLabels[i])) {
            font[kKeys[i]] = std::string(buf.data());
            changed = true;
        }
        ImGui::SameLine();
        if (hinted(ImGui::Button(_("Choose...")), "Choose font file")) {
            static const SDL_DialogFileFilter filters[] = {{"Fonts", "ttf;otf;ttc"}};
            SDL_ShowOpenFileDialog(onFontChosen, &pick, window, filters, 1, nullptr, false);
        }
        fs::path resolved(cur);
        if (resolved.is_relative()) resolved = assetsRoot.parent_path() / resolved;
        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec)) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", _("File not found: a system font is used instead."));
        }
        ImGui::PopID();
    }
    struct SizeRow {
        const char* key;
        const char* label;
        int def;
        bool bold;
    };
    static const SizeRow kSizes[] = {{"size_small", "Small text", 13, false}, {"size_medium", "Body text", 16, false},
                                     {"size_large", "Heading", 22, true},     {"size_title", "Title", 30, true},
                                     {"size_hero", "Big title", 48, true}};
    const std::string regular = font.value("regular", std::string()), bold = font.value("bold", std::string());
    for (const SizeRow& row : kSizes) {
        int size = font.value(row.key, row.def);
        ImGui::PushID(row.key);
        ImGui::SetNextItemWidth(120.0f);
        if (hinted(ImGui::DragInt(_(row.label), &size, 0.2f, 6, 120), row.label)) {
            font[row.key] = std::clamp(size, 6, 120);
            changed = true;
        }
        ImGui::PopID();
        SDL_Texture* tex = fontPreview(row.bold ? bold : regular, static_cast<float>(size),
                                       "Open Art Of War - Свобода ради свободы 0123456789");
        if (tex) {
            float w = 0.0f, h = 0.0f;
            SDL_GetTextureSize(tex, &w, &h);
            ImGui::Image(reinterpret_cast<ImTextureID>(tex), ImVec2(w, h));
        }
    }
    if (changed) envDirty = true;
    ImGui::Separator();
    if (hinted(ImGui::Button(_("Save")), "Save settings")) saveEnvironment();
    ImGui::SameLine();
    if (hinted(ImGui::Button(_("Reload")), "Reload")) {
        envTried = false;
        ensureEnvironment();
    }
    if (envDirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f), "%s", _("Unsaved changes"));
    }
    ImGui::TextDisabled("assets/data/game_environment.json");
}

void CharacterEditor::drawEnvironmentProperties() {
    ensureEnvironment();
    if (!envLoaded) {
        ImGui::TextWrapped("%s", _("The environment file could not be loaded."));
        return;
    }
    if (hinted(ImGui::Button(_("Save")), "Save settings")) saveEnvironment();
    if (envDirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f), "%s", _("Unsaved changes"));
    }
    ImGui::Separator();
    if (editEnvJson(envJson, std::string(), 0)) envDirty = true;
}

} // namespace CharEditor
