#include "CharacterEditor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace CharEditor
{

namespace
{

enum class Kind
{
    Bool,
    Int,
    Float,
    String,
    Color,
    NumberList,
    Other
};

bool IsInt(const std::string &s)
{
    if (s.empty())
        return false;
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size())
        return false;
    for (; i < s.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i])))
            return false;
    return true;
}

bool IsFloat(const std::string &s)
{
    if (s.empty())
        return false;
    char *end = nullptr;
    std::strtod(s.c_str(), &end);
    return end && *end == '\0' && s.find_first_of("0123456789") != std::string::npos
           && s.find_first_of("xXnNiI") == std::string::npos;
}

bool IsHexColor(const std::string &s)
{
    if (s.size() != 7 && s.size() != 9)
        return false;
    if (s[0] != '#')
        return false;
    for (std::size_t i = 1; i < s.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(s[i])))
            return false;
    return true;
}

Kind KindOf(const YAML::Node &n)
{
    if (n.IsSequence())
    {
        for (const auto &item : n)
        {
            if (!item.IsScalar() || !IsFloat(item.Scalar()))
                return Kind::Other;
        }
        return Kind::NumberList;
    }
    if (!n.IsScalar())
        return Kind::Other;
    const std::string &s = n.Scalar();
    if (s == "true" || s == "false")
        return Kind::Bool;
    if (IsInt(s))
        return Kind::Int;
    if (IsFloat(s))
        return Kind::Float;
    if (IsHexColor(s))
        return Kind::Color;
    return Kind::String;
}

std::string FormatFloat(float v)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    std::string s = buf;
    if (s.find_first_of(".e") == std::string::npos)
        s += ".0";
    return s;
}

std::string Quote(const std::string &s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string Pretty(const std::string &key)
{
    std::string s = key;
    for (char &c : s)
        if (c == '_')
            c = ' ';
    if (!s.empty())
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::string LastKey(const std::string &path)
{
    const std::size_t dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(dot + 1);
}

void CommentTooltip(const std::string &text)
{
    if (text.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
        return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

} // namespace

bool CharacterEditor::LoadYamlFile(CommentedYaml &y, const char *name)
{
    std::string error;
    if (!y.load(charactersDir / name, error))
    {
        status = std::string(_("Could not load YAML: ")) + error;
        return false;
    }
    return true;
}

bool CharacterEditor::SaveYamlFile(CommentedYaml &y)
{
    std::string error;
    if (!y.Save(error))
    {
        status = error;
        return false;
    }
    status = std::string(_("Saved: ")) + y.Path().filename().string();
    return true;
}

void CharacterEditor::DrawSettingsBar(CommentedYaml &y, const char *fileName)
{
    if (Hinted(ImGui::Button(_("Save")), "Save settings"))
        SaveActiveSettings();
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Reload")), "Reload"))
    {
        LoadYamlFile(y, fileName);
        if (&y == &effectsYaml)
            EffectsChanged();
        if (&y == &projectilesYaml)
            ProjectilesChanged();
    }
    if (y.Dirty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f), "%s", _("Unsaved changes"));
    }
    ImGui::TextDisabled("%s", fileName);
}

bool CharacterEditor::EditYamlScalar(CommentedYaml &y, const std::string &path, const std::string &label)
{
    const YAML::Node n = y.At(path);
    if (!n.IsDefined())
        return false;
    const Kind kind = KindOf(n);
    const std::string comment = y.Comment(path);
    bool changed = false;
    ImGui::PushID(path.c_str());
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.5f));
    switch (kind)
    {
        case Kind::Bool:
        {
            bool v = n.Scalar() == "true";
            if (ImGui::Checkbox(label.c_str(), &v))
                changed = y.Set(path, v ? "true" : "false");
            break;
        }
        case Kind::Int:
        {
            int v = std::atoi(n.Scalar().c_str());
            if (ImGui::DragInt(label.c_str(), &v, 1.0f))
                changed = y.Set(path, std::to_string(v));
            break;
        }
        case Kind::Float:
        {
            float v = static_cast<float>(std::atof(n.Scalar().c_str()));
            const float step = std::max(0.001f, std::abs(v) * 0.01f);
            if (ImGui::DragFloat(label.c_str(), &v, step, 0.0f, 0.0f, "%.4g"))
                changed = y.Set(path, FormatFloat(v));
            break;
        }
        case Kind::Color:
        {
            const std::string &s = n.Scalar();
            unsigned r = 0, g = 0, b = 0, a = 255;
            std::sscanf(s.c_str() + 1, "%2x%2x%2x", &r, &g, &b);
            if (s.size() == 9)
                std::sscanf(s.c_str() + 7, "%2x", &a);
            float col[4] = {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
            if (ImGui::ColorEdit4(
                    label.c_str(), col, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf))
            {
                char buf[16];
                std::snprintf(
                    buf,
                    sizeof buf,
                    "\"#%02X%02X%02X%02X\"",
                    static_cast<int>(col[0] * 255.0f + 0.5f),
                    static_cast<int>(col[1] * 255.0f + 0.5f),
                    static_cast<int>(col[2] * 255.0f + 0.5f),
                    static_cast<int>(col[3] * 255.0f + 0.5f));
                changed = y.Set(path, buf);
            }
            break;
        }
        case Kind::NumberList:
        {
            auto &buf = yamlTextBuffers[path];
            if (!ImGui::IsAnyItemActive() || buf[0] == '\0')
            {
                std::string joined;
                for (std::size_t i = 0; i < n.size(); ++i)
                {
                    if (i)
                        joined += ", ";
                    joined += n[i].Scalar();
                }
                if (!ImGui::IsAnyItemActive())
                    std::snprintf(buf.data(), buf.size(), "%s", joined.c_str());
            }
            if (ImGui::InputText(label.c_str(), buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                std::string text = std::string("[") + buf.data() + "]";
                changed = y.Set(path, text);
                if (!changed)
                    status = _("Not a list of numbers: ") + std::string(buf.data());
            }
            break;
        }
        case Kind::String:
        {
            auto &buf = yamlTextBuffers[path];
            if (!ImGui::IsAnyItemActive())
                std::snprintf(buf.data(), buf.size(), "%s", n.Scalar().c_str());
            if (ImGui::InputText(label.c_str(), buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                changed = y.Set(path, Quote(buf.data()));
            }
            break;
        }
        case Kind::Other:
            ImGui::TextDisabled("%s", label.c_str());
            break;
    }
    CommentTooltip(comment);

    // Animation references: a button arms the gallery, its next click goes into this key
    if (yamlAnimPaths.count(path))
    {
        ImGui::SameLine();
        const bool armed = animPickPath == path;
        if (armed)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
        if (Hinted(ImGui::SmallButton(armed ? _("Pick: click an animation") : _("Pick")), "Pick animation"))
        {
            animPickPath = armed ? std::string() : path;
        }
        if (armed)
            ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return changed;
}

bool CharacterEditor::EditYamlTree(CommentedYaml &y, const std::string &path, int depth)
{
    const YAML::Node n = y.At(path);
    if (!n.IsDefined() || !n.IsMap())
        return false;
    // Keys first: the loop edits the text, which re-parses the document
    std::vector<std::string> keys;
    for (const auto &kv : n)
        keys.push_back(kv.first.as<std::string>());
    bool changed = false;
    for (const std::string &key : keys)
    {
        const std::string sub = path.empty() ? key : path + "." + key;
        const YAML::Node v = y.At(sub);
        if (!v.IsDefined())
            continue;
        if (v.IsMap())
        {
            ImGui::SetNextItemOpen(depth < 1, ImGuiCond_Once);
            const bool open = ImGui::TreeNodeEx((Pretty(key) + "##" + sub).c_str(), ImGuiTreeNodeFlags_Framed);
            CommentTooltip(y.Comment(sub));
            if (open)
            {
                changed |= EditYamlTree(y, sub, depth + 1);
                ImGui::TreePop();
            }
        }
        else
        {
            changed |= EditYamlScalar(y, sub, Pretty(LastKey(sub)));
        }
    }
    return changed;
}

void CharacterEditor::SaveActiveSettings()
{
    switch (editorMode)
    {
        case EditorMode::Explosions:
        case EditorMode::Wrecks:
            if (SaveYamlFile(effectsYaml))
            {
                EffectsChanged();
                ApplySaved(false);
            }
            break;
        case EditorMode::Projectiles:
            if (SaveYamlFile(projectilesYaml))
            {
                ProjectilesChanged();
                ApplySaved(false);
            }
            break;
        case EditorMode::GameUi:
            if (SaveYamlFile(uiYaml))
                ApplySaved(false);
            break;
        case EditorMode::AiDifficulty:
            if (SaveYamlFile(aiYaml))
                ApplySaved(false);
            break;
        case EditorMode::Environment:
            SaveEnvironment();
            break;
        default:
            break;
    }
}

} // namespace CharEditor
