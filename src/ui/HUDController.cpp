#include "ui/HUDController.hpp"
#include "game/Players.hpp"
#include "ui/UiKit.hpp"
#include "core/Localization.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/TextureManager.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/Research.hpp"
#include "graphics/RenderUtility.hpp"
#include "game/Sides.hpp"
#include <algorithm>
#include <cmath>

namespace OpenArtOfWar
{

    using ui::UiRect;
    namespace col = ui::color;

    namespace
    {

        // ---------------------------------------------------------------------------
        // Static catalogs shown in the sidebar
        // ---------------------------------------------------------------------------

        // Sidebar cards for trainable units; name / cost come from the YAML config, the description from its translation key
        struct UnitCameoDef
        {
            int archetype;
            int compId;
            const char *name;
            int cost;
            const char *desc;
        };

        const UnitCameoDef kResistanceInfantry[] = {
            {0, 2057, "RIFLEMAN", 80, "Standard Resistance infantry rifleman"},
            {2, 2058, "GRENADIER", 120, "Heavy rocket soldier vs vehicles and walls"},
            {4, 2059, "SNIPER", 160, "Long-range sniper (Camouflage mode [G])"},
        };

        const UnitCameoDef kResistanceVehicles[] = {
            {6, 2060, "COYOTE", 220, "Fast scout buggy (spawns at Factory)"},
            {8, 2061, "ARMADILLO", 350, "Main battle tank of Resistance"},
            {10, 2062, "RHINO", 480, "Heavy twin-cannon tank (Siege mode [G])"},
            {12, 2063, "PORCUPINE", 550, "Multiple Launch Rocket System (MLRS)"},
        };

        const UnitCameoDef kConfederationInfantry[] = {
            {1, 2049, "RIFLEMAN", 80, "Standard Confederation infantry rifleman"},
            {3, 2050, "HEAVY ASSAULT", 120, "Rocket infantry vs vehicles and walls"},
            {5, 2051, "FLAMETHROWER", 140, "Short-range flame infantry vs infantry"},
        };

        const UnitCameoDef kConfederationVehicles[] = {
            {7, 2052, "FORTRESS", 220, "Armored machine-gun carrier"},
            {9, 2053, "HAMMER", 350, "Main battle tank of Confederation"},
            {11, 2054, "ZEUS", 500, "Heavy tank (Siege mode [G])"},
            {13, 2056, "GRAD", 700, "Multiple Launch Rocket System (MLRS)"},
        };

        struct UnitCatalog
        {
            const UnitCameoDef *items;
            int count;
        };

        // Units the player's faction trains at Barracks (infantry) or Factory (vehicles)
        UnitCatalog unitCatalog(bool infantry)
        {
            const bool resistance = playerFaction() == Faction::Resistance;
            if(infantry)
            {
                return resistance ? UnitCatalog {kResistanceInfantry, static_cast<int>(std::size(kResistanceInfantry))} : UnitCatalog {kConfederationInfantry, static_cast<int>(std::size(kConfederationInfantry))};
            }
            return resistance ? UnitCatalog {kResistanceVehicles, static_cast<int>(std::size(kResistanceVehicles))} : UnitCatalog {kConfederationVehicles, static_cast<int>(std::size(kConfederationVehicles))};
        }

        struct OrderDef
        {
            const char *key; // keyboard shortcut, empty when the order is button-only
            const char *name;
            const char *desc;
        };

        const OrderDef kOrders[] = {
            {"Q", "ATTACK-MOVE", "Units advance to destination, attacking all encountered enemies"},
            {"G", "SIEGE", "Toggle Siege mode for Sniper and Rhino"},
            {"", "STOP", "Immediately halt all orders and movement"},
            {"", "DEFEND", "Hold current position and engage enemies in range"},
        };

        const char *const kTabNames[HUDController::TabCount] = {"BUILD", "INFANTRY", "VEHICLES", "TECH", "ORDERS"};
        // Tab icons per faction: HQ, rifleman, main tank, a research cameo; Orders draws a crosshair
        const int kResistanceTabIcons[HUDController::TabCount] = {2074, 2057, 2061, 2115, -1};
        const int kConfederationTabIcons[HUDController::TabCount] = {2064, 2049, 2053, 2087, -1};

        int tabIcon(int tab)
        {
            return (playerFaction() == Faction::Confederation ? kConfederationTabIcons : kResistanceTabIcons)[tab];
        }

        constexpr int kCameoW = 60; // native cameo composite size
        constexpr int kCameoH = 38;

        // ---------------------------------------------------------------------------
        // Sidebar geometry (shared by rendering and hit-testing)
        // ---------------------------------------------------------------------------

        struct SidebarLayout
        {
            UiRect panel;
            UiRect header;
            UiRect radar;
            UiRect menuBtn;
            UiRect objectivesBtn;
            UiRect tabs[HUDController::TabCount];
            UiRect strip;
            UiRect scrollUp;
            UiRect scrollDown;
            UiRect primaryBtn;
            UiRect grid;
            UiRect info;

            static constexpr int kCols = 2;
            static constexpr int kCellW = 136;
            static constexpr int kCellH = 84;
            static constexpr int kGap = 8;

            UiRect cell(int index, int scrollRow) const
            {
                int col = index % kCols;
                int row = index / kCols - scrollRow;
                return {grid.x + col * (kCellW + kGap), grid.y + row * (kCellH + kGap), kCellW, kCellH};
            }
            int visibleRows() const
            {
                return std::max(1, (grid.h + kGap) / (kCellH + kGap));
            }
        };

        SidebarLayout sidebarLayout(int windowW, int windowH, int top)
        {
            SidebarLayout l;
            const int w = HUDController::kSidebarWidth;
            const int x = windowW - w;
            const int pad = 10;
            const int innerW = w - 2 * pad;

            l.panel = {x, 0, w, windowH};
            l.header = {x + pad, top + 8, innerW, 40};
            l.radar = {x + pad, top + 54, innerW, 150};
            l.menuBtn = {x + pad, top + 212, (innerW - 8) / 2, 30};
            l.objectivesBtn = {l.menuBtn.right() + 8, top + 212, innerW - l.menuBtn.w - 8, 30};

            const int tabGap = 5;
            const int tabW = (innerW - (HUDController::TabCount - 1) * tabGap) / HUDController::TabCount;
            for(int t = 0; t < HUDController::TabCount; ++t)
            {
                l.tabs[t] = {x + pad + t * (tabW + tabGap), top + 252, tabW, 44};
            }

            l.strip = {x + pad, top + 302, innerW, 24};
            l.scrollDown = {l.strip.right() - 24, l.strip.y, 24, l.strip.h};
            l.scrollUp = {l.scrollDown.x - 26, l.strip.y, 24, l.strip.h};
            l.primaryBtn = {l.strip.x + innerW / 2 - 10, l.strip.y, innerW / 2 - 44, l.strip.h};

            l.info = {x + pad, windowH - 82, innerW, 72};
            l.grid = {x + pad, top + 332, innerW, std::max(SidebarLayout::kCellH, l.info.y - 8 - (top + 332))};
            return l;
        }

        // Map-to-radar transform: the whole map letterboxed, or with zoom a window of it around (centerX, centerY)
        struct RadarView
        {
            float scale {1.0f};
            int ox {0};
            int oy {0};
            int w {0};
            int h {0};
            float viewX {0.0f}; // first tile shown (zoomed radar)
            float viewY {0.0f};

            float toScreenX(float tileX) const
            {
                return ox + (tileX - viewX) * scale;
            }
            float toScreenY(float tileY) const
            {
                return oy + (tileY - viewY) * scale;
            }
        };

        RadarView radarView(const UiRect &radar, int mapW, int mapH, int zoom = 1, float centerX = 0.0f, float centerY = 0.0f)
        {
            RadarView v;
            if(mapW <= 0 || mapH <= 0)
                return v;
            const float innerW = static_cast<float>(radar.w - 4);
            const float innerH = static_cast<float>(radar.h - 4);
            v.scale = std::min(innerW / mapW, innerH / mapH) * std::max(1, zoom);
            auto axis = [&](float inner, int mapSize, float center, int start, int size, int &o, int &len, float &view)
            {
                const float tiles = inner / v.scale;
                if(tiles >= mapSize)
                {
                    len = static_cast<int>(mapSize * v.scale);
                    o = start + (size - len) / 2;
                    view = 0.0f;
                }
                else
                {
                    len = static_cast<int>(inner);
                    o = start + 2;
                    view = std::clamp(center - tiles * 0.5f, 0.0f, mapSize - tiles);
                }
            };
            axis(innerW, mapW, centerX, radar.x, radar.w, v.ox, v.w, v.viewX);
            axis(innerH, mapH, centerY, radar.y, radar.h, v.oy, v.h, v.viewY);
            return v;
        }

        // "Destroy" for the player's selected structures, bottom-right in the info box
        UiRect destroyBtn(const UiRect &info)
        {
            return {info.right() - 92, info.bottom() - 26, 84, 20};
        }

        std::vector<const Building *> ownSelectedBuildings(const EntityManager &entities, const std::vector<int> &selected)
        {
            std::vector<const Building *> out;
            for(int id : selected)
            {
                const Building *b = entities.getBuilding(id);
                if(b && b->alive && b->state != ActionState::Die && b->owner == Players::local())
                    out.push_back(b);
            }
            return out;
        }

        // Zoom buttons in the radar's bottom-right corner
        UiRect radarZoomInBtn(const UiRect &radar)
        {
            return {radar.right() - 22, radar.bottom() - 22, 20, 20};
        }
        UiRect radarZoomOutBtn(const UiRect &radar)
        {
            return {radar.right() - 44, radar.bottom() - 22, 20, 20};
        }

        SDL_Color radarFactionColor(Faction f, bool unit)
        {
            switch(f)
            {
                case Faction::Resistance:
                    return unit ? SDL_Color {255, 160, 140, 255} : SDL_Color {236, 92, 76, 255};
                case Faction::Confederation:
                    return unit ? SDL_Color {150, 215, 255, 255} : SDL_Color {72, 170, 236, 255};
                default:
                    return unit ? SDL_Color {190, 230, 150, 255} : SDL_Color {120, 190, 90, 255};
            }
        }

        std::string unitName(const UnitCameoDef &def, const EntityManager &entities)
        {
            const auto *cfg = entities.getUnitConfig(def.archetype);
            return (cfg && !cfg->name.empty()) ? cfg->name : std::string(def.name);
        }

        int unitCost(const UnitCameoDef &def, const EntityManager &entities)
        {
            const auto *cfg = entities.getUnitConfig(def.archetype);
            return cfg ? cfg->creditCost : def.cost;
        }

        std::string unitDesc(const UnitCameoDef &def, const EntityManager &entities)
        {
            const auto *cfg = entities.getUnitConfig(def.archetype);
            return (cfg && !cfg->description.empty()) ? cfg->description : std::string(def.desc);
        }

        void drawCostTag(Renderer &r, BitmapFontRenderer &font, const UiRect &cell, const std::string &text, SDL_Color color)
        {
            int tw = font.getTextWidth(text, ui::FONT_SMALL_BOLD) + 10;
            UiRect tag {cell.right() - tw - 3, cell.y + 3, tw, 18};
            r.fillRectScreen(tag.x, tag.y, tag.w, tag.h, {6, 8, 12, 215});
            font.drawString(text, tag.centerX(), tag.centerY(), ui::FONT_SMALL_BOLD, color, false, Anchor::HCENTER | Anchor::VCENTER);
        }

        // Common cameo frame: icon, bottom name band, hover/selection border
        void drawCameo(Renderer &r, BitmapFontRenderer &font, const UiRect &cell, int compId, const std::string &name, bool hovered, bool selected, bool locked)
        {
            r.fillRectScreen(cell.x, cell.y, cell.w, cell.h, col::PanelSunken);
            if(compId >= 0)
            {
                int iconX = cell.x + (cell.w - kCameoW * 2) / 2;
                int iconY = cell.y + 3;
                r.drawCompositeScreen(compId, iconX, iconY, 2.0f, playerFaction());
            }
            if(locked)
            {
                r.fillRectScreen(cell.x, cell.y, cell.w, cell.h, {10, 12, 16, 170});
            }
            UiRect band {cell.x, cell.bottom() - 20, cell.w, 20};
            r.fillRectScreen(band.x, band.y, band.w, band.h, {6, 8, 12, 225});
            font.drawString(ui::fitText(font, name, ui::FONT_SMALL_BOLD, band.w - 10), band.centerX(), band.centerY(), ui::FONT_SMALL_BOLD, locked ? col::TextMuted : (selected ? col::Accent : col::Text), false, Anchor::HCENTER | Anchor::VCENTER);

            SDL_Color border = selected ? col::Accent : (hovered ? col::BorderStrong : col::Border);
            r.drawRectScreen(cell.x, cell.y, cell.w, cell.h, border);
            if(selected)
                r.drawRectScreen(cell.x + 1, cell.y + 1, cell.w - 2, cell.h - 2, ui::withAlpha(col::Accent, 120));
        }

        // Progress "shutter" that uncovers the cameo from the top while an item is being produced
        void drawProgressShutter(Renderer &r, BitmapFontRenderer &font, const UiRect &cell, float progress, SDL_Color tint)
        {
            int coverH = static_cast<int>((cell.h - 20) * (1.0f - std::clamp(progress, 0.0f, 1.0f)));
            if(coverH > 0)
                r.fillRectScreen(cell.x + 1, cell.bottom() - 20 - coverH, cell.w - 2, coverH, {6, 8, 12, 160});
            r.fillRectScreen(cell.x + 1, cell.bottom() - 22, static_cast<int>((cell.w - 2) * progress), 2, tint);
            std::string pct = std::to_string(static_cast<int>(progress * 100.0f)) + "%";
            font.drawString(pct, cell.centerX(), cell.y + (cell.h - 20) / 2, ui::FONT_H2, ui::color::Text, true, Anchor::HCENTER | Anchor::VCENTER);
        }

        void drawLockBadge(Renderer &r, BitmapFontRenderer &font, const UiRect &cell)
        {
            font.drawString(_("LOCK"), cell.centerX(), cell.y + (cell.h - 20) / 2, ui::FONT_SMALL_BOLD, col::Danger, true, Anchor::HCENTER | Anchor::VCENTER);
            (void) r;
        }

        // Crosshair glyph used as the Orders tab icon
        void drawCrosshair(Renderer &r, int cx, int cy, int size, SDL_Color c)
        {
            r.drawRectScreen(cx - size / 2, cy - size / 2, size, size, c);
            r.fillRectScreen(cx - size / 2 - 4, cy, size + 9, 1, c);
            r.fillRectScreen(cx, cy - size / 2 - 4, 1, size + 9, c);
        }

    } // namespace

    int unitCameoCompId(int archetype)
    {
        for(bool infantry : {true, false})
        {
            for(Faction f : {Faction::Resistance, Faction::Confederation})
            {
                const bool res = (f == Faction::Resistance);
                UnitCatalog cat = infantry ? (res ? UnitCatalog {kResistanceInfantry, static_cast<int>(std::size(kResistanceInfantry))} : UnitCatalog {kConfederationInfantry, static_cast<int>(std::size(kConfederationInfantry))})
                                           : (res ? UnitCatalog {kResistanceVehicles, static_cast<int>(std::size(kResistanceVehicles))} : UnitCatalog {kConfederationVehicles, static_cast<int>(std::size(kConfederationVehicles))});
                for(int i = 0; i < cat.count; ++i)
                {
                    if(cat.items[i].archetype == archetype)
                        return cat.items[i].compId;
                }
            }
        }
        return -1;
    }

    // ---------------------------------------------------------------------------
    // Simulation-side state
    // ---------------------------------------------------------------------------

    void HUDController::setActiveTab(int tab)
    {
        if(tab >= 0 && tab < TabCount)
            m_activeTab = tab;
    }

    void HUDController::issue(SimCommand command)
    {
        if(!m_sim)
            return;
        command.player = Players::local();
        m_sim->issue(std::move(command));
    }

    void HUDController::update(float deltaTime, EntityManager &entities, Economy &economy, const std::vector<SimEvent> &events)
    {
        (void) economy;
        m_destroyArmed = std::max(0.0f, m_destroyArmed - deltaTime);
        for(const SimEvent &e : events)
        {
            if(e.type == SimEvent::Type::ResearchCompleted && e.player == Players::local())
                announceResearch(e.value);
        }

        // Validate & auto-assign primary barracks and factory if needed
        auto barracksList = entities.getBuildingsByType(UnitType::Barracks, Players::local());
        if(m_primaryBarracksId != 0)
        {
            const Entity *b = entities.getEntity(m_primaryBarracksId);
            if(!b || !b->alive || b->owner != Players::local() || b->type != UnitType::Barracks)
            {
                m_primaryBarracksId = 0;
            }
        }
        if(m_primaryBarracksId == 0 && !barracksList.empty())
        {
            m_primaryBarracksId = barracksList[0];
        }

        auto factoryList = entities.getBuildingsByType(UnitType::Factory, Players::local());
        if(m_primaryFactoryId != 0)
        {
            const Entity *f = entities.getEntity(m_primaryFactoryId);
            if(!f || !f->alive || f->owner != Players::local() || f->type != UnitType::Factory)
            {
                m_primaryFactoryId = 0;
            }
        }
        if(m_primaryFactoryId == 0 && !factoryList.empty())
        {
            m_primaryFactoryId = factoryList[0];
        }

        if(m_hudNotificationTimer > 0.0f)
        {
            m_hudNotificationTimer -= deltaTime;
            if(m_hudNotificationTimer <= 0.0f)
            {
                m_hudNotification.clear();
            }
        }
    }

    void HUDController::announceResearch(int upgradeIndex)
    {
        const ResearchDef &r = researchDef(playerFaction(), upgradeIndex);
        if(r.effect == ResearchEffect::FortressFlamethrower)
        {
            setNotification(_("FORTRESS FLAMETHROWER UPGRADE UNLOCKED!"), 3.0f);
        }
        else if(r.effect == ResearchEffect::BuildRadius)
        {
            setNotification(_("HQ BUILD RADIUS INCREASED (+25%)!"), 3.0f);
        }
        else
        {
            setNotification(std::string(_("RESEARCH COMPLETE: ")) + _(r.name), 3.0f);
        }
    }

    void HUDController::focusBuilding(int buildingId, const EntityManager &entities)
    {
        const Building *b = entities.getBuilding(buildingId);
        if(!b || b->owner != Players::local())
            return;
        switch(b->type)
        {
            case UnitType::Barracks:
                m_primaryBarracksId = b->id;
                setActiveTab(TabInfantry);
                break;
            case UnitType::Factory:
                m_primaryFactoryId = b->id;
                setActiveTab(TabVehicles);
                break;
            case UnitType::ResearchLab:
                setActiveTab(TabResearch);
                break;
            default:
                setActiveTab(TabBuild);
                break;
        }
        m_buildMode = false;
    }

    void HUDController::resetForMission()
    {
        m_primaryBarracksId = 0;
        m_primaryFactoryId = 0;
        m_buildMode = false;
        m_attackMoveMode = false;
        m_hudNotification.clear();
        m_hudNotificationTimer = 0.0f;
    }

    std::vector<int> HUDController::productionTargets(UnitType buildingType, const EntityManager &entities, const std::vector<int> &selected) const
    {
        auto usable = [&](int id)
        {
            const Building *b = entities.getBuilding(id);
            return b && b->alive && b->state != ActionState::Die && b->owner == Players::local() && b->type == buildingType && b->buildProgress >= 1.0f;
        };
        std::vector<int> targets;
        for(int id : selected)
        {
            if(usable(id))
                targets.push_back(id);
        }
        if(targets.empty())
        {
            const int primary = (buildingType == UnitType::Barracks) ? m_primaryBarracksId : m_primaryFactoryId;
            if(usable(primary))
                targets.push_back(primary);
        }
        return targets;
    }

    bool HUDController::orderUnit(int archetype, UnitType buildingType, EntityManager &entities, Economy &economy, const std::vector<int> &selected)
    {
        const std::vector<int> targets = productionTargets(buildingType, entities, selected);
        if(targets.empty())
            return false;

        // The selected building with the shortest queue trains it
        int best = targets.front();
        for(int id : targets)
        {
            if(entities.getBuilding(id)->productionQueue.size() < entities.getBuilding(best)->productionQueue.size())
                best = id;
        }
        const auto *cfg = entities.getUnitConfig(archetype);
        const int cost = cfg ? cfg->creditCost : 100;
        if(!economy.canAfford(cost))
        {
            setNotification(_("INSUFFICIENT CREDITS!"));
            return false;
        }
        SimCommand c;
        c.type = CommandType::Produce;
        c.a = archetype;
        c.b = best;
        issue(std::move(c));
        return true;
    }

    bool HUDController::cancelUnitOrder(int archetype, UnitType buildingType, EntityManager &entities, Economy &economy, const std::vector<int> &selected)
    {
        // Take the order back from the building that has most of that unit queued
        int best = 0;
        int bestCount = 0;
        for(int id : productionTargets(buildingType, entities, selected))
        {
            const Building *b = entities.getBuilding(id);
            int n = static_cast<int>(std::count_if(b->productionQueue.begin(), b->productionQueue.end(), [&](const ProductionItem &it) { return it.archetype == archetype; }));
            if(n >= bestCount && n > 0)
            {
                bestCount = n;
                best = id;
            }
        }
        (void) economy;
        if(best <= 0)
            return false;
        SimCommand c;
        c.type = CommandType::CancelProduce;
        c.a = archetype;
        c.b = best;
        issue(std::move(c));
        return true;
    }

    int HUDController::itemCountForTab(int tab) const
    {
        switch(tab)
        {
            case TabBuild:
                return std::min(8, static_cast<int>(EntityManager::getBlueprints().size()));
            case TabInfantry:
                return unitCatalog(true).count;
            case TabVehicles:
                return unitCatalog(false).count;
            case TabResearch:
                return kResearchCount;
            case TabOrders:
                return static_cast<int>(std::size(kOrders));
        }
        return 0;
    }

    int HUDController::maxScrollRows(int tab, int gridHeight) const
    {
        int rows = (itemCountForTab(tab) + SidebarLayout::kCols - 1) / SidebarLayout::kCols;
        int visible = std::max(1, (gridHeight + SidebarLayout::kGap) / (SidebarLayout::kCellH + SidebarLayout::kGap));
        return std::max(0, rows - visible);
    }

    void HUDController::activateCell(int tab, int index, bool cancel, Economy &economy, EntityManager &entities, std::vector<int> &selectedUnits)
    {
        if(cancel)
        {
            // Right click on a unit card takes one order back (with a refund)
            if(tab == TabInfantry || tab == TabVehicles)
            {
                const bool inf = (tab == TabInfantry);
                cancelUnitOrder(unitCatalog(inf).items[index].archetype, inf ? UnitType::Barracks : UnitType::Factory, entities, economy, selectedUnits);
            }
            return;
        }
        switch(tab)
        {
            case TabBuild:
            {
                const auto &blueprints = EntityManager::getBlueprints();
                if(index < static_cast<int>(blueprints.size()) && blueprints[index].buildingIndex == 6 && !isRadarUnlocked(Players::local()))
                {
                    setNotification(_("RESEARCH REQUIRED TO BUILD RADAR LOCATORS!"));
                }
                else if(index < static_cast<int>(blueprints.size()))
                {
                    m_selectedBlueprint = index;
                    m_buildMode = true;
                    m_attackMoveMode = false;
                }
                break;
            }
            case TabInfantry:
                if(!isUnitUnlocked(Players::local(), unitCatalog(true).items[index].archetype))
                {
                    setNotification(_("RESEARCH REQUIRED TO TRAIN THIS UNIT!"));
                }
                else if(!entities.hasBuildingType(UnitType::Barracks, playerFaction()))
                {
                    setNotification(_("BARRACKS REQUIRED FOR INFANTRY RECRUITMENT!"));
                }
                else
                {
                    orderUnit(unitCatalog(true).items[index].archetype, UnitType::Barracks, entities, economy, selectedUnits);
                }
                break;
            case TabVehicles:
                if(!isUnitUnlocked(Players::local(), unitCatalog(false).items[index].archetype))
                {
                    setNotification(_("RESEARCH REQUIRED TO BUILD THIS UNIT!"));
                }
                else if(!entities.hasBuildingType(UnitType::Factory, playerFaction()))
                {
                    setNotification(_("VEHICLE FACTORY REQUIRED FOR PRODUCTION!"));
                }
                else
                {
                    orderUnit(unitCatalog(false).items[index].archetype, UnitType::Factory, entities, economy, selectedUnits);
                }
                break;
            case TabResearch:
            {
                const auto &r = researchDef(playerFaction(), index);
                if(!hasResearchBuilding(Players::local(), entities))
                {
                    setNotification(playerFaction() == Faction::Confederation ? _("TECH CENTER REQUIRED FOR RESEARCH!") : _("LABORATORY REQUIRED FOR RESEARCH!"));
                }
                else if(isResearched(Players::local(), index))
                {
                    setNotification(_("THIS TECHNOLOGY IS ALREADY RESEARCHED!"));
                }
                else if(!isResearchAvailable(Players::local(), index))
                {
                    setNotification(std::string(_("REQUIRES: ")) + _(researchDef(playerFaction(), r.needs).name));
                }
                else if(m_sim && m_sim->research(Players::local()).active >= 0)
                {
                    setNotification(_("ANOTHER RESEARCH IS ALREADY IN PROGRESS!"));
                }
                else if(economy.canAfford(r.cost))
                {
                    SimCommand c;
                    c.type = CommandType::StartResearch;
                    c.a = index;
                    issue(std::move(c));
                }
                else
                {
                    setNotification(_("INSUFFICIENT CREDITS FOR RESEARCH!"));
                }
                break;
            }
            case TabOrders:
                switch(index)
                {
                    case 0:
                    { // Attack-move
                        bool hasCombat = false;
                        for(int uid : selectedUnits)
                        {
                            const Entity *e = entities.getEntity(uid);
                            if(e && e->alive && !e->isBuilding)
                            {
                                hasCombat = true;
                                break;
                            }
                        }
                        if(hasCombat)
                        {
                            m_attackMoveMode = !m_attackMoveMode;
                            m_buildMode = false;
                        }
                        break;
                    }
                    case 1: // Siege / transform
                    case 2: // Stop
                    case 3:
                    { // Defend: hold position
                        static const CommandType kTypes[] = {CommandType::ToggleSiege, CommandType::Stop, CommandType::HoldPosition};
                        SimCommand c;
                        c.type = kTypes[index - 1];
                        c.ids.assign(selectedUnits.begin(), selectedUnits.end());
                        issue(std::move(c));
                        break;
                    }
                }
                break;
        }
    }

    void HUDController::cyclePrimaryBuilding(int tab, EntityManager &entities, Camera &camera)
    {
        bool infantry = (tab == TabInfantry);
        auto list = entities.getBuildingsByType(infantry ? UnitType::Barracks : UnitType::Factory, Players::local());
        if(list.empty())
            return;
        int &primary = infantry ? m_primaryBarracksId : m_primaryFactoryId;
        auto it = std::find(list.begin(), list.end(), primary);
        primary = (it == list.end() || std::next(it) == list.end()) ? list.front() : *std::next(it);
        if(const Entity *ent = entities.getEntity(primary))
        {
            camera.centerOn(ent->tileX * TILE_WIDTH, ent->tileY * TILE_HEIGHT);
        }
    }

    void HUDController::updateRadarDrag(int mx, int my, bool leftMouseDown, float frameDeltaTime, int windowW, int windowH, const TileMap &map, Camera &camera)
    {
        if(!leftMouseDown)
            m_radarDragging = false;
        if(!m_radarDragging)
            return;
        const SidebarLayout l = sidebarLayout(windowW, windowH, m_topInset);
        const RadarView v = radarView(l.radar, map.getWidth(), map.getHeight(), m_radarZoom, m_radarCenterX, m_radarCenterY);
        if(v.w <= 0 || v.h <= 0)
            return;
        // Outside the radar the point sticks to its edge
        const float px = std::clamp(static_cast<float>(mx), static_cast<float>(v.ox), static_cast<float>(v.ox + v.w));
        const float py = std::clamp(static_cast<float>(my), static_cast<float>(v.oy), static_cast<float>(v.oy + v.h));
        const float targetX = (v.viewX + (px - v.ox) / v.scale) * TILE_WIDTH;
        const float targetY = (v.viewY + (py - v.oy) / v.scale) * TILE_HEIGHT;
        // One radar pixel is more than a tile: easing instead of following the pixel steps keeps it fluid,
        // and the exponential form gives the same feel at any frame rate
        const float halfW = camera.getViewportWidth() * 0.5f / camera.getZoom();
        const float halfH = camera.getViewportHeight() * 0.5f / camera.getZoom();
        const float curX = camera.getX() + halfW, curY = camera.getY() + halfH;
        const float k = 1.0f - std::exp(-frameDeltaTime * kRadarFollowRate);
        camera.centerOn(curX + (targetX - curX) * k, curY + (targetY - curY) * k);
    }

    bool HUDController::handleHUDClick(
        int mx, int my, bool leftClicked, bool rightClicked, bool leftMouseDown, int wheelSteps, int windowW, int windowH, Economy &economy, EntityManager &entities, TileMap &map, Camera &camera, bool &outOpenMenu, bool &outOpenObjectives, std::vector<int> &selectedUnits)
    {
        if(!leftMouseDown)
            m_radarDragging = false;
        // A radar drag owns the mouse until the button is released, also over the map (no box selection)
        if(m_radarDragging)
            return true;
        if(!isOverSidebar(mx, windowW))
            return false;

        SidebarLayout l = sidebarLayout(windowW, windowH, m_topInset);

        // Radar: wheel and the +/- buttons zoom, click or drag moves the camera
        if(l.radar.contains(mx, my))
        {
            if(wheelSteps != 0)
            {
                m_radarZoom = std::clamp(m_radarZoom + (wheelSteps > 0 ? 1 : -1), 1, kRadarMaxZoom);
            }
            if(leftClicked && !m_radarDragging)
            {
                if(radarZoomInBtn(l.radar).contains(mx, my))
                {
                    m_radarZoom = std::min(kRadarMaxZoom, m_radarZoom + 1);
                    return true;
                }
                if(radarZoomOutBtn(l.radar).contains(mx, my))
                {
                    m_radarZoom = std::max(1, m_radarZoom - 1);
                    return true;
                }
            }
            const bool onButtons = radarZoomInBtn(l.radar).contains(mx, my) || radarZoomOutBtn(l.radar).contains(mx, my);
            if((leftMouseDown || leftClicked) && !onButtons)
            {
                // The click jumps there at once; holding the button goes on in updateRadarDrag, every frame
                RadarView v = radarView(l.radar, map.getWidth(), map.getHeight(), m_radarZoom, m_radarCenterX, m_radarCenterY);
                if(v.w > 0 && v.h > 0)
                {
                    m_radarDragging = leftMouseDown;
                    float tx = std::clamp(v.viewX + static_cast<float>(mx - v.ox) / v.scale, 0.0f, static_cast<float>(map.getWidth()));
                    float ty = std::clamp(v.viewY + static_cast<float>(my - v.oy) / v.scale, 0.0f, static_cast<float>(map.getHeight()));
                    camera.centerOn(tx * TILE_WIDTH, ty * TILE_HEIGHT);
                }
            }
            return true;
        }

        int maxScroll = maxScrollRows(m_activeTab, l.grid.h);
        m_scrollRow[m_activeTab] = std::clamp(m_scrollRow[m_activeTab] - wheelSteps, 0, maxScroll);

        if(rightClicked && l.grid.contains(mx, my))
        {
            int count = itemCountForTab(m_activeTab);
            for(int i = 0; i < count; ++i)
            {
                UiRect c = l.cell(i, m_scrollRow[m_activeTab]);
                if(c.contains(mx, my))
                {
                    activateCell(m_activeTab, i, true, economy, entities, selectedUnits);
                    break;
                }
            }
            return true;
        }
        if(!leftClicked)
            return true;

        // Destroy the selected own structures: the first click arms the button, the second one confirms
        if(destroyBtn(l.info).contains(mx, my))
        {
            const auto own = ownSelectedBuildings(entities, selectedUnits);
            if(own.empty())
                return true;
            if(m_destroyArmed <= 0.0f)
            {
                m_destroyArmed = 3.0f;
                return true;
            }
            m_destroyArmed = 0.0f;
            SimCommand c;
            c.type = CommandType::Demolish;
            for(const Building *b : own)
                c.ids.push_back(static_cast<uint32_t>(b->id));
            // They are gone on the tick the command runs: out of the selection already
            selectedUnits.erase(std::remove_if(selectedUnits.begin(), selectedUnits.end(), [&](int id) { return std::find(c.ids.begin(), c.ids.end(), static_cast<uint32_t>(id)) != c.ids.end(); }), selectedUnits.end());
            issue(std::move(c));
            return true;
        }

        if(l.menuBtn.contains(mx, my))
        {
            outOpenMenu = true;
            return true;
        }
        if(l.objectivesBtn.contains(mx, my))
        {
            outOpenObjectives = true;
            return true;
        }
        for(int t = 0; t < TabCount; ++t)
        {
            if(l.tabs[t].contains(mx, my))
            {
                m_activeTab = t;
                return true;
            }
        }
        if(maxScroll > 0 && l.scrollUp.contains(mx, my))
        {
            m_scrollRow[m_activeTab] = std::max(0, m_scrollRow[m_activeTab] - 1);
            return true;
        }
        if(maxScroll > 0 && l.scrollDown.contains(mx, my))
        {
            m_scrollRow[m_activeTab] = std::min(maxScroll, m_scrollRow[m_activeTab] + 1);
            return true;
        }
        if((m_activeTab == TabInfantry || m_activeTab == TabVehicles) && l.primaryBtn.contains(mx, my))
        {
            cyclePrimaryBuilding(m_activeTab, entities, camera);
            return true;
        }
        if(l.grid.contains(mx, my))
        {
            int count = itemCountForTab(m_activeTab);
            for(int i = 0; i < count; ++i)
            {
                UiRect c = l.cell(i, m_scrollRow[m_activeTab]);
                if(c.contains(mx, my) && c.y >= l.grid.y && c.bottom() <= l.grid.bottom() + SidebarLayout::kGap)
                {
                    activateCell(m_activeTab, i, false, economy, entities, selectedUnits);
                    break;
                }
            }
        }
        return true;
    }

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

    void HUDController::renderGameHUD(Renderer &renderer, BitmapFontRenderer &fontRenderer, const Economy &economy, const EntityManager &entities, const TileMap &map, const Camera &camera, int mx, int my, int windowW, int windowH, const std::vector<int> &selected)
    {
        SidebarLayout l = sidebarLayout(windowW, windowH, m_topInset);

        // Frame
        ui::fillVerticalGradient(renderer, l.panel, {20, 24, 32, 255}, {12, 15, 20, 255});
        renderer.fillRectScreen(l.panel.x, 0, 1, windowH, col::BorderStrong);
        renderer.fillRectScreen(l.panel.x + 1, 0, 1, windowH, {0, 0, 0, 120});

        // --- Header: credits & power ---------------------------------------------------------
        {
            const UiRect &h = l.header;
            std::string credits = "$ " + std::to_string(economy.getCredits());
            fontRenderer.drawString(credits, h.x, h.y - 2, ui::FONT_H2, col::Accent, true);

            int prod = economy.getPowerProduced();
            int cons = economy.getPowerConsumed();
            bool deficit = economy.hasPowerDeficit();
            SDL_Color pCol = deficit ? col::Danger : col::Ok;
            std::string pwr = std::to_string(cons) + " / " + std::to_string(prod) + _(" kW");
            fontRenderer.drawString(pwr, h.right(), h.y + 2, ui::FONT_SMALL_BOLD, pCol, false, Anchor::RIGHT | Anchor::TOP);
            fontRenderer.drawString(_("POWER"), h.right(), h.y + 17, ui::FONT_SMALL, col::TextMuted, false, Anchor::RIGHT | Anchor::TOP);

            float ratio = static_cast<float>(cons) / static_cast<float>(std::max(1, prod));
            ui::drawProgressBar(renderer, {h.x, h.bottom() - 6, h.w, 6}, ratio, pCol);
        }

        // --- Radar -----------------------------------------------------------------------------
        {
            const UiRect &rd = l.radar;
            renderer.fillRectScreen(rd.x, rd.y, rd.w, rd.h, {6, 12, 10, 255});
            const int mapW = map.getWidth();
            const int mapH = map.getHeight();
            // A zoomed radar follows the camera, except while the player drags on it (that moves the camera)
            const float camCenterX = (camera.getX() + camera.getViewportWidth() * 0.5f / camera.getZoom()) / TILE_WIDTH;
            const float camCenterY = (camera.getY() + camera.getViewportHeight() * 0.5f / camera.getZoom()) / TILE_HEIGHT;
            if(!m_radarDragging || m_radarCenterX < 0.0f)
            {
                m_radarCenterX = camCenterX;
                m_radarCenterY = camCenterY;
            }
            RadarView v = radarView(rd, mapW, mapH, m_radarZoom, m_radarCenterX, m_radarCenterY);
            if(v.w > 0 && v.h > 0)
            {
                renderer.setClipRect({v.ox, v.oy, v.w, v.h});
                renderer.fillRectScreen(v.ox, v.oy, v.w, v.h, {14, 30, 22, 255});
                const bool fog = m_radarFog && map.isFogEnabled();
                const int tx0 = std::max(0, static_cast<int>(v.viewX));
                const int ty0 = std::max(0, static_cast<int>(v.viewY));
                const int tx1 = std::min(mapW, static_cast<int>(std::ceil(v.viewX + v.w / v.scale)) + 1);
                const int ty1 = std::min(mapH, static_cast<int>(std::ceil(v.viewY + v.h / v.scale)) + 1);
                // Cells from rounded edges: neighbours meet exactly, nothing shimmers as the view moves
                auto cellRect = [&](float x, float y, float w, float h, int &px, int &py, int &pw, int &ph)
                {
                    px = static_cast<int>(std::lround(v.toScreenX(x)));
                    py = static_cast<int>(std::lround(v.toScreenY(y)));
                    pw = std::max(1, static_cast<int>(std::lround(v.toScreenX(x + w))) - px);
                    ph = std::max(1, static_cast<int>(std::lround(v.toScreenY(y + h))) - py);
                };
                const SDL_Color hiddenCol {4, 6, 8, 255};
                for(int ty = ty0; ty < ty1; ++ty)
                {
                    for(int tx = tx0; tx < tx1; ++tx)
                    {
                        int px, py, pw, ph;
                        cellRect(static_cast<float>(tx), static_cast<float>(ty), 1.0f, 1.0f, px, py, pw, ph);
                        const FogState fs = fog ? map.getFogState(tx, ty) : FogState::Visible;
                        if(fs == FogState::Hidden)
                        {
                            renderer.fillRectScreen(px, py, pw, ph, hiddenCol);
                            continue;
                        }
                        if(map.getWall(tx, ty) > 0)
                        {
                            renderer.fillRectScreen(px, py, pw, ph, radarFactionColor(map.getWallFaction(tx, ty), false));
                        }
                        else if(!map.isPassable(tx, ty))
                        {
                            renderer.fillRectScreen(px, py, pw, ph, {30, 54, 40, 255});
                        }
                    }
                }

                for(const auto &e : entities.getAllEntities())
                {
                    if(!e.alive || e.state == ActionState::Die)
                        continue;
                    const bool own = (e.owner == Players::local());
                    const Building *b = e.asBuilding();
                    if(fog && !own)
                    {
                        if(b)
                        {
                            // Enemy structures appear once they have been seen (never while they are being built out of sight)
                            if(!entities.isHostileBuildingShown(*b, map))
                                continue;
                        }
                        else if(!entities.isEntityVisibleToPlayer(e, Players::local(), map))
                        {
                            continue; // units only while in sight
                        }
                    }
                    int px, py, pw, ph;
                    if(b)
                    {
                        cellRect(static_cast<float>(b->tileX), static_cast<float>(b->tileY), static_cast<float>(b->buildWidth), static_cast<float>(b->buildHeight), px, py, pw, ph);
                        renderer.fillRectScreen(px, py, std::max(3, pw), std::max(3, ph), radarFactionColor(e.faction, false));
                    }
                    else
                    {
                        cellRect(e.getUnitCenterPixelX() / TILE_WIDTH - 0.5f, e.getUnitCenterPixelY() / TILE_HEIGHT - 0.5f, 1.0f, 1.0f, px, py, pw, ph);
                        const int s = std::max(2, std::min(pw, ph));
                        renderer.fillRectScreen(px + (pw - s) / 2, py + (ph - s) / 2, s, s, radarFactionColor(e.faction, true));
                    }
                }

                // Camera frame from rounded edges (truncating position and size separately made it wobble)
                const float camWorldW = camera.getViewportWidth() / camera.getZoom();
                const float camWorldH = camera.getViewportHeight() / camera.getZoom();
                int fx, fy, fw, fh;
                cellRect(camera.getX() / TILE_WIDTH, camera.getY() / TILE_HEIGHT, camWorldW / TILE_WIDTH, camWorldH / TILE_HEIGHT, fx, fy, fw, fh);
                renderer.drawRectScreen(fx, fy, std::max(6, fw), std::max(4, fh), {255, 255, 255, 220});
                renderer.resetClipRect();
            }
            renderer.drawRectScreen(rd.x, rd.y, rd.w, rd.h, col::BorderStrong);

            // Zoom buttons
            const UiRect zoomBtns[2] = {radarZoomOutBtn(rd), radarZoomInBtn(rd)};
            for(int k = 0; k < 2; ++k)
            {
                const UiRect &b = zoomBtns[k];
                const bool enabled = (k == 0) ? m_radarZoom > 1 : m_radarZoom < kRadarMaxZoom;
                const bool hot = enabled && b.contains(mx, my);
                renderer.fillRectScreen(b.x, b.y, b.w, b.h, hot ? col::Hover : SDL_Color {10, 16, 14, 210});
                renderer.drawRectScreen(b.x, b.y, b.w, b.h, enabled ? col::BorderStrong : col::Border);
                const SDL_Color sc = enabled ? col::Text : col::TextMuted;
                renderer.fillRectScreen(b.x + 5, b.centerY() - 1, b.w - 10, 2, sc);
                if(k == 1)
                    renderer.fillRectScreen(b.centerX() - 1, b.y + 5, 2, b.h - 10, sc);
            }
            if(m_radarZoom > 1)
            {
                fontRenderer.drawString("x" + std::to_string(m_radarZoom), rd.x + 6, rd.bottom() - 12, ui::FONT_SMALL_BOLD, col::TextDim, false, Anchor::LEFT | Anchor::VCENTER);
            }
        }

        // --- Menu / objectives -------------------------------------------------------------------
        ui::drawButton(renderer, fontRenderer, l.menuBtn, _("MENU"), ui::ButtonStyle::Secondary, l.menuBtn.contains(mx, my));
        ui::drawButton(renderer, fontRenderer, l.objectivesBtn, _("OBJECTIVES"), ui::ButtonStyle::Secondary, l.objectivesBtn.contains(mx, my));

        // --- Tabs ----------------------------------------------------------------------------------
        for(int t = 0; t < TabCount; ++t)
        {
            const UiRect &tr = l.tabs[t];
            bool active = (t == m_activeTab);
            bool hovered = tr.contains(mx, my);
            renderer.fillRectScreen(tr.x, tr.y, tr.w, tr.h, active ? col::Hover : (hovered ? col::PanelRaised : col::PanelSunken));
            renderer.drawRectScreen(tr.x, tr.y, tr.w, tr.h, active ? col::BorderStrong : col::Border);
            if(active)
                renderer.fillRectScreen(tr.x, tr.bottom() - 3, tr.w, 3, col::Accent);

            if(tabIcon(t) >= 0)
            {
                renderer.setClipRect({tr.x + 1, tr.y + 1, tr.w - 2, tr.h - 4});
                float s = 0.75f;
                int iw = static_cast<int>(kCameoW * s);
                renderer.drawCompositeScreen(tabIcon(t), tr.centerX() - iw / 2, tr.y + 6, s, playerFaction());
                renderer.resetClipRect();
                if(!active)
                    renderer.fillRectScreen(tr.x + 1, tr.y + 1, tr.w - 2, tr.h - 4, {10, 12, 16, hovered ? Uint8(60) : Uint8(110)});
            }
            else
            {
                drawCrosshair(renderer, tr.centerX(), tr.centerY() - 1, 16, active ? col::Accent : col::TextDim);
            }
        }

        // --- Strip: tab title, primary building, scroll arrows ---------------------------------------
        int maxScroll = maxScrollRows(m_activeTab, l.grid.h);
        int &scroll = m_scrollRow[m_activeTab];
        scroll = std::clamp(scroll, 0, maxScroll);
        {
            ui::drawCaption(fontRenderer, renderer, l.strip.x, l.strip.y + 4, _(kTabNames[m_activeTab]));

            if(m_activeTab == TabInfantry || m_activeTab == TabVehicles)
            {
                bool inf = (m_activeTab == TabInfantry);
                int primary = inf ? m_primaryBarracksId : m_primaryFactoryId;
                auto list = entities.getBuildingsByType(inf ? UnitType::Barracks : UnitType::Factory, Players::local());
                if(!list.empty())
                {
                    std::string label = std::string(inf ? _("Barracks") : _("Factory")) + " #" + std::to_string(primary);
                    if(list.size() > 1)
                        label += "  \xE2\x80\xBA";
                    bool hot = list.size() > 1 && l.primaryBtn.contains(mx, my);
                    fontRenderer.drawString(ui::fitText(fontRenderer, label, ui::FONT_SMALL, l.primaryBtn.w), l.primaryBtn.right(), l.primaryBtn.centerY(), ui::FONT_SMALL, hot ? col::Accent : col::TextDim, false, Anchor::RIGHT | Anchor::VCENTER);
                }
            }
            else if(m_activeTab == TabOrders)
            {
                int selected = 0;
                for(const auto &e : entities.getAllEntities())
                {
                    if(e.alive && e.selected && !e.isBuilding)
                        ++selected;
                }
                std::string label = std::string(_("Selected")) + ": " + std::to_string(selected);
                fontRenderer.drawString(label, l.primaryBtn.right(), l.primaryBtn.centerY(), ui::FONT_SMALL, col::TextDim, false, Anchor::RIGHT | Anchor::VCENTER);
            }

            if(maxScroll > 0)
            {
                auto arrow = [&](const UiRect &r, bool up, bool enabled)
                {
                    bool hot = enabled && r.contains(mx, my);
                    renderer.fillRectScreen(r.x, r.y, r.w, r.h, hot ? col::Hover : col::PanelRaised);
                    renderer.drawRectScreen(r.x, r.y, r.w, r.h, hot ? col::BorderStrong : col::Border);
                    SDL_Color c = enabled ? (hot ? col::Accent : col::Text) : col::TextMuted;
                    for(int i = 0; i < 5; ++i)
                    {
                        int yy = up ? (r.centerY() - 2 + i) : (r.centerY() + 2 - i);
                        renderer.fillRectScreen(r.centerX() - i, yy, 2 * i + 1, 1, c);
                    }
                };
                arrow(l.scrollUp, true, scroll > 0);
                arrow(l.scrollDown, false, scroll < maxScroll);
            }
        }

        // --- Cameo grid -----------------------------------------------------------------------------------
        std::string hoverTitle;
        std::string hoverText;
        bool hasBarracks = entities.hasBuildingType(UnitType::Barracks, playerFaction());
        bool hasFactory = entities.hasBuildingType(UnitType::Factory, playerFaction());
        bool hasLab = hasResearchBuilding(Players::local(), entities);
        // Orders shown on unit cards come from the buildings that would train them
        auto queuedAt = [&](UnitType buildingType)
        {
            std::vector<const Building *> out;
            for(int id : productionTargets(buildingType, entities, selected))
                out.push_back(entities.getBuilding(id));
            return out;
        };
        const std::vector<const Building *> barracksTargets = queuedAt(UnitType::Barracks);
        const std::vector<const Building *> factoryTargets = queuedAt(UnitType::Factory);

        renderer.setClipRect({l.grid.x, l.grid.y, l.grid.w, l.grid.h});
        int count = itemCountForTab(m_activeTab);
        for(int i = 0; i < count; ++i)
        {
            UiRect c = l.cell(i, scroll);
            if(c.bottom() < l.grid.y || c.y > l.grid.bottom())
                continue;
            bool hovered = c.contains(mx, my) && l.grid.contains(mx, my);

            switch(m_activeTab)
            {
                case TabBuild:
                {
                    const auto &bp = EntityManager::getBlueprints()[i];
                    bool selected = (m_buildMode && m_selectedBlueprint == i);
                    bool canAfford = economy.canAfford(bp.creditCost);
                    const bool radarLocked = bp.buildingIndex == 6 && !isRadarUnlocked(Players::local());
                    drawCameo(renderer, fontRenderer, c, bp.iconCompId, _(bp.name), hovered, selected, radarLocked);
                    drawCostTag(renderer, fontRenderer, c, "$" + std::to_string(bp.creditCost), canAfford ? col::Accent : col::Danger);
                    std::string pw = (bp.powerCost >= 0 ? "+" : "") + std::to_string(bp.powerCost);
                    fontRenderer.drawString(pw, c.x + 6, c.y + 5, ui::FONT_SMALL_BOLD, bp.powerCost >= 0 ? col::Ok : SDL_Color {255, 170, 80, 255}, true);
                    if(hovered)
                    {
                        hoverTitle = std::string(_(bp.name)) + "  $" + std::to_string(bp.creditCost) + ", " + pw + _(" kW");
                        hoverText = _(bp.desc);
                    }
                    break;
                }
                case TabInfantry:
                case TabVehicles:
                {
                    bool inf = (m_activeTab == TabInfantry);
                    const UnitCameoDef &def = unitCatalog(inf).items[i];
                    const auto &producers = inf ? barracksTargets : factoryTargets;
                    const bool unlocked = isUnitUnlocked(Players::local(), def.archetype);
                    bool available = (inf ? hasBarracks : hasFactory) && unlocked;
                    int cost = unitCost(def, entities);
                    std::string name = _(unitName(def, entities));
                    drawCameo(renderer, fontRenderer, c, def.compId, name, hovered, false, !available);

                    int queued = 0;
                    float progress = -1.0f; // most advanced order of this unit currently in training
                    for(const Building *b : producers)
                    {
                        for(const auto &q : b->productionQueue)
                        {
                            if(q.archetype == def.archetype)
                                ++queued;
                        }
                        if(!b->productionQueue.empty() && b->productionQueue.front().archetype == def.archetype)
                        {
                            progress = std::max(progress, b->currentProductionProgress());
                        }
                    }
                    if(!available)
                    {
                        drawLockBadge(renderer, fontRenderer, c);
                    }
                    else
                    {
                        drawCostTag(renderer, fontRenderer, c, "$" + std::to_string(cost), economy.canAfford(cost) ? col::Accent : col::Danger);
                        if(progress >= 0.0f)
                        {
                            drawProgressShutter(renderer, fontRenderer, c, progress, col::Accent);
                        }
                        if(queued > 0)
                        {
                            std::string badge = "\xC3\x97" + std::to_string(queued);
                            renderer.fillRectScreen(c.x + 3, c.y + 3, fontRenderer.getTextWidth(badge, ui::FONT_SMALL_BOLD) + 10, 18, col::Accent);
                            fontRenderer.drawString(badge, c.x + 8, c.y + 4, ui::FONT_SMALL_BOLD, col::AccentText, false);
                        }
                    }
                    if(hovered)
                    {
                        hoverTitle = name + "  $" + std::to_string(cost);
                        if(!unlocked)
                        {
                            hoverText = std::string(_("[RESEARCH REQUIRED] ")) + _(researchDef(playerFaction(), unitUnlockResearch(playerFaction(), def.archetype)).name);
                        }
                        else if(!available)
                        {
                            hoverText = inf ? _("[BARRACKS REQUIRED] Construct Barracks to recruit ") + name : _("[FACTORY REQUIRED] Construct Factory to build ") + name;
                        }
                        else
                        {
                            hoverText = _(unitDesc(def, entities));
                            if(queued > 0)
                                hoverText += std::string("  ") + _("(RMB - cancel one)");
                        }
                    }
                    break;
                }
                case TabResearch:
                {
                    const auto &r = researchDef(playerFaction(), i);
                    bool done = isResearched(Players::local(), i);
                    const ResearchProgress *progress = m_sim ? &m_sim->research(Players::local()) : nullptr;
                    bool active = progress && progress->active == i;
                    const bool reqMet = isResearchAvailable(Players::local(), i);
                    drawCameo(renderer, fontRenderer, c, r.compId, _(r.name), hovered, done, !hasLab || !reqMet);
                    if(!hasLab)
                    {
                        drawLockBadge(renderer, fontRenderer, c);
                    }
                    else if(!reqMet)
                    {
                        drawLockBadge(renderer, fontRenderer, c);
                    }
                    else if(done)
                    {
                        drawCostTag(renderer, fontRenderer, c, _("READY"), col::Ok);
                    }
                    else if(active)
                    {
                        drawProgressShutter(renderer, fontRenderer, c, progress->timer / std::max(0.1f, progress->duration), col::Info);
                    }
                    else
                    {
                        drawCostTag(renderer, fontRenderer, c, "$" + std::to_string(r.cost), economy.canAfford(r.cost) ? col::Accent : col::Danger);
                    }
                    if(hovered)
                    {
                        hoverTitle = std::string(_(r.name)) + (done ? "" : "  $" + std::to_string(r.cost));
                        hoverText = !hasLab ? std::string(playerFaction() == Faction::Confederation ? _("[TECH CENTER REQUIRED] Research is paused until a Tech Center is built") : _("[LAB REQUIRED] Research is paused until a Laboratory is built"))
                                            : (done ? std::string(_("[RESEARCHED] ")) + _(r.desc) : (reqMet ? std::string(_(r.desc)) : std::string(_("[REQUIRES] ")) + _(researchDef(playerFaction(), r.needs).name)));
                    }
                    break;
                }
                case TabOrders:
                {
                    const auto &o = kOrders[i];
                    bool active = (i == 0 && m_attackMoveMode);
                    renderer.fillRectScreen(c.x, c.y, c.w, c.h, active ? SDL_Color {60, 44, 20, 255} : (hovered ? col::Hover : col::PanelRaised));
                    renderer.drawRectScreen(c.x, c.y, c.w, c.h, active ? col::Accent : (hovered ? col::BorderStrong : col::Border));
                    fontRenderer.drawString(ui::fitText(fontRenderer, _(o.name), ui::FONT_BODY_BOLD, c.w - 16), c.centerX(), c.centerY() + 6, ui::FONT_BODY_BOLD, active ? col::Accent : col::Text, false, Anchor::HCENTER | Anchor::VCENTER);
                    if(o.key[0] != '\0')
                    {
                        UiRect chip {c.x + 6, c.y + 6, 22, 22};
                        renderer.fillRectScreen(chip.x, chip.y, chip.w, chip.h, col::PanelSunken);
                        renderer.drawRectScreen(chip.x, chip.y, chip.w, chip.h, col::BorderStrong);
                        fontRenderer.drawString(o.key, chip.centerX(), chip.centerY(), ui::FONT_SMALL_BOLD, col::Accent, false, Anchor::HCENTER | Anchor::VCENTER);
                    }
                    if(hovered)
                    {
                        hoverTitle = _(o.name);
                        if(o.key[0] != '\0')
                            hoverTitle += std::string("  [") + o.key + "]";
                        hoverText = _(o.desc);
                    }
                    break;
                }
            }
        }
        renderer.resetClipRect();

        // Scroll position hint on the right edge of the grid
        if(maxScroll > 0)
        {
            int trackX = l.panel.right() - 5;
            renderer.fillRectScreen(trackX, l.grid.y, 2, l.grid.h, col::PanelRaised);
            int rows = maxScroll + l.visibleRows();
            int thumbH = std::max(16, l.grid.h * l.visibleRows() / rows);
            int thumbY = l.grid.y + (l.grid.h - thumbH) * scroll / maxScroll;
            renderer.fillRectScreen(trackX, thumbY, 2, thumbH, col::Accent);
        }

        // --- Info box -------------------------------------------------------------------------------------
        {
            const UiRect &ib = l.info;
            ui::drawPanel(renderer, ib, col::PanelSunken, col::Border);
            int textW = ib.w - 20;

            if(!m_hudNotification.empty())
            {
                renderer.fillRectScreen(ib.x, ib.y, 3, ib.h, col::Accent);
                fontRenderer.drawTextWrapped(m_hudNotification, ib.x + 10, ib.y + 8, textW, ui::FONT_SMALL_BOLD, col::Accent, false);
            }
            else if(!hoverTitle.empty())
            {
                fontRenderer.drawString(ui::fitText(fontRenderer, hoverTitle, ui::FONT_SMALL_BOLD, textW), ib.x + 10, ib.y + 7, ui::FONT_SMALL_BOLD, col::Text, false);
                fontRenderer.drawTextWrapped(hoverText, ib.x + 10, ib.y + 26, textW, ui::FONT_SMALL, col::TextDim, false);
            }
            else if(m_buildMode && m_selectedBlueprint >= 0 && m_selectedBlueprint < static_cast<int>(EntityManager::getBlueprints().size()))
            {
                fontRenderer.drawString(_(EntityManager::getBlueprints()[m_selectedBlueprint].name), ib.x + 10, ib.y + 7, ui::FONT_SMALL_BOLD, col::Accent, false);
                fontRenderer.drawTextWrapped(_("LMB - build, RMB / Esc - cancel"), ib.x + 10, ib.y + 26, textW, ui::FONT_SMALL, col::TextDim, false);
            }
            else if(
                const Building *busy = [&]() -> const Building *
                {
                    const auto &list = (m_activeTab == TabVehicles) ? factoryTargets : barracksTargets;
                    for(const Building *b : list)
                    {
                        if(!b->productionQueue.empty())
                            return b;
                    }
                    return nullptr;
                }();
                busy && (m_activeTab == TabInfantry || m_activeTab == TabVehicles))
            {
                const auto &item = busy->productionQueue.front();
                std::string what = _("Production");
                if(const auto *cfg = entities.getUnitConfig(item.archetype))
                    what = _(cfg->name);
                float prog = busy->currentProductionProgress();
                std::string line = what + "  " + std::to_string(static_cast<int>(prog * 100.0f)) + "%";
                if(busy->productionQueue.size() > 1)
                {
                    line += "   " + std::string(_("queue")) + ": " + std::to_string(busy->productionQueue.size() - 1);
                }
                fontRenderer.drawString(ui::fitText(fontRenderer, line, ui::FONT_SMALL_BOLD, textW), ib.x + 10, ib.y + 7, ui::FONT_SMALL_BOLD, col::Text, false);
                ui::drawProgressBar(renderer, {ib.x + 10, ib.y + 30, textW, 8}, prog, col::Accent);
            }
            else if(const auto own = ownSelectedBuildings(entities, selected); !own.empty())
            {
                const Building *b = own.front();
                std::string title = _("Structure");
                if(const BuildingConfig *cfg = entities.getBuildingConfig(b->rawBuildingType))
                    title = _(cfg->name);
                if(own.size() > 1)
                    title += "  x" + std::to_string(own.size());
                fontRenderer.drawString(ui::fitText(fontRenderer, title, ui::FONT_SMALL_BOLD, textW), ib.x + 10, ib.y + 7, ui::FONT_SMALL_BOLD, col::Text, false);
                const float hp = b->maxHp > 0 ? static_cast<float>(b->currentHp) / b->maxHp : 1.0f;
                ui::drawProgressBar(renderer, {ib.x + 10, ib.y + 28, textW, 6}, hp, hp > 0.35f ? col::Ok : col::Danger);
                fontRenderer.drawString(std::to_string(b->currentHp) + " / " + std::to_string(b->maxHp), ib.x + 10, ib.y + 40, ui::FONT_SMALL, col::TextDim, false);
            }
            else
            {
                static const char *const kTabHints[TabCount] = {
                    "Select building to construct",
                    "Recruit infantry at the selected or primary Barracks (RMB - cancel)",
                    "Build vehicles at the selected or primary Factory (RMB - cancel)",
                    "Select technology to research",
                    "Select units and issue a command",
                };
                fontRenderer.drawTextWrapped(_(kTabHints[m_activeTab]), ib.x + 10, ib.y + 8, textW, ui::FONT_SMALL, col::TextMuted, false);
            }

            if(!ownSelectedBuildings(entities, selected).empty())
            {
                const UiRect db = destroyBtn(ib);
                const bool armed = m_destroyArmed > 0.0f;
                const bool hot = db.contains(mx, my);
                renderer.fillRectScreen(db.x, db.y, db.w, db.h, armed ? col::Danger : (hot ? ui::mix(col::PanelRaised, col::Danger, 0.35f) : col::PanelRaised));
                renderer.drawRectScreen(db.x, db.y, db.w, db.h, col::Danger);
                fontRenderer.drawString(armed ? _("CONFIRM?") : _("DESTROY"), db.centerX(), db.centerY(), ui::FONT_SMALL_BOLD, armed ? col::Text : col::Danger, false, Anchor::HCENTER | Anchor::VCENTER);
            }
        }
    }

    namespace
    {

    } // namespace

    namespace
    {
        // Build radius of a headquarters / command center: purple
        const SDL_Color kBuildRadiusColor {160, 80, 230, 90};
    } // namespace

    void HUDController::renderProductionOverlay(Renderer &renderer, BitmapFontRenderer &fontRenderer, const EntityManager &entities, const TileMap &map, const Camera &camera) const
    {
        const float zoom = camera.getZoom();

        // Selected headquarters: the zone where buildings and walls may be placed (purple, 70% transparent, 2 px).
        // The radius is measured in tiles and tiles are wider than tall, so on screen it is an ellipse.
        for(const auto &e : entities.getAllEntities())
        {
            const Building *b = e.asBuilding();
            if(!b || !b->selected || !b->alive || b->state == ActionState::Die || b->owner != Players::local() || b->type != UnitType::Headquarters)
            {
                continue;
            }
            const float cx = (b->tileX + b->buildWidth * 0.5f) * TILE_WIDTH;
            const float cy = (b->tileY + b->buildHeight * 0.5f) * TILE_HEIGHT;
            const float r = static_cast<float>(entities.getBuildRadius());
            // Counted in tiles (the placement check is a distance in tile units), so it is an ellipse on screen
            RenderUtility::DrawTileRadius(renderer, camera, cx, cy, r, kBuildRadiusColor, 2.0f);
        }

        // Selected defensive structures: their firing range (own green, enemy red)
        for(const auto &e : entities.getAllEntities())
        {
            const Building *b = e.asBuilding();
            if(!b || !b->selected || !b->alive || b->state == ActionState::Die || b->attackRange <= 0.0f || b->attackDamage <= 0)
            {
                continue;
            }
            // The same look as the build radius: a ground ellipse, 2 px, 70% transparent
            const SDL_Color c = (b->owner == Players::local()) ? SDL_Color {90, 230, 120, 77} : SDL_Color {240, 80, 70, 77};
            RenderUtility::DrawTileRadius(renderer, camera, b->getUnitCenterPixelX(), b->getUnitCenterPixelY(), b->attackRange, c, 2.0f);
        }

        for(const auto &e : entities.getAllEntities())
        {
            const Building *b = e.asBuilding();
            if(!b || !b->alive || b->state == ActionState::Die || b->owner != Players::local() || !b->isUnitProducer())
            {
                continue;
            }
            float sx = 0.0f, sy = 0.0f;
            camera.worldToScreen(b->tileX * TILE_WIDTH, b->tileY * TILE_HEIGHT, sx, sy);
            const int w = static_cast<int>(b->buildWidth * TILE_WIDTH * zoom);

            // Training progress above the building, with the number of orders waiting behind it
            if(!b->productionQueue.empty())
            {
                const int barW = std::max(36, w - 16);
                const int bx = static_cast<int>(sx) + (w - barW) / 2;
                const int by = static_cast<int>(sy) - 22;
                renderer.fillRectScreen(bx - 1, by - 1, barW + 2, 8, {10, 12, 16, 220});
                renderer.fillRectScreen(bx, by, static_cast<int>(barW * b->currentProductionProgress()), 6, col::Accent);
                const int count = static_cast<int>(b->productionQueue.size());
                std::string label = std::to_string(count);
                const int lw = fontRenderer.getTextWidth(label, ui::FONT_SMALL_BOLD) + 8;
                renderer.fillRectScreen(bx + barW + 3, by - 5, lw, 16, col::Accent);
                fontRenderer.drawString(label, bx + barW + 3 + lw / 2, by + 3, ui::FONT_SMALL_BOLD, col::AccentText, false, Anchor::HCENTER | Anchor::VCENTER);
            }

            // Rally line from the exit to the rally point, only while the building is selected
            if(!b->selected)
                continue;
            int rx = 0, ry = 0;
            rallyPointOf(*b, map, rx, ry);
            const float x1 = (b->tileX + b->buildWidth * 0.5f) * TILE_WIDTH;
            const float y1 = (b->tileY + b->buildHeight) * TILE_HEIGHT;
            const float x2 = (rx + 0.5f) * TILE_WIDTH;
            const float y2 = (ry + 0.5f) * TILE_HEIGHT;
            const SDL_Color yellow {255, 220, 40, 150};
            float ax = 0, ay = 0, bx2 = 0, by2 = 0;
            camera.worldToScreen(x1, y1, ax, ay);
            camera.worldToScreen(x2, y2, bx2, by2);
            // 2 px wide: two parallel 1 px lines offset across the line's main direction
            const bool steep = std::abs(by2 - ay) > std::abs(bx2 - ax);
            for(int k = 0; k < 2; ++k)
            {
                const int ox = steep ? k : 0;
                const int oy = steep ? 0 : k;
                renderer.drawLineScreen(static_cast<int>(ax) + ox, static_cast<int>(ay) + oy, static_cast<int>(bx2) + ox, static_cast<int>(by2) + oy, yellow);
            }
            const int m = std::max(4, static_cast<int>(5 * zoom));
            renderer.fillRectScreen(static_cast<int>(bx2) - m / 2, static_cast<int>(by2) - m / 2, m, m, yellow);
            renderer.drawRectScreen(static_cast<int>(bx2) - m, static_cast<int>(by2) - m, 2 * m, 2 * m, yellow);
        }
    }

    void HUDController::renderBuildGhost(Renderer &renderer, const TileMap &map, const Camera &camera, int mouseTileX, int mouseTileY, const Economy &economy, const EntityManager &entities)
    {
        const auto &blueprints = EntityManager::getBlueprints();
        if(m_selectedBlueprint < 0 || m_selectedBlueprint >= static_cast<int>(blueprints.size()))
            return;

        const auto &bp = blueprints[m_selectedBlueprint];

        bool canPlace = entities.canPlaceBuilding(mouseTileX, mouseTileY, bp.width, bp.height, map, Players::local());
        bool canAfford = economy.canAfford(bp.creditCost);

        SDL_Color boxColor = (canPlace && canAfford) ? SDL_Color {0, 255, 0, 80} : SDL_Color {255, 0, 0, 80};
        SDL_Color borderColor = (canPlace && canAfford) ? SDL_Color {0, 255, 0, 220} : SDL_Color {255, 0, 0, 220};

        // Footprint tiles in world space so they scale with the camera zoom like the building itself
        for(int ty = mouseTileY; ty < mouseTileY + bp.height; ++ty)
        {
            for(int tx = mouseTileX; tx < mouseTileX + bp.width; ++tx)
            {
                const float wx = static_cast<float>(tx * TILE_WIDTH);
                const float wy = static_cast<float>(ty * TILE_HEIGHT);
                renderer.fillRectWorld(wx, wy, static_cast<float>(TILE_WIDTH), static_cast<float>(TILE_HEIGHT), camera, boxColor);
                renderer.drawRectWorld(wx, wy, static_cast<float>(TILE_WIDTH), static_cast<float>(TILE_HEIGHT), camera, borderColor);
            }
        }

        // A tower / bunker shows where it will reach from this spot
        if(const BuildingConfig *cfg = entities.getBuildingConfig(bp.buildingIndex))
        {
            if(cfg->canAttack && cfg->attackRange > 0.0f)
            {
                RenderUtility::DrawTileRadius(renderer, camera, (mouseTileX + bp.width * 0.5f) * TILE_WIDTH, (mouseTileY + bp.height * 0.5f) * TILE_HEIGHT, cfg->attackRange, {90, 230, 120, 77}, 2.0f);
            }
        }

        // Walls are drawn exactly like the placed segment (building 15 / 25, the reinforced 20 after Active Wall Armor),
        // not with a stray composite
        const int drawIndex = (bp.buildingIndex == 15 && isEffectResearched(Players::local(), ResearchEffect::WallStrength)) ? 20 : bp.buildingIndex;
        renderer.drawBuildingWorld(drawIndex, mouseTileX * TILE_WIDTH, mouseTileY * TILE_HEIGHT, 0, camera, playerFaction(), 0, 1.0f, false);
        renderer.drawBuildingWorld(drawIndex, mouseTileX * TILE_WIDTH, mouseTileY * TILE_HEIGHT, 1, camera, playerFaction(), 0, 1.0f, false);
    }

} // namespace OpenArtOfWar
