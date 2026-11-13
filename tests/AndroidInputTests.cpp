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

    // A menu scrolls with the finger: 32 px of travel = one wheel step (dragging up = later rows), and it is no tap
    input.ConsumeWheel();
    touch(SDL_EVENT_FINGER_DOWN, 1, 600, 500);
    touch(SDL_EVENT_FINGER_MOTION, 1, 600, 400);
    touch(SDL_EVENT_FINGER_MOTION, 1, 600, 300);
    touch(SDL_EVENT_FINGER_UP, 1, 600, 300);
    Check(input.ConsumeWheel() == -6, "swiping up 200 px must scroll a menu by six steps");
    Check(!input.WasLeftClickIssued(x, y), "a scroll must not click");
    touch(SDL_EVENT_FINGER_DOWN, 1, 600, 300);
    touch(SDL_EVENT_FINGER_MOTION, 1, 600, 380);
    touch(SDL_EVENT_FINGER_UP, 1, 600, 380);
    Check(input.ConsumeWheel() == 2, "swiping down must scroll back");

    // A window that scrolls both ways (the technology tree) follows the pointer itself: the press is reported, the
    // pointer travels with the finger, and no wheel steps are made of the drag
    input.SetTouchWheelScroll(false);
    input.ConsumeWheel();
    int pressX = 0, pressY = 0;
    touch(SDL_EVENT_FINGER_DOWN, 1, 600, 500);
    Check(input.ConsumeLeftPress(pressX, pressY) && pressX == 600 && pressY == 500, "the press must be reported");
    touch(SDL_EVENT_FINGER_MOTION, 1, 595, 495);
    Check(input.IsLeftMouseDown(), "the finger holds the pointer down while it is dragged");
    touch(SDL_EVENT_FINGER_MOTION, 1, 500, 420);
    Check(input.GetMouseScreenX() == 500 && input.GetMouseScreenY() == 420, "the pointer must follow the finger");
    touch(SDL_EVENT_FINGER_UP, 1, 500, 420);
    Check(!input.IsLeftMouseDown(), "the lift must release the pointer");
    Check(input.ConsumeWheel() == 0, "no wheel steps are made of the drag");
    Check(!input.WasLeftClickIssued(x, y), "a drag must not click");
    input.SetTouchWheelScroll(true);

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

    // Touch HUD mode (full-screen viewport, always-on box selection except during building placement)
    Camera touchCam(1280, 720);
    touchCam.SetPosition(400.0f, 300.0f);
    input.SetMenuOpen(false);
    input.SetTouchMode(true);
    input.SetTouchPan(false);
    const SDL_Rect radarRect {8, 582, 176, 130};
    const SDL_Rect buildBtnRect {1072, 652, 96, 60};
    input.SetTouchUiRects({radarRect, buildBtnRect});

    auto touchFull = [&](Uint32 type, SDL_FingerID id, float px, float py) {
        SDL_Event event {};
        event.type = type;
        event.tfinger.fingerID = id;
        event.tfinger.x = px / 1280.0f;
        event.tfinger.y = py / 720.0f;
        Check(SDL_ConvertEventToRenderCoordinates(renderer, &event), "touch conversion");
        input.ProcessEvent(event, touchCam);
        input.Update(0, touchCam);
    };

    // A quick map drag pans; holding first switches the same gesture to selection.
    const float camBeforeX = touchCam.GetX();
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 200, 200);
    Check(!input.GetSelectionBox().active, "map touch waits for gesture");
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 360, 320);
    Check(!input.GetSelectionBox().active && touchCam.GetX() != camBeforeX, "quick drag pans");
    touchFull(SDL_EVENT_FINGER_UP, 1, 360, 320);
    Check(!input.WasLeftClickIssued(x, y), "pan must not click");
    const float selectionCamX = touchCam.GetX();
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 200, 200);
    SDL_Delay(510);
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 360, 320);
    Check(input.GetSelectionBox().active && input.GetSelectionBox().HasSignificantArea(), "hold drag selects");
    Check(touchCam.GetX() == selectionCamX, "selection must not pan");
    touchFull(SDL_EVENT_FINGER_UP, 1, 360, 320);
    Check(input.GetSelectionBox().active, "completed selection persists until consumed");
    input.CancelSelectionBox();
    Check(!input.WasLeftClickIssued(x, y), "selection must not click");

    // 2) Small diagonal drag (>12px Euclidean, <=12px per axis) must not issue a stray tap click
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 200, 200);
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 209, 209);
    touchFull(SDL_EVENT_FINGER_UP, 1, 209, 209);
    Check(!input.GetSelectionBox().active, "diagonal drag ends cleanly");
    Check(!input.WasLeftClickIssued(x, y), "diagonal drag past slop must not issue a tap click");

    // 3) Second finger during box drag switches to 2-finger pan/pinch and cancels selection box
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 200, 200);
    SDL_Delay(510);
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 250, 240);
    Check(input.GetSelectionBox().active, "box active before second finger");
    touchFull(SDL_EVENT_FINGER_DOWN, 2, 450, 240);
    Check(!input.GetSelectionBox().active, "second finger cancels box selection");
    const float pinchZoomBefore = touchCam.GetZoom();
    touchFull(SDL_EVENT_FINGER_MOTION, 2, 550, 240);
    Check(touchCam.GetZoom() > pinchZoomBefore, "2-finger pinch zooms during touch mode");
    touchFull(SDL_EVENT_FINGER_UP, 1, 250, 240);
    touchFull(SDL_EVENT_FINGER_UP, 2, 550, 240);
    Check(!input.WasLeftClickIssued(x, y) && !input.WasRightClickIssued(x, y), "multi-touch must not click");

    // 4) Dragging on minimap/radar or HUD buttons must never trigger box selection
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 50, 640);
    Check(!input.GetSelectionBox().active, "touch on radar must not start selection box");
    Check(input.IsLeftMouseDown(), "touch on radar holds left mouse for radar drag");
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 120, 680);
    Check(!input.GetSelectionBox().active, "dragging on radar must not activate selection box");
    touchFull(SDL_EVENT_FINGER_UP, 1, 120, 680);
    Check(!input.IsLeftMouseDown(), "lifting from radar releases left mouse");
    Check(!input.WasLeftClickIssued(x, y), "dragging on radar must not emit tap click");

    touchFull(SDL_EVENT_FINGER_DOWN, 1, 1100, 680);
    Check(!input.GetSelectionBox().active, "touch on HUD button must not start selection box");
    touchFull(SDL_EVENT_FINGER_UP, 1, 1100, 680);
    Check(input.WasLeftClickIssued(x, y), "tapping HUD button inside viewport still clicks on release");

    // Swiping a HUD panel scrolls its content without a click or a map zoom.
    input.SetTouchUiRects({radarRect, buildBtnRect, SDL_Rect {900, 300, 300, 200}});
    input.ConsumeUiWheel();
    input.ConsumeMapWheel();
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 1000, 450);
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 1000, 350);
    touchFull(SDL_EVENT_FINGER_UP, 1, 1000, 350);
    Check(input.ConsumeUiWheel() == -2, "HUD swipe scrolls two rows");
    Check(input.ConsumeMapWheel() == 0 && !input.WasLeftClickIssued(x, y), "HUD swipe must not zoom or click");

    // The cards panel follows the pointer directly, without converting motion to wheel steps.
    input.SetTouchScrollPanel(SDL_Rect {900, 300, 300, 200});
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 1000, 450);
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 1000, 425);
    Check(input.IsLeftMouseDown() && input.GetMouseScreenY() == 425, "cards follow the finger during a drag");
    Check(input.ConsumeUiWheel() == 0, "cards drag must not jump by wheel steps");
    touchFull(SDL_EVENT_FINGER_UP, 1, 1000, 425);
    Check(!input.WasLeftClickIssued(x, y), "cards drag must not activate a card");
    input.SetTouchScrollPanel(SDL_Rect {0, 0, 0, 0});

    // A mouse wheel over the touch grid (desktop --touch-ui) must scroll it rather than zoom the map.
    SDL_Event wheel {};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = -1.0f;
    wheel.wheel.mouse_x = 1000.0f;
    wheel.wheel.mouse_y = 400.0f;
    input.ProcessEvent(wheel, touchCam);
    Check(input.ConsumeUiWheel() == -1, "wheel over touch grid must scroll the HUD");
    Check(input.ConsumeMapWheel() == 0, "wheel over touch grid must not zoom the map");
    wheel.wheel.mouse_x = 400.0f;
    input.ProcessEvent(wheel, touchCam);
    Check(input.ConsumeMapWheel() == -1, "wheel over the map must still zoom");

    // 5) Building placement mode (SetTouchPan(true)) disables box selection:
    //    dragging outside the ghost pans the camera; dragging on the ghost holds pointer without panning
    input.SetTouchPan(true);
    const SDL_Rect ghostRect {580, 300, 120, 120};
    input.SetTouchUiRects({radarRect, buildBtnRect, ghostRect});
    input.SetTouchPlacementGhost(ghostRect);
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 640, 360);
    SDL_Delay(510);
    Check(!input.WasLeftClickIssued(x, y), "holding the ghost must not build before release");
    touchFull(SDL_EVENT_FINGER_UP, 1, 640, 360);
    Check(input.WasLeftClickIssued(x, y), "releasing a stationary ghost confirms placement");
    Check(!input.WasRightClickIssued(x, y), "holding the ghost must not issue a contextual command");

    const float placeCamX = touchCam.GetX();
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 200, 200);
    Check(!input.GetSelectionBox().active, "placing mode must not start selection box on map");
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 260, 200);
    Check(!input.GetSelectionBox().active, "placing mode must not show selection box while dragging map");
    Check(touchCam.GetX() < placeCamX, "dragging map outside ghost pans camera during placement");
    touchFull(SDL_EVENT_FINGER_UP, 1, 260, 200);
    Check(!input.WasLeftClickIssued(x, y), "panning during placement must not emit tap click");

    touchFull(SDL_EVENT_FINGER_DOWN, 1, 640, 360);
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 660, 360);
    touchFull(SDL_EVENT_FINGER_UP, 1, 660, 360);
    Check(input.WasLeftClickIssued(x, y), "release confirms when the ghost stayed on the same tile");

    const float ghostCamX = touchCam.GetX();
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 640, 360);
    Check(!input.GetSelectionBox().active, "touch on ghost must not start selection box");
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 700, 360);
    Check(input.GetLeftPressX() == 640 && input.GetLeftPressY() == 360,
          "drag retains the initial touch position after moving");
    Check(!input.GetSelectionBox().active, "dragging ghost must not start selection box");
    Check(touchCam.GetX() == ghostCamX, "dragging ghost must not pan camera");
    Check(input.IsLeftMouseDown(), "dragging ghost keeps left mouse down for TouchHUD");
    touchFull(SDL_EVENT_FINGER_UP, 1, 700, 360);
    Check(!input.WasLeftClickIssued(x, y), "releasing a dragged ghost must not build");

    // 6) Small touch motion (<= 12px) that still moved the ghost must not confirm on release
    input.SetTouchPlacementGhost(ghostRect);
    touchFull(SDL_EVENT_FINGER_DOWN, 1, 640, 360);
    input.SetTouchPlacementGhost(SDL_Rect {604, 300, 120, 120});
    touchFull(SDL_EVENT_FINGER_MOTION, 1, 646, 360);
    touchFull(SDL_EVENT_FINGER_UP, 1, 646, 360);
    Check(!input.WasLeftClickIssued(x, y), "small touch move that shifts ghost must not build on release");

    // 7) Desktop --touch-ui mouse on placement ghost waits for release and cancels if ghost moved
    input.SetTouchPlacementGhost(ghostRect);
    auto mouseEvt = [&](Uint32 type, float mx, float my) {
        SDL_Event event {};
        event.type = type;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = mx;
        event.button.y = my;
        input.ProcessEvent(event, touchCam);
        input.Update(0, touchCam);
    };
    mouseEvt(SDL_EVENT_MOUSE_BUTTON_DOWN, 640.0f, 360.0f);
    Check(!input.WasLeftClickIssued(x, y), "mouse press on ghost must not build before release");
    mouseEvt(SDL_EVENT_MOUSE_BUTTON_UP, 640.0f, 360.0f);
    Check(input.WasLeftClickIssued(x, y), "mouse release on stationary ghost confirms placement");

    mouseEvt(SDL_EVENT_MOUSE_BUTTON_DOWN, 640.0f, 360.0f);
    input.SetTouchPlacementGhost(SDL_Rect {604, 300, 120, 120});
    mouseEvt(SDL_EVENT_MOUSE_BUTTON_UP, 646.0f, 360.0f);
    Check(!input.WasLeftClickIssued(x, y), "mouse release after moving ghost must not build");

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << "Android touch tests passed\n";
}
