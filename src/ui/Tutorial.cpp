#include "ui/Tutorial.hpp"

#include "core/Config.hpp"
#include "core/Localization.hpp"
#include "game/Building.hpp"
#include "game/EntityManager.hpp"
#include "game/TileMap.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/Renderer.hpp"
#include "input/InputManager.hpp"

#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace OpenArtOfWar
{

namespace
{

bool Contains(const SDL_Rect &r, int x, int y)
{
    return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

bool Intersects(const SDL_Rect &a, const ui::UiRect &b)
{
    return a.w > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// building_index of the blueprint of that type in the build menu of this mission (walls: the wall), -1 when none
int BuildingIndexOf(UnitType type)
{
    for (const BuildingBlueprint &bp : EntityManager::GetBlueprints())
    {
        if (bp.mineKind >= 0)
            continue;
        if (type == UnitType::Wall ? bp.isWall : (!bp.isWall && bp.unitType == type))
            return bp.buildingIndex;
    }
    return -1;
}

constexpr int kTypeSlots = 32;

int Slot(UnitType type)
{
    const int t = static_cast<int>(type);
    return t >= 0 && t < kTypeSlots ? t : 0;
}

struct Census
{
    int units {0};
    std::array<int, kTypeSlots> all {};      // buildings by type, also the ones still being built
    std::array<int, kTypeSlots> complete {}; // finished ones
    bool barracksQueue {false};
    bool factoryQueue {false};
    int walls {0}; // wall segments of the player (counted on the wall steps only)
};

Census Count(const EntityManager &entities, const TileMap &map, PlayerId local, bool countWalls)
{
    Census c;
    for (const Entity &e : entities.GetAllEntities())
    {
        if (!e.alive || e.owner != local)
            continue;
        const Building *b = e.AsBuilding();
        if (!b)
        {
            ++c.units;
            continue;
        }
        ++c.all[static_cast<std::size_t>(Slot(b->type))];
        if (b->buildProgress >= 1.0f)
        {
            ++c.complete[static_cast<std::size_t>(Slot(b->type))];
            if (b->type == UnitType::Barracks)
                c.barracksQueue = c.barracksQueue || !b->productionQueue.empty();
            if (b->type == UnitType::Factory)
                c.factoryQueue = c.factoryQueue || !b->productionQueue.empty();
        }
    }
    if (countWalls)
    {
        for (int y = 0; y < map.GetHeight(); ++y)
        {
            for (int x = 0; x < map.GetWidth(); ++x)
                c.walls += map.IsWall(x, y) && map.GetWallOwner(x, y) == local;
        }
    }
    return c;
}

std::vector<std::pair<int, int>> RallyPoints(const EntityManager &entities, PlayerId local)
{
    std::vector<std::pair<int, int>> out;
    for (const Entity &e : entities.GetAllEntities())
    {
        const Building *b = e.AsBuilding();
        if (b && b->alive && b->owner == local && b->type == UnitType::Barracks)
            out.emplace_back(b->rallyTileX * 100000 + b->rallyTileY, b->id);
    }
    return out;
}

// Splits the text into lines no wider than maxW (paragraphs at '\n')
std::vector<std::string> Wrap(BitmapFontRenderer &font, const std::string &text, int scale, int maxW)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string paragraph = text.substr(start, end - start);
        std::string line;
        std::size_t pos = 0;
        while (pos <= paragraph.size())
        {
            std::size_t space = paragraph.find(' ', pos);
            if (space == std::string::npos)
                space = paragraph.size();
            const std::string word = paragraph.substr(pos, space - pos);
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && font.GetTextWidth(candidate, scale) > maxW)
            {
                lines.push_back(line);
                line = word;
            }
            else
            {
                line = candidate;
            }
            pos = space + 1;
        }
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

void Frame(Renderer &r, const SDL_Rect &rect, int thickness, SDL_Color color)
{
    r.FillRectScreen(rect.x - thickness, rect.y - thickness, rect.w + 2 * thickness, thickness, color);
    r.FillRectScreen(rect.x - thickness, rect.y + rect.h, rect.w + 2 * thickness, thickness, color);
    r.FillRectScreen(rect.x - thickness, rect.y, thickness, rect.h, color);
    r.FillRectScreen(rect.x + rect.w, rect.y, thickness, rect.h, color);
}

} // namespace

// ---------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------

void Tutorial::Offer(bool touch)
{
    m_active = true;
    m_touch = touch;
    m_techRequest = false;
    m_endRequest = false;
    m_taken = false;
    m_cardValid = false;
    Enter(StepOffer);
}

void Tutorial::Stop()
{
    if (m_active && m_taken)
        m_endRequest = true;
    m_active = false;
    m_cardValid = false;
}

bool Tutorial::StepModal(int step) const
{
    return step == StepOffer || step == StepWelcome || step == StepBase || step == StepFinish;
}

bool Tutorial::IsModal() const
{
    return m_active && StepModal(m_step);
}

UnitType Tutorial::BuildType(int step)
{
    switch (step)
    {
        case StepPickLab:
        case StepPlaceLab:
        case StepWaitLab:
            return UnitType::ResearchLab;
        case StepPickPower:
        case StepPlacePower:
            return UnitType::PowerPlant;
        case StepPickFactory:
        case StepPlaceFactory:
        case StepWaitFactory:
            return UnitType::Factory;
        case StepPickBunker:
        case StepPlaceBunker:
            return UnitType::Bunker;
        case StepPickWall:
        case StepPlaceWall:
            return UnitType::Wall;
        case StepPickTower:
        case StepPlaceTower:
            return UnitType::Tower;
        default:
            return UnitType::Unknown;
    }
}

bool Tutorial::IsPick(int step)
{
    return step == StepPickLab || step == StepPickPower || step == StepPickFactory || step == StepPickBunker
           || step == StepPickWall || step == StepPickTower;
}

bool Tutorial::IsPlace(int step)
{
    return step == StepPlaceLab || step == StepPlacePower || step == StepPlaceFactory || step == StepPlaceBunker
           || step == StepPlaceWall || step == StepPlaceTower;
}

bool Tutorial::MapAllowed(int step) const
{
    switch (step)
    {
        case StepCamera:
        case StepSelect:
        case StepMove:
        case StepSelectBarracks:
        case StepRally:
        case StepWaitLab:
        case StepWaitFactory:
        case StepSelectFactory:
            return true;
        default:
            return IsPlace(step);
    }
}

HUDController::TutorialTarget Tutorial::Target(int step) const
{
    using T = HUDController::TutorialTarget;
    if (IsPick(step))
        return T::BuildCard;
    if (IsPlace(step))
        return m_touch ? T::PlaceConfirm : T::None;
    switch (step)
    {
        case StepCamera:
            return T::Radar;
        case StepBase:
            return T::Resources;
        case StepTrain:
        case StepTrainVehicle:
            return T::UnitCard;
        case StepOpenResearch:
            return T::ResearchButton;
        case StepFinish:
            return T::ObjectivesButton;
        default:
            return T::None;
    }
}

int Tutorial::TargetArg(int step) const
{
    if (IsPick(step))
        return BuildingIndexOf(BuildType(step));
    return step == StepTrainVehicle ? 1 : 0; // the unit card: 0 infantry, 1 vehicles
}

void Tutorial::Enter(int step)
{
    m_step = step;
    m_stepTime = 0.0f;
    m_camMoved = 0.0f;
    m_baseRally.clear();
    m_cardValid = false;
    if (m_step >= StepCount)
        Stop();
}

void Tutorial::Update(
    float deltaTime,
    const EntityManager &entities,
    const std::vector<int> &selected,
    const HUDController &hud,
    const Camera &camera,
    const TileMap &map)
{
    if (!m_active)
        return;
    const PlayerId local = Players::Local();
    const UnitType buildType = BuildType(m_step);
    const Census census = Count(entities, map, local, buildType == UnitType::Wall);
    const auto built = [&](UnitType type) {
        return type == UnitType::Wall ? census.walls : census.all[static_cast<std::size_t>(Slot(type))];
    };
    const auto complete = [&](UnitType type) {
        return census.complete[static_cast<std::size_t>(Slot(type))];
    };
    if (m_stepTime == 0.0f)
    {
        // What the step starts from
        m_camX = camera.GetX();
        m_camY = camera.GetY();
        m_baseUnits = census.units;
        m_baseRally = RallyPoints(entities, local);
        if (IsPick(m_step))
            m_baseBuilt = built(buildType);
        // The object of the step in sight (the base for placing a building)
        for (const Entity &e : entities.GetAllEntities())
        {
            if (!e.alive || e.owner != local)
                continue;
            const Building *b = e.AsBuilding();
            const bool wanted
                = m_step == StepSelect                               ? b == nullptr
                  : m_step == StepSelectBarracks                     ? b && b->type == UnitType::Barracks
                  : m_step == StepSelectFactory                      ? b && b->type == UnitType::Factory
                  : (m_step == StepWaitLab || m_step == StepWaitFactory) ? b && b->type == buildType
                  : IsPlace(m_step)                                  ? b && b->type == UnitType::Headquarters
                                                                     : false;
            if (wanted)
            {
                m_focus = true;
                m_focusX = e.GetUnitCenterPixelX();
                m_focusY = e.GetUnitCenterPixelY();
                break;
            }
        }
    }
    m_stepTime += std::max(deltaTime, 0.0001f);

    bool unitSelected = false, unitMoving = false, barracksSelected = false, factorySelected = false;
    for (int id : selected)
    {
        const Entity *e = entities.GetEntity(id);
        if (!e || !e->alive || e->owner != local)
            continue;
        if (const Building *b = e->AsBuilding())
        {
            barracksSelected = barracksSelected || b->type == UnitType::Barracks;
            factorySelected = factorySelected || b->type == UnitType::Factory;
        }
        else
        {
            unitSelected = true;
            unitMoving = unitMoving || e->state == ActionState::Move;
        }
    }

    // Building steps: pick the card, place it (walls: three segments), wait for it where the next step needs it
    if (IsPick(m_step))
    {
        const int index = BuildingIndexOf(buildType);
        const bool hasWait = m_step == StepPickLab || m_step == StepPickFactory;
        if (index < 0)
            Enter(m_step + (hasWait ? 3 : 2)); // not in this build menu: the next lesson
        else if (hasWait && built(buildType) > 0)
            Enter(m_step + 2); // there is one already: wait for it
        else if (hud.PlacingBuildingIndex() == index)
            Enter(m_step + 1);
        return;
    }
    if (IsPlace(m_step))
    {
        const int need = buildType == UnitType::Wall ? 3 : 1;
        if (built(buildType) >= m_baseBuilt + need)
            Enter(m_step + 1);
        else if (hud.PlacingBuildingIndex() != BuildingIndexOf(buildType))
            Enter(m_step - 1); // cancelled: pick the card again
        return;
    }

    switch (m_step)
    {
        case StepCamera:
            m_camMoved += std::abs(camera.GetX() - m_camX) + std::abs(camera.GetY() - m_camY);
            m_camX = camera.GetX();
            m_camY = camera.GetY();
            if (m_camMoved > 320.0f)
                Enter(census.units > 0 ? StepSelect : StepBase);
            break;
        case StepSelect:
            if (census.units == 0)
                Enter(StepBase);
            else if (unitSelected)
                Enter(StepMove);
            break;
        case StepMove:
            if (census.units == 0 || unitMoving)
                Enter(StepBase);
            break;
        case StepSelectBarracks:
            if (complete(UnitType::Barracks) == 0)
                Enter(StepPickLab);
            else if (barracksSelected)
                Enter(StepTrain);
            break;
        case StepTrain:
            if (complete(UnitType::Barracks) == 0)
                Enter(StepPickLab);
            else if (census.barracksQueue || census.units > m_baseUnits)
                Enter(StepRally);
            break;
        case StepRally:
            if (complete(UnitType::Barracks) == 0 || RallyPoints(entities, local) != m_baseRally)
                Enter(StepPickLab);
            break;
        case StepWaitLab:
        case StepWaitFactory:
            if (built(buildType) == 0)
                Enter(m_step - 2);
            else if (complete(buildType) > 0)
                Enter(m_step + 1);
            break;
        case StepOpenResearch:
            if (hud.ResearchActive() >= 0)
                Enter(StepPickPower);
            else if (hud.IsResearchWindowOpen())
                Enter(StepStartResearch);
            break;
        case StepStartResearch:
            if (hud.ResearchActive() >= 0)
                Enter(StepPickPower);
            else if (!hud.IsResearchWindowOpen())
                Enter(StepOpenResearch);
            break;
        case StepSelectFactory:
            if (complete(UnitType::Factory) == 0)
                Enter(StepPickBunker);
            else if (factorySelected)
                Enter(StepTrainVehicle);
            break;
        case StepTrainVehicle:
            if (complete(UnitType::Factory) == 0 || census.factoryQueue || census.units > m_baseUnits)
                Enter(StepPickBunker);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Texts
// ---------------------------------------------------------------------------

std::string Tutorial::Title(int step) const
{
    switch (step)
    {
        case StepOffer:
            return _("TUTORIAL");
        case StepWelcome:
            return _("WELCOME, COMMANDER");
        case StepCamera:
            return _("THE CAMERA");
        case StepSelect:
            return _("SELECTING UNITS");
        case StepMove:
            return _("GIVING ORDERS");
        case StepBase:
            return _("CREDITS AND POWER");
        case StepSelectBarracks:
            return _("YOUR BARRACKS");
        case StepTrain:
            return _("TRAINING SOLDIERS");
        case StepRally:
            return _("RALLY POINT");
        case StepPickLab:
            return _("RESEARCH LAB");
        case StepWaitLab:
        case StepWaitFactory:
            return _("UNDER CONSTRUCTION");
        case StepOpenResearch:
        case StepStartResearch:
            return _("RESEARCH");
        case StepPickPower:
            return _("POWER PLANT");
        case StepPickFactory:
            return _("FACTORY");
        case StepSelectFactory:
            return _("YOUR FACTORY");
        case StepTrainVehicle:
            return _("VEHICLES: THE COYOTE");
        case StepPickBunker:
            return _("BUNKER");
        case StepPickWall:
            return _("WALLS");
        case StepPlaceWall:
            return _("LAYING WALLS");
        case StepPickTower:
            return _("GUARD TOWER");
        case StepFinish:
            return _("GOOD LUCK!");
        default:
            return IsPlace(step) ? _("PLACING A BUILDING") : _("TUTORIAL");
    }
}

std::string Tutorial::Body(int step) const
{
    switch (step)
    {
        case StepOffer:
            return _("This is your first mission. Would you like to take the tutorial? It shows step by step how to "
                     "control units, train soldiers and vehicles, build a base with its defenses and research "
                     "technologies.\nFor the tutorial, advanced technologies are unlocked for you.");
        case StepWelcome:
            return _("Your mission: build up your base, train an army and destroy the enemy base.\nThe highlighted "
                     "part of the screen is what the current step is about; the rest of the interface waits until "
                     "the step is done. Advanced technologies are unlocked and you get extra credits for the "
                     "lessons. The enemy will not attack while the tutorial runs.");
        case StepCamera:
            return m_touch ? _("Move the camera: drag one finger across the map, or hold a finger at an edge of the "
                               "screen. Pinch with two fingers to zoom.\nA tap on the radar (highlighted) jumps "
                               "there at once.")
                           : _("Move the camera: push the mouse against an edge of the screen, or hold W A S D or "
                               "the arrow keys. The mouse wheel zooms.\nA click on the radar (highlighted) jumps "
                               "there at once.");
        case StepSelect:
            return m_touch ? _("Tap one of your soldiers (highlighted) to select it.\nTo select a group: hold a "
                               "finger on the map for a moment, then drag a box around the soldiers.")
                           : _("Click one of your soldiers (highlighted) to select it.\nTo select a group: hold the "
                               "left mouse button and drag a box around the soldiers.");
        case StepMove:
            return m_touch ? _("Tap the ground: the selected soldiers go there.\nTap twice on the same spot: they "
                               "move and attack every enemy on their way (attack-move). Tap an enemy to attack it.")
                           : _("Right-click the ground: the selected soldiers go there. Right-click an enemy to "
                               "attack it.\nQ, then a click on the ground: attack-move, they fight every enemy on "
                               "their way.");
        case StepBase:
            return _("Credits pay for buildings, units and research. Power plants give the power your buildings "
                     "need: without enough power, production slows down.\nKeep an eye on both (highlighted).");
        case StepSelectBarracks:
            return m_touch ? _("The barracks trains infantry. Tap your barracks (highlighted) to select it.")
                           : _("The barracks trains infantry. Click your barracks (highlighted) to select it.");
        case StepTrain:
            return m_touch ? _("The cards show the soldiers the barracks can train. Tap the highlighted card to "
                               "train one (hold a card to cancel an order).")
                           : _("The sidebar shows the soldiers the barracks can train. Click the highlighted card "
                               "to train one (right-click cancels an order).");
        case StepRally:
            return m_touch ? _("With the barracks selected, double-tap the ground: new soldiers will gather there "
                               "(rally point). The barracks stays selected; a single tap releases it.")
                           : _("With the barracks selected, right-click the ground: new soldiers will gather there "
                               "(rally point).");
        case StepPickLab:
            return m_touch ? _("Now let's build. The research lab develops new technologies. Open the building "
                               "cards with the build button (bottom right) and tap the lab card (highlighted). The "
                               "price is shown on the card.")
                           : _("Now let's build. The research lab develops new technologies. Open the BUILD tab "
                               "on the sidebar and click the lab card (highlighted). The price is shown on the "
                               "card.");
        case StepWaitLab:
            return _("The lab is being built: its bar shows the progress. Meanwhile you may move your soldiers.");
        case StepOpenResearch:
            return m_touch ? _("The lab is ready. Tap the highlighted flask button (top) to open the technology tree.")
                           : _("The lab is ready. Click RESEARCH NEW TECHNOLOGIES (highlighted) to open the "
                               "technology tree.");
        case StepStartResearch:
            return _("Pick a technology in the tree and start it. Research costs credits and takes time; "
                     "its effect works for all your units once it is done.");
        case StepPickPower:
            return m_touch ? _("Every building takes power, and the factory you build next takes a lot. Build a "
                               "power plant first: tap its card (highlighted).")
                           : _("Every building takes power, and the factory you build next takes a lot. Build a "
                               "power plant first: click its card (highlighted).");
        case StepPickFactory:
            return m_touch ? _("The factory builds vehicles. Tap its card (highlighted).")
                           : _("The factory builds vehicles. Click its card (highlighted).");
        case StepWaitFactory:
            return _("The factory is being built: its bar shows the progress. Meanwhile you may move your "
                     "soldiers.");
        case StepSelectFactory:
            return m_touch ? _("The factory is ready. Tap your factory (highlighted) to select it.")
                           : _("The factory is ready. Click your factory (highlighted) to select it.");
        case StepTrainVehicle:
            return m_touch ? _("Tap the highlighted card to build a Coyote: a fast light vehicle with a machine gun, "
                               "good for scouting and for hunting infantry.")
                           : _("Click the highlighted card to build a Coyote: a fast light vehicle with a machine "
                               "gun, good for scouting and for hunting infantry.");
        case StepPickBunker:
            return m_touch ? _("Now the defenses. A bunker is a fortified firing position: attackers of your base run "
                               "into its fire. Tap the bunker card (highlighted).")
                           : _("Now the defenses. A bunker is a fortified firing position: attackers of your base run "
                               "into its fire. Click the bunker card (highlighted).");
        case StepPickWall:
            return m_touch ? _("Walls hold the enemy back and lead attacks into the fire of your defenses. Tap the "
                               "wall card (highlighted).")
                           : _("Walls hold the enemy back and lead attacks into the fire of your defenses. Click "
                               "the wall card (highlighted).");
        case StepPlaceWall:
            return m_touch ? _("Walls are laid one segment at a time: tap a free spot, then tap the segment again "
                               "(or the highlighted confirm button) to lay it. Lay 3 segments side by side.")
                           : _("Walls are laid one segment at a time: click 3 free spots side by side next to your "
                               "base. The wall mode stays on until you right-click.");
        case StepPickTower:
            return m_touch ? _("A guard tower fires at enemies from afar. If the wall is still being laid, end it "
                               "with the highlighted cancel button, then tap the tower card (highlighted).")
                           : _("A guard tower fires at enemies from afar. Click the tower card (highlighted).");
        case StepFinish:
            return m_touch ? _("You know the basics now. Train an army, research upgrades and destroy the enemy "
                               "base. The highlighted clipboard button (top) shows the goals of the "
                               "mission.\nGood luck, Commander!")
                           : _("You know the basics now. Train an army, research upgrades and destroy the enemy "
                               "base. The OBJECTIVES button (highlighted) shows the goals of the mission.\nGood "
                               "luck, Commander!");
        default:
            // Placing a building (lab, power plant, factory, bunker, tower)
            return m_touch ? _("Tap a free spot near your base: the building moves there (green: it can stand "
                               "there, red: it cannot). Drag it to adjust, then tap it again or press the "
                               "highlighted confirm button.")
                           : _("Move the mouse to a free spot near your base (green: it can stand there, red: it "
                               "cannot) and click to build. Right-click cancels.");
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

float Tutorial::Scale(int windowH) const
{
    return m_touch ? std::clamp(static_cast<float>(windowH) / 720.0f, 1.0f, 2.0f) : 1.0f;
}

void Tutorial::HandleModalInput(int mx, int my, bool leftClicked, int, int, int)
{
    if (!m_active || !leftClicked || !m_cardValid)
        return;
    if (m_card.primary.Contains(mx, my))
    {
        if (m_step == StepOffer)
            m_techRequest = m_taken = true;
        Enter(m_step + 1);
    }
    else if (m_card.secondary.w > 0 && m_card.secondary.Contains(mx, my))
    {
        Stop(); // NO / SKIP TUTORIAL
    }
}

void Tutorial::FilterInput(
    int mx,
    int my,
    bool &leftClicked,
    bool &rightClicked,
    const HUDController &hud,
    InputManager &input,
    int windowW,
    int windowH,
    int mapViewW)
{
    if (!m_active || StepModal(m_step))
        return;
    // The card itself: its SKIP button, the rest of it is no click on the map
    if (m_cardValid && m_card.panel.Contains(mx, my))
    {
        if (leftClicked && m_card.secondary.Contains(mx, my))
            Stop();
        leftClicked = rightClicked = false;
        input.CancelSelectionBox();
        return;
    }
    if (!leftClicked && !rightClicked)
    {
        if (!MapAllowed(m_step))
            input.CancelSelectionBox();
        return;
    }
    const SDL_Rect target = hud.TutorialRect(Target(m_step), TargetArg(m_step), windowW, windowH);
    const bool onTarget = Contains(target, mx, my);
    const bool onMap = mx < mapViewW && !hud.IsOverHud(mx, my, windowW, windowH);
    // The radar of the camera step moves the camera only; the tech tree window takes its own input
    if (onTarget || (MapAllowed(m_step) && onMap))
        return;
    leftClicked = rightClicked = false;
    input.CancelSelectionBox();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

Tutorial::Card Tutorial::MakeCard(
    BitmapFontRenderer *font, const SDL_Rect &target, int windowW, int windowH, int mapViewW, bool researchOpen) const
{
    const float u = Scale(windowH);
    const auto px = [&](int v) {
        return static_cast<int>(std::lround(v * u));
    };
    const bool modal = StepModal(m_step);
    Card card;
    const int areaW = researchOpen ? windowW : mapViewW;
    const int w = std::min(areaW - 32, modal ? px(620) : px(560));
    const int pad = px(16);
    card.lineH = font ? font->GetTextHeight(ui::FONT_BODY) + 4 : 20;
    if (font)
        card.lines = Wrap(*font, Body(m_step), ui::FONT_BODY, w - 2 * pad);
    const int titleH = font ? font->GetTextHeight(ui::FONT_H2) : 26;
    const int btnH = px(modal ? 40 : 30);
    const int h = pad + titleH + px(10) + static_cast<int>(card.lines.size()) * card.lineH + px(14) + btnH + pad;
    card.panel.w = w;
    card.panel.h = h;
    card.panel.x = (areaW - w) / 2;
    if (modal)
        card.panel.y = std::max(16, (windowH - h) / 2);
    else if (researchOpen)
        card.panel.y = windowH - h - 16;
    else
    {
        card.panel.y = px(m_touch ? 70 : 48);
        if (Intersects(target, card.panel))
            card.panel.y = windowH - h - px(m_touch ? 200 : 90);
    }
    const int btnY = card.panel.Bottom() - pad - btnH;
    const int btnW = px(modal ? 170 : 150);
    if (m_step == StepOffer)
    {
        card.primary = {card.panel.Right() - pad - btnW, btnY, btnW, btnH};
        card.secondary = {card.primary.x - px(12) - btnW, btnY, btnW, btnH};
    }
    else if (modal)
    {
        card.primary = {card.panel.Right() - pad - btnW, btnY, btnW, btnH};
        card.secondary = m_step == StepFinish ? ui::UiRect {} : ui::UiRect {card.panel.x + pad, btnY, btnW + px(40), btnH};
    }
    else if (!researchOpen)
    {
        card.secondary = {card.panel.Right() - pad - btnW, btnY, btnW, btnH};
    }
    return card;
}

void Tutorial::Render(
    Renderer &renderer,
    BitmapFontRenderer &font,
    const HUDController &hud,
    const EntityManager &entities,
    const Camera &camera,
    int mx,
    int my,
    int windowW,
    int windowH,
    int mapViewW) const
{
    if (!m_active)
        return;
    const bool modal = StepModal(m_step);
    const bool researchOpen = hud.IsResearchWindowOpen();
    const SDL_Rect target = researchOpen ? SDL_Rect {0, 0, 0, 0}
                                         : hud.TutorialRect(Target(m_step), TargetArg(m_step), windowW, windowH);
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(SDL_GetTicks() % 100000) * 0.006f);

    // Shade: everything but the target when the step locks the map (an explaining card shades all of it)
    const bool shade = !researchOpen && (modal || (!MapAllowed(m_step) && target.w > 0));
    if (shade)
    {
        const SDL_Color dim {0, 0, 0, static_cast<Uint8>(modal ? 150 : 120)};
        if (target.w > 0)
        {
            renderer.FillRectScreen(0, 0, windowW, target.y, dim);
            renderer.FillRectScreen(0, target.y + target.h, windowW, windowH - target.y - target.h, dim);
            renderer.FillRectScreen(0, target.y, target.x, target.h, dim);
            renderer.FillRectScreen(target.x + target.w, target.y, windowW - target.x - target.w, target.h, dim);
        }
        else
        {
            renderer.FillRectScreen(0, 0, windowW, windowH, dim);
        }
    }

    // The target: a pulsing frame
    if (target.w > 0)
    {
        SDL_Color c = ui::color::Accent;
        Frame(renderer, target, 3, c);
        c.a = static_cast<Uint8>(60 + 120 * pulse);
        const int grow = 4 + static_cast<int>(6.0f * pulse);
        Frame(renderer, {target.x - grow, target.y - grow, target.w + 2 * grow, target.h + 2 * grow}, 2, c);
    }

    // Objects on the map: the soldiers to select, the barracks
    if (!modal && !researchOpen
        && (m_step == StepSelect || m_step == StepSelectBarracks || m_step == StepSelectFactory))
    {
        const float zoom = std::max(0.01f, camera.GetZoom());
        int marked = 0;
        for (const Entity &e : entities.GetAllEntities())
        {
            if (!e.alive || e.owner != Players::Local() || marked >= 24)
                continue;
            const Building *b = e.AsBuilding();
            const UnitType markType = m_step == StepSelectFactory ? UnitType::Factory : UnitType::Barracks;
            if (m_step == StepSelect ? b != nullptr : (!b || b->type != markType))
                continue;
            float sx = 0.0f, sy = 0.0f;
            camera.WorldToScreen(e.GetUnitCenterPixelX(), e.GetUnitCenterPixelY(), sx, sy);
            const int half = b ? static_cast<int>(std::max(b->buildWidth, b->buildHeight) * TILE_WIDTH * zoom * 0.55f)
                               : static_cast<int>(18.0f * zoom);
            const int grow = static_cast<int>(4.0f * pulse);
            SDL_Rect r {
                static_cast<int>(sx) - half - grow,
                static_cast<int>(sy) - half - grow,
                2 * (half + grow),
                2 * (half + grow)};
            if (r.x + r.w < 0 || r.y + r.h < 0 || r.x > mapViewW || r.y > windowH)
                continue;
            SDL_Color c = ui::color::Accent;
            c.a = static_cast<Uint8>(140 + 110 * pulse);
            Frame(renderer, r, 2, c);
            ++marked;
        }
    }

    // The card
    m_card = MakeCard(&font, target, windowW, windowH, mapViewW, researchOpen);
    m_cardValid = true;
    const float u = Scale(windowH);
    const int pad = static_cast<int>(std::lround(16 * u));
    ui::DrawPanel(renderer, m_card.panel, ui::color::Panel, ui::color::BorderStrong);
    renderer.FillRectScreen(m_card.panel.x, m_card.panel.y, 4, m_card.panel.h, ui::color::Accent);
    int y = m_card.panel.y + pad;
    font.DrawString(Title(m_step), m_card.panel.x + pad, y, ui::FONT_H2, ui::color::Accent, true);
    if (m_step > StepOffer && m_step < StepFinish)
    {
        const std::string counter = std::to_string(m_step) + " / " + std::to_string(StepFinish - 1);
        font.DrawString(
            counter, m_card.panel.Right() - pad, y, ui::FONT_SMALL, ui::color::TextDim, false, Anchor::TOP | Anchor::RIGHT);
    }
    y += font.GetTextHeight(ui::FONT_H2) + static_cast<int>(std::lround(10 * u));
    for (const std::string &line : m_card.lines)
    {
        font.DrawString(line, m_card.panel.x + pad, y, ui::FONT_BODY, ui::color::Text, false);
        y += m_card.lineH;
    }
    if (m_card.primary.w > 0)
    {
        const std::string label = m_step == StepOffer ? _("YES") : m_step == StepFinish ? _("FINISH") : _("NEXT");
        ui::DrawButton(
            renderer, font, m_card.primary, label, ui::ButtonStyle::Primary, m_card.primary.Contains(mx, my));
    }
    if (m_card.secondary.w > 0)
    {
        const std::string label = m_step == StepOffer ? _("NO") : _("SKIP TUTORIAL");
        ui::DrawButton(
            renderer,
            font,
            m_card.secondary,
            label,
            modal ? ui::ButtonStyle::Secondary : ui::ButtonStyle::Ghost,
            m_card.secondary.Contains(mx, my),
            true,
            modal ? ui::FONT_BODY_BOLD : ui::FONT_SMALL_BOLD);
    }
}

} // namespace OpenArtOfWar
