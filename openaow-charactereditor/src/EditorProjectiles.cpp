#include "CharacterEditor.hpp"

#include "game/Effects.hpp"
#include "game/Projectiles.hpp"

#include <algorithm>
#include <cmath>

// Projectiles tab: how bullets, sniper rounds, shells, rockets and flames fly and hit, and the craters they leave
// (assets/characters/projectiles.yaml). The canvas fires the chosen kind at a target again and again.
namespace CharEditor
{

namespace
{

using OpenArtOfWar::ProjectileSpec;
namespace Projectiles = OpenArtOfWar::Projectiles;

struct ProjectileKind
{
    const char *key;   // key in projectiles.yaml
    const char *label; // gettext msgid
};

const ProjectileKind kKinds[] = {
    {"bullet", "Bullet"},
    {"sniper", "Sniper round"},
    {"shell", "Shell"},
    {"rocket", "Rocket"},
    {"flame", "Flame"},
};

// Projectile pictures of the game: first composite and its number of directions
struct SpriteFamily
{
    int composite;
    int directions;
    const char *label; // gettext msgid
};

const SpriteFamily kSpriteFamilies[] = {
    {1309, 32, "Bullet tracer"},
    {1373, 32, "Heavy tracer"},
    {1341, 32, "Shell"},
    {1405, 16, "Rocket"},
    {1421, 16, "Rocket with flame"},
};

// Small craters of the game (composites)
constexpr int kCraterFirst = 1784;
constexpr int kCraterLast = 1800;

std::string ListText(const std::vector<int> &v)
{
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i)
        s += (i ? ", " : "") + std::to_string(v[i]);
    return s + "]";
}

std::vector<int> ListOf(const YAML::Node &n)
{
    std::vector<int> v;
    if (n.IsSequence())
    {
        for (const auto &item : n)
        {
            try
            {
                v.push_back(item.as<int>());
            }
            catch (...)
            {}
        }
    }
    return v;
}

} // namespace

SDL_Texture *CharacterEditor::ZoomedThumbnail(int composite, float scale)
{
    const auto key = std::make_pair(composite, static_cast<int>(scale * 100.0f));
    auto it = zoomThumbs.find(key);
    if (it != zoomThumbs.end())
        return it->second;
    constexpr int kW = 96, kH = 64;
    SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, kW, kH);
    if (tex)
    {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_Texture *previous = SDL_GetRenderTarget(renderer);
        SDL_SetRenderTarget(renderer, tex);
        SDL_SetRenderClipRect(renderer, nullptr);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        // Composites are 180x120 canvases centred on the object: magnified around that centre
        gameRenderer->DrawCompositeScreen(
            composite, static_cast<int>(kW * 0.5f - 90.0f * scale), static_cast<int>(kH * 0.5f - 60.0f * scale), scale);
        SDL_SetRenderTarget(renderer, previous);
    }
    zoomThumbs[key] = tex;
    return tex;
}

void CharacterEditor::EnsureProjectiles()
{
    if (projectilesTried)
        return;
    projectilesTried = true;
    if (!LoadYamlFile(projectilesYaml, "projectiles.yaml"))
        return;
    for (const ProjectileKind &k : kKinds)
    {
        const std::string base = std::string("projectiles.") + k.key;
        yamlAnimPaths.insert(base + ".impact");
        yamlAnimPaths.insert(base + ".impact_wall");
        yamlAnimPaths.insert(base + ".wall_destroyed");
    }
    ProjectilesChanged();
}

void CharacterEditor::ProjectilesChanged()
{
    if (projectilesYaml.Loaded())
        Projectiles::Apply(projectilesYaml.text());
    projectileUsers.clear(); // recomputed when shown
}

const ProjectileSpec &CharacterEditor::projectileSpec() const
{
    return Projectiles::of(Projectiles::FromKey(kKinds[std::clamp(projKind, 0, 4)].key));
}

void CharacterEditor::FireProjectilePreview()
{
    const ProjectileSpec &spec = projectileSpec();
    ProjShot shot;
    // The shooter stands left of the centre, the target at the chosen distance and angle
    const float angle = projSweep ? projSweepAngle : projAngleDeg * static_cast<float>(M_PI) / 180.0f;
    shot.x = shot.sx = -projDistance * 0.5f * std::cos(angle);
    shot.y = shot.sy = -projDistance * 0.5f * std::sin(angle);
    shot.tx = projDistance * 0.5f * std::cos(angle);
    shot.ty = projDistance * 0.5f * std::sin(angle);
    shot.dir16 = OpenArtOfWar::Utility::DirFromNorth(shot.tx - shot.x, shot.ty - shot.y, 16);
    shot.dir32 = OpenArtOfWar::Utility::DirFromNorth(shot.tx - shot.x, shot.ty - shot.y, 32);
    shot.impactAnim = spec.PickImpact(projShotCount);
    shot.mark = spec.PickGroundMark(projShotCount);
    ++projShotCount;
    projShots.push_back(shot);
    if (projSweep)
        projSweepAngle += 2.0f * static_cast<float>(M_PI) / static_cast<float>(std::max(1, spec.directions));
}

void CharacterEditor::UpdateProjectilesSim(float dt)
{
    if (editorMode != EditorMode::Projectiles)
        return;
    EnsureProjectiles();
    const ProjectileSpec &spec = projectileSpec();
    const float frameTime = OpenArtOfWar::EffectsConfig::Get().frameTime;
    // The game moves a projectile `speed` pixels every simulation tick
    const float pixelsPerSecond = spec.speed / static_cast<float>(OpenArtOfWar::SIMULATION_TIMESTEP);
    for (ProjShot &s : projShots)
    {
        if (!s.landed)
        {
            const float dx = s.tx - s.x, dy = s.ty - s.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const float step = pixelsPerSecond * dt;
            if (dist <= step)
            {
                s.x = s.tx;
                s.y = s.ty;
                s.landed = true;
                if (s.mark >= 0 && spec.groundMarkSeconds > 0.0f)
                    projCraters.push_back({s.tx, s.ty, s.mark, spec.groundMarkSeconds});
            }
            else
            {
                s.x += dx / dist * step;
                s.y += dy / dist * step;
                if (spec.smokeTrail)
                {
                    s.smokeTimer += dt;
                    if (s.smokeTimer >= 0.03f)
                    {
                        s.smokeTimer = 0.0f;
                        projSmoke.push_back({s.x, s.y, 0.0f});
                    }
                }
            }
        }
        else if (s.impactAnim >= 0)
        {
            s.frameTimer += dt;
            if (s.frameTimer >= frameTime)
            {
                s.frameTimer = 0.0f;
                const OpenArtOfWar::IndexRange r = textures->GetAnimRange(s.impactAnim);
                if (++s.frame >= std::max(1, r.end - r.start + 1))
                    s.done = true;
            }
        }
        else
        {
            s.done = true;
        }
    }
    projShots.erase(
        std::remove_if(projShots.begin(), projShots.end(), [](const ProjShot &s) { return s.done; }), projShots.end());
    for (auto &p : projSmoke)
        p.age += dt;
    projSmoke.erase(
        std::remove_if(projSmoke.begin(), projSmoke.end(), [](const ProjSmoke &p) { return p.age >= 0.35f; }),
        projSmoke.end());
    // Craters of the preview fade faster than in the game (time scale of the canvas), so the next ones show
    for (auto &c : projCraters)
        c.timeLeft -= dt * projCraterSpeed;
    projCraters.erase(
        std::remove_if(projCraters.begin(), projCraters.end(), [](const ProjCrater &c) { return c.timeLeft <= 0.0f; }),
        projCraters.end());
    if (projLoop)
    {
        projFireTimer -= dt;
        if (projFireTimer <= 0.0f)
        {
            projFireTimer = projInterval;
            FireProjectilePreview();
        }
    }
}

void CharacterEditor::DrawProjectilesScene()
{
    const ProjectileSpec &spec = projectileSpec();
    const auto faction = OpenArtOfWar::Faction::Resistance;
    for (const ProjCrater &c : projCraters)
        gameRenderer->DrawCompositeWorld(c.composite, c.x - 90.0f, c.y - 60.0f, *camera, faction);
    // Shooter and target
    const float angle = projSweep ? projSweepAngle : projAngleDeg * static_cast<float>(M_PI) / 180.0f;
    const float sx = -projDistance * 0.5f * std::cos(angle), sy = -projDistance * 0.5f * std::sin(angle);
    const float tx = -sx, ty = -sy;
    gameRenderer->FillRectWorld(sx - 4.0f, sy - 4.0f, 8.0f, 8.0f, *camera, SDL_Color {110, 200, 140, 220});
    gameRenderer->DrawRectWorld(
        tx - OpenArtOfWar::TILE_WIDTH * 0.5f,
        ty - OpenArtOfWar::TILE_HEIGHT * 0.5f,
        static_cast<float>(OpenArtOfWar::TILE_WIDTH),
        static_cast<float>(OpenArtOfWar::TILE_HEIGHT),
        *camera,
        SDL_Color {236, 111, 77, 200});
    for (const ProjSmoke &p : projSmoke)
    {
        gameRenderer->DrawSmokePuffWorld(static_cast<int>(p.age / 0.35f * 13.0f), p.x, p.y, *camera);
    }
    for (const ProjShot &s : projShots)
    {
        if (!s.landed)
        {
            if (projKind == 4)
            {
                // The flame is a jet from the nozzle, playing its 12 frames over the flight
                const float total = std::max(1.0f, std::hypot(s.tx - s.sx, s.ty - s.sy));
                const float prog = std::clamp(std::hypot(s.x - s.sx, s.y - s.sy) / total, 0.0f, 1.0f);
                gameRenderer->DrawAnimSequenceWorldRotated(
                    59,
                    std::clamp(static_cast<int>(prog * 11.0f), 0, 11),
                    s.sx,
                    s.sy,
                    std::atan2(s.ty - s.sy, s.tx - s.sx),
                    *camera,
                    faction);
            }
            else if (spec.sprite > 0)
            {
                gameRenderer->DrawProjectileWorld(
                    spec.sprite,
                    spec.directions == 16 ? s.dir16 : s.dir32,
                    s.x,
                    s.y,
                    *camera,
                    faction,
                    spec.directions);
            }
        }
        else if (s.impactAnim >= 0)
        {
            float ox = 0.0f, oy = 0.0f;
            OpenArtOfWar::ExplosionCentering(s.impactAnim, ox, oy);
            gameRenderer->DrawAnimSequenceWorld(s.impactAnim, s.frame, s.x + ox, s.y + oy, *camera, faction);
        }
    }
}

void CharacterEditor::DrawProjectileControls()
{
    EnsureProjectiles();
    for (int i = 0; i < 5; ++i)
    {
        if (i)
            ImGui::SameLine();
        const bool active = projKind == i;
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
        if (Hinted(ImGui::Button(_(kKinds[i].label)), kKinds[i].label))
        {
            projKind = i;
            projectileUsers.clear();
            projShots.clear();
            projSmoke.clear();
            projCraters.clear();
            projFireTimer = 0.0f;
        }
        if (active)
            ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Fire")), "Fire"))
        FireProjectilePreview();
    ImGui::SameLine();
    Hinted(ImGui::Checkbox(_("Loop"), &projLoop), "Fire again and again");
    ImGui::SameLine();
    Hinted(ImGui::Checkbox(_("All directions"), &projSweep), "All directions");
    ImGui::SetNextItemWidth(150.0f);
    Hinted(ImGui::SliderFloat(_("Distance"), &projDistance, 40.0f, 600.0f, "%.0f px"), "Shot distance");
    ImGui::SameLine();
    if (!projSweep)
    {
        ImGui::SetNextItemWidth(150.0f);
        Hinted(ImGui::SliderFloat(_("Angle"), &projAngleDeg, -180.0f, 180.0f, "%.0f deg"), "Shot angle");
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(120.0f);
    Hinted(ImGui::SliderFloat(_("Interval"), &projInterval, 0.1f, 3.0f, "%.1f s"), "Shot interval");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    Hinted(ImGui::SliderFloat(_("Speed"), &playbackSpeed, 0.1f, 2.0f, "%.2fx"), "Playback speed");
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Clear craters")), "Clear craters"))
        projCraters.clear();
}

void CharacterEditor::DrawProjectileProperties()
{
    EnsureProjectiles();
    if (!projectilesYaml.Loaded())
    {
        ImGui::TextWrapped("%s", _("The projectiles file could not be loaded."));
        return;
    }
    DrawSettingsBar(projectilesYaml, "projectiles.yaml");
    ImGui::Separator();
    const ProjectileKind &kind = kKinds[std::clamp(projKind, 0, 4)];
    const std::string base = std::string("projectiles.") + kind.key;
    ImGui::Text("%s: %s", _("Projectile"), _(kind.label));
    ImGui::TextDisabled("projectile: %s", kind.key);
    bool changed = false;

    if (ImGui::CollapsingHeader(_("Flight"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        changed |= EditYamlScalar(projectilesYaml, base + ".hitscan", _("Hit-scan"));
        FieldHint("Hit-scan");
        changed |= EditYamlScalar(projectilesYaml, base + ".speed", _("Speed (px per tick)"));
        FieldHint("Speed (px per tick)");
        changed |= EditYamlScalar(projectilesYaml, base + ".spread", _("Spread (radians)"));
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f deg", projectileSpec().spread * 180.0f / static_cast<float>(M_PI));
        changed |= EditYamlScalar(projectilesYaml, base + ".smoke_trail", _("Smoke trail"));
        changed |= EditYamlScalar(projectilesYaml, base + ".sprite", _("Sprite (composite)"));
        changed |= EditYamlScalar(projectilesYaml, base + ".directions", _("Directions"));
        // The projectile pictures of the game: a click sets the composite and its directions
        ImGui::TextUnformatted(_("Projectile sprites"));
        FieldHint("Projectile sprites");
        const int cur = projectilesYaml.At(base + ".sprite").as<int>(0);
        for (size_t f = 0; f < std::size(kSpriteFamilies); ++f)
        {
            if (f)
                ImGui::SameLine();
            ImGui::PushID(static_cast<int>(f));
            const SpriteFamily &fam = kSpriteFamilies[f];
            const bool selected = fam.composite == cur;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
            // East-facing frame: a quarter of the directions after north
            SDL_Texture *thumb = ZoomedThumbnail(fam.composite + fam.directions / 4, 2.5f);
            const bool clicked = thumb ? ImGui::ImageButton("spr", reinterpret_cast<ImTextureID>(thumb), ImVec2(48, 32))
                                       : ImGui::Button(std::to_string(fam.composite).c_str());
            if (selected)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s (%d, %d)", _(fam.label), fam.composite, fam.directions);
            if (clicked)
            {
                changed |= projectilesYaml.Set(base + ".sprite", std::to_string(fam.composite));
                changed |= projectilesYaml.Set(base + ".directions", std::to_string(fam.directions));
            }
            ImGui::PopID();
        }
        if (projKind == 4)
            ImGui::TextDisabled("%s", _("A flame is drawn as its own jet (animation 59), not as a sprite."));
    }

    if (ImGui::CollapsingHeader(_("Hit effects"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        changed |= EditYamlScalar(projectilesYaml, base + ".impact", _("Impact animations"));
        changed |= EditYamlScalar(projectilesYaml, base + ".impact_wall", _("Wall hit"));
        changed |= EditYamlScalar(projectilesYaml, base + ".wall_destroyed", _("Wall destroyed"));
        // The chosen impacts as pictures
        const std::vector<int> impacts = ListOf(projectilesYaml.At(base + ".impact"));
        for (size_t i = 0; i < impacts.size(); ++i)
        {
            if (i)
                ImGui::SameLine();
            if (SDL_Texture *t = AnimThumbnail(impacts[i]))
            {
                ImGui::Image(reinterpret_cast<ImTextureID>(t), ImVec2(40, 40));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(_("Animation %d"), impacts[i]);
            }
        }
        if (!impacts.empty() && Hinted(ImGui::SmallButton(_("Clear impacts")), "Clear impacts"))
            changed |= projectilesYaml.Set(base + ".impact", "[]");
    }

    if (ImGui::CollapsingHeader(_("Ground marks"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        changed |= EditYamlScalar(projectilesYaml, base + ".ground_marks", _("Crater composites"));
        changed |= EditYamlScalar(projectilesYaml, base + ".ground_mark_seconds", _("Crater seconds"));
        ImGui::TextUnformatted(_("Craters"));
        FieldHint("Craters");
        // Every crater of the game: a click adds it to the list or takes it out
        std::vector<int> marks = ListOf(projectilesYaml.At(base + ".ground_marks"));
        const float avail = ImGui::GetContentRegionAvail().x;
        const int perRow = std::max(1, static_cast<int>(avail / 62.0f));
        int k = 0;
        for (int c = kCraterFirst; c <= kCraterLast; ++c)
        {
            if (k++ % perRow != 0)
                ImGui::SameLine();
            ImGui::PushID(c);
            const bool on = std::find(marks.begin(), marks.end(), c) != marks.end();
            if (on)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
            SDL_Texture *thumb = ZoomedThumbnail(c, 1.2f);
            const bool clicked
                = thumb ? ImGui::ImageButton("crater", reinterpret_cast<ImTextureID>(thumb), ImVec2(48, 32))
                        : ImGui::Button(std::to_string(c).c_str());
            if (on)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s %d", _("Composite"), c);
            if (clicked)
            {
                if (on)
                    marks.erase(std::remove(marks.begin(), marks.end(), c), marks.end());
                else
                    marks.push_back(c);
                std::sort(marks.begin(), marks.end());
                changed |= projectilesYaml.Set(base + ".ground_marks", ListText(marks));
            }
            ImGui::PopID();
        }
        ImGui::SetNextItemWidth(160.0f);
        Hinted(
            ImGui::SliderFloat(_("Preview fade speed"), &projCraterSpeed, 1.0f, 30.0f, "%.0fx"), "Preview fade speed");
    }

    if (ImGui::CollapsingHeader(_("Used by"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Characters whose weapons fire this kind (their siege form included)
        if (projectileUsers.empty())
        {
            for (const CharacterFile &f : files)
            {
                try
                {
                    const YAML::Node doc = YAML::LoadFile(f.path.string());
                    bool uses = false;
                    auto scan = [&](const YAML::Node &combat) {
                        if (!combat || !combat["weapons"] || !combat["weapons"].IsSequence())
                            return;
                        for (const auto &w : combat["weapons"])
                            if (YamlString(w, "projectile", "bullet") == kind.key)
                                uses = true;
                    };
                    scan(doc["combat"]);
                    if (doc["abilities"] && doc["abilities"]["siege"])
                        scan(doc["abilities"]["siege"]["combat"]);
                    if (uses)
                        projectileUsers.push_back(f.name);
                }
                catch (...)
                {}
            }
            if (projectileUsers.empty())
                projectileUsers.push_back("-");
        }
        for (const std::string &n : projectileUsers)
            ImGui::BulletText("%s", n.c_str());
    }

    if (changed)
    {
        ProjectilesChanged();
        projShots.clear();
    }
    ImGui::Separator();
    DrawAnimGallery(std::max(160.0f, ImGui::GetContentRegionAvail().y));
}

} // namespace CharEditor
