#include "SpriteExport.hpp"

#include "core/AssetStore.hpp"
#include "graphics/TextureManager.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <climits>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace CharEditor {

namespace {

using nlohmann::json;

constexpr int kMaxImageSide = 1638400;

// Decoded sprite sheets (RGBA), loaded when first needed
class Sheets {
public:
    explicit Sheets(const fs::path& root) : assets(root) {}
    ~Sheets() { for (auto& [page, s] : surfaces) if (s) SDL_DestroySurface(s); }

    SDL_Surface* get(int page) {
        auto it = surfaces.find(page);
        if (it != surfaces.end()) return it->second;
        SDL_Surface* result = nullptr;
        char name[32];
        std::snprintf(name, sizeof name, "sheet_%02d.png", page);
        std::vector<uint8_t> bytes;
        if (OpenArtOfWar::Assets::readBytes((assets / "sprites" / name).string(), bytes) && !bytes.empty()) {
            SDL_Surface* loaded = SDL_LoadPNG_IO(SDL_IOFromConstMem(bytes.data(), bytes.size()), true);
            if (loaded) {
                result = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
                SDL_DestroySurface(loaded);
                if (result) SDL_SetSurfaceBlendMode(result, SDL_BLENDMODE_NONE); // copy the alpha, do not blend it
            }
        }
        surfaces[page] = result;
        return result;
    }

    // The sprite's rectangle in the sheet's pixels; false for an empty sprite or a missing sheet
    bool rect(const RawSprite& s, SDL_Rect& out) {
        if (s.w <= 0 || s.h <= 0) return false;
        SDL_Surface* sheet = get(s.page);
        if (!sheet) {
            missingSprites.insert(s.id);
            missingPages.insert(s.page);
            return false;
        }
        const float scale = this->scale(s.page);
        const int x0 = static_cast<int>(s.x * scale), y0 = static_cast<int>(s.y * scale);
        const int x1 = std::min(sheet->w, static_cast<int>((s.x + s.w) * scale));
        const int y1 = std::min(sheet->h, static_cast<int>((s.y + s.h) * scale));
        if (x1 <= x0 || y1 <= y0) return false;
        out = {x0, y0, x1 - x0, y1 - y0};
        return true;
    }

    // The sheet may be enlarged against the original: the sprite rectangles are in the original's pixels
    // Sprites that could not be cut out because their sheet does not exist (the data refers to sheets 42..44)
    std::set<int> missingSprites;
    std::set<int> missingPages;

    float scale(int page) {
        SDL_Surface* sheet = get(page);
        const int base = OpenArtOfWar::TextureManager::baseSheetWidth(page);
        return sheet && base > 0 ? static_cast<float>(sheet->w) / static_cast<float>(base) : 1.0f;
    }

private:
    fs::path assets;
    std::map<int, SDL_Surface*> surfaces;
};

struct Placed {
    int id{0};
    SDL_Rect src{};   // in the sheet
    int x{0}, y{0};   // in the atlas
};

bool writeJson(const fs::path& path, const json& doc) {
    std::ofstream out(path);
    if (!out) return false;
    out << doc.dump(2) << '\n';
    return static_cast<bool>(out);
}

bool savePng(SDL_Surface* surface, const fs::path& path, std::string& error) {
    if (!SDL_SavePNG(surface, path.string().c_str())) {
        error = std::string("Could not write ") + path.string() + ": " + SDL_GetError();
        return false;
    }
    return true;
}

// MaxRects (best short side fit): the sprites are fitted into the free space left between the ones already placed,
// largest first, `pad` px apart. The result keeps the order of `items`; width / height are what is really used.
std::vector<Placed> packMaxRects(std::vector<Placed> items, int& width, int& height, int pad) {
    long long area = 0;
    int maxW = 1, maxH = 1;
    for (const Placed& p : items) {
        area += static_cast<long long>(p.src.w + pad) * (p.src.h + pad);
        maxW = std::max(maxW, p.src.w + pad);
        maxH = std::max(maxH, p.src.h + pad);
    }
    std::vector<size_t> order(items.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const int ha = std::max(items[a].src.w, items[a].src.h), hb = std::max(items[b].src.w, items[b].src.h);
        if (ha != hb) return ha > hb;
        return static_cast<long long>(items[a].src.w) * items[a].src.h > static_cast<long long>(items[b].src.w) * items[b].src.h;
    });

    struct Result {
        std::vector<SDL_Rect> pos; // by item
        int w{0}, h{0};
        bool ok{false};
    };
    auto attempt = [&](int binW) {
        Result r;
        r.pos.assign(items.size(), SDL_Rect{0, 0, 0, 0});
        std::vector<SDL_Rect> free{{0, 0, binW, kMaxImageSide}};
        for (size_t idx : order) {
            const int w = items[idx].src.w + pad, h = items[idx].src.h + pad;
            int best = -1, bestShort = INT32_MAX, bestLong = INT32_MAX;
            for (size_t f = 0; f < free.size(); ++f) {
                if (free[f].w < w || free[f].h < h) continue;
                const int shortSide = std::min(free[f].w - w, free[f].h - h), longSide = std::max(free[f].w - w, free[f].h - h);
                if (shortSide < bestShort || (shortSide == bestShort && longSide < bestLong)) {
                    best = static_cast<int>(f);
                    bestShort = shortSide;
                    bestLong = longSide;
                }
            }
            if (best < 0) return r; // does not fit
            const SDL_Rect placed{free[static_cast<size_t>(best)].x, free[static_cast<size_t>(best)].y, w, h};
            r.pos[idx] = placed;
            r.w = std::max(r.w, placed.x + items[idx].src.w);
            r.h = std::max(r.h, placed.y + items[idx].src.h);
            // Cut the placed rectangle out of every free rectangle it touches
            std::vector<SDL_Rect> next;
            for (const SDL_Rect& f : free) {
                if (placed.x >= f.x + f.w || placed.x + placed.w <= f.x || placed.y >= f.y + f.h || placed.y + placed.h <= f.y) {
                    next.push_back(f);
                    continue;
                }
                if (placed.x > f.x) next.push_back({f.x, f.y, placed.x - f.x, f.h});
                if (placed.x + placed.w < f.x + f.w) next.push_back({placed.x + placed.w, f.y, f.x + f.w - placed.x - placed.w, f.h});
                if (placed.y > f.y) next.push_back({f.x, f.y, f.w, placed.y - f.y});
                if (placed.y + placed.h < f.y + f.h) next.push_back({f.x, placed.y + placed.h, f.w, f.y + f.h - placed.y - placed.h});
            }
            // Drop the free rectangles that lie inside another one
            free.clear();
            for (size_t i = 0; i < next.size(); ++i) {
                bool inside = false;
                for (size_t j = 0; j < next.size() && !inside; ++j) {
                    inside = i != j && next[i].x >= next[j].x && next[i].y >= next[j].y &&
                             next[i].x + next[i].w <= next[j].x + next[j].w && next[i].y + next[i].h <= next[j].y + next[j].h &&
                             (next[i].x != next[j].x || next[i].y != next[j].y || next[i].w != next[j].w || next[i].h != next[j].h || i > j);
                }
                if (!inside) free.push_back(next[i]);
            }
        }
        r.ok = true;
        return r;
    };

    // A few widths around a square: the smallest picture wins
    const double square = std::sqrt(static_cast<double>(area) * 1.05);
    Result best;
    long long bestArea = -1;
    for (double factor : {0.8, 1.0, 1.25, 1.6}) {
        const int binW = std::clamp(static_cast<int>(std::ceil(square * factor / 16.0)) * 16, maxW, kMaxImageSide);
        Result r = attempt(binW);
        if (!r.ok) continue;
        const long long used = static_cast<long long>(r.w) * r.h;
        if (bestArea < 0 || used < bestArea) {
            bestArea = used;
            best = std::move(r);
        }
    }
    if (bestArea < 0) { // very unlikely: one long strip
        best = attempt(kMaxImageSide);
    }
    width = std::max(1, best.w);
    height = std::max(1, best.h);
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].x = best.pos[i].x;
        items[i].y = best.pos[i].y;
    }
    return items;
}

// FNV-1a over the RGBA bytes of a rectangle: written into the exports, so that an import tells an edited sprite from
// one that was left alone even when other sprites share the same pixels of the sheet
std::string hashRect(const SDL_Surface* surface, int x, int y, int w, int h) {
    uint64_t hash = 1469598103934665603ull;
    for (int row = 0; row < h; ++row) {
        const uint8_t* p = static_cast<const uint8_t*>(surface->pixels) + static_cast<size_t>(y + row) * surface->pitch + x * 4;
        for (int i = 0; i < w * 4; ++i) hash = (hash ^ p[i]) * 1099511628211ull;
    }
    char text[24];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

json spriteJson(const RawSprite& s, const SDL_Rect& src, float scale, const std::string& hash) {
    return {{"id", s.id}, {"page", s.page}, {"palette", s.palette}, {"hash", hash},
            {"source", {{"x", src.x}, {"y", src.y}, {"w", src.w}, {"h", src.h}}},
            {"original", {{"x", s.x}, {"y", s.y}, {"w", s.w}, {"h", s.h}}},
            {"sheet_scale", scale}};
}

std::string skippedNote(const Sheets& sheets) {
    if (sheets.missingSprites.empty()) return "";
    std::string pages;
    for (int page : sheets.missingPages) pages += (pages.empty() ? "" : ", ") + std::to_string(page);
    return " (" + std::to_string(sheets.missingSprites.size()) + " sprites skipped: sheet " + pages + " does not exist)";
}

} // namespace

std::vector<int> pickObjectThumbnails(const SpriteCatalog& catalog, const fs::path& assetsRoot) {
    Sheets sheets(assetsRoot);
    std::map<int, long long> solid; // sprite id -> number of nearly opaque pixels
    auto score = [&](int id) {
        auto it = solid.find(id);
        if (it != solid.end()) return it->second;
        long long count = 0;
        SDL_Rect r;
        if (sheets.rect(catalog.sprites()[static_cast<size_t>(id)], r)) {
            SDL_Surface* sheet = sheets.get(catalog.sprites()[static_cast<size_t>(id)].page);
            for (int y = r.y; y < r.y + r.h; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(sheet->pixels) + static_cast<size_t>(y) * sheet->pitch;
                for (int x = r.x; x < r.x + r.w; ++x) count += row[x * 4 + 3] >= 250 ? 1 : 0;
            }
        }
        return solid[id] = count;
    };
    std::vector<int> result(catalog.objects().size(), -1);
    for (size_t i = 0; i < catalog.objects().size(); ++i) {
        long long best = 0;
        const SpriteObject& o = catalog.objects()[i];
        const std::set<int> standing = o.thumbSequence >= 0 ? catalog.spritesOfSequence(o.thumbSequence) : std::set<int>();
        for (int id : (standing.empty() ? o.sprites : standing)) {
            const long long s = score(id);
            if (s > best) {
                best = s;
                result[i] = id;
            }
        }
    }
    return result;
}

bool exportSpriteObjects(const SpriteCatalog& catalog, const fs::path& assetsRoot,
                         const fs::path& outDir, const std::vector<int>& objectIndices, std::string& report) {
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        report = "Could not create " + outDir.string() + ": " + ec.message();
        return false;
    }
    Sheets sheets(assetsRoot);
    json index = json::object();
    index["objects"] = json::array();
    int number = 0;
    std::string error;

    for (int objectIndex : objectIndices) {
        if (objectIndex < 0 || objectIndex >= static_cast<int>(catalog.objects().size())) continue;
        const SpriteObject& o = catalog.objects()[static_cast<size_t>(objectIndex)];
        ++number;

        std::vector<Placed> items;
        for (int id : o.sprites) {
            Placed p;
            p.id = id;
            if (sheets.rect(catalog.sprites()[static_cast<size_t>(id)], p.src)) items.push_back(p);
        }
        int w = 1, h = 1;
        items = packMaxRects(std::move(items), w, h, 1);

        char stem[256];
        std::string faction = o.faction;
        std::transform(faction.begin(), faction.end(), faction.begin(), [](unsigned char c) { return std::tolower(c); });
        std::snprintf(stem, sizeof stem, "%03d_%s_%s_%s_id%d", number, o.kindName.c_str(), faction.c_str(), o.slug.c_str(), o.id);

        json sprites = json::array();
        if (!items.empty()) {
            if (w > kMaxImageSide || h > kMaxImageSide) {
                report = std::string("Atlas too large for ") + stem;
                return false;
            }
            SDL_Surface* atlas = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
            if (!atlas) {
                report = std::string("Could not create the atlas: ") + SDL_GetError();
                return false;
            }
            SDL_FillSurfaceRect(atlas, nullptr, 0);
            for (const Placed& p : items) {
                SDL_Surface* sheet = sheets.get(catalog.sprites()[static_cast<size_t>(p.id)].page);
                SDL_Rect dst{p.x, p.y, p.src.w, p.src.h};
                SDL_BlitSurface(sheet, &p.src, atlas, &dst);
                const RawSprite& s = catalog.sprites()[static_cast<size_t>(p.id)];
                json item = spriteJson(s, p.src, sheets.scale(s.page), hashRect(sheet, p.src.x, p.src.y, p.src.w, p.src.h));
                item["atlas_rect"] = {{"x", p.x}, {"y", p.y}, {"w", p.src.w}, {"h", p.src.h}};
                sprites.push_back(item);
            }
            const bool saved = savePng(atlas, outDir / (std::string(stem) + ".png"), error);
            SDL_DestroySurface(atlas);
            if (!saved) {
                report = error;
                return false;
            }
        }

        json doc = json::object();
        doc["object"] = {{"kind", o.kindName}, {"id", o.id}, {"faction", o.faction}, {"name", o.name}, {"slug", o.slug}};
        if (o.kind == SpriteObjectKind::Unit) doc["object"]["archetype_id"] = o.id;
        if (o.kind == SpriteObjectKind::Building) doc["object"]["building_index"] = o.id;
        if (o.kind == SpriteObjectKind::Tileset) doc["object"]["tileset"] = o.id;
        doc["atlas"] = {{"file", items.empty() ? "" : std::string(stem) + ".png"}, {"width", items.empty() ? 0 : w},
                        {"height", items.empty() ? 0 : h}, {"padding", 1}};
        doc["sprites"] = sprites;
        doc["graphics"] = catalog.describe(o);
        if (!writeJson(outDir / (std::string(stem) + ".json"), doc)) {
            report = "Could not write " + (outDir / (std::string(stem) + ".json")).string();
            return false;
        }
        index["objects"].push_back({{"name", stem}, {"kind", o.kindName}, {"id", o.id}, {"faction", o.faction},
                                    {"title", o.name}, {"sprites", static_cast<int>(sprites.size())}});
    }

    index["count"] = number;
    if (!writeJson(outDir / "index.json", index)) {
        report = "Could not write index.json";
        return false;
    }
    report = "Exported " + std::to_string(number) + " objects to " + outDir.string() +
             skippedNote(sheets);
    return true;
}

namespace {

// One grid file; returns false (and the error) when nothing could be written
bool writeGrid(const SpriteCatalog& catalog, Sheets& sheets, const std::vector<int>& ids, int columns, bool compress, bool packed, const fs::path& outDir,
               const std::string& name, const std::string& title, json& indexEntry, std::string& error) {
    std::vector<Placed> items;
    int cellW = 1, cellH = 1;
    for (int id : ids) {
        Placed p;
        p.id = id;
        if (!sheets.rect(catalog.sprites()[static_cast<size_t>(id)], p.src)) continue;
        cellW = std::max(cellW, p.src.w);
        cellH = std::max(cellH, p.src.h);
        items.push_back(p);
    }
    if (items.empty()) {
        error = "No sprites in " + name;
        indexEntry = {{"name", name}, {"title", title}, {"sprites", 0}};
        return true;
    }
    if (packed) {
        int w = 1, h = 1;
        items = packMaxRects(std::move(items), w, h, 1);
        SDL_Surface* atlas = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        if (!atlas) {
            error = std::string("Could not create the image: ") + SDL_GetError();
            return false;
        }
        SDL_FillSurfaceRect(atlas, nullptr, 0);
        json packedSprites = json::array();
        for (const Placed& p : items) { // items stay in the order of their ids
            const RawSprite& s = catalog.sprites()[static_cast<size_t>(p.id)];
            SDL_Rect dst{p.x, p.y, p.src.w, p.src.h};
            SDL_BlitSurface(sheets.get(s.page), &p.src, atlas, &dst);
            json item = spriteJson(s, p.src, sheets.scale(s.page), hashRect(sheets.get(s.page), p.src.x, p.src.y, p.src.w, p.src.h));
            item["cell_rect"] = {{"x", p.x}, {"y", p.y}, {"w", p.src.w}, {"h", p.src.h}};
            packedSprites.push_back(item);
        }
        const bool saved = savePng(atlas, outDir / (name + ".png"), error);
        SDL_DestroySurface(atlas);
        if (!saved) return false;
        json doc = {{"title", title}, {"file", name + ".png"}, {"packed", true}, {"image", {{"w", w}, {"h", h}}},
                    {"sprites", packedSprites}};
        if (!writeJson(outDir / (name + ".json"), doc)) {
            error = "Could not write " + (outDir / (name + ".json")).string();
            return false;
        }
        indexEntry = {{"name", name}, {"title", title}, {"sprites", static_cast<int>(items.size())}, {"packed", true}, {"width", w}, {"height", h}};
        return true;
    }
    const int cols = std::clamp(columns, 1, static_cast<int>(items.size()));
    const int rows = (static_cast<int>(items.size()) + cols - 1) / cols;
    std::vector<int> rowH(static_cast<size_t>(rows), 0);
    std::vector<long long> rowW(static_cast<size_t>(rows), 0);
    for (size_t i = 0; i < items.size(); ++i) {
        const size_t r = i / static_cast<size_t>(cols);
        rowH[r] = std::max(rowH[r], items[i].src.h);
        rowW[r] += items[i].src.w;
    }
    std::vector<long long> rowY(static_cast<size_t>(rows), 0);
    long long sumH = 0, maxW = 0;
    for (size_t r = 0; r < rowH.size(); ++r) {
        rowY[r] = sumH;
        sumH += rowH[r];
        maxW = std::max(maxW, rowW[r]);
    }
    const long long width = compress ? maxW : static_cast<long long>(cols) * cellW;
    const long long height = compress ? sumH : static_cast<long long>(rows) * cellH;
    if (width > kMaxImageSide || height > kMaxImageSide) {
        error = name + ": " + std::to_string(width) + "x" + std::to_string(height) + " px is too large, use more columns or fewer groups";
        return false;
    }
    SDL_Surface* image = SDL_CreateSurface(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32);
    if (!image) {
        error = std::string("Could not create the image: ") + SDL_GetError();
        return false;
    }
    SDL_FillSurfaceRect(image, nullptr, 0);
    json sprites = json::array();
    long long cursorX = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        Placed& p = items[i];
        const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
        if (compress) {
            if (col == 0) cursorX = 0;
            p.x = static_cast<int>(cursorX);
            p.y = static_cast<int>(rowY[static_cast<size_t>(row)]);
            cursorX += p.src.w;
        } else {
            p.x = col * cellW;
            p.y = row * cellH;
        }
        const RawSprite& s = catalog.sprites()[static_cast<size_t>(p.id)];
        SDL_Rect dst{p.x, p.y, p.src.w, p.src.h};
        SDL_BlitSurface(sheets.get(s.page), &p.src, image, &dst);
        json item = spriteJson(s, p.src, sheets.scale(s.page), hashRect(sheets.get(s.page), p.src.x, p.src.y, p.src.w, p.src.h));
        item["row"] = row;
        item["column"] = col;
        item["cell_rect"] = {{"x", p.x}, {"y", p.y}, {"w", p.src.w}, {"h", p.src.h}};
        sprites.push_back(item);
    }
    const bool saved = savePng(image, outDir / (name + ".png"), error);
    SDL_DestroySurface(image);
    if (!saved) return false;
    json doc = {{"title", title}, {"file", name + ".png"}, {"columns", cols}, {"rows", rows},
                {"cell", {{"w", cellW}, {"h", cellH}}}, {"image", {{"w", width}, {"h", height}}}, {"sprites", sprites}};
    doc["compressed"] = compress;
    if (!writeJson(outDir / (name + ".json"), doc)) {
        error = "Could not write " + (outDir / (name + ".json")).string();
        return false;
    }
    indexEntry = {{"name", name}, {"title", title}, {"sprites", static_cast<int>(items.size())}, {"columns", cols}, {"rows", rows}};
    return true;
}

} // namespace

bool exportSingleSprites(const SpriteCatalog& catalog, const fs::path& assetsRoot,
                         const fs::path& outDir, const SingleExportOptions& options, std::string& report) {
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        report = "Could not create " + outDir.string() + ": " + ec.message();
        return false;
    }
    std::vector<int> byGroup[kSpriteGroupCount];
    for (const RawSprite& s : catalog.sprites()) {
        byGroup[static_cast<int>(catalog.groupOfSprite(s.id))].push_back(s.id);
    }
    Sheets sheets(assetsRoot);
    json index = json::object();
    index["files"] = json::array();
    std::string error;
    int total = 0;

    auto emit = [&](const std::vector<int>& ids, const std::string& name, const std::string& title) {
        json entry;
        if (!writeGrid(catalog, sheets, ids, options.columns, options.compress, options.packed, outDir, name, title, entry, error)) return false;
        total += entry.value("sprites", 0);
        index["files"].push_back(entry);
        return true;
    };

    if (options.allInOne) {
        std::vector<int> ids;
        for (int g = 0; g < kSpriteGroupCount; ++g) {
            if (options.groups[g]) ids.insert(ids.end(), byGroup[g].begin(), byGroup[g].end());
        }
        if (ids.empty()) {
            report = "No group is chosen";
            return false;
        }
        if (!emit(ids, "sprites_all", "All chosen groups")) {
            report = error;
            return false;
        }
    } else {
        bool any = false;
        for (int g = 0; g < kSpriteGroupCount; ++g) {
            if (!options.groups[g]) continue;
            any = true;
            const auto group = static_cast<SpriteGroup>(g);
            if (!emit(byGroup[g], std::string("sprites_") + spriteGroupSlug(group), spriteGroupName(group))) {
                report = error;
                return false;
            }
        }
        if (!any) {
            report = "No group is chosen";
            return false;
        }
    }
    index["columns"] = options.columns;
    if (!writeJson(outDir / "sprites_index.json", index)) {
        report = "Could not write sprites_index.json";
        return false;
    }
    report = "Exported " + std::to_string(total) + " sprites to " + outDir.string() + skippedNote(sheets);
    return true;
}

} // namespace CharEditor

// ---------------------------------------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------------------------------------

namespace CharEditor {

namespace {

SDL_Surface* loadRgba(const fs::path& path) {
    std::vector<uint8_t> bytes;
    if (!OpenArtOfWar::Assets::readBytes(path.string(), bytes) || bytes.empty()) return nullptr;
    SDL_Surface* loaded = SDL_LoadPNG_IO(SDL_IOFromConstMem(bytes.data(), bytes.size()), true);
    if (!loaded) return nullptr;
    SDL_Surface* converted = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (converted) SDL_SetSurfaceBlendMode(converted, SDL_BLENDMODE_NONE);
    return converted;
}

bool rectInside(const SDL_Surface* s, int x, int y, int w, int h) {
    return s && w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= s->w && y + h <= s->h;
}

// The two rectangles hold different pixels
bool differs(const SDL_Surface* a, int ax, int ay, const SDL_Surface* b, int bx, int by, int w, int h) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* ra = static_cast<const uint8_t*>(a->pixels) + static_cast<size_t>(ay + y) * a->pitch + ax * 4;
        const uint8_t* rb = static_cast<const uint8_t*>(b->pixels) + static_cast<size_t>(by + y) * b->pitch + bx * 4;
        if (std::memcmp(ra, rb, static_cast<size_t>(w) * 4) != 0) return true;
    }
    return false;
}

// The pixels of src[sx..] pasted over dst[dx..]; true when dst really changed
bool paste(const SDL_Surface* src, int sx, int sy, SDL_Surface* dst, int dx, int dy, int w, int h) {
    bool changed = false;
    for (int y = 0; y < h; ++y) {
        const uint8_t* from = static_cast<const uint8_t*>(src->pixels) + static_cast<size_t>(sy + y) * src->pitch + sx * 4;
        uint8_t* to = static_cast<uint8_t*>(dst->pixels) + static_cast<size_t>(dy + y) * dst->pitch + dx * 4;
        if (std::memcmp(from, to, static_cast<size_t>(w) * 4) != 0) {
            std::memcpy(to, from, static_cast<size_t>(w) * 4);
            changed = true;
        }
    }
    return changed;
}

int kindCode(const std::string& kind, bool buildingOrBody) {
    if (kind == "composite") return 1;
    if (kind == "anim") return buildingOrBody ? 2 : 0;
    return 0; // sprite
}

// An edit: `value` differs from the table as it was (`original`) -> written into `target`. A row that was left alone
// never undoes an edit made to the same table row through another object's file.
template <typename T>
bool setIfEdited(json& target, const json& original, const char* key, const T& value) {
    if (original.contains(key) && original[key] == value) return false;
    if (target.contains(key) && target[key] == value) return false;
    target[key] = value;
    return true;
}

// The rows of a table range, replaced when the edited list has the same length; counts what changed
struct TableEdit {
    int changed{0};
    int skipped{0};
};

} // namespace

bool importSpriteFolder(const fs::path& assetsRoot, const fs::path& inDir, std::string& report) {
    std::error_code ec;
    if (!fs::is_directory(inDir, ec)) {
        report = "Import folder not found: " + inDir.string();
        return false;
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(inDir, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() == ".json" && name != "index.json" && name != "sprites_index.json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        report = "No exported .json files in " + inDir.string();
        return false;
    }

    Sheets sheets(assetsRoot);
    // Several sprites may share the same rectangle of a sheet: only what differs from the sheet as it was is an edit,
    // so a sprite that was left alone can never undo an edited one
    std::map<int, SDL_Surface*> pristine;
    struct PristineCleanup {
        std::map<int, SDL_Surface*>& surfaces;
        ~PristineCleanup() { for (auto& [page, surface] : surfaces) if (surface) SDL_DestroySurface(surface); }
    } cleanup{pristine};
    auto original = [&](int page, SDL_Surface* sheet) {
        auto it = pristine.find(page);
        if (it == pristine.end()) it = pristine.emplace(page, SDL_DuplicateSurface(sheet)).first;
        return it->second;
    };
    std::set<int> modifiedPages;
    int pasted = 0, unchanged = 0, skipped = 0, filesUsed = 0;
    TableEdit tables;
    json gd, gdOriginal;
    bool haveGd = false, gdChanged = false;
    const fs::path gdPath = assetsRoot / "data" / "graphics_data.json";

    auto loadGd = [&]() {
        if (haveGd) return true;
        std::ifstream in(gdPath);
        if (!in) return false;
        try { gd = json::parse(in); } catch (...) { return false; }
        gdOriginal = gd;
        return haveGd = true;
    };

    // Rows of a flat table (ranges + items): `edit(item, entry)` returns true when it changed the item
    auto applyRows = [&](const char* table, const char* items, int id, const json& rows, auto&& edit) {
        if (!loadGd() || !gd.contains(table) || !gd[table].contains("ranges")) return;
        auto& ranges = gd[table]["ranges"];
        if (id < 0 || id >= static_cast<int>(ranges.size())) { ++tables.skipped; return; }
        const int first = ranges[static_cast<size_t>(id)][0].get<int>(), last = ranges[static_cast<size_t>(id)][1].get<int>();
        if (!rows.is_array() || static_cast<int>(rows.size()) != last - first + 1) { ++tables.skipped; return; }
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            json& item = gd[table][items][static_cast<size_t>(first + i)];
            const json& orig = gdOriginal[table][items][static_cast<size_t>(first + i)];
            if (edit(item, orig, rows[static_cast<size_t>(i)])) { ++tables.changed; gdChanged = true; }
        }
    };

    for (const fs::path& file : files) {
        json doc;
        try {
            std::ifstream in(file);
            doc = json::parse(in);
        } catch (...) {
            ++skipped;
            continue;
        }
        if (!doc.contains("sprites") || !doc["sprites"].is_array()) continue;
        // Object export: atlas.file + atlas_rect; single export: file + cell_rect
        const std::string imageName = doc.contains("atlas") ? doc["atlas"].value("file", std::string()) : doc.value("file", std::string());
        SDL_Surface* image = imageName.empty() ? nullptr : loadRgba(inDir / imageName);
        if (!imageName.empty() && !image) {
            report = "Could not read " + (inDir / imageName).string();
            return false;
        }
        bool used = false;
        for (const auto& s : doc["sprites"]) {
            const json& rect = s.contains("atlas_rect") ? s["atlas_rect"] : s["cell_rect"];
            const json& src = s["source"];
            const int page = s.value("page", -1);
            if (!image || !rect.is_object() || !src.is_object() || page < 0) { ++skipped; continue; }
            SDL_Surface* sheet = sheets.get(page);
            const int w = rect.value("w", 0), h = rect.value("h", 0);
            if (!sheet || w != src.value("w", -1) || h != src.value("h", -1) ||
                !rectInside(image, rect.value("x", -1), rect.value("y", -1), w, h) ||
                !rectInside(sheet, src.value("x", -1), src.value("y", -1), w, h)) {
                ++skipped;
                continue;
            }
            used = true;
            // Edited = the pixels no longer match what the export wrote (its hash); an older export without hashes is
            // compared with the sheet as it was before this import
            bool edited;
            if (s.contains("hash") && s["hash"].is_string()) {
                edited = hashRect(image, rect["x"].get<int>(), rect["y"].get<int>(), w, h) != s["hash"].get<std::string>();
            } else {
                const SDL_Surface* before = original(page, sheet);
                edited = before && differs(image, rect["x"].get<int>(), rect["y"].get<int>(), before, src["x"].get<int>(), src["y"].get<int>(), w, h);
            }
            if (!edited) {
                ++unchanged;
            } else if (paste(image, rect["x"].get<int>(), rect["y"].get<int>(), sheet, src["x"].get<int>(), src["y"].get<int>(), w, h)) {
                ++pasted;
                modifiedPages.insert(page);
            } else {
                ++unchanged;
            }
        }
        if (image) SDL_DestroySurface(image);
        if (used) ++filesUsed;

        // The graphics tables of an object export
        if (doc.contains("graphics") && doc["graphics"].is_object()) {
            const json& g = doc["graphics"];
            // An entry left as exported (same digest) is not an edit
            auto edited = [](const json& entry) { return !entry.contains("digest") || graphicsDigest(entry) != entry["digest"].get<std::string>(); };
            auto childEdit = [&](json& item, const json& orig, const json& row, bool buildingOrBody) {
                bool changed = false;
                changed |= setIfEdited(item, orig, "child", row.value("child", 0));
                changed |= setIfEdited(item, orig, "ox", row.value("ox", 0));
                changed |= setIfEdited(item, orig, "oy", row.value("oy", 0));
                changed |= setIfEdited(item, orig, "is_comp", kindCode(row.value("kind", std::string("sprite")), buildingOrBody));
                return changed;
            };
            for (const auto& c : g.value("composites", json::array()))
                if (edited(c)) applyRows("composite_sprites", "parts", c.value("id", -1), c["parts"], [&](json& i, const json& o, const json& r) { return childEdit(i, o, r, false); });
            for (const auto& a : g.value("animations", json::array()))
                if (edited(a)) applyRows("anim_sequences", "frames", a.value("id", -1), a["frames"], [&](json& i, const json& o, const json& r) { return childEdit(i, o, r, false); });
            for (const auto& b : g.value("bodies", json::array()))
                if (edited(b)) applyRows("unit_bodies", "layers", b.value("id", -1), b["layers"], [&](json& i, const json& o, const json& r) {
                    bool ch = childEdit(i, o, r, true);
                    ch |= setIfEdited(i, o, "flag", r.value("flag", 0));
                    return ch;
                });
            for (const auto& q : g.value("sequences", json::array()))
                if (edited(q)) applyRows("unit_sequences", "steps", q.value("id", -1), q["steps"], [&](json& i, const json& o, const json& r) {
                    bool ch = setIfEdited(i, o, "body", r.value("body", 0));
                    ch |= setIfEdited(i, o, "ox", r.value("ox", 0));
                    ch |= setIfEdited(i, o, "oy", r.value("oy", 0));
                    return ch;
                });
            for (const auto& b : g.value("buildings", json::array())) {
                if (!edited(b)) continue;
                const int id = b.value("id", -1);
                applyRows("buildings", "parts", id, b["parts"], [&](json& i, const json& o, const json& r) {
                    bool ch = childEdit(i, o, r, true);
                    ch |= setIfEdited(i, o, "layer", r.value("layer", 0));
                    ch |= setIfEdited(i, o, "state", r.value("state", 0));
                    return ch;
                });
                if (loadGd() && id >= 0 && id < static_cast<int>(gd["buildings"]["widths"].size())) {
                    if (b.contains("width") && gdOriginal["buildings"]["widths"][static_cast<size_t>(id)] != b["width"] &&
                        gd["buildings"]["widths"][static_cast<size_t>(id)] != b["width"]) {
                        gd["buildings"]["widths"][static_cast<size_t>(id)] = b["width"];
                        ++tables.changed;
                        gdChanged = true;
                    }
                    if (b.contains("height") && gdOriginal["buildings"]["heights"][static_cast<size_t>(id)] != b["height"] &&
                        gd["buildings"]["heights"][static_cast<size_t>(id)] != b["height"]) {
                        gd["buildings"]["heights"][static_cast<size_t>(id)] = b["height"];
                        ++tables.changed;
                        gdChanged = true;
                    }
                }
            }
        }
    }

    // Only what really changed is written
    std::string error;
    for (int page : modifiedPages) {
        char name[32];
        std::snprintf(name, sizeof name, "sheet_%02d.png", page);
        if (!savePng(sheets.get(page), assetsRoot / "sprites" / name, error)) {
            report = error;
            return false;
        }
    }
    if (gdChanged) {
        std::ofstream out(gdPath);
        if (!out) {
            report = "Could not write " + gdPath.string();
            return false;
        }
        out << gd.dump(1) << '\n';
    }
    report = "Imported from " + inDir.string() + ": " + std::to_string(pasted) + " sprites changed in " +
             std::to_string(modifiedPages.size()) + " sheets (" + std::to_string(unchanged) + " already the same), " +
             std::to_string(tables.changed) + " table rows changed" +
             (tables.skipped ? ", " + std::to_string(tables.skipped) + " tables skipped (the number of rows differs)" : std::string()) +
             (skipped ? ", " + std::to_string(skipped) + " entries skipped" : std::string()) + ". Restart the editor to see them.";
    if (filesUsed == 0 && tables.changed == 0) report = "Nothing to import in " + inDir.string();
    return true;
}

} // namespace CharEditor
