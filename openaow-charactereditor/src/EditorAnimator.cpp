// Animator tab: the timing of the pictures of the selected character (the animations: block of its yaml file) and
// a timeline of one volley that shows when the shots, the firing pose and the muzzle flash happen. The canvas on
// the left is the usual preview (Idle / Move / Attack / Die) and follows every change at once.
#include "CharacterEditor.hpp"
#include "core/Localization.hpp"
#include "core/Utility.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace CharEditor
{

AnimTiming ReadAnimTiming(const YAML::Node &doc)
{
    AnimTiming t;
    const YAML::Node a = doc["animations"];
    if (!a || !a.IsMap())
        return t;
    t.moveFrame = std::clamp(YamlFloat(a, "move_frame_time", t.moveFrame), 0.02f, 1.0f);
    t.attackFrame = YamlFloat(a, "attack_frame_time", t.attackFrame);
    t.attackPose = YamlFloat(a, "attack_pose_time", t.attackPose);
    t.burstPose = std::clamp(YamlFloat(a, "burst_pose_time", t.burstPose), 0.02f, 5.0f);
    const std::string mode = YamlString(a, "flash_mode", "loop");
    t.flashMode = mode == "once" ? 1 : (mode == "hold" ? 2 : 0);
    t.flashFrames = std::max(0, YamlInt(a, "flash_frames", 0));
    return t;
}

float AttackFrameTime(const AnimTiming &t, int archetypeId)
{
    if (t.attackFrame > 0.0f)
        return t.attackFrame;
    return (archetypeId == 7 || archetypeId == 18) ? 0.04f : 0.07f;
}

float DefaultAttackPose(const std::string &projectile)
{
    return (projectile == "bullet" || projectile == "sniper") ? 0.12f : 0.22f;
}

namespace
{

constexpr ImU32 kShotColor = IM_COL32(255, 210, 90, 255);
constexpr ImU32 kPoseColor = IM_COL32(80, 150, 230, 200);
constexpr ImU32 kFlashColor = IM_COL32(255, 120, 60, 230);
constexpr ImU32 kWaitColor = IM_COL32(120, 120, 120, 120);

// Removes an optional key (the game then uses its default)
void ClearKey(YAML::Node node, const char *key)
{
    if (node[key].IsDefined())
        node.remove(key);
}

} // namespace

void CharacterEditor::DrawAnimatorProperties()
{
    if (!document.IsDefined())
    {
        ImGui::TextWrapped("%s", _("Select a YAML configuration from the list."));
        return;
    }
    const bool isBuilding = YamlString(document, "type") == "building";
    const int archetypeId = YamlInt(document, "archetype_id", 0);
    ImGui::Text("%s", YamlString(document, "name", "?").c_str());
    ImGui::TextDisabled("%s", activePath.filename().string().c_str());
    ImGui::TextWrapped(
        "%s",
        _("The canvas shows the character; the values below change how the game plays its pictures. "
          "Ctrl+S writes them to the file."));
    if (isBuilding)
    {
        ImGui::Separator();
        ImGui::TextWrapped(
            "%s", _("Buildings flash for a fixed time after a shot; their pictures have no timing in the file yet."));
        return;
    }

    bool changed = false;
    YAML::Node animations = Section("animations");
    const AnimTiming timing = ReadAnimTiming(document);
    YAML::Node combat = document["combat"];
    YAML::Node gun = combat && combat.IsMap() ? EnsureMainWeapon(Section("combat")) : YAML::Node();

    // A float with an on/off box: off = the key is absent and the game uses its default
    auto optionalFloat = [&](const char *key, const char *label, float automatic, float lo, float hi) {
        bool set = animations[key].IsDefined();
        ImGui::PushID(key);
        if (ImGui::Checkbox("##set", &set))
        {
            if (set)
                animations[key] = automatic;
            else
                ClearKey(animations, key);
            changed = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!set);
        float value = set ? YamlFloat(animations, key, automatic) : automatic;
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::DragFloat(_(label), &value, 0.005f, lo, hi, "%.3f s"))
        {
            animations[key] = std::clamp(value, lo, hi);
            changed = true;
        }
        FieldHint(label);
        ImGui::EndDisabled();
        if (!set)
        {
            ImGui::SameLine();
            ImGui::TextDisabled(_("(default %.3f s)"), automatic);
        }
        ImGui::PopID();
    };

    // ----- Idle -----
    ImGui::SeparatorText(_("1. Idle"));
    ImGui::TextDisabled("%s", _("A still picture: nothing to time."));

    // ----- Move -----
    ImGui::SeparatorText(_("2. Move"));
    {
        float v = timing.moveFrame;
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::DragFloat(_("Walking frame time"), &v, 0.005f, 0.02f, 1.0f, "%.3f s"))
        {
            if (std::fabs(v - 0.09f) < 0.0005f)
                ClearKey(animations, "move_frame_time");
            else
                animations["move_frame_time"] = v;
            changed = true;
        }
        FieldHint("Walking frame time");
        ImGui::SameLine();
        if (Hinted(ImGui::SmallButton(_("Preview##animmove")), "Preview walk"))
            SetMode(PreviewMode::Move);
    }

    // ----- Attack -----
    ImGui::SeparatorText(_("3. Attack"));
    if (!YamlBool(combat, "can_attack", true) || !gun.IsDefined())
    {
        ImGui::TextDisabled("%s", _("This character does not attack."));
    }
    else
    {
        const std::string projectile = YamlString(gun, "projectile", "bullet");
        const float firstDefault = DefaultAttackPose(projectile);
        optionalFloat("attack_frame_time", "Firing frame time", AttackFrameTime(timing, archetypeId), 0.01f, 1.0f);
        optionalFloat("attack_pose_time", "Pose after the first shot", firstDefault, 0.02f, 5.0f);
        float burstPose = timing.burstPose;
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::DragFloat(_("Pose after each next shot"), &burstPose, 0.005f, 0.02f, 5.0f, "%.3f s"))
        {
            if (std::fabs(burstPose - 0.15f) < 0.0005f)
                ClearKey(animations, "burst_pose_time");
            else
                animations["burst_pose_time"] = burstPose;
            changed = true;
        }
        FieldHint("Pose after each next shot");

        const char *modes[] = {_("Loop while firing"), _("Once per shot"), _("Once, hold the last frame")};
        int mode = timing.flashMode;
        ImGui::SetNextItemWidth(220.0f);
        if (Hinted(ImGui::Combo(_("Muzzle flash"), &mode, modes, 3), "Muzzle flash"))
        {
            if (mode == 0)
                ClearKey(animations, "flash_mode");
            else
                animations["flash_mode"] = mode == 1 ? "once" : "hold";
            changed = true;
        }
        int frames = timing.flashFrames;
        ImGui::SetNextItemWidth(130.0f);
        if (Hinted(ImGui::InputInt(_("Flash frames (0 = all)"), &frames), "Flash frames"))
        {
            frames = std::max(0, frames);
            if (frames == 0)
                ClearKey(animations, "flash_frames");
            else
                animations["flash_frames"] = frames;
            changed = true;
        }
        if (Hinted(ImGui::SmallButton(_("Preview##animattack")), "Preview attack"))
            SetMode(PreviewMode::Attack);
        ImGui::SameLine();
        if (Hinted(ImGui::SmallButton(_("Reset timing")), "Reset timing"))
        {
            for (const char *key :
                 {"move_frame_time",
                  "attack_frame_time",
                  "attack_pose_time",
                  "burst_pose_time",
                  "flash_mode",
                  "flash_frames"})
            {
                ClearKey(animations, key);
            }
            changed = true;
        }

        // ----- The volley -----
        ImGui::SeparatorText(_("Volley"));
        changed |= EditInt(gun, "burst_count", "Burst count", 1);
        changed |= EditFloat(gun, "burst_delay", "Burst delay (seconds)", 0.1f);
        changed |= EditWeaponDelay(gun);

        const int count = std::max(1, YamlInt(gun, "burst_count", 1));
        const float delay = std::max(0.01f, YamlFloat(gun, "burst_delay", 0.1f));
        const float wait = std::max(0.05f, WeaponCooldown(gun, false, 0.7f));
        const float firstPose = timing.attackPose >= 0.0f ? timing.attackPose : firstDefault;
        const float frameTime = AttackFrameTime(timing, archetypeId);
        const float span = static_cast<float>(count - 1) * delay;
        const float total = span + wait;

        const float width = std::max(220.0f, ImGui::GetContentRegionAvail().x - 8.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float rowH = 16.0f;
        ImGui::Dummy(ImVec2(width, rowH * 4 + 26.0f));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float scale = width / std::max(0.1f, total + timing.burstPose);
        auto X = [&](float t) {
            return origin.x + t * scale;
        };
        // rows: shots, pose, flash, cooldown
        const char *rowNames[] = {"shots", "pose", "flash", "wait"};
        (void) rowNames;
        for (int k = 0; k < count; ++k)
        {
            const float t = static_cast<float>(k) * delay;
            const float pose = (k == 0) ? firstPose : timing.burstPose;
            dl->AddLine(ImVec2(X(t), origin.y), ImVec2(X(t), origin.y + rowH - 2), kShotColor, 2.0f);
            dl->AddRectFilled(ImVec2(X(t), origin.y + rowH), ImVec2(X(t + pose), origin.y + rowH * 2 - 3), kPoseColor);
            const int flashLen = timing.flashFrames > 0 ? timing.flashFrames : 6;
            const float flash = timing.flashMode == 0 ? pose : std::min(pose, flashLen * frameTime);
            dl->AddRectFilled(
                ImVec2(X(t), origin.y + rowH * 2), ImVec2(X(t + flash), origin.y + rowH * 3 - 3), kFlashColor);
        }
        dl->AddRectFilled(ImVec2(X(span), origin.y + rowH * 3), ImVec2(X(total), origin.y + rowH * 4 - 3), kWaitColor);
        dl->AddText(
            ImVec2(origin.x, origin.y + rowH * 4),
            IM_COL32(200, 200, 200, 255),
            (std::string(_("shot")) + " / " + _("pose") + " / " + _("flash") + " / " + _("cooldown")).c_str());

        // What is wrong with it
        if (count > 1 && timing.burstPose < delay - 0.001f)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.72f, 0.28f, 1.0f),
                _("The pose (%.2f s) ends before the next shot (%.2f s): the animation stops between shots."),
                timing.burstPose,
                delay);
        }
        if (count > 1 && firstPose < delay - 0.001f)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.72f, 0.28f, 1.0f),
                _("The first pose (%.2f s) ends before the second shot (%.2f s)."),
                firstPose,
                delay);
        }
        if (timing.flashMode == 0 && OpenArtOfWar::Utility::IsInfantryArchetype(archetypeId))
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.72f, 0.28f, 1.0f),
                "%s",
                _("Infantry keeps firing while it has a target: with the looping flash the muzzle burns "
                  "all the time. Use \"Once per shot\"."));
        }
        ImGui::TextDisabled(_("One volley: %d shot(s) over %.2f s, then %.2f s of cooldown."), count, span, wait);
    }

    // ----- Die / Wreck -----
    ImGui::SeparatorText(_("4. Die"));
    ImGui::TextDisabled("%s", _("Death sequences and the wreck are set in the Preview tab (Animation)."));
    if (Hinted(ImGui::SmallButton(_("Preview##animdie")), "Preview death"))
        SetMode(PreviewMode::Die);

    if (changed)
        dirty = true;
    // Nothing was set: no empty block in the file
    if (animations.IsMap() && animations.size() == 0)
        document.remove("animations");
}

} // namespace CharEditor
