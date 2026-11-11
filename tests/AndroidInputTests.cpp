#include "input/InputManager.hpp"
#include <cstdlib>
#include <iostream>

using namespace OpenArtOfWar;

static void Check(bool ok, const char *message)
{
    if (!ok)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main()
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS), "SDL init");
    SDL_Window *window = SDL_CreateWindow("touch test", 1920, 1080, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "software");
    Check(renderer != nullptr, "software renderer");
    Check(SDL_SetRenderLogicalPresentation(renderer, 1280, 720, SDL_LOGICAL_PRESENTATION_LETTERBOX), "logical size");
    Camera camera(976, 720);
    InputManager input;
    input.SetRenderer(renderer);
    auto touch = [&](Uint32 type, SDL_FingerID id, float x, float y) {
        SDL_Event event {};
        event.type = type;
        event.tfinger.fingerID = id;
        event.tfinger.x = x / 1280.0f;
        event.tfinger.y = y / 720.0f;
        Check(SDL_ConvertEventToRenderCoordinates(renderer, &event), "touch conversion");
        input.ProcessEvent(event, camera);
        input.Update(0, camera);
    };
    float x, y;
    touch(SDL_EVENT_FINGER_DOWN, 1, 1100, 300);
    Check(!input.WasLeftClickIssued(x, y), "HUD must wait for release");
    touch(SDL_EVENT_FINGER_UP, 1, 1100, 300);
    Check(input.WasLeftClickIssued(x, y), "HUD tap must click");
    Check(
        input.GetMouseScreenX() == 1100 && input.GetMouseScreenY() == 300,
        "touch pointer must retain logical position");
    Check(!input.WasLeftClickIssued(x, y), "tap must not duplicate");

    touch(SDL_EVENT_FINGER_DOWN, 1, 200, 200);
    SDL_Delay(510);
    touch(SDL_EVENT_FINGER_UP, 1, 200, 200);
    Check(input.WasRightClickIssued(x, y), "hold must issue contextual command");
    Check(!input.WasLeftClickIssued(x, y), "hold must not also select");
    Check(!input.IsLeftMouseDown(), "hold must release pointer");

    touch(SDL_EVENT_FINGER_DOWN, 1, 200, 200);
    touch(SDL_EVENT_FINGER_DOWN, 2, 400, 200);
    const float oldZoom = camera.GetZoom();
    touch(SDL_EVENT_FINGER_MOTION, 2, 500, 200);
    Check(camera.GetZoom() > oldZoom, "pinch must zoom");
    Check(!input.GetSelectionBox().active, "pinch must cancel selection");
    touch(SDL_EVENT_FINGER_UP, 1, 200, 200);
    touch(SDL_EVENT_FINGER_UP, 2, 500, 200);
    Check(!input.WasLeftClickIssued(x, y) && !input.WasRightClickIssued(x, y), "gesture must not click");

    input.SetMenuOpen(true);
    const float menuZoom = camera.GetZoom();
    touch(SDL_EVENT_FINGER_DOWN, 1, 200, 200);
    touch(SDL_EVENT_FINGER_DOWN, 2, 400, 200);
    touch(SDL_EVENT_FINGER_MOTION, 2, 600, 200);
    Check(camera.GetZoom() == menuZoom, "menu must block map gestures");
    touch(SDL_EVENT_FINGER_CANCELED, 1, 200, 200);
    touch(SDL_EVENT_FINGER_UP, 2, 600, 200);
    Check(!input.WasLeftClickIssued(x, y) && !input.IsLeftMouseDown(), "cancel must not leave a stuck pointer");

    touch(SDL_EVENT_FINGER_DOWN, 1, 1100, 300);
    SDL_Event focus {};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    input.ProcessEvent(focus, camera);
    touch(SDL_EVENT_FINGER_UP, 1, 1100, 300);
    Check(!input.WasLeftClickIssued(x, y) && !input.IsLeftMouseDown(), "focus loss must discard touch");

    SDL_Event back {};
    back.type = SDL_EVENT_KEY_DOWN;
    back.key.scancode = SDL_SCANCODE_AC_BACK;
    input.ProcessEvent(back, camera);
    Check(input.IsKeyJustPressed(SDL_SCANCODE_ESCAPE), "Android Back must map to Escape");
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << "Android touch tests passed\n";
}
