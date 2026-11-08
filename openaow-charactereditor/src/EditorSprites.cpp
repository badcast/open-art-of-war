// Sprite Tools tab: the per-object sprites of the game (assets/sprites/sprite_<name>.png + sprite_<name>_meta.yaml).
// One object at a time: its image with the frames on it; frames are moved, resized, added, sliced from a grid or
// found in the image, renamed and removed, and the meta is saved (only its frame lines change). A new object is made
// from a PNG. Parts, animations and families are written in the meta by hand; the tab lists them.
#include "CharacterEditor.hpp"

#include "game/Effects.hpp"
#include "game/Projectiles.hpp"
#include "graphics/SpriteRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>

namespace CharEditor
{

namespace
{

using OpenArtOfWar::SpriteKind;
using OpenArtOfWar::Sprites;

void SDLCALL OnPngChosen(void *userdata, const char *const *list, int /*filter*/)
{
    auto *pick = static_cast<FolderPick *>(userdata);
    std::lock_guard<std::mutex> lock(pick->mutex);
    if (list && list[0])
    {
        pick->path = list[0];
        pick->ready = true;
    }
}

std::string Num(float v)
{
    if (std::fabs(v - std::round(v)) < 1e-4f)
        return std::to_string(static_cast<long long>(std::lround(v)));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    return buf;
}

const char *const kRecolorNames[] = {"none", "blue", "red"};

bool ValidName(const std::string &n)
{
    if (n.empty())
        return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    return true;
}

std::string ReadFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool WriteFile(const fs::path &p, const std::string &text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

// The PNG as RGBA32 (for finding frames); nullptr when it cannot be read
SDL_Surface *LoadRgba(const fs::path &png)
{
    SDL_Surface *loaded = SDL_LoadPNG(png.string().c_str());
    if (!loaded)
        return nullptr;
    SDL_Surface *rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    return rgba;
}

bool Opaque(const SDL_Surface *s, int x, int y)
{
    const auto *row = static_cast<const std::uint8_t *>(s->pixels) + y * s->pitch;
    return row[x * 4 + 3] > 8;
}

} // namespace

fs::path CharacterEditor::SpriteDir() const
{
    return assetsRoot / "sprites";
}

fs::path CharacterEditor::SpriteMetaPath(const std::string &object) const
{
    return SpriteDir() / ("sprite_" + object + "_meta.yaml");
}

void SpriteCanvasCamera(
    const ImVec2 &origin,
    const ImVec2 &avail,
    bool hovered,
    bool active,
    bool middleDoubleClick,
    const float content[4],
    const float *focus,
    float &zoom,
    ImVec2 &pan,
    SpriteCamera &pending)
{
    const ImGuiIO &io = ImGui::GetIO();
    // Buttons in the top right corner, Home / double middle click fit the content, F the focus
    {
        const float pad = ImGui::GetStyle().FramePadding.x * 2.0f;
        const float wFit = ImGui::CalcTextSize(_("Fit")).x + pad;
        const float wOne = ImGui::CalcTextSize("1:1").x + pad;
        const float wFrame = ImGui::CalcTextSize(_("Frame")).x + pad;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + avail.x - (wFit + wOne + wFrame + 2.0f * spacing) - 6.0f, origin.y + 6.0f));
        if (Hinted(ImGui::SmallButton(_("Fit")), "Fit"))
            pending = SpriteCamera::Fit;
        ImGui::SameLine();
        if (Hinted(ImGui::SmallButton("1:1"), "1:1"))
            pending = SpriteCamera::Actual;
        ImGui::SameLine();
        ImGui::BeginDisabled(!focus);
        if (Hinted(ImGui::SmallButton(_("Frame")), "Frame"))
            pending = SpriteCamera::Frame;
        ImGui::EndDisabled();
        const bool keys = (hovered || ImGui::IsWindowFocused()) && !io.WantTextInput;
        if (middleDoubleClick || (keys && ImGui::IsKeyPressed(ImGuiKey_Home, false)))
            pending = SpriteCamera::Fit;
        if (keys && focus && ImGui::IsKeyPressed(ImGuiKey_F, false))
            pending = SpriteCamera::Frame;
    }
    if (pending != SpriteCamera::None)
    {
        // The status line at the bottom stays free
        const float margin = 16.0f;
        const float viewW = std::max(1.0f, avail.x - 2.0f * margin);
        const float viewH = std::max(1.0f, avail.y - 2.0f * margin - ImGui::GetTextLineHeight());
        const float *r = pending == SpriteCamera::Frame && focus ? focus : content;
        const float rw = std::max(r[2], 1.0f), rh = std::max(r[3], 1.0f);
        if (pending == SpriteCamera::Actual)
            zoom = 1.0f;
        else
            zoom = std::clamp(std::min(viewW / rw, viewH / rh), 0.1f, pending == SpriteCamera::Frame ? 16.0f : 8.0f);
        pan.x = margin + (viewW - rw * zoom) * 0.5f - r[0] * zoom;
        pan.y = margin + (viewH - rh * zoom) * 0.5f - r[1] * zoom;
        pending = SpriteCamera::None;
    }
    // Zoom about the cursor, pan with the right / middle button
    if (hovered && io.MouseWheel != 0.0f)
    {
        const float mx = (io.MousePos.x - origin.x - pan.x) / zoom;
        const float my = (io.MousePos.y - origin.y - pan.y) / zoom;
        const float old = zoom;
        zoom = std::clamp(zoom * (io.MouseWheel > 0 ? 1.25f : 0.8f), 0.1f, 32.0f);
        pan.x -= mx * (zoom - old);
        pan.y -= my * (zoom - old);
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
    {
        pan.x += io.MouseDelta.x;
        pan.y += io.MouseDelta.y;
    }
}

void CharacterEditor::LoadSpriteObject(const std::string &object)
{
    spriteObject = object;
    spriteFrames.clear();
    spriteRemoved.clear();
    spriteFrameSel = -1;
    spriteDirty = false;
    spriteCameraPending = SpriteCamera::Fit;
    spriteScale = 1.0f;
    spriteRecolor = 0;
    spriteImageW = spriteImageH = 0;
    if (object.empty())
        return;
    float declaredW = 0.0f, declaredH = 0.0f; // "image_size": the image the rectangles are written for
    try
    {
        const YAML::Node meta = YAML::LoadFile(SpriteMetaPath(object).string());
        spriteScale = meta["scale"] ? meta["scale"].as<float>() : 1.0f;
        if (spriteScale <= 0.0f)
            spriteScale = 1.0f;
        const std::string rc = meta["recolor"] ? meta["recolor"].as<std::string>() : "none";
        spriteRecolor = rc == "blue" ? 1 : rc == "red" ? 2 : 0;
        spriteImageName = meta["image"] ? meta["image"].as<std::string>() : "sprite_" + object + ".png";
        if (const YAML::Node d = meta["image_size"]; d && d.IsSequence() && d.size() == 2)
        {
            declaredW = d[0].as<float>(0.0f);
            declaredH = d[1].as<float>(0.0f);
        }
        if (const YAML::Node frames = meta["frames"])
        {
            for (const auto &kv : frames)
            {
                SpriteFrameEdit f;
                f.name = f.original = kv.first.as<std::string>();
                const YAML::Node v = kv.second;
                const YAML::Node rect = v.IsMap() ? v["rect"] : v;
                for (int i = 0; i < 4 && rect && rect.IsSequence() && i < static_cast<int>(rect.size()); ++i)
                    f.rect[i] = static_cast<int>(std::lround(rect[i].as<float>()));
                f.ppu[0] = f.ppu[1] = spriteScale;
                if (v.IsMap() && v["size"] && v["size"].size() == 2)
                {
                    const float sw = v["size"][0].as<float>(), sh = v["size"][1].as<float>();
                    if (sw > 0.0f && f.rect[2] > 0)
                        f.ppu[0] = f.rect[2] / sw;
                    if (sh > 0.0f && f.rect[3] > 0)
                        f.ppu[1] = f.rect[3] / sh;
                    f.size[0] = sw;
                    f.size[1] = sh;
                    f.ownSize = true;
                }
                else
                {
                    f.size[0] = f.rect[2] / spriteScale;
                    f.size[1] = f.rect[3] / spriteScale;
                }
                if (v.IsMap() && v["recolor"])
                {
                    const std::string r = v["recolor"].as<std::string>();
                    f.recolor = r == "blue" ? 1 : r == "red" ? 2 : 0;
                }
                if (v.IsMap() && v["palette"])
                    f.palette = v["palette"].as<int>();
                spriteFrames.push_back(f);
            }
        }
    }
    catch (const std::exception &e)
    {
        spriteReport = SpriteMetaPath(object).string() + ": " + e.what();
    }
    const int page = Sprites().ImageOf(object);
    if (SDL_Texture *tex = textures->GetSpriteSheet(page))
    {
        float w = 0.0f, h = 0.0f;
        SDL_GetTextureSize(tex, &w, &h);
        spriteImageW = static_cast<int>(w);
        spriteImageH = static_cast<int>(h);
    }
    else if (SDL_Surface *s = LoadRgba(SpriteDir() / spriteImageName))
    {
        spriteImageW = s->w;
        spriteImageH = s->h;
        SDL_DestroySurface(s);
    }
    // The image was scaled since the rectangles were written: they follow it in pixels, the drawn sizes stay.
    // Evenly scaled, the scale follows too; otherwise every frame keeps its size. Saving writes the image's pixels
    if (declaredW > 0.0f && declaredH > 0.0f && spriteImageW > 0 && spriteImageH > 0
        && (std::lround(declaredW) != spriteImageW || std::lround(declaredH) != spriteImageH))
    {
        const float kx = spriteImageW / declaredW, ky = spriteImageH / declaredH;
        const bool even = std::fabs(kx - ky) < 1e-3f;
        if (even)
            spriteScale *= kx;
        for (SpriteFrameEdit &f : spriteFrames)
        {
            f.rect[0] = static_cast<int>(std::lround(f.rect[0] * kx));
            f.rect[1] = static_cast<int>(std::lround(f.rect[1] * ky));
            f.rect[2] = std::max(1, static_cast<int>(std::lround(f.rect[2] * kx)));
            f.rect[3] = std::max(1, static_cast<int>(std::lround(f.rect[3] * ky)));
            f.ppu[0] *= kx;
            f.ppu[1] *= ky;
            if (!even)
                f.ownSize = true;
        }
        spriteReport = _("The image is ") + std::to_string(spriteImageW) + "x" + std::to_string(spriteImageH)
                       + _(", the frames were written for ") + std::to_string(std::lround(declaredW)) + "x"
                       + std::to_string(std::lround(declaredH)) + _(": scaled with it (Save writes them so)");
    }
}

std::string CharacterEditor::SpriteFrameLine(const SpriteFrameEdit &f) const
{
    std::string s = "{rect: [" + std::to_string(f.rect[0]) + ", " + std::to_string(f.rect[1]) + ", "
                    + std::to_string(f.rect[2]) + ", " + std::to_string(f.rect[3]) + "]";
    const bool defaultSize = std::fabs(f.ppu[0] - spriteScale) < 1e-4f && std::fabs(f.ppu[1] - spriteScale) < 1e-4f;
    if (f.ownSize || !defaultSize)
        s += ", size: [" + Num(f.size[0]) + ", " + Num(f.size[1]) + "]";
    if (f.recolor >= 0 && f.recolor != spriteRecolor)
        s += std::string(", recolor: ") + kRecolorNames[f.recolor];
    if (f.palette)
        s += ", palette: " + std::to_string(f.palette);
    return s + "}";
}

bool CharacterEditor::SaveSpriteObject()
{
    if (spriteObject.empty())
        return false;
    const fs::path path = SpriteMetaPath(spriteObject);
    std::string text = ReadFile(path);
    // Renamed frames: their references in this meta follow
    for (const SpriteFrameEdit &f : spriteFrames)
    {
        if (!f.original.empty() && f.original != f.name)
        {
            const std::regex ref("(frame:\\s*)" + f.original + "\\b");
            text = std::regex_replace(text, ref, "$1" + f.name);
        }
    }
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
    }
    std::map<std::string, const SpriteFrameEdit *> byOriginal;
    for (const SpriteFrameEdit &f : spriteFrames)
        if (!f.original.empty())
            byOriginal[f.original] = &f;
    std::set<std::string> removed(spriteRemoved.begin(), spriteRemoved.end());

    std::vector<std::string> out;
    bool inFrames = false, sawFrames = false, skipping = false, sawScale = false, sawImageSize = false;
    const std::string sizeLine
        = "image_size: [" + std::to_string(spriteImageW) + ", " + std::to_string(spriteImageH) + "]";
    auto addNew = [&]() {
        for (const SpriteFrameEdit &f : spriteFrames)
            if (f.original.empty())
                out.push_back("  " + f.name + ": " + SpriteFrameLine(f));
    };
    const std::regex key("^  ([A-Za-z0-9_]+):(.*)$");
    for (const std::string &line : lines)
    {
        const bool topLevel = !line.empty() && line[0] != ' ' && line[0] != '#';
        if (topLevel)
        {
            if (inFrames)
                addNew();
            inFrames = line.rfind("frames:", 0) == 0;
            sawFrames |= inFrames;
            skipping = false;
            // The scale and the size of the image the rectangles are written for
            if (line.rfind("scale:", 0) == 0)
            {
                out.push_back("scale: " + Num(spriteScale));
                sawScale = true;
                continue;
            }
            if (line.rfind("image_size:", 0) == 0)
            {
                if (spriteImageW > 0)
                    out.push_back(sizeLine);
                sawImageSize = true;
                continue;
            }
            out.push_back(line);
            continue;
        }
        if (inFrames)
        {
            std::smatch m;
            if (std::regex_match(line, m, key))
            {
                skipping = false;
                const std::string name = m[1];
                if (removed.count(name))
                {
                    skipping = true; // and its continuation lines
                    continue;
                }
                const auto it = byOriginal.find(name);
                if (it != byOriginal.end())
                {
                    out.push_back("  " + it->second->name + ": " + SpriteFrameLine(*it->second));
                    skipping = true;
                    continue;
                }
            }
            else if (skipping && line.rfind("   ", 0) == 0)
            {
                continue;
            }
        }
        out.push_back(line);
    }
    if (inFrames)
        addNew();
    if (!sawFrames)
    {
        out.push_back("frames:");
        addNew();
    }
    // Missing scale / image_size lines go before the frames
    {
        auto at = std::find_if(out.begin(), out.end(), [](const std::string &l) { return l.rfind("frames:", 0) == 0; });
        if (!sawImageSize && spriteImageW > 0)
            at = out.insert(at, sizeLine) + 1;
        if (!sawScale && std::fabs(spriteScale - 1.0f) > 1e-4f)
            out.insert(at, "scale: " + Num(spriteScale));
    }
    std::string result;
    for (const std::string &l : out)
        result += l + "\n";
    if (!WriteFile(path, result))
    {
        spriteReport = _("Could not write ") + path.string();
        return false;
    }
    ReloadSpriteData();
    spriteReport = _("Saved ") + path.filename().string();
    return true;
}

bool CharacterEditor::ScaleSpriteImage(float factor, bool smooth)
{
    if (spriteObject.empty() || spriteImageW <= 0)
        return false;
    if (spriteDirty)
    {
        spriteReport = _("Save or revert the changes first");
        return false;
    }
    const fs::path metaPath = SpriteMetaPath(spriteObject);
    std::vector<std::string> images = {spriteImageName};
    try
    {
        const YAML::Node meta = YAML::LoadFile(metaPath.string());
        if (meta["resistance_image"])
            images.push_back(meta["resistance_image"].as<std::string>());
    }
    catch (const std::exception &e)
    {
        spriteReport = metaPath.filename().string() + ": " + e.what();
        return false;
    }
    // The images, all of the same layout
    for (const std::string &name : images)
    {
        SDL_Surface *s = LoadRgba(SpriteDir() / name);
        if (!s)
        {
            spriteReport = _("Not a PNG image: ") + (SpriteDir() / name).string();
            return false;
        }
        const int w = std::max(1, static_cast<int>(std::lround(s->w * factor)));
        const int h = std::max(1, static_cast<int>(std::lround(s->h * factor)));
        SDL_Surface *scaled = SDL_ScaleSurface(s, w, h, smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_DestroySurface(s);
        const bool ok = scaled && SDL_SavePNG(scaled, (SpriteDir() / name).string().c_str());
        if (scaled)
            SDL_DestroySurface(scaled);
        if (!ok)
        {
            spriteReport = _("Could not write ") + (SpriteDir() / name).string() + ": " + SDL_GetError();
            return false;
        }
    }
    // The meta keeps describing the old size: loading scales the frames, saving writes them in the new pixels
    const std::string oldSize
        = "image_size: [" + std::to_string(spriteImageW) + ", " + std::to_string(spriteImageH) + "]";
    std::string text = ReadFile(metaPath);
    const std::regex sizeLine("(^|\n)image_size:[^\n]*");
    if (std::regex_search(text, sizeLine))
        text = std::regex_replace(text, sizeLine, "$1" + oldSize);
    else
    {
        const std::regex framesLine("(^|\n)frames:");
        text = std::regex_search(text, framesLine)
                   ? std::regex_replace(
                         text, framesLine, "$1" + oldSize + "\nframes:", std::regex_constants::format_first_only)
                   : text + oldSize + "\n";
    }
    WriteFile(metaPath, text);
    const int oldW = spriteImageW, oldH = spriteImageH;
    ReloadSpriteData();
    if (!SaveSpriteObject())
        return false;
    spriteReport = _("Scaled ") + std::to_string(oldW) + "x" + std::to_string(oldH) + " -> "
                   + std::to_string(spriteImageW) + "x" + std::to_string(spriteImageH)
                   + _(": frames and scale follow, the drawn sizes are the same");
    return true;
}

void CharacterEditor::ReloadSpriteData()
{
    textures->ReloadSprites();
    // The configs keep sprite handles: read them again against the new tables
    OpenArtOfWar::EffectsConfig::reload();
    OpenArtOfWar::Projectiles::Reload();
    ClearThumbnails();
    const std::string keep = spriteObject;
    const int sel = spriteFrameSel;
    LoadSpriteObject(keep);
    spriteFrameSel = sel < static_cast<int>(spriteFrames.size()) ? sel : -1;
    const auto &errors = Sprites().Errors();
    if (!errors.empty())
        spriteReport = std::to_string(errors.size()) + _(" sprite error(s); first: ") + errors.front();
}

bool CharacterEditor::CreateSpriteObject(const fs::path &png, const std::string &name)
{
    if (!ValidName(name))
    {
        spriteReport = _("A sprite name is letters, digits and '_'");
        return false;
    }
    if (fs::exists(SpriteMetaPath(name)))
    {
        spriteReport = _("This sprite exists already: ") + name;
        return false;
    }
    SDL_Surface *s = LoadRgba(png);
    if (!s)
    {
        spriteReport = _("Not a PNG image: ") + png.string();
        return false;
    }
    const int w = s->w, h = s->h;
    SDL_DestroySurface(s);
    std::error_code ec;
    fs::copy_file(png, SpriteDir() / ("sprite_" + name + ".png"), fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        spriteReport = ec.message();
        return false;
    }
    const std::string meta = "# Sprite \"" + name + "\" (made in the character editor from " + png.filename().string()
                             + ")\nimage: sprite_" + name + ".png\nimage_size: [" + std::to_string(w) + ", "
                             + std::to_string(h) + "]\nscale: 1\nframes:\n  f0: {rect: [0, 0, " + std::to_string(w)
                             + ", " + std::to_string(h) + "]}\n";
    if (!WriteFile(SpriteMetaPath(name), meta))
    {
        spriteReport = _("Could not write ") + SpriteMetaPath(name).string();
        return false;
    }
    textures->ReloadSprites();
    LoadSpriteObject(name);
    spriteReport = _("Made sprite ") + name;
    return true;
}

std::string CharacterEditor::FreeFrameName(const std::string &prefix) const
{
    for (int i = 0;; ++i)
    {
        const std::string n = prefix + std::to_string(i);
        bool used = false;
        for (const SpriteFrameEdit &f : spriteFrames)
            used |= f.name == n;
        if (!used)
            return n;
    }
}

void CharacterEditor::AddSpriteFrame(int x, int y, int w, int h, const std::string &prefix)
{
    SpriteFrameEdit f;
    f.name = FreeFrameName(prefix);
    f.rect[0] = x;
    f.rect[1] = y;
    f.rect[2] = std::max(1, w);
    f.rect[3] = std::max(1, h);
    f.ppu[0] = f.ppu[1] = spriteScale;
    f.size[0] = f.rect[2] / spriteScale;
    f.size[1] = f.rect[3] / spriteScale;
    spriteFrames.push_back(f);
    spriteFrameSel = static_cast<int>(spriteFrames.size()) - 1;
    spriteDirty = true;
}

void CharacterEditor::RemoveSpriteFrame(int index)
{
    if (index < 0 || index >= static_cast<int>(spriteFrames.size()))
        return;
    if (!spriteFrames[index].original.empty())
        spriteRemoved.push_back(spriteFrames[index].original);
    spriteFrames.erase(spriteFrames.begin() + index);
    spriteFrameSel = std::min(spriteFrameSel, static_cast<int>(spriteFrames.size()) - 1);
    spriteDirty = true;
}

void CharacterEditor::SliceSpriteGrid()
{
    if (spriteImageW <= 0 || spriteGridCols < 1 || spriteGridRows < 1)
        return;
    SDL_Surface *s = spriteSkipEmpty ? LoadRgba(SpriteDir() / spriteImageName) : nullptr;
    const int cw = spriteImageW / spriteGridCols, ch = spriteImageH / spriteGridRows;
    int added = 0;
    for (int r = 0; r < spriteGridRows; ++r)
    {
        for (int c = 0; c < spriteGridCols; ++c)
        {
            const int x0 = c * cw, y0 = r * ch;
            bool any = !s;
            for (int y = y0; s && !any && y < y0 + ch && y < s->h; ++y)
                for (int x = x0; !any && x < x0 + cw && x < s->w; ++x)
                    any = Opaque(s, x, y);
            if (!any)
                continue;
            AddSpriteFrame(x0, y0, cw, ch, "cell_");
            ++added;
        }
    }
    if (s)
        SDL_DestroySurface(s);
    spriteReport = std::to_string(added) + _(" frame(s) added from the grid");
}

void CharacterEditor::DetectSpriteFrames()
{
    SDL_Surface *s = LoadRgba(SpriteDir() / spriteImageName);
    if (!s)
    {
        spriteReport = _("Cannot read the image");
        return;
    }
    // Islands of opaque pixels (8-connected); islands closer than `spriteDetectGap` pixels make one frame
    const int w = s->w, h = s->h;
    std::vector<int> label(static_cast<std::size_t>(w) * h, -1);
    struct Box
    {
        int x0, y0, x1, y1;
    };
    std::vector<Box> boxes;
    std::vector<int> stack;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            if (label[y * w + x] >= 0 || !Opaque(s, x, y))
                continue;
            Box b {x, y, x, y};
            const int id = static_cast<int>(boxes.size());
            stack.assign(1, y * w + x);
            label[y * w + x] = id;
            while (!stack.empty())
            {
                const int p = stack.back();
                stack.pop_back();
                const int px = p % w, py = p / w;
                b.x0 = std::min(b.x0, px);
                b.y0 = std::min(b.y0, py);
                b.x1 = std::max(b.x1, px);
                b.y1 = std::max(b.y1, py);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = px + dx, ny = py + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || label[ny * w + nx] >= 0 || !Opaque(s, nx, ny))
                            continue;
                        label[ny * w + nx] = id;
                        stack.push_back(ny * w + nx);
                    }
            }
            boxes.push_back(b);
        }
    }
    SDL_DestroySurface(s);
    // Merge boxes that touch (within the gap) until nothing changes
    bool merged = true;
    const int gap = std::max(0, spriteDetectGap);
    while (merged)
    {
        merged = false;
        for (std::size_t i = 0; i < boxes.size() && !merged; ++i)
            for (std::size_t j = i + 1; j < boxes.size() && !merged; ++j)
            {
                const Box &a = boxes[i], &b = boxes[j];
                if (a.x0 - gap <= b.x1 && b.x0 - gap <= a.x1 && a.y0 - gap <= b.y1 && b.y0 - gap <= a.y1)
                {
                    boxes[i] = {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
                    boxes.erase(boxes.begin() + static_cast<long>(j));
                    merged = true;
                }
            }
    }
    std::sort(boxes.begin(), boxes.end(), [](const Box &a, const Box &b) {
        return a.y0 != b.y0 ? a.y0 < b.y0 : a.x0 < b.x0;
    });
    int added = 0;
    for (const Box &b : boxes)
    {
        if ((b.x1 - b.x0 + 1) * (b.y1 - b.y0 + 1) < spriteDetectMinArea)
            continue;
        AddSpriteFrame(b.x0, b.y0, b.x1 - b.x0 + 1, b.y1 - b.y0 + 1, "f");
        ++added;
    }
    spriteReport = std::to_string(added) + _(" frame(s) found in the image");
}

void CharacterEditor::SyncSpriteObjectFromActive()
{
    // The object of the active character ("sprite: <object>" or "<object>.<building>")
    const std::string ref = YamlString(document, "sprite", "");
    const std::string object = ref.substr(0, ref.find('.'));
    if (!object.empty() && object != spriteObject && !spriteDirty && Sprites().HasObject(object))
        LoadSpriteObject(object);
}

void CharacterEditor::DrawSpriteTools()
{
    // Pick up a PNG chosen in the file dialog: a new sprite from it
    {
        std::lock_guard<std::mutex> lock(spritePngPick.mutex);
        if (spritePngPick.ready)
        {
            spritePngPick.ready = false;
            std::strncpy(spriteNewPng.data(), spritePngPick.path.c_str(), spriteNewPng.size() - 1);
            if (spriteNewName[0] == '\0')
            {
                std::string stem = fs::path(spritePngPick.path).stem().string();
                for (char &c : stem)
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                        c = '_';
                std::strncpy(spriteNewName.data(), stem.c_str(), spriteNewName.size() - 1);
            }
        }
    }

    // Left: the objects
    ImGui::BeginChild("sprite-objects", ImVec2(230.0f, 0.0f), true);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", _("Search"), spriteSearch.data(), spriteSearch.size());
    const std::string filter = spriteSearch.data();
    for (const std::string &name : Sprites().ObjectNames())
    {
        if (!filter.empty() && name.find(filter) == std::string::npos)
            continue;
        if (ImGui::Selectable(name.c_str(), name == spriteObject))
        {
            if (spriteDirty)
                spriteReport = _("Save or revert the changes of ") + spriteObject + _(" first");
            else
                LoadSpriteObject(name);
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: the image of the object with its frames
    ImGui::BeginChild(
        "sprite-canvas", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetNextItemAllowOverlap(); // the camera buttons sit on top of the canvas
    ImGui::InvisibleButton(
        "canvas",
        avail,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool canvasActive = ImGui::IsItemActive();
    const bool middleDoubleClick = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(24, 26, 30, 255));
    if (spriteObject.empty())
    {
        dl->AddText(
            ImVec2(origin.x + 12, origin.y + 12),
            IM_COL32(200, 200, 200, 255),
            _("Choose a sprite on the left, or make one from a PNG (right panel)."));
        ImGui::EndChild();
        return;
    }
    const ImGuiIO &io = ImGui::GetIO();

    float focus[4] {0, 0, 0, 0};
    const bool hasFocus = spriteFrameSel >= 0 && spriteFrameSel < static_cast<int>(spriteFrames.size());
    if (hasFocus)
        for (int i = 0; i < 4; ++i)
            focus[i] = static_cast<float>(spriteFrames[spriteFrameSel].rect[i]);
    const float content[4] {0.0f, 0.0f, static_cast<float>(spriteImageW), static_cast<float>(spriteImageH)};
    SpriteCanvasCamera(
        origin,
        avail,
        hovered,
        canvasActive,
        middleDoubleClick,
        content,
        hasFocus ? focus : nullptr,
        spriteZoom,
        spritePan,
        spriteCameraPending);
    auto toScreen = [&](float x, float y) {
        return ImVec2(origin.x + spritePan.x + x * spriteZoom, origin.y + spritePan.y + y * spriteZoom);
    };
    const ImVec2 mouse = io.MousePos;
    const float mx = (mouse.x - origin.x - spritePan.x) / spriteZoom;
    const float my = (mouse.y - origin.y - spritePan.y) / spriteZoom;

    // The image and a checkerboard under it
    const int page = Sprites().ImageOf(spriteObject);
    SDL_Texture *tex = textures->GetSpriteSheet(page);
    const ImVec2 i0 = toScreen(0, 0), i1 = toScreen(static_cast<float>(spriteImageW), static_cast<float>(spriteImageH));
    dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    dl->AddRectFilled(i0, i1, IM_COL32(60, 60, 64, 255));
    if (tex)
        dl->AddImage(reinterpret_cast<ImTextureID>(tex), i0, i1);
    dl->AddRect(i0, i1, IM_COL32(120, 120, 130, 255));

    // Frames; the selected one has corner handles
    const float handle = 5.0f;
    int hoveredFrame = -1;
    for (int i = 0; i < static_cast<int>(spriteFrames.size()); ++i)
    {
        const SpriteFrameEdit &f = spriteFrames[i];
        const ImVec2 a = toScreen(static_cast<float>(f.rect[0]), static_cast<float>(f.rect[1]));
        const ImVec2 b = toScreen(static_cast<float>(f.rect[0] + f.rect[2]), static_cast<float>(f.rect[1] + f.rect[3]));
        const bool inside = mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;
        if (inside && hovered)
            hoveredFrame = i; // the last (topmost) one wins
        const bool sel = i == spriteFrameSel;
        dl->AddRect(a, b, sel ? IM_COL32(255, 180, 60, 255) : IM_COL32(90, 200, 160, 200), 0.0f, 0, sel ? 2.0f : 1.0f);
        if (sel || (inside && hovered))
            dl->AddText(
                ImVec2(a.x + 2, a.y - ImGui::GetTextLineHeight() - 1), IM_COL32(255, 230, 180, 255), f.name.c_str());
        if (sel)
            for (int c = 0; c < 4; ++c)
            {
                const ImVec2 p((c & 1) ? b.x : a.x, (c & 2) ? b.y : a.y);
                dl->AddRectFilled(
                    ImVec2(p.x - handle, p.y - handle),
                    ImVec2(p.x + handle, p.y + handle),
                    IM_COL32(255, 180, 60, 255));
            }
    }

    // Left button: draw a new frame (draw mode), drag a corner of the selected frame, move it, or select
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        spriteDrag = SpriteDrag::None;
        spriteDragStart = ImVec2(std::floor(mx), std::floor(my));
        if (spriteDrawMode)
        {
            spriteDrag = SpriteDrag::Draw;
        }
        else
        {
            if (spriteFrameSel >= 0)
            {
                const SpriteFrameEdit &f = spriteFrames[spriteFrameSel];
                const ImVec2 a = toScreen(static_cast<float>(f.rect[0]), static_cast<float>(f.rect[1]));
                const ImVec2 b
                    = toScreen(static_cast<float>(f.rect[0] + f.rect[2]), static_cast<float>(f.rect[1] + f.rect[3]));
                for (int c = 0; c < 4; ++c)
                {
                    const ImVec2 p((c & 1) ? b.x : a.x, (c & 2) ? b.y : a.y);
                    if (std::fabs(mouse.x - p.x) <= handle + 1 && std::fabs(mouse.y - p.y) <= handle + 1)
                    {
                        spriteDrag = SpriteDrag::Resize;
                        spriteDragCorner = c;
                    }
                }
            }
            if (spriteDrag == SpriteDrag::None && hoveredFrame >= 0)
            {
                spriteFrameSel = hoveredFrame;
                spriteDrag = SpriteDrag::Move;
            }
            else if (spriteDrag == SpriteDrag::None)
            {
                spriteFrameSel = -1;
            }
            if (spriteFrameSel >= 0)
                std::copy(spriteFrames[spriteFrameSel].rect, spriteFrames[spriteFrameSel].rect + 4, spriteDragRect);
        }
    }
    if (spriteDrag != SpriteDrag::None && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const int dx = static_cast<int>(std::floor(mx) - spriteDragStart.x);
        const int dy = static_cast<int>(std::floor(my) - spriteDragStart.y);
        if (spriteDrag == SpriteDrag::Draw)
        {
            const ImVec2 a = toScreen(spriteDragStart.x, spriteDragStart.y),
                         b = toScreen(std::floor(mx) + 1, std::floor(my) + 1);
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.5f);
        }
        else if (spriteFrameSel >= 0 && (dx || dy))
        {
            SpriteFrameEdit &f = spriteFrames[spriteFrameSel];
            int x0 = spriteDragRect[0], y0 = spriteDragRect[1];
            int x1 = x0 + spriteDragRect[2], y1 = y0 + spriteDragRect[3];
            if (spriteDrag == SpriteDrag::Move)
            {
                x0 += dx, x1 += dx, y0 += dy, y1 += dy;
            }
            else
            {
                (spriteDragCorner & 1 ? x1 : x0) += dx;
                (spriteDragCorner & 2 ? y1 : y0) += dy;
            }
            SetSpriteFrameRect(
                f, std::min(x0, x1), std::min(y0, y1), std::max(1, std::abs(x1 - x0)), std::max(1, std::abs(y1 - y0)));
        }
    }
    if (spriteDrag != SpriteDrag::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        if (spriteDrag == SpriteDrag::Draw)
        {
            const int x0 = static_cast<int>(std::min(spriteDragStart.x, std::floor(mx)));
            const int y0 = static_cast<int>(std::min(spriteDragStart.y, std::floor(my)));
            const int w = static_cast<int>(std::fabs(std::floor(mx) - spriteDragStart.x)) + 1;
            const int h = static_cast<int>(std::fabs(std::floor(my) - spriteDragStart.y)) + 1;
            if (w > 1 && h > 1)
                AddSpriteFrame(x0, y0, w, h, "f");
        }
        spriteDrag = SpriteDrag::None;
    }

    // Arrows move the selected frame by a pixel (Shift: 10), with Ctrl they resize it
    if (spriteFrameSel >= 0 && ImGui::IsWindowFocused() && !io.WantTextInput)
    {
        SpriteFrameEdit &f = spriteFrames[spriteFrameSel];
        const int step = io.KeyShift ? 10 : 1;
        int d[2] = {0, 0};
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            d[0] = -step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            d[0] = step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            d[1] = -step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            d[1] = step;
        if (d[0] || d[1])
        {
            if (io.KeyCtrl)
                SetSpriteFrameRect(
                    f, f.rect[0], f.rect[1], std::max(1, f.rect[2] + d[0]), std::max(1, f.rect[3] + d[1]));
            else
                SetSpriteFrameRect(f, f.rect[0] + d[0], f.rect[1] + d[1], f.rect[2], f.rect[3]);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))
            RemoveSpriteFrame(spriteFrameSel);
    }
    dl->PopClipRect();

    // Status line
    char line[256];
    std::snprintf(
        line,
        sizeof(line),
        "%s  %dx%d  zoom %.2f  (%d, %d)%s",
        spriteObject.c_str(),
        spriteImageW,
        spriteImageH,
        spriteZoom,
        static_cast<int>(std::floor(mx)),
        static_cast<int>(std::floor(my)),
        spriteDirty ? _("  - not saved") : "");
    dl->AddText(
        ImVec2(origin.x + 8, origin.y + avail.y - ImGui::GetTextLineHeight() - 6), IM_COL32(220, 220, 220, 255), line);
    ImGui::EndChild();
}

void CharacterEditor::SetSpriteFrameRect(SpriteFrameEdit &f, int x, int y, int w, int h)
{
    f.rect[0] = x;
    f.rect[1] = y;
    f.rect[2] = w;
    f.rect[3] = h;
    // The drawn size follows at the frame's pixels per unit
    f.size[0] = f.ppu[0] > 0.0f ? w / f.ppu[0] : static_cast<float>(w);
    f.size[1] = f.ppu[1] > 0.0f ? h / f.ppu[1] : static_cast<float>(h);
    spriteDirty = true;
}

void CharacterEditor::DrawSpriteProperties()
{
    using OpenArtOfWar::SpriteKind;
    // --- New sprite from a PNG ---
    if (ImGui::CollapsingHeader(_("New sprite from PNG"), spriteObject.empty() ? ImGuiTreeNodeFlags_DefaultOpen : 0))
    {
        ImGui::InputText(_("PNG file"), spriteNewPng.data(), spriteNewPng.size());
        ImGui::SameLine();
        if (ImGui::Button(_("Browse...")))
        {
            static const SDL_DialogFileFilter filter[] = {{"PNG", "png"}};
            SDL_ShowOpenFileDialog(OnPngChosen, &spritePngPick, window, filter, 1, nullptr, false);
        }
        ImGui::InputText(_("Name"), spriteNewName.data(), spriteNewName.size());
        ImGui::TextDisabled("%s", _("Becomes sprites/sprite_<name>.png and sprite_<name>_meta.yaml"));
        if (ImGui::Button(_("Make sprite")))
            CreateSpriteObject(spriteNewPng.data(), spriteNewName.data());
    }
    if (!spriteReport.empty())
        ImGui::TextWrapped("%s", spriteReport.c_str());
    if (spriteObject.empty())
        return;

    ImGui::Separator();
    ImGui::Text("%s", spriteObject.c_str());
    ImGui::TextDisabled(
        "%s  %dx%d px, %s %.3g", spriteImageName.c_str(), spriteImageW, spriteImageH, _("scale"), spriteScale);
    {
        const char *kinds[] = {"parts", "anims", "bodies", "sequences", "buildings"};
        const SpriteKind k[]
            = {SpriteKind::Part, SpriteKind::Anim, SpriteKind::Body, SpriteKind::Sequence, SpriteKind::Building};
        std::string summary = std::to_string(spriteFrames.size()) + " frames";
        for (int i = 0; i < 5; ++i)
        {
            const std::size_t n = Sprites().ItemNames(spriteObject, k[i]).size();
            if (n)
                summary += ", " + std::to_string(n) + " " + kinds[i];
        }
        const std::size_t fams = Sprites().FamilyNames(spriteObject).size();
        if (fams)
            summary += ", " + std::to_string(fams) + " families";
        ImGui::TextWrapped("%s", summary.c_str());
    }

    if (Hinted(ImGui::Button(_("Save")), "Save sprite"))
        SaveSpriteObject();
    ImGui::SameLine();
    if (ImGui::Button(_("Revert")))
        LoadSpriteObject(spriteObject);
    ImGui::SameLine();
    if (Hinted(ImGui::Button(_("Reload images")), "Reload images"))
        ReloadSpriteData();
    ImGui::Checkbox(_("Draw new frames (drag on the image)"), &spriteDrawMode);

    // --- Scaling the image: the frames follow, the drawn sizes stay ---
    {
        static const float kFactors[] = {0.25f, 0.5f, 2.0f, 3.0f, 4.0f};
        static const char *const kFactorNames[] = {"x0.25", "x0.5", "x2", "x3", "x4"};
        ImGui::SetNextItemWidth(80.0f);
        ImGui::Combo("##scale-factor", &spriteScaleFactor, kFactorNames, IM_ARRAYSIZE(kFactorNames));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        const char *filters[] = {_("Pixels"), _("Smooth")};
        Hinted(ImGui::Combo("##scale-filter", &spriteScaleSmooth, filters, 2), "##scale-filter");
        ImGui::SameLine();
        if (Hinted(ImGui::Button(_("Scale image")), "Scale image"))
            ScaleSpriteImage(kFactors[std::clamp(spriteScaleFactor, 0, 4)], spriteScaleSmooth == 1);
    }

    // --- Frames ---
    if (ImGui::CollapsingHeader(_("Frames"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::BeginChild("frame-list", ImVec2(0.0f, 140.0f), true);
        for (int i = 0; i < static_cast<int>(spriteFrames.size()); ++i)
        {
            const SpriteFrameEdit &f = spriteFrames[i];
            char label[160];
            std::snprintf(
                label,
                sizeof(label),
                "%s  [%d %d %d %d]##%d",
                f.name.c_str(),
                f.rect[0],
                f.rect[1],
                f.rect[2],
                f.rect[3],
                i);
            if (ImGui::Selectable(label, i == spriteFrameSel))
                spriteFrameSel = i;
        }
        ImGui::EndChild();
        if (spriteFrameSel >= 0 && spriteFrameSel < static_cast<int>(spriteFrames.size()))
        {
            SpriteFrameEdit &f = spriteFrames[spriteFrameSel];
            char name[64];
            std::strncpy(name, f.name.c_str(), sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            if (ImGui::InputText(_("Frame name"), name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                const std::string n = name;
                bool taken = false;
                for (const SpriteFrameEdit &o : spriteFrames)
                    taken |= &o != &f && o.name == n;
                if (ValidName(n) && !taken)
                {
                    f.name = n;
                    spriteDirty = true;
                }
                else
                {
                    spriteReport = _("A frame name is letters, digits and '_', and unique in the sprite");
                }
            }
            int r[4] = {f.rect[0], f.rect[1], f.rect[2], f.rect[3]};
            if (ImGui::DragInt4(_("Pixels x y w h"), r, 0.2f))
                SetSpriteFrameRect(f, r[0], r[1], std::max(1, r[2]), std::max(1, r[3]));
            float size[2] = {f.size[0], f.size[1]};
            if (ImGui::DragFloat2(_("Drawn size"), size, 0.1f, 0.0f, 4096.0f, "%.2f"))
            {
                f.size[0] = std::max(0.01f, size[0]);
                f.size[1] = std::max(0.01f, size[1]);
                f.ppu[0] = f.rect[2] / f.size[0];
                f.ppu[1] = f.rect[3] / f.size[1];
                f.ownSize = true;
                spriteDirty = true;
            }
            ImGui::TextDisabled(_("%.3g x %.3g pixels per game unit"), f.ppu[0], f.ppu[1]);
            const char *recolors[] = {_("as the sprite"), "none", "blue", "red"};
            int rc = f.recolor + 1;
            if (ImGui::Combo(_("Team colour"), &rc, recolors, 4))
            {
                f.recolor = rc - 1;
                spriteDirty = true;
            }
            if (ImGui::Button(_("Remove frame")))
                RemoveSpriteFrame(spriteFrameSel);
            // The frame at a readable size
            if (spriteFrameSel >= 0 && spriteFrameSel < static_cast<int>(spriteFrames.size()))
            {
                const SpriteFrameEdit &g = spriteFrames[spriteFrameSel];
                SDL_Texture *tex = textures->GetSpriteSheet(Sprites().ImageOf(spriteObject));
                if (tex && spriteImageW > 0 && g.rect[2] > 0 && g.rect[3] > 0)
                {
                    const float box = std::min(ImGui::GetContentRegionAvail().x, 200.0f);
                    const float z = std::min(box / g.rect[2], box / g.rect[3]);
                    ImGui::Image(
                        reinterpret_cast<ImTextureID>(tex),
                        ImVec2(g.rect[2] * z, g.rect[3] * z),
                        ImVec2(
                            static_cast<float>(g.rect[0]) / spriteImageW, static_cast<float>(g.rect[1]) / spriteImageH),
                        ImVec2(
                            static_cast<float>(g.rect[0] + g.rect[2]) / spriteImageW,
                            static_cast<float>(g.rect[1] + g.rect[3]) / spriteImageH));
                }
            }
        }
    }

    // --- Cutting the image into frames ---
    if (ImGui::CollapsingHeader(_("Cut the image into frames")))
    {
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt(_("Columns"), &spriteGridCols);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt(_("Rows"), &spriteGridRows);
        spriteGridCols = std::clamp(spriteGridCols, 1, 256);
        spriteGridRows = std::clamp(spriteGridRows, 1, 256);
        ImGui::Checkbox(_("Skip empty cells"), &spriteSkipEmpty);
        if (Hinted(ImGui::Button(_("Slice grid")), "Slice grid"))
            SliceSpriteGrid();
        ImGui::Separator();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt(_("Join gap (px)"), &spriteDetectGap);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt(_("Smallest area (px)"), &spriteDetectMinArea);
        if (Hinted(ImGui::Button(_("Find frames in the image")), "Find frames"))
            DetectSpriteFrames();
        if (!spriteFrames.empty() && ImGui::Button(_("Remove all frames")))
            while (!spriteFrames.empty())
                RemoveSpriteFrame(0);
    }

    // --- What the meta builds from the frames (edited in the file) ---
    if (ImGui::CollapsingHeader(_("Parts, animations, families")))
    {
        ImGui::TextWrapped("%s", _("Edit these in the meta file; references are <item> or <object>.<item>."));
        const struct
        {
            const char *label;
            SpriteKind kind;
        } groups[]
            = {{"parts", SpriteKind::Part},
               {"anims", SpriteKind::Anim},
               {"bodies", SpriteKind::Body},
               {"sequences", SpriteKind::Sequence},
               {"buildings", SpriteKind::Building}};
        for (const auto &g : groups)
        {
            const std::vector<std::string> names = Sprites().ItemNames(spriteObject, g.kind);
            if (names.empty())
                continue;
            if (ImGui::TreeNode(g.label, "%s (%zu)", g.label, names.size()))
            {
                for (const std::string &n : names)
                    ImGui::BulletText("%s", n.c_str());
                ImGui::TreePop();
            }
        }
        const std::vector<std::string> fams = Sprites().FamilyNames(spriteObject);
        if (!fams.empty() && ImGui::TreeNode("families", "families (%zu)", fams.size()))
        {
            for (const std::string &n : fams)
            {
                const OpenArtOfWar::SpriteFamily *f = Sprites().FindFamily(spriteObject, n);
                ImGui::BulletText(
                    "%s: %d %s", n.c_str(), f ? f->count : 0, f ? OpenArtOfWar::SpriteKindName(f->kind) : "");
            }
            ImGui::TreePop();
        }
        ImGui::TextDisabled("%s", SpriteMetaPath(spriteObject).string().c_str());
    }
}

} // namespace CharEditor

namespace CharEditor
{

void CharacterEditor::ClearThumbnails()
{
    for (auto *cache : {&wreckThumbs, &animThumbs})
    {
        for (auto &[key, tex] : *cache)
            if (tex)
                SDL_DestroyTexture(tex);
        cache->clear();
    }
    for (auto &[key, tex] : zoomThumbs)
        if (tex)
            SDL_DestroyTexture(tex);
    zoomThumbs.clear();
}

} // namespace CharEditor
