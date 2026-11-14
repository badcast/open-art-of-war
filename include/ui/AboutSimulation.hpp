#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace OpenArtOfWar
{
class BitmapFontRenderer;
class Renderer;
class SoundEngine;
class TextureManager;

// A self-contained showcase world for the About screen.
class AboutSimulation
{
public:
    AboutSimulation();
    ~AboutSimulation();
    bool Start(
        TextureManager &textures,
        int width,
        int height,
        std::string_view configPath = "assets/data/aboutgame_simulation.yaml");
    void Update(float deltaTime, SoundEngine *sound = nullptr);
    void Render(
        Renderer &renderer,
        BitmapFontRenderer &font,
        const std::vector<std::string> &credits,
        int width,
        int height,
        bool showCredits = true);
    const std::string &MusicName() const;
    float SimulationSeconds() const;
    float CameraZoom() const;
    int ProducedUnits(unsigned player) const;
    int ProducingFactories(unsigned player) const;
    int MaxLivingUnitsOfAnyType() const;
    int InitialUnits(unsigned player) const;
    int MovedInitialUnits(unsigned player) const;
    int DamagedInitialUnits(unsigned player) const;
    unsigned WorldGeneration() const;
    int DistinctProducedTypes() const;
    bool AllResearchCompleted() const;
    float MusicVolume() const;
    float SfxVolume() const;
    void RestartCredits();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace OpenArtOfWar
