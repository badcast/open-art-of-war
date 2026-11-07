#pragma once

#include "core/Config.hpp"

namespace OpenArtOfWar
{

class InputManager;
class Camera;
class TileMap;
class Pathfinder;
class EntityManager;
class Economy;
class AIController;
class MissionController;
class HUDController;
class MenuController;
class SoundEngine;

// What an Update step of a screen needs, bundled into one parameter. Only references: nothing is owned or copied,
// built once per frame by Application and passed as `const FrameContext &`.
struct FrameContext
{
    float deltaTime;
    int windowW;
    int windowH;
    InputManager &input;
    Camera &camera;
    TileMap &map;
    Pathfinder &pathfinder;
    EntityManager &entities;
    Economy &economy;
    AIController &ai;
    MissionController &mission;
    HUDController &hud;
    MenuController &menu;
    SoundEngine &sound;
    AppState &state; // out: the screens switch the application state
};

} // namespace OpenArtOfWar
