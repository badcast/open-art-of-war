#pragma once

#include "BattleSim.hpp"
#include "CommentedYaml.hpp"
#include "EditorCommon.hpp"
#include "graphics/ShaderEffects.hpp"
#include "game/Projectiles.hpp"

#include <mutex>
#include <nlohmann/json.hpp>
#include <set>

namespace CharEditor
{

// A frame of the sprite being edited in the Sprite Tools tab
struct SpriteFrameEdit
{
    std::string name;
    std::string original;       // its name in the file; empty: added since the load
    int rect[4] {0, 0, 1, 1};   // pixels in the image
    float size[2] {1.0f, 1.0f}; // drawn size in game units
    float ppu[2] {1.0f, 1.0f};  // pixels per game unit (the drawn size follows a resize at this ratio)
    bool ownSize {false};       // the file gives the size
    int recolor {-1};           // -1: as the sprite
    int palette {0};
};

enum class SpriteDrag
{
    None,
    Move,
    Resize,
    Draw
};

// A frame on the Sprite Merge canvas: where it is in its object's image and where it goes in the result
struct MergePiece
{
    std::string object;
    std::string frame;
    int src[4] {0, 0, 1, 1};
    int x {0}, y {0};
    bool selected {false};
};

enum class MergeDrag
{
    None,
    Move,
    Box
};

class CharacterEditor
{
public:
    const std::string &ErrorMessage() const
    {
        return status;
    }

    bool Init();

    int run();

    ~CharacterEditor();

private:
    void SortCatalogFiles();

    void LoadCatalog();

    bool loadFile(const fs::path &path);

    bool SaveFile();

    bool SaveFileAs();

    void ReloadCatalogSelection();

    YAML::Node Section(const char *name);

    bool EditString(
        YAML::Node parent, const char *key, const char *label, const std::string &scope, bool multiline = false);

    bool EditInt(YAML::Node parent, const char *key, const char *label, int fallback = 0);

    bool EditFloat(YAML::Node parent, const char *key, const char *label, float fallback = 0.0f, float step = 0.05f);

    bool EditBool(YAML::Node parent, const char *key, const char *label, bool fallback = false);

    // Delay between attack cycles: Fixed (always `cooldown`) or Random (a new delay in [min, max] every cycle)
    bool EditWeaponDelay(YAML::Node w);

    bool EditProjectileCombo(YAML::Node parent);

    // A sound of the character file: one of the events of data/audio_sfx.json, or none (the game's default for it).
    // `defaultLabel` names what is heard without one
    bool EditSoundCombo(YAML::Node parent, const char *key, const char *label, const char *defaultLabel);
    // The sound fields of a gun (shot, hit, volume)
    bool EditWeaponSounds(YAML::Node w);
    // The events of audio_sfx.json (name, category), read once
    const std::vector<std::pair<std::string, std::string>> &SfxEvents();
    std::vector<std::pair<std::string, std::string>> sfxEvents;
    bool sfxEventsRead = false;

    void SetMode(PreviewMode mode);

    // Animator tab (EditorAnimator.cpp): timing of the pictures of the selected character
    void DrawAnimatorProperties();

    void DrawEditor();

    // Move preview driven like the game: drive right, turn around on the spot (vehicles swing through
    // south, one 45-degree step per turn delay), drive left, turn back - in a loop
    struct MoveSample
    {
        float offsetX {0.0f};
        int facing {0};
    };

    MoveSample SampleMove(int archetype, float moveSpeed, float time) const;

    // How far the turret sits from where it is with the hull facing south (the facing offsets are tuned in):
    // taken from the sprite's turret layer, exactly like the game does
    void TurretShift(int archetype, int hullFacing, float &dx, float &dy) const;

    // Wreck stage on screen: the stage picked in the Wreck section, otherwise the stages play in a loop
    int CurrentWreckStage(int stages) const;

    // Small picture of a wreck composite (first stage) for the family list, rendered once
    SDL_Texture *WreckThumbnail(int composite)
    {
        return CompositeThumbnail(composite, true);
    }

    // Picture of a composite: centred canvases (wrecks, 180x120) or top-left cameos (icons, 60x38)
    SDL_Texture *CompositeThumbnail(int composite, bool centred);

    // Archetype / building kind picked from what the game knows; everything that follows from it is shown, not edited
    bool DrawIdentity(bool isBuilding);

    // Build-menu icon of a building, picked from the cameo composites of the game
    bool DrawIconPicker(YAML::Node production);

    bool DrawWreckProperties(bool isBuilding);

    void DrawProperties();

    void DrawTileRadius(float centerX, float centerY, float rangeTiles, SDL_Color color, float thickness = 2.0f);
    void DrawRadiusCircle2px(float centerX, float centerY, float radiusPixels, SDL_Color color);

    void SetEditorMode(EditorMode mode);

    // Opponent (B) used by Compare and Simulation; -1 = mirror match against the edited unit itself
    bool LoadCompare(int index);

    void DrawOpponentPicker(bool allowMirror);

    void DrawCompareControls();

    void DrawCompareTable();

    void FrameBattleCamera();

    void StartBattle();

    // After any successful save the whole editor works with what is on disk now: the running battle starts over with
    // the saved values, the Compare opponent is read again when it is the saved file, the preview values are recomputed
    void ApplySaved(bool characterFile);
    void RecomputePreviewDerived();

    void RunBattleBatch();

    void DrawSimulationControls();

    void DrawPreviewControls();

    void DrawCanvasGuides(const ImVec2 &min, const ImVec2 &size);

    int PreviewAction() const;

    void DrawPreviewScene();

    void DrawCharacter(YAML::Node doc, const VisualOffsets &vo, bool primary);

    void DrawPreviewGrid();

    void UpdateCombatSim(float dt);

    // Research tab: the technology tree of one faction (assets/characters/research_<faction>.yaml)
    bool LoadResearch(int faction);
    bool SaveResearch();
    void DrawResearchTree();
    void DrawResearchProperties();

    // Sprite Tools tab: the per-object sprites (assets/sprites/sprite_<name>.png + _meta.yaml)
    fs::path SpriteDir() const;
    fs::path SpriteMetaPath(const std::string &object) const;
    void LoadSpriteObject(const std::string &object);
    std::string SpriteFrameLine(const SpriteFrameEdit &f) const;
    bool SaveSpriteObject();
    void ReloadSpriteData(); // the sprites again from disk, and the configs that keep sprite handles
    // Resamples the image (and the Resistance image) by `factor`; the frames follow, the drawn sizes stay
    bool ScaleSpriteImage(float factor, bool smooth);
    bool CreateSpriteObject(const fs::path &png, const std::string &name);
    std::string FreeFrameName(const std::string &prefix) const;
    void AddSpriteFrame(int x, int y, int w, int h, const std::string &prefix);
    void RemoveSpriteFrame(int index);
    void SetSpriteFrameRect(SpriteFrameEdit &f, int x, int y, int w, int h);
    void SliceSpriteGrid();
    void DetectSpriteFrames();
    void SyncSpriteObjectFromActive();
    void DrawSpriteTools();
    void DrawSpriteProperties();
    void ClearThumbnails();

    // Sprite Merge tab: several objects into one (frames rearranged freely, references rewritten, sources removed),
    // or chosen frames baked into one picture (a new object)
    struct MergeFrameInfo
    {
        std::string name;
        int rect[4];
    };
    const std::vector<MergeFrameInfo> &MergeFramesOf(const std::string &object);
    void MergeAddPieces(const std::string &object, const std::string &frame); // empty frame: all of them
    void MergeRemovePieces(const std::string &object, const std::string &frame);
    void MergeBounds(int &x0, int &y0, int &x1, int &y1) const;
    std::vector<char> MergeOverlaps() const; // per piece: overlaps another one
    void MergePack();
    void MergeReset();
    bool MergePrepare(); // checks, and the text of the confirmation
    bool MergeObjectsSave();
    bool BakeFramesSave();
    void DrawSpriteMerge();
    void DrawSpriteMergeProperties();

    // --- Settings tabs: Explosions, Wrecks, Game UI, Environment, AI Difficulty ---
    // Settings files are edited in the text, so their comments stay (CommentedYaml)
    bool LoadYamlFile(CommentedYaml &y, const char *name);
    bool SaveYamlFile(CommentedYaml &y);
    // One value / a whole map of a settings file as widgets (the comment above a key is its tooltip)
    bool EditYamlScalar(CommentedYaml &y, const std::string &path, const std::string &label);
    bool EditYamlTree(CommentedYaml &y, const std::string &path, int depth = 0);
    void SaveActiveSettings(); // Ctrl+S / the Save button of the current tab
    void DrawSettingsBar(CommentedYaml &y, const char *fileName);

    void EnsureEffects();
    void EffectsChanged(); // the game's shared effects settings follow the editor at once
    void DrawExplosionControls();
    void DrawWreckControls();
    void DrawExplosionProperties();
    void DrawWreckTabProperties();
    void DrawAnimGallery(float height);
    SDL_Texture *AnimThumbnail(int animId);
    void StartExplosionPreview(int preset);
    void UpdateEffectsSim(float dt);
    void DrawEffectsScene();

    // --- Projectiles tab (assets/characters/projectiles.yaml) ---
    void EnsureProjectiles();
    void ProjectilesChanged(); // the game's projectile settings follow the editor at once
    const OpenArtOfWar::ProjectileSpec &projectileSpec() const; // of the kind on the canvas
    void FireProjectilePreview();
    void UpdateProjectilesSim(float dt);
    void DrawProjectilesScene();
    void DrawProjectileControls();
    void DrawProjectileProperties();
    SDL_Texture *ZoomedThumbnail(int composite, float scale); // a composite magnified around its centre (96x64)
    std::map<std::pair<int, int>, SDL_Texture *> zoomThumbs;

    void DrawGameUi();
    void DrawGameUiProperties();
    void EnsureUi();

    void EnsureEnvironment();
    bool SaveEnvironment();
    bool EditEnvJson(nlohmann::ordered_json &node, const std::string &path, int depth);
    void DrawEnvironment();
    void DrawEnvironmentProperties();
    SDL_Texture *FontPreview(const std::string &path, float size, const std::string &text);

    void EnsureAi();
    void DrawAiDifficulty();
    void DrawAiProperties();

    SDL_Window *window {nullptr};
    SDL_Renderer *renderer {nullptr};
    std::unique_ptr<OpenArtOfWar::TextureManager> textures;
    std::unique_ptr<OpenArtOfWar::Renderer> gameRenderer;
    std::unique_ptr<OpenArtOfWar::ShaderEffects> shaderEffects; // army colours need the SDL GPU renderer
    int previewArmyColor {0};
    float previewBackground[3] {0.0f, 0.0f, 0.0f}; // canvas background (black by default)                  // 0 classic
                                                   // sheets, 1..8 the army colours of the game
    void DrawArmyColorCombo();
    std::map<int, SDL_Texture *> wreckThumbs; // wreck family pictures
    int wreckPreviewStage {-1};               // -1: the stages play in a loop
    int simHullFacing {2};                    // hull direction / sprite set of the edited character on screen,
    int simArchetype {0};                     // used by the shot simulation for the turret position
    std::unique_ptr<OpenArtOfWar::Camera> camera;
    fs::path assetsRoot;
    fs::path charactersDir;
    fs::path activePath;
    YAML::Node document;
    YAML::Node documentBase; // `document` as loaded / last saved: saving applies only the edits since (ConfigMerge)
    std::vector<CharacterFile> files;
    std::map<std::string, std::array<char, 1024>> textBuffers;
    std::array<char, 256> searchBuffer {};
    std::array<char, 512> savePath {};
    SDL_Rect previewRect {0, 0, 1, 1};
    std::string status {"Ready"};
    int selectedIndex {0};
    int facing {2};
    int turretFacingDir {2};
    int desiredHullFacingDir {2};
    int desiredTurretFacingDir {2};
    float previewHullTurnTimer {0.0f};
    float previewTurretTurnTimer {0.0f};
    bool linkTurretToHull {true};
    PreviewMode previewMode {PreviewMode::Idle};
    float playbackSpeed {1.0f};
    float previewZoom {2.0f};
    // The zoom range of the preview canvas: the wheel, the slider and the camera keep to it
    static constexpr float kPreviewZoomMin = 0.25f;
    static constexpr float kPreviewZoomMax = 8.0f;
    float previewTime {0.0f};
    float buildingProgress {1.0f};
    int previewHealthPercent {100};
    bool dirty {false};
    bool loopPreview {true};
    bool noGizmos {true};
    bool showAttackRange {true};
    bool showSightRange {true};
    bool showBuildRange {true};
    bool showRadarRange {true};
    bool showGround {true};
    bool showCombatHud {true};
    bool buildingPowered {true};
    float cameraPanX {0.0f};
    float cameraPanY {0.0f};
    float targetWorldX {0.0f};
    float targetWorldY {120.0f};
    bool targetCustomized {false};
    bool testSiegeMode {false};
    float leftPanelWidth {280.0f};
    float rightPanelWidth {460.0f};
    float spriteListWidth {280.0f};

    struct SimProjectile
    {
        float startX {0.0f};
        float startY {0.0f};
        float targetX {0.0f};
        float targetY {0.0f};
        float curX {0.0f};
        float curY {0.0f};
        float dirX {1.0f};
        float dirY {0.0f};
        int dir16 {0};
        int dir32 {0};
        float speed {620.0f};
        float totalDist {1.0f};
        float traveledDist {0.0f};
        std::string projectileType {"bullet"};
        bool isInfantry {false};
        bool active {true};
        bool impacted {false};
        float impactTimer {0.0f};
        float maxImpactTime {0.14f};
    };
    std::vector<SimProjectile> simProjectiles;
    float simShotCooldown {0.0f};
    int simBurstIndex {0};
    float simBurstDelayTimer {0.0f};
    float simShotCooldown2 {0.0f};
    int simBurstIndex2 {0};
    float simBurstDelayTimer2 {0.0f};
    bool testFlameDeath {false};
    float muzzleFlashTimer {0.0f};
    float muzzleFlashX {0.0f};
    float muzzleFlashY {0.0f};
    float previewBulletSpeed {620.0f};
    float previewFxSpeed {1.0f};
    float muzzleOffsetX {0.0f}; // localePosition X (Forward)
    float muzzleOffsetY {0.0f}; // localePosition Y (Lateral)
    float turretOffsetX {0.0f}; // Dome offset X
    float turretOffsetY {0.0f}; // Dome offset Y
    bool hasCustomTurretOffset {false};

    // Multi-Weapon & Siege Testing state
    bool simLabUpgradeUnlocked {true};
    int testWeaponSelectMode {0}; // 0 = Auto (distance/condition), 1 = Force Weapon 1, 2 = Force Weapon 2
    int activeFiringWeaponIndex {0};

    // Compare / Simulation modes
    EditorMode editorMode {EditorMode::Preview};
    YAML::Node compareDoc;
    fs::path comparePath;
    int compareIndex {-1};
    VisualOffsets compareOffsets;
    float compareSpacing {90.0f};
    YAML::Node researchDoc[2]; // 0 Confederation, 1 Resistance
    YAML::Node researchBase[2];
    fs::path researchPath[2];
    bool researchDirty[2] {false, false};
    int researchFaction {0};
    int researchSelected {0};
    CommentedYaml effectsYaml, uiYaml, aiYaml, projectilesYaml;
    bool projectilesTried {false};
    int projKind {0}; // 0 bullet, 1 sniper, 2 shell, 3 rocket, 4 flame
    float projDistance {260.0f};
    float projAngleDeg {0.0f};
    bool projSweep {false}; // every shot one direction further: all sprite directions in turn
    float projSweepAngle {0.0f};
    bool projLoop {true};
    float projInterval {0.6f};
    float projFireTimer {0.0f};
    float projCraterSpeed {8.0f}; // craters of the canvas go this many times faster than in the game
    int projShotCount {0};
    struct ProjShot
    {
        float sx {0.0f}, sy {0.0f}, x {0.0f}, y {0.0f}, tx {0.0f}, ty {0.0f};
        int dir16 {0}, dir32 {0};
        bool landed {false}, done {false};
        int impactAnim {-1}, mark {-1}, frame {0};
        float frameTimer {0.0f}, smokeTimer {0.0f};
    };
    struct ProjSmoke
    {
        float x {0.0f}, y {0.0f}, age {0.0f};
    };
    struct ProjCrater
    {
        float x {0.0f}, y {0.0f};
        int composite {0};
        float timeLeft {0.0f};
    };
    std::vector<ProjShot> projShots;
    std::vector<ProjSmoke> projSmoke;
    std::vector<ProjCrater> projCraters;
    std::vector<std::string> projectileUsers; // characters firing the kind on screen (filled when shown)
    bool effectsTried {false}, uiTried {false}, aiTried {false}, envTried {false};
    std::set<std::string> yamlAnimPaths; // keys of the open file that hold animation ids (get a Pick button)
    std::string animPickPath;            // the key the gallery click goes to ("" none)
    std::map<std::string, std::array<char, 256>> yamlTextBuffers;
    std::map<int, SDL_Texture *> animThumbs;
    int fxPreset {0}; // 0 vehicle, 1 small building, 2 power building, 3 large building, 4 one animation
    int fxAnim {11};  // animation of preset 4
    float fxTime {0.0f};
    float fxRestartTimer {0.0f};
    bool fxLoop {true};
    struct FxEvent
    {
        float x {0.0f}, y {0.0f};
        int anim {0};
        float delay {0.0f};
        float timer {0.0f};
        int frame {0};
        bool done {false};
    };
    std::vector<FxEvent> fxEvents;
    int wreckClass {0}; // 0 vehicle, 1 light, 2 heavy, 3..5 building rubble
    bool wreckPlay {true};
    float wreckPlaySeconds {12.0f}; // preview: the whole burial takes this long
    float wreckClock {0.0f};
    int wreckFaction {1};
    int uiTheme {0}; // 0 neutral, 1 resistance, 2 confederation
    nlohmann::ordered_json envJson;
    nlohmann::ordered_json envBase;
    fs::path envPath;
    bool envLoaded {false};
    bool envDirty {false};
    std::map<std::string, SDL_Texture *> fontPreviews;
    std::map<std::string, std::array<char, 512>> envTextBuffers;
    FolderPick fontPick[2]; // result of the file dialog of the regular / bold font
    int aiSelected {0};     // 0 easy .. 3 hardcore
    // Sprite Tools tab
    std::string spriteObject; // the sprite being edited
    std::string spriteImageName;
    std::array<char, 128> spriteSearch {};
    std::vector<SpriteFrameEdit> spriteFrames;
    std::vector<std::string> spriteRemoved; // frames removed since the load (names in the file)
    float spriteScale {1.0f};
    int spriteRecolor {0};
    int spriteImageW {0}, spriteImageH {0};
    int spriteFrameSel {-1};
    bool spriteDirty {false};
    float spriteZoom {2.0f};
    ImVec2 spritePan {16.0f, 16.0f};
    // Camera reset of the sprite canvas, applied on the next draw (it needs the canvas size)
    SpriteCamera spriteCameraPending {SpriteCamera::Fit};
    SpriteDrag spriteDrag {SpriteDrag::None};
    int spriteDragCorner {0};
    ImVec2 spriteDragStart {0.0f, 0.0f};
    int spriteDragRect[4] {0, 0, 0, 0};
    bool spriteDrawMode {false};
    int spriteScaleFactor {4}; // x4
    int spriteScaleSmooth {0}; // 0: pixels (nearest), 1: smooth
    int spriteGridCols {4}, spriteGridRows {1};
    bool spriteSkipEmpty {true};
    int spriteDetectGap {1};
    int spriteDetectMinArea {4};
    std::array<char, 512> spriteNewPng {};
    std::array<char, 64> spriteNewName {};
    FolderPick spritePngPick;
    std::string spriteReport;
    // Sprite Merge tab
    bool mergeBake {false};                // false: merge objects, true: bake frames into one picture
    std::vector<std::string> mergeObjects; // checked objects in the order they were checked (merge)
    std::vector<MergePiece> mergePieces;   // the layout; drawn in this order
    std::map<std::string, std::vector<MergeFrameInfo>> mergeFrameCache;
    std::array<char, 128> mergeSearch {};
    std::array<char, 64> mergeName {};
    int mergePadding {1};
    float mergeZoom {1.0f};
    ImVec2 mergePan {16.0f, 16.0f};
    SpriteCamera mergeCameraPending {SpriteCamera::Fit};
    MergeDrag mergeDrag {MergeDrag::None};
    ImVec2 mergeDragStart {0.0f, 0.0f};
    std::vector<std::array<int, 2>> mergeDragFrom;
    std::string mergeReport;
    std::string mergeConfirmText;
    BattleSim battle;
    BattleSim::Setup battleSetup;
    bool battlePaused {false};
    int battleBatchRuns {20};
    BattleBatchResult batchResult;
};

} // namespace CharEditor
