// Sprite Merge tab: several sprite objects become one (their frames rearranged freely into one image, every item and
// family moved into one meta, the references in the assets rewritten, the sources removed), or frames of any objects
// are laid out freely and baked into one picture, a new object with one frame.
#include "CharacterEditor.hpp"
#include "SpriteMerge.hpp"

#include "graphics/SpriteRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>

#ifndef OPENAOW_SOURCE_DIR
#define OPENAOW_SOURCE_DIR ""
#endif

namespace CharEditor
{

namespace
{

using OpenArtOfWar::Sprites;
namespace SM = SpriteMerge;

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

SDL_Surface *LoadRgba(const fs::path &png)
{
    SDL_Surface *loaded = SDL_LoadPNG(png.string().c_str());
    if (!loaded)
        return nullptr;
    SDL_Surface *rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    return rgba;
}

// Copies (over = false) or draws over with alpha (over = true) a rectangle of `src` to (dx, dy) of `dst`; both RGBA32
void Put(const SDL_Surface *src, const int r[4], SDL_Surface *dst, int dx, int dy, bool over)
{
    for (int y = 0; y < r[3]; ++y)
    {
        const int sy = r[1] + y, ty = dy + y;
        if (sy < 0 || sy >= src->h || ty < 0 || ty >= dst->h)
            continue;
        const auto *srow = static_cast<const std::uint8_t *>(src->pixels) + sy * src->pitch;
        auto *trow = static_cast<std::uint8_t *>(dst->pixels) + ty * dst->pitch;
        for (int x = 0; x < r[2]; ++x)
        {
            const int sx = r[0] + x, tx = dx + x;
            if (sx < 0 || sx >= src->w || tx < 0 || tx >= dst->w)
                continue;
            const std::uint8_t *s = srow + sx * 4;
            std::uint8_t *t = trow + tx * 4;
            if (!over)
            {
                std::copy(s, s + 4, t);
                continue;
            }
            const float sa = s[3] / 255.0f, ta = t[3] / 255.0f;
            const float oa = sa + ta * (1.0f - sa);
            if (oa <= 0.0f)
                continue;
            for (int c = 0; c < 3; ++c)
                t[c] = static_cast<std::uint8_t>(std::lround((s[c] * sa + t[c] * ta * (1.0f - sa)) / oa));
            t[3] = static_cast<std::uint8_t>(std::lround(oa * 255.0f));
        }
    }
}

SDL_Surface *NewSurface(int w, int h)
{
    SDL_Surface *s = SDL_CreateSurface(std::max(1, w), std::max(1, h), SDL_PIXELFORMAT_RGBA32);
    if (s)
        SDL_ClearSurface(s, 0.0f, 0.0f, 0.0f, 0.0f);
    return s;
}

std::string MetaString(const YAML::Node &meta, const char *key, const std::string &def)
{
    try
    {
        if (meta[key] && meta[key].IsScalar())
            return meta[key].as<std::string>();
    }
    catch (...)
    {}
    return def;
}

// Code that names `object` or one of its sprites in a string: such objects cannot be merged
std::string CodeReference(const std::string &object)
{
    const fs::path root = OPENAOW_SOURCE_DIR;
    if (root.empty() || !fs::exists(root))
        return {};
    // "<object>.<item>" or the bare "<object>"
    const std::string dotted = "\"" + object + ".", bare = "\"" + object + "\"";
    for (const char *sub : {"src", "include", "openaow-charactereditor/src"})
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root / sub, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string ext = it->path().extension().string();
            if (!it->is_regular_file() || (ext != ".cpp" && ext != ".hpp" && ext != ".h"))
                continue;
            const std::string text = ReadFile(it->path());
            if (text.find(dotted) != std::string::npos || text.find(bare) != std::string::npos)
                return it->path().lexically_relative(root).string();
        }
    }
    return {};
}

struct Box
{
    int x0, y0, x1, y1;
};

bool Intersects(const Box &a, const Box &b)
{
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

} // namespace

const std::vector<CharacterEditor::MergeFrameInfo> &CharacterEditor::MergeFramesOf(const std::string &object)
{
    auto it = mergeFrameCache.find(object);
    if (it != mergeFrameCache.end())
        return it->second;
    std::vector<MergeFrameInfo> &out = mergeFrameCache[object];
    try
    {
        YAML::Node meta = YAML::LoadFile(SpriteMetaPath(object).string());
        // The rectangles in the pixels of the image as it is (image_size may describe an older size)
        if (SDL_Texture *tex = textures->GetSpriteSheet(Sprites().ImageOf(object)))
        {
            float tw = 0.0f, th = 0.0f;
            SDL_GetTextureSize(tex, &tw, &th);
            SM::FitMetaToImage(meta, static_cast<int>(tw), static_cast<int>(th));
        }
        if (const YAML::Node frames = meta["frames"])
            for (const auto &kv : frames)
            {
                MergeFrameInfo f {kv.first.as<std::string>(), {0, 0, 1, 1}};
                const YAML::Node v = kv.second;
                const YAML::Node rect = v.IsMap() ? v["rect"] : v;
                for (int i = 0; i < 4 && rect && rect.IsSequence() && i < static_cast<int>(rect.size()); ++i)
                    f.rect[i] = static_cast<int>(std::lround(rect[i].as<float>()));
                f.rect[2] = std::max(1, f.rect[2]);
                f.rect[3] = std::max(1, f.rect[3]);
                out.push_back(f);
            }
    }
    catch (const std::exception &e)
    {
        mergeReport = SpriteMetaPath(object).filename().string() + ": " + e.what();
    }
    return out;
}

void CharacterEditor::MergeBounds(int &x0, int &y0, int &x1, int &y1) const
{
    x0 = y0 = x1 = y1 = 0;
    bool first = true;
    for (const MergePiece &p : mergePieces)
    {
        if (first)
        {
            x0 = p.x, y0 = p.y, x1 = p.x + p.src[2], y1 = p.y + p.src[3];
            first = false;
            continue;
        }
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x + p.src[2]);
        y1 = std::max(y1, p.y + p.src[3]);
    }
}

void CharacterEditor::MergeAddPieces(const std::string &object, const std::string &frame)
{
    // To the right of what is there; an object's frames keep their places relative to each other
    int x0, y0, x1, y1;
    MergeBounds(x0, y0, x1, y1);
    const bool empty = mergePieces.empty();
    const int ox = empty ? 0 : x1 + mergePadding + 8, oy = empty ? 0 : y0;
    int minX = 0, minY = 0;
    bool first = true;
    for (const MergeFrameInfo &f : MergeFramesOf(object))
        if (frame.empty() || f.name == frame)
        {
            minX = first ? f.rect[0] : std::min(minX, f.rect[0]);
            minY = first ? f.rect[1] : std::min(minY, f.rect[1]);
            first = false;
        }
    for (MergePiece &p : mergePieces)
        p.selected = false;
    for (const MergeFrameInfo &f : MergeFramesOf(object))
    {
        if (!frame.empty() && f.name != frame)
            continue;
        MergePiece p;
        p.object = object;
        p.frame = f.name;
        std::copy(f.rect, f.rect + 4, p.src);
        p.x = ox + f.rect[0] - minX;
        p.y = oy + f.rect[1] - minY;
        p.selected = true;
        mergePieces.push_back(p);
    }
}

void CharacterEditor::MergeRemovePieces(const std::string &object, const std::string &frame)
{
    mergePieces.erase(
        std::remove_if(
            mergePieces.begin(),
            mergePieces.end(),
            [&](const MergePiece &p) { return p.object == object && (frame.empty() || p.frame == frame); }),
        mergePieces.end());
}

std::vector<char> CharacterEditor::MergeOverlaps() const
{
    // Sweep along x: only pieces whose x ranges meet are compared
    const std::size_t n = mergePieces.size();
    std::vector<char> flags(n, 0);
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(
        order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return mergePieces[a].x < mergePieces[b].x; });
    auto box = [&](std::size_t i) {
        const MergePiece &p = mergePieces[i];
        return Box {p.x, p.y, p.x + p.src[2], p.y + p.src[3]};
    };
    for (std::size_t a = 0; a < n; ++a)
    {
        const Box ba = box(order[a]);
        for (std::size_t b = a + 1; b < n; ++b)
        {
            const Box bb = box(order[b]);
            if (bb.x0 >= ba.x1)
                break;
            if (Intersects(ba, bb))
                flags[order[a]] = flags[order[b]] = 1;
        }
    }
    return flags;
}

void CharacterEditor::MergePack()
{
    // The selected pieces, or all of them when none is selected
    std::vector<std::size_t> which;
    for (std::size_t i = 0; i < mergePieces.size(); ++i)
        if (mergePieces[i].selected)
            which.push_back(i);
    if (which.empty())
    {
        which.resize(mergePieces.size());
        std::iota(which.begin(), which.end(), 0);
    }
    std::vector<std::array<int, 2>> sizes;
    for (std::size_t i : which)
        sizes.push_back({mergePieces[i].src[2], mergePieces[i].src[3]});
    const auto pos = SM::ShelfPack(sizes, mergePadding);
    int baseX = 0, baseY = 0;
    if (which.size() != mergePieces.size())
    {
        // The packed group goes below the others
        int x0, y0, x1, y1;
        std::vector<MergePiece> rest;
        for (std::size_t i = 0; i < mergePieces.size(); ++i)
            if (!mergePieces[i].selected)
                rest.push_back(mergePieces[i]);
        std::swap(rest, mergePieces);
        MergeBounds(x0, y0, x1, y1);
        std::swap(rest, mergePieces);
        baseX = x0;
        baseY = y1 + mergePadding + 8;
    }
    for (std::size_t k = 0; k < which.size(); ++k)
    {
        mergePieces[which[k]].x = baseX + pos[k][0];
        mergePieces[which[k]].y = baseY + pos[k][1];
    }
    mergeCameraPending = SpriteCamera::Fit;
}

void CharacterEditor::MergeReset()
{
    mergeObjects.clear();
    mergePieces.clear();
    mergeFrameCache.clear();
    mergeDrag = MergeDrag::None;
    mergeConfirmText.clear();
}

bool CharacterEditor::MergePrepare()
{
    const std::string name = mergeName.data();
    mergeConfirmText.clear();
    if (!ValidName(name))
    {
        mergeReport = _("The name of the new sprite is letters, digits and '_'");
        return false;
    }
    if (mergePieces.empty())
    {
        mergeReport = _("Nothing on the canvas");
        return false;
    }
    int x0, y0, x1, y1;
    MergeBounds(x0, y0, x1, y1);
    std::ostringstream text;
    if (mergeBake)
    {
        if (fs::exists(SpriteMetaPath(name)))
        {
            mergeReport = _("This sprite exists already: ") + name;
            return false;
        }
        text << _("Bake ") << mergePieces.size() << _(" frame(s) into a new sprite:") << "\n  sprites/sprite_" << name
             << ".png  (" << (x1 - x0) << "x" << (y1 - y0) << ")\n  sprites/sprite_" << name << "_meta.yaml  ("
             << _("one frame f0") << ")\n"
             << _("The source sprites stay as they are.");
        mergeConfirmText = text.str();
        return true;
    }

    if (mergeObjects.size() < 2)
    {
        mergeReport = _("Check at least two sprites to merge");
        return false;
    }
    const bool isSource = std::find(mergeObjects.begin(), mergeObjects.end(), name) != mergeObjects.end();
    if (!isSource && fs::exists(SpriteMetaPath(name)))
    {
        mergeReport = _("This sprite exists already: ") + name;
        return false;
    }
    if (spriteDirty && std::find(mergeObjects.begin(), mergeObjects.end(), spriteObject) != mergeObjects.end())
    {
        mergeReport = _("Save or revert the changes of ") + spriteObject + _(" in Sprite Tools first");
        return false;
    }
    const std::vector<char> overlaps = MergeOverlaps();
    if (std::count(overlaps.begin(), overlaps.end(), 1))
    {
        mergeReport = _("Frames overlap (red): move them apart or press Pack");
        return false;
    }
    bool terrain = false;
    std::vector<std::string> bare;
    for (std::size_t i = 0; i < mergeObjects.size(); ++i)
    {
        const std::string &o = mergeObjects[i];
        YAML::Node meta;
        try
        {
            meta = YAML::LoadFile(SpriteMetaPath(o).string());
        }
        catch (const std::exception &e)
        {
            mergeReport = SpriteMetaPath(o).filename().string() + ": " + e.what();
            return false;
        }
        const bool t = meta["terrain"] && meta["terrain"].as<bool>(false);
        if (i && t != terrain)
        {
            mergeReport = _("Map tiles (terrain) and other sprites cannot be merged");
            return false;
        }
        terrain = t;
        const std::string code = CodeReference(o);
        if (!code.empty())
        {
            mergeReport = o + _(" is named in the game code (") + code + _("): it cannot be merged");
            return false;
        }
        std::error_code ec;
        for (fs::directory_iterator it(assetsRoot / "characters", ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".yaml" && SM::BindsBare(ReadFile(it->path()), o))
            {
                bare.push_back(o);
                break;
            }
    }
    if (bare.size() > 1)
    {
        mergeReport = _("Units use their sprite by name (") + bare[0] + ", " + bare[1]
                      + _("): at most one of the merged sprites may be a unit's");
        return false;
    }
    // The unit's sprite goes first: its names (the families the game looks up) stay
    if (!bare.empty())
    {
        mergeObjects.erase(std::find(mergeObjects.begin(), mergeObjects.end(), bare[0]));
        mergeObjects.insert(mergeObjects.begin(), bare[0]);
    }
    text << _("Merge into sprites/sprite_") << name << ".png (" << (x1 - x0) << "x" << (y1 - y0) << ", "
         << mergePieces.size() << _(" frames) and its meta:") << "\n";
    for (const std::string &o : mergeObjects)
        text << "  " << o << "\n";
    text << "\n"
         << _("The references to these sprites in assets (characters, effects, projectiles, research, other "
              "sprites) are rewritten to the new sprite, and the files of the sources are removed.\n"
              "Names that collide get the object's name in front (shown in the report).\n"
              "assets is a git repository: git checkout undoes it.");
    mergeConfirmText = text.str();
    return true;
}

bool CharacterEditor::BakeFramesSave()
{
    const std::string name = mergeName.data();
    int x0, y0, x1, y1;
    MergeBounds(x0, y0, x1, y1);
    SDL_Surface *out = NewSurface(x1 - x0, y1 - y0);
    if (!out)
        return false;
    std::map<std::string, SDL_Surface *> images;
    std::map<std::string, YAML::Node> metas;
    for (const MergePiece &p : mergePieces)
    {
        if (!metas.count(p.object))
        {
            try
            {
                metas[p.object] = YAML::LoadFile(SpriteMetaPath(p.object).string());
            }
            catch (...)
            {
                metas[p.object] = YAML::Node(YAML::NodeType::Map);
            }
            images[p.object]
                = LoadRgba(SpriteDir() / MetaString(metas[p.object], "image", "sprite_" + p.object + ".png"));
            if (images[p.object])
                SM::FitMetaToImage(metas[p.object], images[p.object]->w, images[p.object]->h);
        }
        if (SDL_Surface *s = images[p.object])
            Put(s, p.src, out, p.x - x0, p.y - y0, true);
    }
    for (auto &[o, s] : images)
        if (s)
            SDL_DestroySurface(s);
    const fs::path png = SpriteDir() / ("sprite_" + name + ".png");
    const bool saved = SDL_SavePNG(out, png.string().c_str());
    const int w = out->w, h = out->h;
    SDL_DestroySurface(out);
    if (!saved)
    {
        mergeReport = _("Could not write ") + png.string() + ": " + SDL_GetError();
        return false;
    }
    const YAML::Node &first = metas[mergePieces.front().object];
    std::string header = "# Sprite \"" + name + "\" (baked in the character editor from";
    for (std::size_t i = 0; i < mergePieces.size() && i < 8; ++i)
        header += " " + mergePieces[i].object + "." + mergePieces[i].frame;
    header += mergePieces.size() > 8 ? " ...)\n" : ")\n";
    std::string meta = header + "image: sprite_" + name + ".png\nimage_size: [" + std::to_string(w) + ", "
                       + std::to_string(h) + "]\nscale: " + MetaString(first, "scale", "1") + "\n";
    const std::string recolor = MetaString(first, "recolor", "none");
    if (recolor != "none")
        meta += "recolor: " + recolor + "\n";
    meta += "frames:\n  f0: {rect: [0, 0, " + std::to_string(w) + ", " + std::to_string(h) + "]}\n";
    if (!WriteFile(SpriteMetaPath(name), meta))
    {
        mergeReport = _("Could not write ") + SpriteMetaPath(name).string();
        return false;
    }
    spriteDirty = false;
    spriteObject = name;
    ReloadSpriteData();
    mergeFrameCache.clear();
    mergeReport
        = _("Baked ") + std::to_string(mergePieces.size()) + _(" frame(s) into ") + name + _(" (open in Sprite Tools)");
    return true;
}

bool CharacterEditor::MergeObjectsSave()
{
    const std::string name = mergeName.data();
    // The sources: metas and images (in the order of MergePrepare: a unit's sprite first)
    std::vector<SM::Source> sources;
    std::map<std::string, SDL_Surface *> images, resist;
    std::vector<fs::path> remove;
    bool anyResistance = false;
    auto cleanup = [&]() {
        for (auto *m : {&images, &resist})
            for (auto &[o, s] : *m)
                if (s)
                    SDL_DestroySurface(s);
    };
    for (const std::string &o : mergeObjects)
    {
        SM::Source src {o, YAML::LoadFile(SpriteMetaPath(o).string())};
        const std::string image = MetaString(src.meta, "image", "sprite_" + o + ".png");
        images[o] = LoadRgba(SpriteDir() / image);
        if (!images[o])
        {
            mergeReport = _("Not a PNG image: ") + (SpriteDir() / image).string();
            cleanup();
            return false;
        }
        SM::FitMetaToImage(src.meta, images[o]->w, images[o]->h);
        remove.push_back(SpriteDir() / image);
        remove.push_back(SpriteMetaPath(o));
        const std::string alt = MetaString(src.meta, "resistance_image", "");
        if (!alt.empty())
        {
            anyResistance = true;
            resist[o] = LoadRgba(SpriteDir() / alt);
            remove.push_back(SpriteDir() / alt);
        }
        sources.push_back(src);
    }

    // The images: exact pixels at the new places, the layout starting at 0, 0
    int x0, y0, x1, y1;
    MergeBounds(x0, y0, x1, y1);
    SDL_Surface *out = NewSurface(x1 - x0, y1 - y0);
    SDL_Surface *outRes = anyResistance ? NewSurface(x1 - x0, y1 - y0) : nullptr;
    SM::FramePlaces places;
    for (const MergePiece &p : mergePieces)
    {
        Put(images[p.object], p.src, out, p.x - x0, p.y - y0, false);
        if (outRes)
            Put(resist[p.object] ? resist[p.object] : images[p.object], p.src, outRes, p.x - x0, p.y - y0, false);
        places[{p.object, p.frame}] = {p.x - x0, p.y - y0};
    }
    cleanup();

    const std::string imageName = "sprite_" + name + ".png";
    const std::string resName = anyResistance ? "sprite_" + name + "_resistance.png" : "";
    const SM::NameMap names = SM::BuildNameMap(sources);
    const YAML::Node merged = SM::MergeMetas(sources, names, places, imageName, resName, x1 - x0, y1 - y0);
    std::string header = "# Sprite \"" + name + "\" (merged in the character editor from";
    for (const SM::Source &s : sources)
        header += " " + s.object;
    const std::string metaText = SM::EmitMeta(merged, header + ")\n");

    // References in the other asset files
    std::set<fs::path> skip;
    for (const std::string &o : mergeObjects)
        skip.insert(fs::weakly_canonical(SpriteMetaPath(o)));
    int files = 0, refs = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(assetsRoot, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end;
         it.increment(ec))
    {
        const fs::path &p = it->path();
        if (it->is_directory())
        {
            if (p.filename().string().rfind('.', 0) == 0)
                it.disable_recursion_pending();
            continue;
        }
        const std::string ext = p.extension().string();
        if (ext != ".yaml" && ext != ".yml" && ext != ".json")
            continue;
        if (skip.count(fs::weakly_canonical(p)))
            continue;
        const std::string text = ReadFile(p);
        bool mentions = false;
        for (const std::string &o : mergeObjects)
            mentions |= text.find(o) != std::string::npos;
        if (!mentions)
            continue;
        int count = 0;
        const std::string changed = SM::RewriteText(text, names, name, count);
        if (count && WriteFile(p, changed))
        {
            ++files;
            refs += count;
        }
    }

    // The sources go; the new files come
    const std::set<fs::path> keep = {SpriteDir() / imageName, SpriteMetaPath(name), SpriteDir() / resName};
    for (const fs::path &p : remove)
        if (!keep.count(p))
            fs::remove(p, ec);
    bool ok = SDL_SavePNG(out, (SpriteDir() / imageName).string().c_str());
    if (outRes)
        ok &= SDL_SavePNG(outRes, (SpriteDir() / resName).string().c_str());
    SDL_DestroySurface(out);
    if (outRes)
        SDL_DestroySurface(outRes);
    ok &= WriteFile(SpriteMetaPath(name), metaText);

    int renamed = 0;
    std::string example;
    for (const auto &[o, m] : names)
        for (const auto &[from, to] : m)
            if (from != to)
            {
                if (!renamed)
                    example = o + "." + from + " -> " + to;
                ++renamed;
            }
    spriteDirty = false;
    spriteObject = name;
    ReloadSpriteData();
    const std::string sources_ = std::to_string(sources.size());
    MergeReset();
    mergeReport = (ok ? _("Merged ") : _("Merged with write errors: ")) + sources_ + _(" sprites into ") + name + ", "
                  + std::to_string(refs) + _(" reference(s) in ") + std::to_string(files) + _(" file(s) rewritten");
    if (renamed)
        mergeReport += ", " + std::to_string(renamed) + _(" name(s) renamed (") + example + ")";
    const auto &errors = Sprites().Errors();
    if (!errors.empty())
        mergeReport += "\n" + std::to_string(errors.size()) + _(" sprite error(s); first: ") + errors.front();
    return ok;
}

void CharacterEditor::DrawSpriteMerge()
{
    // Left: the objects (merge) or the objects with their frames (bake)
    ImGui::BeginChild("merge-objects", ImVec2(260.0f, 0.0f), true);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", _("Search"), mergeSearch.data(), mergeSearch.size());
    const std::string filter = mergeSearch.data();
    std::set<std::pair<std::string, std::string>> placed;
    if (mergeBake)
        for (const MergePiece &p : mergePieces)
            placed.insert({p.object, p.frame});
    for (const std::string &name : Sprites().ObjectNames())
    {
        if (!filter.empty() && name.find(filter) == std::string::npos)
            continue;
        if (!mergeBake)
        {
            const auto it = std::find(mergeObjects.begin(), mergeObjects.end(), name);
            bool on = it != mergeObjects.end();
            if (ImGui::Checkbox(name.c_str(), &on))
            {
                if (on)
                {
                    mergeObjects.push_back(name);
                    MergeAddPieces(name, {});
                    if (mergeName[0] == '\0')
                        std::strncpy(mergeName.data(), (name + "_merged").c_str(), mergeName.size() - 1);
                }
                else
                {
                    mergeObjects.erase(it);
                    MergeRemovePieces(name, {});
                }
                mergeCameraPending = SpriteCamera::Fit;
            }
            continue;
        }
        if (!ImGui::TreeNode(name.c_str()))
            continue;
        const auto &frames = MergeFramesOf(name);
        if (ImGui::SmallButton(_("All")))
        {
            MergeRemovePieces(name, {});
            MergeAddPieces(name, {}); // as they are in the object's image
            mergeCameraPending = SpriteCamera::Fit;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(_("None")))
            MergeRemovePieces(name, {});
        for (const MergeFrameInfo &f : frames)
        {
            bool on = placed.count({name, f.name}) > 0;
            const std::string label = f.name + "  " + std::to_string(f.rect[2]) + "x" + std::to_string(f.rect[3]) + "##"
                                      + name + "." + f.name;
            if (ImGui::Checkbox(label.c_str(), &on))
            {
                if (on)
                {
                    MergeAddPieces(name, f.name);
                    if (mergeName[0] == '\0')
                        std::strncpy(mergeName.data(), (name + "_baked").c_str(), mergeName.size() - 1);
                }
                else
                {
                    MergeRemovePieces(name, f.name);
                }
            }
        }
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: the layout
    ImGui::BeginChild("merge-canvas", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton(
        "canvas",
        avail,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool canvasActive = ImGui::IsItemActive();
    const bool middleDoubleClick = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(24, 26, 30, 255));
    if (mergePieces.empty())
    {
        dl->AddText(
            ImVec2(origin.x + 12, origin.y + 12),
            IM_COL32(200, 200, 200, 255),
            mergeBake ? _("Check frames on the left: they are laid out here and baked into one picture.")
                      : _("Check two or more sprites on the left: their frames are laid out here and merged "
                          "into one sprite."));
        ImGui::EndChild();
        return;
    }
    const ImGuiIO &io = ImGui::GetIO();
    int bx0, by0, bx1, by1;
    MergeBounds(bx0, by0, bx1, by1);
    float focus[4] {0, 0, 0, 0};
    bool hasFocus = false;
    for (const MergePiece &p : mergePieces)
        if (p.selected)
        {
            const float px0 = static_cast<float>(p.x), py0 = static_cast<float>(p.y);
            const float px1 = px0 + p.src[2], py1 = py0 + p.src[3];
            if (!hasFocus)
                focus[0] = px0, focus[1] = py0, focus[2] = px1, focus[3] = py1;
            focus[0] = std::min(focus[0], px0);
            focus[1] = std::min(focus[1], py0);
            focus[2] = std::max(focus[2], px1);
            focus[3] = std::max(focus[3], py1);
            hasFocus = true;
        }
    focus[2] -= focus[0];
    focus[3] -= focus[1];
    const float content[4] {
        static_cast<float>(bx0), static_cast<float>(by0), static_cast<float>(bx1 - bx0), static_cast<float>(by1 - by0)};
    SpriteCanvasCamera(
        origin,
        avail,
        hovered,
        canvasActive,
        middleDoubleClick,
        content,
        hasFocus ? focus : nullptr,
        mergeZoom,
        mergePan,
        mergeCameraPending);
    auto toScreen = [&](float x, float y) {
        return ImVec2(origin.x + mergePan.x + x * mergeZoom, origin.y + mergePan.y + y * mergeZoom);
    };
    const ImVec2 mouse = io.MousePos;
    const float mx = (mouse.x - origin.x - mergePan.x) / mergeZoom;
    const float my = (mouse.y - origin.y - mergePan.y) / mergeZoom;

    dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    dl->AddRectFilled(
        toScreen(static_cast<float>(bx0), static_cast<float>(by0)),
        toScreen(static_cast<float>(bx1), static_cast<float>(by1)),
        IM_COL32(60, 60, 64, 255));
    const std::vector<char> overlaps = mergeBake ? std::vector<char>(mergePieces.size(), 0) : MergeOverlaps();
    int hoveredPiece = -1;
    for (int i = 0; i < static_cast<int>(mergePieces.size()); ++i)
    {
        const MergePiece &p = mergePieces[i];
        const ImVec2 a = toScreen(static_cast<float>(p.x), static_cast<float>(p.y));
        const ImVec2 b = toScreen(static_cast<float>(p.x + p.src[2]), static_cast<float>(p.y + p.src[3]));
        if (SDL_Texture *tex = textures->GetSpriteSheet(Sprites().ImageOf(p.object)))
        {
            float tw = 1.0f, th = 1.0f;
            SDL_GetTextureSize(tex, &tw, &th);
            dl->AddImage(
                reinterpret_cast<ImTextureID>(tex),
                a,
                b,
                ImVec2(p.src[0] / tw, p.src[1] / th),
                ImVec2((p.src[0] + p.src[2]) / tw, (p.src[1] + p.src[3]) / th));
        }
        if (hovered && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y)
            hoveredPiece = i; // the last (topmost) one wins
    }
    for (int i = 0; i < static_cast<int>(mergePieces.size()); ++i)
    {
        const MergePiece &p = mergePieces[i];
        const ImVec2 a = toScreen(static_cast<float>(p.x), static_cast<float>(p.y));
        const ImVec2 b = toScreen(static_cast<float>(p.x + p.src[2]), static_cast<float>(p.y + p.src[3]));
        const ImU32 col = overlaps[i]  ? IM_COL32(255, 70, 70, 255)
                          : p.selected ? IM_COL32(255, 180, 60, 255)
                                       : IM_COL32(90, 200, 160, 110);
        dl->AddRect(a, b, col, 0.0f, 0, p.selected || overlaps[i] ? 2.0f : 1.0f);
        if (i == hoveredPiece)
            dl->AddText(
                ImVec2(a.x + 2, a.y - ImGui::GetTextLineHeight() - 1),
                IM_COL32(255, 230, 180, 255),
                (p.object + "." + p.frame).c_str());
    }
    dl->AddRect(
        toScreen(static_cast<float>(bx0), static_cast<float>(by0)),
        toScreen(static_cast<float>(bx1), static_cast<float>(by1)),
        IM_COL32(150, 150, 170, 255));

    // Left button: select (Ctrl adds / removes), drag the selection, or draw a box on the empty space
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        mergeDragStart = ImVec2(mx, my);
        if (hoveredPiece >= 0)
        {
            MergePiece &hp = mergePieces[hoveredPiece];
            if (io.KeyCtrl)
                hp.selected = !hp.selected;
            else if (!hp.selected)
                for (int i = 0; i < static_cast<int>(mergePieces.size()); ++i)
                    mergePieces[i].selected = i == hoveredPiece;
            mergeDrag = hp.selected ? MergeDrag::Move : MergeDrag::None;
            mergeDragFrom.clear();
            for (const MergePiece &p : mergePieces)
                mergeDragFrom.push_back({p.x, p.y});
        }
        else
        {
            if (!io.KeyCtrl)
                for (MergePiece &p : mergePieces)
                    p.selected = false;
            mergeDrag = MergeDrag::Box;
        }
    }
    if (mergeDrag != MergeDrag::None && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (mergeDrag == MergeDrag::Move && mergeDragFrom.size() == mergePieces.size())
        {
            const int dx = static_cast<int>(std::lround(mx - mergeDragStart.x));
            const int dy = static_cast<int>(std::lround(my - mergeDragStart.y));
            for (std::size_t i = 0; i < mergePieces.size(); ++i)
                if (mergePieces[i].selected)
                {
                    mergePieces[i].x = mergeDragFrom[i][0] + dx;
                    mergePieces[i].y = mergeDragFrom[i][1] + dy;
                }
        }
        else if (mergeDrag == MergeDrag::Box)
        {
            dl->AddRect(
                toScreen(mergeDragStart.x, mergeDragStart.y), mouse, IM_COL32(255, 255, 255, 220), 0.0f, 0, 1.0f);
        }
    }
    if (mergeDrag != MergeDrag::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        if (mergeDrag == MergeDrag::Box)
        {
            const Box sel {
                static_cast<int>(std::floor(std::min(mergeDragStart.x, mx))),
                static_cast<int>(std::floor(std::min(mergeDragStart.y, my))),
                static_cast<int>(std::ceil(std::max(mergeDragStart.x, mx))),
                static_cast<int>(std::ceil(std::max(mergeDragStart.y, my)))};
            for (MergePiece &p : mergePieces)
                if (Intersects(sel, Box {p.x, p.y, p.x + p.src[2], p.y + p.src[3]}))
                    p.selected = true;
        }
        mergeDrag = MergeDrag::None;
    }

    // Arrows move the selection by a pixel (Shift: 10); Delete takes frames off the picture (bake)
    if (ImGui::IsWindowFocused() && !io.WantTextInput)
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
        for (MergePiece &p : mergePieces)
            if (p.selected)
            {
                p.x += d[0];
                p.y += d[1];
            }
        if (ImGui::IsKeyPressed(ImGuiKey_A) && io.KeyCtrl)
            for (MergePiece &p : mergePieces)
                p.selected = true;
        if (mergeBake && ImGui::IsKeyPressed(ImGuiKey_Delete))
            mergePieces.erase(
                std::remove_if(mergePieces.begin(), mergePieces.end(), [](const MergePiece &p) { return p.selected; }),
                mergePieces.end());
    }
    dl->PopClipRect();

    // Status line
    const int selected = static_cast<int>(
        std::count_if(mergePieces.begin(), mergePieces.end(), [](const MergePiece &p) { return p.selected; }));
    const int bad = static_cast<int>(std::count(overlaps.begin(), overlaps.end(), 1));
    char line[256];
    std::snprintf(
        line,
        sizeof(line),
        "%dx%d  %d %s, %d %s  zoom %.2f  (%d, %d)%s",
        bx1 - bx0,
        by1 - by0,
        static_cast<int>(mergePieces.size()),
        _("frames"),
        selected,
        _("selected"),
        mergeZoom,
        static_cast<int>(std::floor(mx)),
        static_cast<int>(std::floor(my)),
        bad ? _("  - frames overlap") : "");
    dl->AddText(
        ImVec2(origin.x + 8, origin.y + avail.y - ImGui::GetTextLineHeight() - 6),
        bad ? IM_COL32(255, 120, 120, 255) : IM_COL32(220, 220, 220, 255),
        line);
    ImGui::EndChild();
}

void CharacterEditor::DrawSpriteMergeProperties()
{
    int mode = mergeBake ? 1 : 0;
    Hinted(ImGui::RadioButton(_("Merge sprites"), &mode, 0), "Merge sprites");
    ImGui::SameLine();
    Hinted(ImGui::RadioButton(_("Bake frames"), &mode, 1), "Bake frames");
    if ((mode == 1) != mergeBake)
    {
        mergeBake = mode == 1;
        MergeReset();
        mergeName[0] = '\0';
    }
    ImGui::TextWrapped(
        "%s",
        mergeBake ? _("Frames of any sprites, laid out freely (they may overlap), become one "
                      "picture: a new sprite with one frame. The sources stay.")
                  : _("The checked sprites become one: all frames in one image (arrange them "
                      "freely), all parts, animations and families in one meta. References "
                      "are rewritten, the sources removed."));
    ImGui::Separator();
    Hinted(ImGui::InputText(_("New sprite"), mergeName.data(), mergeName.size()), "New sprite##merge");
    ImGui::SetNextItemWidth(120.0f);
    if (Hinted(ImGui::InputInt(_("Padding"), &mergePadding), "Padding##merge"))
        mergePadding = std::clamp(mergePadding, 0, 64);
    ImGui::BeginDisabled(mergePieces.empty());
    if (Hinted(ImGui::Button(_("Pack")), "Pack##merge"))
        MergePack();
    ImGui::SameLine();
    if (ImGui::Button(_("Select all")))
        for (MergePiece &p : mergePieces)
            p.selected = true;
    ImGui::SameLine();
    if (ImGui::Button(_("Clear")))
    {
        MergeReset();
        mergeName[0] = '\0';
    }
    ImGui::EndDisabled();

    // The selection
    std::vector<int> sel;
    for (int i = 0; i < static_cast<int>(mergePieces.size()); ++i)
        if (mergePieces[i].selected)
            sel.push_back(i);
    if (!mergeObjects.empty() && !mergeBake)
    {
        ImGui::SeparatorText(_("Sprites"));
        for (const std::string &o : mergeObjects)
            ImGui::BulletText("%s", o.c_str());
    }
    if (sel.size() == 1)
    {
        MergePiece &p = mergePieces[sel[0]];
        ImGui::SeparatorText(_("Frame"));
        ImGui::Text("%s.%s", p.object.c_str(), p.frame.c_str());
        ImGui::TextDisabled("%s [%d, %d, %d, %d]", _("in its image"), p.src[0], p.src[1], p.src[2], p.src[3]);
        int xy[2] = {p.x, p.y};
        if (ImGui::InputInt2(_("Position"), xy))
        {
            p.x = xy[0];
            p.y = xy[1];
        }
    }
    else if (sel.size() > 1)
    {
        ImGui::SeparatorText(_("Frames"));
        ImGui::Text("%d %s", static_cast<int>(sel.size()), _("selected (Pack packs only them)"));
    }
    if (mergeBake && !sel.empty())
    {
        // Drawing order: later pieces are drawn over earlier ones
        auto moveSel = [&](int dir) {
            if (dir == 2 || dir == -2)
            {
                std::stable_partition(mergePieces.begin(), mergePieces.end(), [&](const MergePiece &p) {
                    return dir == 2 ? !p.selected : p.selected;
                });
                return;
            }
            const int n = static_cast<int>(mergePieces.size());
            if (dir > 0)
            {
                for (int i = n - 2; i >= 0; --i)
                    if (mergePieces[i].selected && !mergePieces[i + 1].selected)
                        std::swap(mergePieces[i], mergePieces[i + 1]);
            }
            else
            {
                for (int i = 1; i < n; ++i)
                    if (mergePieces[i].selected && !mergePieces[i - 1].selected)
                        std::swap(mergePieces[i], mergePieces[i - 1]);
            }
        };
        if (Hinted(ImGui::Button(_("Up")), "Up##merge"))
            moveSel(1);
        ImGui::SameLine();
        if (ImGui::Button(_("Down")))
            moveSel(-1);
        ImGui::SameLine();
        if (ImGui::Button(_("To front")))
            moveSel(2);
        ImGui::SameLine();
        if (ImGui::Button(_("To back")))
            moveSel(-2);
        ImGui::SameLine();
        if (ImGui::Button(_("Remove")))
            mergePieces.erase(
                std::remove_if(mergePieces.begin(), mergePieces.end(), [](const MergePiece &p) { return p.selected; }),
                mergePieces.end());
    }

    ImGui::Separator();
    ImGui::BeginDisabled(mergePieces.empty());
    if (Hinted(ImGui::Button(mergeBake ? _("Bake...") : _("Merge...")), mergeBake ? "Bake..." : "Merge..."))
    {
        if (MergePrepare())
            ImGui::OpenPopup("##merge-confirm");
    }
    ImGui::EndDisabled();
    if (!mergeReport.empty())
        ImGui::TextWrapped("%s", mergeReport.c_str());

    if (ImGui::BeginPopupModal("##merge-confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(mergeConfirmText.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (ImGui::Button(mergeBake ? _("Bake") : _("Merge"), ImVec2(120.0f, 0.0f)))
        {
            try
            {
                if (mergeBake)
                    BakeFramesSave();
                else
                    MergeObjectsSave();
            }
            catch (const std::exception &e)
            {
                mergeReport = e.what();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_("Cancel"), ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace CharEditor
