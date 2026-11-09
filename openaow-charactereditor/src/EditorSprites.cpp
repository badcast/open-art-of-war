// Sprite Tools tab: the per-object sprites of the game (assets/sprites/sprite_<name>.png + sprite_<name>_meta.yaml).
// One object at a time: its image with the frames on it; frames are moved, resized, added, sliced from a grid or
// found in the image, renamed and removed, and the meta is saved (only its frame lines change). A new object is made
// from a PNG. Parts, animations and families are written in the meta by hand; the tab lists them.
#include "CharacterEditor.hpp"
#include "SpriteMerge.hpp"

#include "game/Effects.hpp"
#include "game/Projectiles.hpp"
#include "graphics/SpriteRegistry.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numeric>
#include <cstring>
#include <map>
#include <fstream>
#include <limits>
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

// A number for YAML: always with a point (snprintf follows the locale: "15,29" breaks the file)
std::string Num(float v)
{
    if (std::fabs(v - std::round(v)) < 1e-4f)
        return std::to_string(static_cast<long long>(std::lround(v)));
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, 6);
    return std::string(buf, r.ptr);
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

// The pixels of an RGBA32 surface without row padding
std::vector<unsigned char> PackedPixels(const SDL_Surface *s)
{
    std::vector<unsigned char> out(static_cast<std::size_t>(s->w) * s->h * 4);
    for (int y = 0; y < s->h; ++y)
        std::memcpy(
            &out[static_cast<std::size_t>(y) * s->w * 4],
            static_cast<const unsigned char *>(s->pixels) + static_cast<std::size_t>(y) * s->pitch,
            static_cast<std::size_t>(s->w) * 4);
    return out;
}

SDL_Surface *SurfaceFromPixels(const std::vector<unsigned char> &px, int w, int h)
{
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!s)
        return nullptr;
    for (int y = 0; y < h; ++y)
        std::memcpy(
            static_cast<unsigned char *>(s->pixels) + static_cast<std::size_t>(y) * s->pitch,
            &px[static_cast<std::size_t>(y) * w * 4],
            static_cast<std::size_t>(w) * 4);
    return s;
}

void FreeSurfaces(std::map<std::string, SDL_Surface *> &m)
{
    for (auto &[name, s] : m)
        if (s)
            SDL_DestroySurface(s);
    m.clear();
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

void CharacterEditor::LoadSpriteObject(
    const std::string &object, const fs::path &metaFile, const std::string &imageSuffix)
{
    DiscardPending();
    spriteMetaBase.clear();
    spriteObject = object;
    spriteFrames.clear();
    spriteRemoved.clear();
    spriteFrameSel = -1;
    spriteMulti.clear();
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
        const fs::path metaPath = metaFile.empty() ? SpriteMetaPath(object) : metaFile;
        if (!metaFile.empty())
            spriteMetaBase = ReadFile(metaFile);
        const YAML::Node meta = YAML::LoadFile(metaPath.string());
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
    // A backup comes with its pictures: they are unsaved changes like any other
    if (!imageSuffix.empty())
    {
        for (const std::string &name : SpriteImageNames())
        {
            SDL_Surface *img = LoadRgba(SpriteDir() / (name + imageSuffix));
            if (!img)
            {
                DiscardPending();
                break;
            }
            spritePending[name] = img;
        }
    }
    const auto pendingMain = spritePending.find(spriteImageName);
    const int page = Sprites().ImageOf(object);
    if (pendingMain != spritePending.end())
    {
        spriteImageW = pendingMain->second->w;
        spriteImageH = pendingMain->second->h;
        RefreshPendingTexture();
    }
    else if (SDL_Texture *tex = textures->GetSpriteSheet(page))
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
        ApplyImageScaleToFrames(spriteImageW / declaredW, spriteImageH / declaredH, false);
        spriteReport = _("The image is ") + std::to_string(spriteImageW) + "x" + std::to_string(spriteImageH)
                       + _(", the frames were written for ") + std::to_string(std::lround(declaredW)) + "x"
                       + std::to_string(std::lround(declaredH)) + _(": scaled with it (Save writes them so)");
    }
    if (!spritePending.empty() && !imageSuffix.empty())
        spriteDirty = true;
}

void CharacterEditor::ApplyImageScaleToFrames(float kx, float ky, bool forceEven)
{
    const bool even = forceEven || std::fabs(kx - ky) < 1e-3f;
    if (even)
        spriteScale *= kx;
    for (SpriteFrameEdit &f : spriteFrames)
    {
        f.rect[0] = static_cast<int>(std::lround(f.rect[0] * kx));
        f.rect[1] = static_cast<int>(std::lround(f.rect[1] * ky));
        f.rect[2] = std::max(1, static_cast<int>(std::lround(f.rect[2] * kx)));
        f.rect[3] = std::max(1, static_cast<int>(std::lround(f.rect[3] * ky)));
        f.ppu[0] *= kx;
        f.ppu[1] *= forceEven ? kx : ky;
        if (!even)
            f.ownSize = true;
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

bool CharacterEditor::SaveSpriteObject(bool backup)
{
    if (spriteObject.empty())
        return false;
    const fs::path path = SpriteMetaPath(spriteObject);
    if (backup)
        BackupSpriteFiles();
    // The edited pictures first: when one cannot be written the meta stays as it was
    for (const auto &[name, surface] : spritePending)
        if (!SDL_SavePNG(surface, (SpriteDir() / name).string().c_str()))
        {
            spriteReport = _("Could not write ") + (SpriteDir() / name).string() + ": " + SDL_GetError();
            return false;
        }
    std::string text = spriteMetaBase.empty() ? ReadFile(path) : spriteMetaBase;
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

bool CharacterEditor::EnsurePending()
{
    if (!spritePending.empty())
        return true;
    for (const std::string &name : SpriteImageNames())
    {
        SDL_Surface *img = LoadRgba(SpriteDir() / name);
        if (!img || img->w != spriteImageW || img->h != spriteImageH)
        {
            if (img)
                SDL_DestroySurface(img);
            DiscardPending();
            spriteReport = _("Not a PNG image of the sprite's size: ") + (SpriteDir() / name).string();
            return false;
        }
        spritePending[name] = img;
    }
    RefreshPendingTexture();
    return !spritePending.empty();
}

void CharacterEditor::SetPending(const std::string &name, SDL_Surface *surface)
{
    const auto it = spritePending.find(name);
    if (it == spritePending.end())
        spritePending[name] = surface;
    else if (it->second != surface)
    {
        SDL_DestroySurface(it->second);
        it->second = surface;
    }
}

void CharacterEditor::DiscardPending()
{
    FreeSurfaces(spritePending);
    FreeSurfaces(spriteScaleBase);
    if (spritePendingTex)
        SDL_DestroyTexture(spritePendingTex);
    spritePendingTex = nullptr;
}

void CharacterEditor::RefreshPendingTexture()
{
    if (spritePendingTex)
        SDL_DestroyTexture(spritePendingTex);
    spritePendingTex = nullptr;
    const auto it = spritePending.find(spriteImageName);
    if (it == spritePending.end() || !renderer)
        return;
    // (the surface was blitted with blending off: the picture must still be drawn with its transparency)
    SDL_SetSurfaceBlendMode(it->second, SDL_BLENDMODE_BLEND);
    spritePendingTex = SDL_CreateTextureFromSurface(renderer, it->second);
    if (spritePendingTex)
    {
        SDL_SetTextureBlendMode(spritePendingTex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(spritePendingTex, SDL_SCALEMODE_NEAREST);
    }
}

SDL_Texture *CharacterEditor::SpriteCanvasTexture()
{
    return spritePendingTex ? spritePendingTex : textures->GetSpriteSheet(Sprites().ImageOf(spriteObject));
}

bool CharacterEditor::ResizeSpriteImage(int w, int h, bool smooth, bool uniform)
{
    if (spriteObject.empty() || spriteImageW <= 0)
        return false;
    w = std::clamp(w, 1, 16384);
    h = std::clamp(h, 1, 16384);
    if (w == spriteImageW && h == spriteImageH)
    {
        spriteReport = _("The image already has this size");
        return true;
    }
    if (!EnsurePending())
        return false;
    // Every resize starts from the pixels before the first one: going 512 -> 4096 -> 1024 does not blur twice
    if (spriteScaleBase.empty())
        for (const auto &[name, img] : spritePending)
            spriteScaleBase[name] = SDL_DuplicateSurface(img);
    std::map<std::string, SDL_Surface *> made;
    for (const auto &[name, base] : spriteScaleBase)
    {
        SDL_Surface *scaled = SDL_ScaleSurface(base, w, h, smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        if (!scaled)
        {
            spriteReport = std::string(_("Could not resize: ")) + SDL_GetError();
            FreeSurfaces(made);
            return false;
        }
        made[name] = scaled;
    }
    for (auto &[name, img] : made)
        SetPending(name, img);
    const int oldW = spriteImageW, oldH = spriteImageH;
    spriteImageW = w;
    spriteImageH = h;
    ApplyImageScaleToFrames(static_cast<float>(w) / oldW, static_cast<float>(h) / oldH, uniform);
    spriteDirty = true;
    RefreshPendingTexture();
    spriteCameraPending = SpriteCamera::Fit;
    spriteReport = _("Resized ") + std::to_string(oldW) + "x" + std::to_string(oldH) + " -> " + std::to_string(w) + "x"
                   + std::to_string(h) + _(": frames and scale follow, the drawn sizes are the same (not saved yet)");
    return true;
}

bool CharacterEditor::FitSpriteImage()
{
    namespace SM = SpriteMerge;
    if (spriteObject.empty() || spriteImageW <= 0 || !EnsurePending())
        return false;
    std::vector<std::array<int, 4>> rects;
    for (const SpriteFrameEdit &f : spriteFrames)
        rects.push_back({f.rect[0], f.rect[1], f.rect[2], f.rect[3]});
    // The pictures share one layout: the factor that is exact for all of them
    int k = 0;
    for (const auto &[name, img] : spritePending)
    {
        const std::vector<unsigned char> px = PackedPixels(img);
        k = std::gcd(k, SM::DetectIntegerUpscale(px.data(), img->w, img->h, rects));
    }
    k = std::max(1, k);
    const int oldW = spriteImageW, oldH = spriteImageH;
    if (k > 1)
    {
        std::map<std::string, SDL_Surface *> made;
        for (const auto &[name, img] : spritePending)
        {
            const std::vector<unsigned char> small = SM::DownscaleInteger(PackedPixels(img).data(), img->w, img->h, k);
            SDL_Surface *s = SurfaceFromPixels(small, img->w / k, img->h / k);
            if (!s)
            {
                FreeSurfaces(made);
                spriteReport = SDL_GetError();
                return false;
            }
            made[name] = s;
        }
        for (auto &[name, img] : made)
            SetPending(name, img);
        spriteImageW = oldW / k;
        spriteImageH = oldH / k;
        ApplyImageScaleToFrames(1.0f / k, 1.0f / k, true);
        FreeSurfaces(spriteScaleBase);
        spriteDirty = true;
        RefreshPendingTexture();
    }
    // Then the empty space goes: the frames packed tightly, when that makes the image smaller
    const bool packed = !spriteFrames.empty() && RearrangeSpriteImage(SpriteArrange::Compact, true);
    if (k == 1 && !packed)
    {
        spriteReport = _("Already the smallest size: no exact upscale, no empty space to cut");
        return true;
    }
    spriteCameraPending = SpriteCamera::Fit;
    spriteReport = _("Set to fit: ") + std::to_string(oldW) + "x" + std::to_string(oldH) + " -> "
                   + std::to_string(spriteImageW) + "x" + std::to_string(spriteImageH)
                   + (k > 1 ? _(" (exact x") + std::to_string(k) + _(" upscale undone)") : std::string())
                   + _(" (not saved yet)");
    return true;
}

std::vector<std::string> CharacterEditor::SpriteImageNames() const
{
    std::vector<std::string> images = {spriteImageName};
    try
    {
        const YAML::Node meta = YAML::LoadFile(SpriteMetaPath(spriteObject).string());
        if (meta["resistance_image"])
            images.push_back(meta["resistance_image"].as<std::string>());
    }
    catch (const std::exception &)
    {}
    return images;
}

void CharacterEditor::BackupSpriteFiles()
{
    std::error_code ec;
    std::vector<fs::path> files = {SpriteMetaPath(spriteObject)};
    for (const std::string &name : SpriteImageNames())
        files.push_back(SpriteDir() / name);
    for (const fs::path &f : files)
        if (fs::exists(f, ec))
            fs::copy_file(f, fs::path(f.string() + ".bak"), fs::copy_options::overwrite_existing, ec);
}

bool CharacterEditor::HasSpriteBackup() const
{
    std::error_code ec;
    return !spriteObject.empty() && fs::exists(fs::path(SpriteMetaPath(spriteObject).string() + ".bak"), ec);
}

bool CharacterEditor::RestoreSpriteBackup()
{
    if (!HasSpriteBackup())
        return false;
    // The backup becomes the unsaved state of the editor: Save writes it over the files, Revert drops it
    const std::string keep = spriteObject;
    LoadSpriteObject(keep, fs::path(SpriteMetaPath(keep).string() + ".bak"), ".bak");
    spriteDirty = true;
    spriteCameraPending = SpriteCamera::Fit;
    spriteReport = _("The backup (.bak) is loaded: Save writes it to the files, Revert drops it");
    return true;
}

bool CharacterEditor::RearrangeSpriteImage(SpriteArrange mode, bool onlyIfSmaller)
{
    if (spriteObject.empty() || spriteImageW <= 0 || spriteFrames.empty())
        return false;
    namespace SM = SpriteMerge;
    std::vector<std::array<int, 2>> sizes;
    for (const SpriteFrameEdit &f : spriteFrames)
        sizes.push_back({std::max(1, f.rect[2]), std::max(1, f.rect[3])});
    const int pad = std::clamp(spriteArrangePadding, 0, 64);
    std::vector<std::array<int, 2>> pos;
    switch (mode)
    {
        case SpriteArrange::Shuffle:
            pos = SM::ShufflePack(sizes, pad, static_cast<unsigned>(SDL_GetTicksNS() ^ (SDL_GetTicksNS() >> 17)));
            break;
        case SpriteArrange::Compact:
            pos = SM::ShelfPack(sizes, pad);
            break;
        case SpriteArrange::Grid:
            pos = SM::GridPack(sizes, pad);
            break;
        case SpriteArrange::Assemble:
        {
            // The composites of the meta (parts) with their frames at the game's offsets
            std::map<std::string, int> index;
            for (std::size_t i = 0; i < spriteFrames.size(); ++i)
            {
                index[spriteFrames[i].name] = static_cast<int>(i);
                if (!spriteFrames[i].original.empty())
                    index[spriteFrames[i].original] = static_cast<int>(i); // the meta still uses the old name
            }
            YAML::Node meta;
            try
            {
                meta = YAML::Load(spriteMetaBase.empty() ? ReadFile(SpriteMetaPath(spriteObject)) : spriteMetaBase);
            }
            catch (const std::exception &)
            {}
            pos = SM::AssemblePack(sizes, SM::AssemblyGroupsFromMeta(meta, spriteObject, index, spriteScale), pad);
            break;
        }
    }
    const std::array<int, 2> extent = SM::PackExtent(sizes, pos);
    if (onlyIfSmaller
        && static_cast<long long>(extent[0]) * extent[1] >= static_cast<long long>(spriteImageW) * spriteImageH)
        return false;
    if (!EnsurePending())
        return false;

    // Every image of the sprite (the faction variant has the same layout) is redrawn in memory
    std::map<std::string, SDL_Surface *> made;
    for (const auto &[name, src] : spritePending)
    {
        SDL_Surface *dst = SDL_CreateSurface(extent[0], extent[1], SDL_PIXELFORMAT_RGBA32);
        if (!dst)
        {
            FreeSurfaces(made);
            spriteReport = SDL_GetError();
            return false;
        }
        SDL_ClearSurface(dst, 0.0f, 0.0f, 0.0f, 0.0f);
        SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
        for (std::size_t i = 0; i < spriteFrames.size(); ++i)
        {
            const SDL_Rect from {
                spriteFrames[i].rect[0], spriteFrames[i].rect[1], spriteFrames[i].rect[2], spriteFrames[i].rect[3]};
            SDL_Rect to {pos[i][0], pos[i][1], from.w, from.h};
            SDL_BlitSurface(src, &from, dst, &to);
        }
        made[name] = dst;
    }
    for (auto &[name, img] : made)
        SetPending(name, img);
    for (std::size_t i = 0; i < spriteFrames.size(); ++i)
    {
        spriteFrames[i].rect[0] = pos[i][0];
        spriteFrames[i].rect[1] = pos[i][1];
    }
    spriteImageW = extent[0];
    spriteImageH = extent[1];
    FreeSurfaces(spriteScaleBase);
    spriteDirty = true;
    RefreshPendingTexture();
    spriteCameraPending = SpriteCamera::Fit;
    spriteReport = _("Frames rearranged, the image is now ") + std::to_string(extent[0]) + "x"
                   + std::to_string(extent[1]) + _(" (not saved yet; pixels outside the frames are dropped)");
    return true;
}

std::vector<int> CharacterEditor::SpriteSelection() const
{
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(spriteFrames.size()); ++i)
        if (i == spriteFrameSel || (i < static_cast<int>(spriteMulti.size()) && spriteMulti[i]))
            out.push_back(i);
    return out;
}

std::string CharacterEditor::SpriteGroupMoveBlocker(const std::vector<int> &frames, int dx, int dy) const
{
    if (frames.empty())
        return _("No frame");
    std::vector<char> moving(spriteFrames.size(), 0);
    for (int i : frames)
    {
        if (i < 0 || i >= static_cast<int>(spriteFrames.size()))
            return _("No frame");
        moving[i] = 1;
    }
    auto crosses = [](const int *a, int ax, int ay, const int *o) {
        return ax < o[0] + o[2] && o[0] < ax + a[2] && ay < o[1] + o[3] && o[1] < ay + a[3];
    };
    for (int i : frames)
    {
        const SpriteFrameEdit &f = spriteFrames[i];
        const int x = f.rect[0] + dx, y = f.rect[1] + dy;
        if (x < 0 || y < 0 || x + f.rect[2] > spriteImageW || y + f.rect[3] > spriteImageH)
            return frames.size() > 1 ? _("The frames would leave the image") : _("The frame would leave the image");
        for (int j = 0; j < static_cast<int>(spriteFrames.size()); ++j)
        {
            if (moving[j])
                continue; // the moved ones keep their places among each other
            if (crosses(f.rect, f.rect[0], f.rect[1], spriteFrames[j].rect))
                return f.name + _(" overlaps ") + spriteFrames[j].name + _(" already: separate them first");
            if (crosses(f.rect, x, y, spriteFrames[j].rect))
                return f.name + _(" would overlap ") + spriteFrames[j].name;
        }
    }
    return "";
}

std::string CharacterEditor::SpriteMoveBlocker(int index, int x, int y) const
{
    if (index < 0 || index >= static_cast<int>(spriteFrames.size()))
        return _("No frame");
    return SpriteGroupMoveBlocker({index}, x - spriteFrames[index].rect[0], y - spriteFrames[index].rect[1]);
}

bool CharacterEditor::MoveSpriteFramePixels(int index, int x, int y)
{
    if (index < 0 || index >= static_cast<int>(spriteFrames.size()))
        return false;
    return MoveSpriteFramesPixels({index}, x - spriteFrames[index].rect[0], y - spriteFrames[index].rect[1]);
}

bool CharacterEditor::MoveSpriteFramesPixels(const std::vector<int> &frames, int dx, int dy)
{
    const std::string why = SpriteGroupMoveBlocker(frames, dx, dy);
    if (!why.empty())
    {
        spriteReport = _("Not moved: ") + why;
        return false;
    }
    if (dx == 0 && dy == 0)
        return true;
    if (!EnsurePending())
        return false;
    // All the pieces are cut first, then their old places cleared, then they go to the new ones (the new places may
    // cross the old ones, the group's own frames may share pixels); all in memory
    for (auto &[name, img] : spritePending)
    {
        SDL_SetSurfaceBlendMode(img, SDL_BLENDMODE_NONE);
        std::vector<SDL_Surface *> pieces;
        for (int i : frames)
        {
            const SpriteFrameEdit &f = spriteFrames[i];
            const SDL_Rect from {f.rect[0], f.rect[1], f.rect[2], f.rect[3]};
            SDL_Surface *piece = SDL_CreateSurface(from.w, from.h, SDL_PIXELFORMAT_RGBA32);
            if (!piece)
            {
                for (SDL_Surface *p : pieces)
                    SDL_DestroySurface(p);
                spriteReport = SDL_GetError();
                return false;
            }
            SDL_SetSurfaceBlendMode(piece, SDL_BLENDMODE_NONE);
            SDL_BlitSurface(img, &from, piece, nullptr);
            pieces.push_back(piece);
        }
        for (int i : frames)
        {
            const SpriteFrameEdit &f = spriteFrames[i];
            const SDL_Rect from {f.rect[0], f.rect[1], f.rect[2], f.rect[3]};
            SDL_FillSurfaceRect(img, &from, 0);
        }
        for (std::size_t k = 0; k < frames.size(); ++k)
        {
            const SpriteFrameEdit &f = spriteFrames[frames[k]];
            SDL_Rect to {f.rect[0] + dx, f.rect[1] + dy, f.rect[2], f.rect[3]};
            SDL_BlitSurface(pieces[k], nullptr, img, &to);
            SDL_DestroySurface(pieces[k]);
        }
    }
    for (int i : frames)
    {
        spriteFrames[i].rect[0] += dx;
        spriteFrames[i].rect[1] += dy;
    }
    FreeSurfaces(spriteScaleBase);
    spriteDirty = true;
    RefreshPendingTexture();
    spriteReport = (frames.size() == 1 ? _("Moved ") + spriteFrames[frames[0]].name
                                       : _("Moved ") + std::to_string(frames.size()) + _(" frames"))
                   + _(" with their pixels (not saved yet)");
    return true;
}

void CharacterEditor::ReloadSpriteData()
{
    const std::string selectedAnimation = SpriteRefName(SpriteKind::Anim, fxAnim);
    textures->ReloadSprites();
    // The configs keep sprite handles: read them again against the new tables
    OpenArtOfWar::EffectsConfig::reload();
    OpenArtOfWar::Projectiles::Reload();
    // Re-resolve unsaved editor drafts as well as disk defaults.
    EffectsChanged();
    ProjectilesChanged();
    fxAnim = selectedAnimation.empty()
                 ? -1
                 : SpriteHandle(selectedAnimation, SpriteKind::Anim, "editor animation selection");
    // Live previews contain handles into the old registry.
    fxEvents.clear();
    projShots.clear();
    projSmoke.clear();
    projCraters.clear();
    simProjectiles.clear();
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
    ReloadSpriteData();
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
    if (index < static_cast<int>(spriteMulti.size()))
        spriteMulti.erase(spriteMulti.begin() + index);
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
    SDL_Texture *tex = SpriteCanvasTexture();
    const ImVec2 i0 = toScreen(0, 0), i1 = toScreen(static_cast<float>(spriteImageW), static_cast<float>(spriteImageH));
    dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    dl->AddRectFilled(i0, i1, IM_COL32(60, 60, 64, 255));
    if (tex)
        dl->AddImage(reinterpret_cast<ImTextureID>(tex), i0, i1);
    dl->AddRect(i0, i1, IM_COL32(120, 120, 130, 255));

    // The selection: the main frame (its properties, its resize handles) and the frames picked along with it
    spriteMulti.resize(spriteFrames.size(), 0);
    auto isSelected = [&](int i) {
        return i == spriteFrameSel || (i >= 0 && i < static_cast<int>(spriteMulti.size()) && spriteMulti[i]);
    };
    auto clearSelection = [&]() {
        std::fill(spriteMulti.begin(), spriteMulti.end(), 0);
        spriteFrameSel = -1;
    };

    // Frames; the selected ones are orange, the main one has corner handles
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
        const bool main = i == spriteFrameSel, sel = isSelected(i);
        if (sel && !main)
            dl->AddRectFilled(a, b, IM_COL32(255, 180, 60, 40));
        dl->AddRect(
            a,
            b,
            sel ? IM_COL32(255, 180, 60, 255) : IM_COL32(90, 200, 160, 200),
            0.0f,
            0,
            main  ? 2.0f
            : sel ? 1.5f
                  : 1.0f);
        if (main || (inside && hovered))
            dl->AddText(
                ImVec2(a.x + 2, a.y - ImGui::GetTextLineHeight() - 1), IM_COL32(255, 230, 180, 255), f.name.c_str());
        if (main)
            for (int c = 0; c < 4; ++c)
            {
                const ImVec2 p((c & 1) ? b.x : a.x, (c & 2) ? b.y : a.y);
                dl->AddRectFilled(
                    ImVec2(p.x - handle, p.y - handle),
                    ImVec2(p.x + handle, p.y + handle),
                    IM_COL32(255, 180, 60, 255));
            }
    }

    // Magnetism: the image borders and the edges of the other frames attract what is being dragged (Alt: off)
    const bool magnet = spriteMagnet && !io.KeyAlt;
    const int snapThreshold = std::max(1, static_cast<int>(std::lround(8.0f / spriteZoom)));
    constexpr int kNoGuide = std::numeric_limits<int>::min();
    constexpr int kSkipSelected = -2;         // the targets leave out the whole selection (it is what moves)
    int guideX = kNoGuide, guideY = kNoGuide; // the lines the dragged edge stuck to
    auto targetsFor = [&](bool xAxis, int skip) {
        std::vector<int> t = {0, xAxis ? spriteImageW : spriteImageH};
        for (int i = 0; i < static_cast<int>(spriteFrames.size()); ++i)
        {
            if (i == skip || (skip == kSkipSelected && isSelected(i)))
                continue;
            const int *r = spriteFrames[i].rect;
            t.push_back(r[xAxis ? 0 : 1]);
            t.push_back(r[xAxis ? 0 : 1] + r[xAxis ? 2 : 3]);
        }
        return t;
    };
    // Moves the span [a0, a1] of an axis onto a near target, and remembers the line for the guide
    auto snapSpan = [&](bool xAxis, int skip, int a0, int a1) {
        if (!magnet)
            return 0;
        int met = kNoGuide;
        const int d = SpriteMerge::SnapDelta(a0, a1, targetsFor(xAxis, skip), snapThreshold, &met);
        if (met != kNoGuide)
            (xAxis ? guideX : guideY) = met;
        return d;
    };
    // The box being drawn as grid lines {x0, y0, x1, y1}: with magnetism both ends are grid points that stick
    auto drawBox = [&](int &x0, int &y0, int &x1, int &y1) {
        if (magnet)
        {
            const int ox = static_cast<int>(std::lround(mx)), oy = static_cast<int>(std::lround(my));
            const int ex = ox + snapSpan(true, -1, ox, ox), ey = oy + snapSpan(false, -1, oy, oy);
            x0 = std::min(spriteDragStartV[0], ex);
            x1 = std::max(spriteDragStartV[0], ex);
            y0 = std::min(spriteDragStartV[1], ey);
            y1 = std::max(spriteDragStartV[1], ey);
        }
        else
        {
            x0 = static_cast<int>(std::min(spriteDragStart.x, std::floor(mx)));
            y0 = static_cast<int>(std::min(spriteDragStart.y, std::floor(my)));
            x1 = static_cast<int>(std::max(spriteDragStart.x, std::floor(mx))) + 1;
            y1 = static_cast<int>(std::max(spriteDragStart.y, std::floor(my))) + 1;
        }
    };
    // The group being dragged: the offset of the press, stuck to a target by the group's bounding box
    auto groupOffset = [&](int dx, int dy, int &ox, int &oy) {
        int bx0 = std::numeric_limits<int>::max(), by0 = bx0, bx1 = std::numeric_limits<int>::min(), by1 = bx1;
        for (const auto &g : spriteGroupStart)
        {
            bx0 = std::min(bx0, g[1]);
            by0 = std::min(by0, g[2]);
            bx1 = std::max(bx1, g[1] + g[3]);
            by1 = std::max(by1, g[2] + g[4]);
        }
        ox = dx + snapSpan(true, kSkipSelected, bx0 + dx, bx1 + dx);
        oy = dy + snapSpan(false, kSkipSelected, by0 + dy, by1 + dy);
    };
    auto rememberGroup = [&]() {
        spriteGroupStart.clear();
        for (int i : SpriteSelection())
        {
            const int *r = spriteFrames[i].rect;
            spriteGroupStart.push_back({i, r[0], r[1], r[2], r[3]});
        }
    };

    // Left button: draw a new frame (draw mode), drag a corner of the main frame, move the selection, pick frames
    // (Ctrl: one in / out, Shift: one more), or a box on empty ground
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        spriteDrag = SpriteDrag::None;
        spriteDragStart = ImVec2(std::floor(mx), std::floor(my));
        if (spriteDrawMode)
        {
            spriteDrag = SpriteDrag::Draw;
            const int sx = static_cast<int>(std::lround(mx)), sy = static_cast<int>(std::lround(my));
            spriteDragStartV[0] = sx + snapSpan(true, -1, sx, sx);
            spriteDragStartV[1] = sy + snapSpan(false, -1, sy, sy);
            guideX = guideY = kNoGuide;
        }
        else
        {
            if (!spriteMoveMode && spriteFrameSel >= 0 && !io.KeyCtrl && !io.KeyShift)
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
            if (spriteDrag == SpriteDrag::None)
            {
                const int h = hoveredFrame;
                if (h >= 0 && io.KeyCtrl)
                {
                    // Ctrl: this frame in or out of the selection (no drag)
                    if (isSelected(h))
                    {
                        spriteMulti[h] = 0;
                        if (spriteFrameSel == h)
                        {
                            spriteFrameSel = -1;
                            for (int i = 0; i < static_cast<int>(spriteMulti.size()) && spriteFrameSel < 0; ++i)
                                if (spriteMulti[i])
                                    spriteFrameSel = i;
                        }
                    }
                    else
                    {
                        if (spriteFrameSel >= 0)
                            spriteMulti[spriteFrameSel] = 1;
                        spriteMulti[h] = 1;
                        spriteFrameSel = h;
                    }
                }
                else if (h >= 0)
                {
                    if (io.KeyShift)
                    {
                        if (spriteFrameSel >= 0)
                            spriteMulti[spriteFrameSel] = 1;
                        spriteMulti[h] = 1;
                    }
                    else if (!isSelected(h))
                    {
                        clearSelection();
                    }
                    spriteFrameSel = h; // the grabbed frame becomes the main one, the group moves along
                    rememberGroup();
                    spriteGroupMove[0] = spriteGroupMove[1] = 0;
                    spriteDrag = spriteMoveMode ? SpriteDrag::MovePixels : SpriteDrag::Move;
                }
                else
                {
                    // Empty ground: a box picks frames (with Ctrl / Shift it adds to the selection)
                    if (!io.KeyCtrl && !io.KeyShift)
                        clearSelection();
                    spriteDrag = SpriteDrag::BoxSelect;
                }
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
            int x0, y0, x1, y1;
            drawBox(x0, y0, x1, y1);
            dl->AddRect(
                toScreen(static_cast<float>(x0), static_cast<float>(y0)),
                toScreen(static_cast<float>(x1), static_cast<float>(y1)),
                IM_COL32(255, 255, 255, 255),
                0.0f,
                0,
                1.5f);
        }
        else if (spriteDrag == SpriteDrag::BoxSelect)
        {
            const ImVec2 a
                = toScreen(std::min(spriteDragStart.x, std::floor(mx)), std::min(spriteDragStart.y, std::floor(my)));
            const ImVec2 b = toScreen(
                std::max(spriteDragStart.x, std::floor(mx)) + 1, std::max(spriteDragStart.y, std::floor(my)) + 1);
            dl->AddRectFilled(a, b, IM_COL32(80, 160, 255, 40));
            dl->AddRect(a, b, IM_COL32(80, 160, 255, 220), 0.0f, 0, 1.0f);
        }
        else if (spriteDrag == SpriteDrag::MovePixels)
        {
            // Ghosts of the frames at the new places: green when they fit, red when it is not allowed
            int ox = 0, oy = 0;
            groupOffset(dx, dy, ox, oy);
            spriteGroupMove[0] = ox;
            spriteGroupMove[1] = oy;
            std::vector<int> moving;
            for (const auto &g : spriteGroupStart)
                moving.push_back(g[0]);
            const bool ok = SpriteGroupMoveBlocker(moving, ox, oy).empty();
            for (const auto &g : spriteGroupStart)
            {
                const ImVec2 a = toScreen(static_cast<float>(g[1] + ox), static_cast<float>(g[2] + oy));
                const ImVec2 b = toScreen(static_cast<float>(g[1] + ox + g[3]), static_cast<float>(g[2] + oy + g[4]));
                if (tex && spriteImageW > 0 && spriteImageH > 0)
                    dl->AddImage(
                        reinterpret_cast<ImTextureID>(tex),
                        a,
                        b,
                        ImVec2(static_cast<float>(g[1]) / spriteImageW, static_cast<float>(g[2]) / spriteImageH),
                        ImVec2(
                            static_cast<float>(g[1] + g[3]) / spriteImageW,
                            static_cast<float>(g[2] + g[4]) / spriteImageH),
                        IM_COL32(255, 255, 255, 170));
                dl->AddRectFilled(a, b, ok ? IM_COL32(60, 220, 120, 50) : IM_COL32(255, 60, 60, 90));
                dl->AddRect(a, b, ok ? IM_COL32(60, 255, 140, 255) : IM_COL32(255, 70, 70, 255), 0.0f, 0, 2.0f);
            }
        }
        else if (spriteDrag == SpriteDrag::Move && (dx || dy))
        {
            int ox = 0, oy = 0;
            groupOffset(dx, dy, ox, oy);
            for (const auto &g : spriteGroupStart)
                if (g[0] < static_cast<int>(spriteFrames.size()))
                    SetSpriteFrameRect(spriteFrames[g[0]], g[1] + ox, g[2] + oy, g[3], g[4]);
        }
        else if (spriteDrag == SpriteDrag::Resize && spriteFrameSel >= 0 && (dx || dy))
        {
            SpriteFrameEdit &f = spriteFrames[spriteFrameSel];
            int x0 = spriteDragRect[0], y0 = spriteDragRect[1];
            int x1 = x0 + spriteDragRect[2], y1 = y0 + spriteDragRect[3];
            int &vx = spriteDragCorner & 1 ? x1 : x0;
            int &vy = spriteDragCorner & 2 ? y1 : y0;
            vx += dx;
            vy += dy;
            vx += snapSpan(true, spriteFrameSel, vx, vx);
            vy += snapSpan(false, spriteFrameSel, vy, vy);
            SetSpriteFrameRect(
                f, std::min(x0, x1), std::min(y0, y1), std::max(1, std::abs(x1 - x0)), std::max(1, std::abs(y1 - y0)));
        }
    }
    // The lines a dragged edge stuck to
    if (guideX != kNoGuide)
        dl->AddLine(
            toScreen(static_cast<float>(guideX), 0.0f),
            toScreen(static_cast<float>(guideX), static_cast<float>(spriteImageH)),
            IM_COL32(80, 220, 255, 220),
            1.0f);
    if (guideY != kNoGuide)
        dl->AddLine(
            toScreen(0.0f, static_cast<float>(guideY)),
            toScreen(static_cast<float>(spriteImageW), static_cast<float>(guideY)),
            IM_COL32(80, 220, 255, 220),
            1.0f);
    if (spriteDrag != SpriteDrag::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        if (spriteDrag == SpriteDrag::Draw)
        {
            int x0, y0, x1, y1;
            drawBox(x0, y0, x1, y1);
            if (x1 - x0 > 1 && y1 - y0 > 1)
            {
                AddSpriteFrame(x0, y0, x1 - x0, y1 - y0, "f");
                spriteMulti.assign(spriteFrames.size(), 0);
            }
        }
        if (spriteDrag == SpriteDrag::BoxSelect)
        {
            const int x0 = static_cast<int>(std::min(spriteDragStart.x, std::floor(mx)));
            const int y0 = static_cast<int>(std::min(spriteDragStart.y, std::floor(my)));
            const int x1 = static_cast<int>(std::max(spriteDragStart.x, std::floor(mx))) + 1;
            const int y1 = static_cast<int>(std::max(spriteDragStart.y, std::floor(my))) + 1;
            if (x1 - x0 > 1 || y1 - y0 > 1)
                for (int i = 0; i < static_cast<int>(spriteFrames.size()); ++i)
                {
                    const int *r = spriteFrames[i].rect;
                    if (r[0] < x1 && x0 < r[0] + r[2] && r[1] < y1 && y0 < r[1] + r[3])
                    {
                        spriteMulti[i] = 1;
                        if (spriteFrameSel < 0)
                            spriteFrameSel = i;
                    }
                }
        }
        if (spriteDrag == SpriteDrag::MovePixels)
        {
            std::vector<int> moving;
            for (const auto &g : spriteGroupStart)
                moving.push_back(g[0]);
            MoveSpriteFramesPixels(moving, spriteGroupMove[0], spriteGroupMove[1]);
        }
        spriteDrag = SpriteDrag::None;
    }

    // Keys: arrows move the selection by a pixel (Shift: 10), with Ctrl they resize the main frame; Delete removes the
    // selection, Ctrl+A selects every frame, Esc none
    if (ImGui::IsWindowFocused() && !io.WantTextInput)
    {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
        {
            spriteMulti.assign(spriteFrames.size(), 1);
            if (spriteFrameSel < 0 && !spriteFrames.empty())
                spriteFrameSel = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            clearSelection();
    }
    if (spriteFrameSel >= 0 && ImGui::IsWindowFocused() && !io.WantTextInput)
    {
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
            {
                SpriteFrameEdit &f = spriteFrames[spriteFrameSel];
                SetSpriteFrameRect(
                    f, f.rect[0], f.rect[1], std::max(1, f.rect[2] + d[0]), std::max(1, f.rect[3] + d[1]));
            }
            else
                for (int i : SpriteSelection())
                {
                    SpriteFrameEdit &f = spriteFrames[i];
                    SetSpriteFrameRect(f, f.rect[0] + d[0], f.rect[1] + d[1], f.rect[2], f.rect[3]);
                }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            const std::vector<int> gone = SpriteSelection();
            for (auto it = gone.rbegin(); it != gone.rend(); ++it)
                RemoveSpriteFrame(*it);
            clearSelection();
        }
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

// The panel: Save / Revert / Reload stay at its top, always in sight and full size; the rest scrolls under them
void CharacterEditor::DrawSpriteProperties()
{
    if (!spriteObject.empty())
    {
        const float w = ImGui::GetContentRegionAvail().x;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float saveW = std::max(80.0f, (w - 2.0f * gap) * 0.4f);
        const float otherW = std::max(40.0f, (w - 2.0f * gap - saveW) * 0.5f);
        if (spriteDirty)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.12f, 1.0f));
        const bool save = ImGui::Button(_("Save##sprite"), ImVec2(saveW, 30.0f));
        if (spriteDirty)
            ImGui::PopStyleColor();
        FieldHint("Save sprite");
        if (save)
            SaveSpriteObject();
        ImGui::SameLine();
        if (ImGui::Button(_("Revert##sprite"), ImVec2(otherW, 30.0f)))
            LoadSpriteObject(spriteObject);
        ImGui::SameLine();
        if (Hinted(ImGui::Button(_("Reload images##sprite"), ImVec2(otherW, 30.0f)), "Reload images"))
            ReloadSpriteData();
        if (spriteDirty)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f), "%s", _("Unsaved changes"));
        ImGui::Separator();
    }
    ImGui::BeginChild("##sprite-props-body", ImVec2(0.0f, 0.0f), false);
    DrawSpritePropertiesBody();
    ImGui::EndChild();
}

void CharacterEditor::DrawSpritePropertiesBody()
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

    if (ImGui::Checkbox(_("Draw new frames (drag on the image)"), &spriteDrawMode) && spriteDrawMode)
        spriteMoveMode = false;
    if (ImGui::Checkbox(_("Sprite change mode (drag a frame with its pixels)"), &spriteMoveMode) && spriteMoveMode)
        spriteDrawMode = false;
    ImGui::SetItemTooltip(
        "%s", _("Drag a frame to another place of the image; it is refused (red) when it would cover another frame."));

    ImGui::Checkbox(_("Magnetism (stick to edges and corners)"), &spriteMagnet);
    ImGui::SetItemTooltip(
        "%s",
        _("A frame being drawn, moved or resized sticks to the image borders and to the edges and corners of the "
          "other frames. Hold Alt to move freely."));

    // --- The image size: the frames follow, the drawn sizes stay; nothing is written before Save ---
    {
        ImGui::Separator();
        ImGui::Text(_("Image size: %d x %d px"), spriteImageW, spriteImageH);
        static const int kPresets[] = {512, 1024, 2048, 4096};
        const std::array<int, 2> target
            = SpriteMerge::FitToLongestSide(spriteImageW, spriteImageH, kPresets[std::clamp(spriteSizePreset, 0, 3)]);
        const std::string names[4]
            = {"512 (" + std::to_string(SpriteMerge::FitToLongestSide(spriteImageW, spriteImageH, 512)[0]) + "x"
                   + std::to_string(SpriteMerge::FitToLongestSide(spriteImageW, spriteImageH, 512)[1]) + ")",
               "1024 (" + std::to_string(SpriteMerge::FitToLongestSide(spriteImageW, spriteImageH, 1024)[0]) + "x"
                   + std::to_string(SpriteMerge::FitToLongestSide(spriteImageW, spriteImageH, 1024)[1]) + ")",
               "2048 (" + std::to_string(SpriteMerge::FitToLongestSide(spriteImageW, spriteImageH, 2048)[0]) + "x"
                   + std::to_string(SpriteMerge::FitToLongestSide(spriteImageW, spriteImageH, 2048)[1]) + ")",
               "4096 (" + std::to_string(SpriteMerge::FitToLongestSide(spriteImageW, spriteImageH, 4096)[0]) + "x"
                   + std::to_string(SpriteMerge::FitToLongestSide(spriteImageW, spriteImageH, 4096)[1]) + ")"};
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo("##size-preset", names[std::clamp(spriteSizePreset, 0, 3)].c_str()))
        {
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable((names[i] + "##preset" + std::to_string(i)).c_str(), i == spriteSizePreset))
                    spriteSizePreset = i;
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", _("The longer side of the image; the other follows (the aspect ratio is kept)"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        const char *filters[] = {_("Pixels"), _("Smooth")};
        Hinted(ImGui::Combo("##scale-filter", &spriteScaleSmooth, filters, 2), "##scale-filter");
        ImGui::SameLine();
        if (Hinted(ImGui::Button(_("Apply##preset")), "Apply size"))
            ResizeSpriteImage(target[0], target[1], spriteScaleSmooth == 1, true);

        // Custom size: the numbers follow the image until they are edited
        static int syncedW = 0, syncedH = 0;
        if (syncedW != spriteImageW || syncedH != spriteImageH || spriteCustomW <= 0 || spriteCustomH <= 0)
        {
            syncedW = spriteCustomW = spriteImageW;
            syncedH = spriteCustomH = spriteImageH;
        }
        ImGui::TextDisabled("%s", _("Custom size"));
        ImGui::SetNextItemWidth(90.0f);
        const bool editedW = ImGui::InputInt("##custom-w", &spriteCustomW, 0);
        ImGui::SameLine();
        ImGui::TextUnformatted("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        const bool editedH = ImGui::InputInt("##custom-h", &spriteCustomH, 0);
        if (editedW || editedH)
        {
            const std::array<int, 2> c = SpriteMerge::CustomSize(
                spriteImageW, spriteImageH, spriteCustomW, spriteCustomH, spriteKeepAspect, editedW);
            spriteCustomW = c[0];
            spriteCustomH = c[1];
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(_("Keep aspect##custom"), &spriteKeepAspect) && spriteKeepAspect)
        {
            const std::array<int, 2> c
                = SpriteMerge::CustomSize(spriteImageW, spriteImageH, spriteCustomW, spriteCustomH, true, true);
            spriteCustomW = c[0];
            spriteCustomH = c[1];
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Apply##custom")))
            ResizeSpriteImage(spriteCustomW, spriteCustomH, spriteScaleSmooth == 1, spriteKeepAspect);
        if (Hinted(ImGui::Button(_("Set to fit")), "Set to fit"))
            FitSpriteImage();
        ImGui::SetItemTooltip(
            "%s",
            _("The smallest image without losing quality: an exact integer upscale is undone and the empty space "
              "around the frames is cut away."));
    }

    // --- Rearranging the frames in the image (a backup of the files is kept as .bak) ---
    {
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt(_("Gap"), &spriteArrangePadding, 0);
        spriteArrangePadding = std::clamp(spriteArrangePadding, 0, 64);
        const struct
        {
            const char *label;
            const char *tip;
            SpriteArrange mode;
        } buttons[] = {
            {_("Shuffle"),
             _("Lay the frames out in another random order (press again for another try)"),
             SpriteArrange::Shuffle},
            {_("Compact"), _("Pack the frames tightly: the smallest image"), SpriteArrange::Compact},
            {_("Beautify"),
             _("A tidy grid of equal cells, frames in their order, close to a square"),
             SpriteArrange::Grid},
            {_("Assemble"),
             _("Lay the frames out as the game puts them together: every composite (parts) as one block with its "
               "frames at their offsets, the others packed around. Composites whose layers overlap become a row."),
             SpriteArrange::Assemble},
        };
        for (const auto &b : buttons)
        {
            ImGui::SameLine();
            if (ImGui::Button(b.label))
                RearrangeSpriteImage(b.mode);
            ImGui::SetItemTooltip("%s", b.tip);
        }
        ImGui::BeginDisabled(!HasSpriteBackup());
        if (ImGui::Button(_("Restore backup")))
            RestoreSpriteBackup();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", _("Put back the files saved before the last Save / rearrangement (.bak)"));
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
                SDL_Texture *tex = SpriteCanvasTexture();
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
