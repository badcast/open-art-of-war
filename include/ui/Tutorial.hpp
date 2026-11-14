#pragma once

#include "ui/HUDController.hpp"
#include "ui/UiKit.hpp"

#include <SDL3/SDL_rect.h>
#include <string>
#include <vector>

namespace OpenArtOfWar
{
class EntityManager;
class Renderer;
class BitmapFontRenderer;
class Camera;
class InputManager;
class TileMap;

// The tutorial of the first campaign mission. It is offered when the mission starts (yes / no); taken, the player
// plays at the "advanced" tech level with extra credits while the enemy stays calm, and is led step by step: camera,
// units, barracks, research lab and research, power plant, factory and a vehicle, bunker, walls, tower. A card
// explains the step, the button or object of the step is highlighted, everything else of the interface is locked
// until the step is done. Cards that only explain hold the game; the other steps play on.
// For the PC (mouse) and the touch interface alike. Pure UI: what it gives the world are the CommandType::TutorialTech
// and TutorialEnd commands (TakeTechRequest / TakeEndRequest), issued like any other command.
class Tutorial
{
public:
    // Offers the tutorial (the yes / no card)
    void Offer(bool touch);
    void Stop();
    bool IsActive() const
    {
        return m_active;
    }
    // A card the player has to answer: the game waits
    bool IsModal() const;
    // The card on the screen (last frame), empty when none: the input takes a press there as a click at once
    SDL_Rect CardRect() const
    {
        if (!m_active || !m_cardValid)
            return {0, 0, 0, 0};
        return {m_card.panel.x, m_card.panel.y, m_card.panel.w, m_card.panel.h};
    }
    // The player took the tutorial: the advanced tech level is to be requested (once)
    bool TakeTechRequest()
    {
        const bool r = m_techRequest;
        m_techRequest = false;
        return r;
    }

    // The tutorial that was taken is over (finished, skipped): the enemy is to be released (once)
    bool TakeEndRequest()
    {
        const bool r = m_endRequest;
        m_endRequest = false;
        return r;
    }
    // A step about an object on the map brings the camera there once (world pixels)
    bool TakeFocus(float &x, float &y)
    {
        if (!m_focus)
            return false;
        m_focus = false;
        x = m_focusX;
        y = m_focusY;
        return true;
    }
    // Every update of the game: checks whether the step is done
    void Update(
        float deltaTime,
        const EntityManager &entities,
        const std::vector<int> &selected,
        const HUDController &hud,
        const Camera &camera,
        const TileMap &map);
    // A card the player has to answer takes the input (its buttons)
    void HandleModalInput(int mx, int my, bool leftClicked, int windowW, int windowH, int mapViewW);
    // An action step: a click on the card's own button is handled, clicks outside the step's targets are dropped
    // (and the box selection with them when the map is locked)
    void FilterInput(
        int mx,
        int my,
        bool &leftClicked,
        bool &rightClicked,
        const HUDController &hud,
        InputManager &input,
        int windowW,
        int windowH,
        int mapViewW);
    void Render(
        Renderer &renderer,
        BitmapFontRenderer &font,
        const HUDController &hud,
        const EntityManager &entities,
        const Camera &camera,
        int mx,
        int my,
        int windowW,
        int windowH,
        int mapViewW) const;

private:
    enum Step
    {
        StepOffer,
        StepWelcome,
        StepCamera,
        StepSelect,
        StepMove,
        StepBase,
        StepSelectBarracks,
        StepTrain,
        StepRally,
        StepPickLab,
        StepPlaceLab,
        StepWaitLab,
        StepOpenResearch,
        StepStartResearch,
        StepPickPower,
        StepPlacePower,
        StepPickFactory,
        StepPlaceFactory,
        StepWaitFactory,
        StepSelectFactory,
        StepTrainVehicle,
        StepPickBunker,
        StepPlaceBunker,
        StepPickWall,
        StepPlaceWall,
        StepPickTower,
        StepPlaceTower,
        StepFinish,
        StepCount
    };
    struct Card
    {
        ui::UiRect panel;
        ui::UiRect primary;   // YES / NEXT / FINISH (empty on an action step)
        ui::UiRect secondary; // NO / SKIP TUTORIAL
        std::vector<std::string> lines;
        int lineH {18};
    };

    void Enter(int step);
    bool StepModal(int step) const;
    bool MapAllowed(int step) const;
    HUDController::TutorialTarget Target(int step) const;
    int TargetArg(int step) const;
    // The building a Pick / Place / Wait step is about (Unknown: none)
    static UnitType BuildType(int step);
    static bool IsPick(int step);
    static bool IsPlace(int step);
    std::string Title(int step) const;
    std::string Body(int step) const;
    float Scale(int windowH) const;
    Card MakeCard(
        BitmapFontRenderer *font, const SDL_Rect &target, int windowW, int windowH, int mapViewW, bool researchOpen)
        const;

    bool m_active {false};
    bool m_touch {false};
    bool m_techRequest {false};
    bool m_endRequest {false};
    bool m_taken {false}; // the player said yes
    bool m_focus {false};
    float m_focusX {0.0f}, m_focusY {0.0f};
    int m_step {StepOffer};
    float m_stepTime {0.0f};
    // What the step started from
    float m_camX {0.0f}, m_camY {0.0f};
    float m_camMoved {0.0f};
    int m_baseUnits {0};
    int m_baseBuilt {0}; // buildings (wall segments) of the step's type when its Pick step began
    std::vector<std::pair<int, int>> m_baseRally;
    // Card geometry of the last frame (for the clicks)
    mutable Card m_card;
    mutable bool m_cardValid {false};
};

} // namespace OpenArtOfWar
