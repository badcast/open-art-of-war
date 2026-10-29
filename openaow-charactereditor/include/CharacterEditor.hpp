#pragma once

#include "BattleSim.hpp"
#include "CommentedYaml.hpp"
#include "EditorCommon.hpp"
#include "SpriteCatalog.hpp"
#include "SpriteExport.hpp"
#include "graphics/ShaderEffects.hpp"

#include <mutex>
#include <nlohmann/json.hpp>
#include <set>

namespace CharEditor {

class CharacterEditor {
public:
    const std::string& errorMessage() const { return status; }

    bool init();

    int run();

    ~CharacterEditor();

private:
    void sortCatalogFiles();

    void loadCatalog();

    bool loadFile(const fs::path& path);

    bool saveFile();

    bool saveFileAs();

    void reloadCatalogSelection();

    YAML::Node section(const char* name);

    bool editString(YAML::Node parent, const char* key, const char* label, const std::string& scope,
                    bool multiline = false);

    bool editInt(YAML::Node parent, const char* key, const char* label, int fallback = 0);

    bool editFloat(YAML::Node parent, const char* key, const char* label, float fallback = 0.0f,
                   float step = 0.05f);

    bool editBool(YAML::Node parent, const char* key, const char* label, bool fallback = false);

    // Delay between attack cycles: Fixed (always `cooldown`) or Random (a new delay in [min, max] every cycle)
    bool editWeaponDelay(YAML::Node w);

    bool editProjectileCombo(YAML::Node parent);

    void setMode(PreviewMode mode);

    void drawEditor();

    // Move preview driven like the game: drive right, turn around on the spot (vehicles swing through
    // south, one 45-degree step per turn delay), drive left, turn back - in a loop
    struct MoveSample {
        float offsetX{0.0f};
        int facing{0};
    };

    MoveSample sampleMove(int archetype, float moveSpeed, float time) const;

    // How far the turret sits from where it is with the hull facing south (the facing offsets are tuned in):
    // taken from the sprite's turret layer, exactly like the game does
    void turretShift(int archetype, int hullFacing, float& dx, float& dy) const;

    // Wreck stage on screen: the stage picked in the Wreck section, otherwise the stages play in a loop
    int currentWreckStage(int stages) const;

    // Small picture of a wreck composite (first stage) for the family list, rendered once
    SDL_Texture* wreckThumbnail(int composite) { return compositeThumbnail(composite, true); }

    // Picture of a composite: centred canvases (wrecks, 180x120) or top-left cameos (icons, 60x38)
    SDL_Texture* compositeThumbnail(int composite, bool centred);

    // Archetype / building kind picked from what the game knows; everything that follows from it is shown, not edited
    bool drawIdentity(bool isBuilding);

    // Build-menu icon of a building, picked from the cameo composites of the game
    bool drawIconPicker(YAML::Node production);

    bool drawWreckProperties(bool isBuilding);

    void drawProperties();

    void drawRadiusCircle2px(float centerX, float centerY, float radiusPixels, SDL_Color color);

    void setEditorMode(EditorMode mode);

    // Opponent (B) used by Compare and Simulation; -1 = mirror match against the edited unit itself
    bool loadCompare(int index);

    void drawOpponentPicker(bool allowMirror);

    void drawCompareControls();

    void drawCompareTable();

    void frameBattleCamera();

    void startBattle();

    void runBattleBatch();

    void drawSimulationControls();

    void drawPreviewControls();

    void drawCanvasGuides(const ImVec2& min, const ImVec2& size);

    int previewAction() const;

    void drawPreviewScene();

    void drawCharacter(YAML::Node doc, const VisualOffsets& vo, bool primary);

    void drawPreviewGrid();

    void updateCombatSim(float dt);

    // Research tab: the technology tree of one faction (assets/characters/research_<faction>.yaml)
    bool loadResearch(int faction);
    bool saveResearch();
    void drawResearchTree();
    void drawResearchProperties();

    // Sprite Tools tab: browser of the sprite data and the exports for repacking the assets
    void ensureSpriteCatalog();
    void drawSpriteTools();
    void drawSpriteProperties();
    void drawSpriteImage(int spriteId, float box);
    void drawSpriteImageAt(const ImVec2& pos, int spriteId, float box);
    void drawDepNode(const DepNode& node, int depth);

    // --- Settings tabs: Explosions, Wrecks, Game UI, Environment, AI Difficulty ---
    // Settings files are edited in the text, so their comments stay (CommentedYaml)
    bool loadYamlFile(CommentedYaml& y, const char* name);
    bool saveYamlFile(CommentedYaml& y);
    // One value / a whole map of a settings file as widgets (the comment above a key is its tooltip)
    bool editYamlScalar(CommentedYaml& y, const std::string& path, const std::string& label);
    bool editYamlTree(CommentedYaml& y, const std::string& path, int depth = 0);
    void saveActiveSettings();             // Ctrl+S / the Save button of the current tab
    void drawSettingsBar(CommentedYaml& y, const char* fileName);

    void ensureEffects();
    void effectsChanged();                 // the game's shared effects settings follow the editor at once
    void drawExplosionControls();
    void drawWreckControls();
    void drawExplosionProperties();
    void drawWreckTabProperties();
    void drawAnimGallery(float height);
    SDL_Texture* animThumbnail(int animId);
    void startExplosionPreview(int preset);
    void updateEffectsSim(float dt);
    void drawEffectsScene();

    void drawGameUi();
    void drawGameUiProperties();
    void ensureUi();

    void ensureEnvironment();
    bool saveEnvironment();
    bool editEnvJson(nlohmann::ordered_json& node, const std::string& path, int depth);
    void drawEnvironment();
    void drawEnvironmentProperties();
    SDL_Texture* fontPreview(const std::string& path, float size, const std::string& text);

    void ensureAi();
    void drawAiDifficulty();
    void drawAiProperties();

    SDL_Window* window{nullptr};
    SDL_Renderer* renderer{nullptr};
    std::unique_ptr<OpenArtOfWar::TextureManager> textures;
    std::unique_ptr<OpenArtOfWar::Renderer> gameRenderer;
    std::unique_ptr<OpenArtOfWar::ShaderEffects> shaderEffects;   // army colours need the SDL GPU renderer
    int previewArmyColor{0};                  // 0 classic sheets, 1..8 the army colours of the game
    void drawArmyColorCombo();
    std::map<int, SDL_Texture*> wreckThumbs;  // wreck family pictures
    int wreckPreviewStage{-1};               // -1: the stages play in a loop
    int simHullFacing{2};                    // hull direction / sprite set of the edited character on screen,
    int simArchetype{0};                     // used by the shot simulation for the turret position
    std::unique_ptr<OpenArtOfWar::Camera> camera;
    fs::path assetsRoot;
    fs::path charactersDir;
    fs::path activePath;
    YAML::Node document;
    std::vector<CharacterFile> files;
    std::map<std::string, std::array<char, 1024>> textBuffers;
    std::array<char, 256> searchBuffer{};
    std::array<char, 512> savePath{};
    SDL_Rect previewRect{0, 0, 1, 1};
    std::string status{"Ready"};
    int selectedIndex{0};
    int facing{2};
    int turretFacingDir{2};
    bool linkTurretToHull{true};
    PreviewMode previewMode{PreviewMode::Idle};
    float playbackSpeed{1.0f};
    float previewZoom{2.0f};
    float previewTime{0.0f};
    float buildingProgress{1.0f};
    int previewHealthPercent{100};
    bool dirty{false};
    bool loopPreview{true};
    bool showAttackRange{true};
    bool showSightRange{true};
    bool showGround{true};
    bool showCombatHud{true};
    bool buildingPowered{true};
    float cameraPanX{0.0f};
    float cameraPanY{0.0f};
    float targetWorldX{0.0f};
    float targetWorldY{120.0f};
    bool targetCustomized{false};
    bool testSiegeMode{false};
    float rightPanelWidth{460.0f};

    struct SimProjectile {
        float startX{0.0f};
        float startY{0.0f};
        float targetX{0.0f};
        float targetY{0.0f};
        float curX{0.0f};
        float curY{0.0f};
        float dirX{1.0f};
        float dirY{0.0f};
        int dir16{0};
        int dir32{0};
        float speed{620.0f};
        float totalDist{1.0f};
        float traveledDist{0.0f};
        std::string projectileType{"bullet"};
        bool isInfantry{false};
        bool active{true};
        bool impacted{false};
        float impactTimer{0.0f};
        float maxImpactTime{0.14f};
    };
    std::vector<SimProjectile> simProjectiles;
    float simShotCooldown{0.0f};
    int simBurstIndex{0};
    float simBurstDelayTimer{0.0f};
    float simShotCooldown2{0.0f};
    int simBurstIndex2{0};
    float simBurstDelayTimer2{0.0f};
    bool testFlameDeath{false};
    float muzzleFlashTimer{0.0f};
    float muzzleFlashX{0.0f};
    float muzzleFlashY{0.0f};
    float previewBulletSpeed{620.0f};
    float previewFxSpeed{1.0f};
    float muzzleOffsetX{0.0f}; // localePosition X (Forward)
    float muzzleOffsetY{0.0f}; // localePosition Y (Lateral)
    float turretOffsetX{0.0f}; // Dome offset X
    float turretOffsetY{0.0f}; // Dome offset Y
    bool hasCustomTurretOffset{false};

    // Multi-Weapon & Siege Testing state
    bool simLabUpgradeUnlocked{true};
    int testWeaponSelectMode{0}; // 0 = Auto (distance/condition), 1 = Force Weapon 1, 2 = Force Weapon 2
    int activeFiringWeaponIndex{0};

    // Compare / Simulation modes
    EditorMode editorMode{EditorMode::Preview};
    YAML::Node compareDoc;
    fs::path comparePath;
    int compareIndex{-1};
    VisualOffsets compareOffsets;
    float compareSpacing{90.0f};
    YAML::Node researchDoc[2]; // 0 Confederation, 1 Resistance
    fs::path researchPath[2];
    bool researchDirty[2]{false, false};
    int researchFaction{0};
    int researchSelected{0};
    CommentedYaml effectsYaml, uiYaml, aiYaml;
    bool effectsTried{false}, uiTried{false}, aiTried{false}, envTried{false};
    std::set<std::string> yamlAnimPaths;       // keys of the open file that hold animation ids (get a Pick button)
    std::string animPickPath;                  // the key the gallery click goes to ("" none)
    std::map<std::string, std::array<char, 256>> yamlTextBuffers;
    std::map<int, SDL_Texture*> animThumbs;
    int fxPreset{0};                           // 0 vehicle, 1 small building, 2 power building, 3 large building, 4 one animation
    int fxAnim{11};                            // animation of preset 4
    float fxTime{0.0f};
    float fxRestartTimer{0.0f};
    bool fxLoop{true};
    struct FxEvent {
        float x{0.0f}, y{0.0f};
        int anim{0};
        float delay{0.0f};
        float timer{0.0f};
        int frame{0};
        bool done{false};
    };
    std::vector<FxEvent> fxEvents;
    int wreckClass{0};                         // 0 vehicle, 1 light, 2 heavy, 3..5 building rubble
    bool wreckPlay{true};
    float wreckPlaySeconds{12.0f};             // preview: the whole burial takes this long
    float wreckClock{0.0f};
    int wreckFaction{1};
    int uiTheme{0};                            // 0 neutral, 1 resistance, 2 confederation
    nlohmann::ordered_json envJson;
    fs::path envPath;
    bool envLoaded{false};
    bool envDirty{false};
    std::map<std::string, SDL_Texture*> fontPreviews;
    std::map<std::string, std::array<char, 512>> envTextBuffers;
    FolderPick fontPick[2];                    // result of the file dialog of the regular / bold font
    int aiSelected{0};                         // 0 easy .. 3 hardcore
    SpriteCatalog spriteCatalog;
    bool spriteCatalogTried{false};
    bool spriteCatalogOk{false};
    std::vector<int> spriteThumbIds;       // per object: the sprite shown in the list
    int spriteGroupFilter{-1};             // -1 all, else SpriteGroup
    int spriteSelected{-1};
    int spriteAnchor{-1};                  // start of a Shift+click range
    std::set<int> spriteChecked;           // objects chosen for export (multi-select)
    int spriteTreeFor{-1};
    DepNode spriteTree;
    std::array<char, 128> spriteSearch{};
    std::array<char, 512> spriteOutDir{};
    SingleExportOptions singleOptions;
    std::string spriteReport;
    FolderPick folderPick;
    FolderPick importPick;                  // result of the folder dialog of the import
    std::array<char, 512> spriteInDir{};    // folder the import reads
    bool importConfirm{false};              // ask before the import overwrites the assets
    BattleSim battle;
    BattleSim::Setup battleSetup;
    bool battlePaused{false};
    int battleBatchRuns{20};
    BattleBatchResult batchResult;
};

} // namespace CharEditor
