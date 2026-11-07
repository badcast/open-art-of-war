#include "audio/VoiceManager.hpp"
#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"
#include "core/LoaderResources.hpp"

#include <iostream>
#include <set>

namespace OpenArtOfWar
{

bool VoiceManager::Load(const std::string &language)
{
    // A fresh set is prepared off to the side and swapped in, so the audio thread never mixes a half-loaded one
    auto mixer = std::make_unique<SfxMixer>();
    std::string text;
    if (Assets::ReadText(LoaderResources::VoicesConfig(), text))
        mixer->Configure(JsonFile::ReadFromString(text));
    else
        std::cerr << "[VoiceManager] " << LoaderResources::VoicesConfig()
                  << " not found: voices play with default settings" << std::endl;
    int lines = mixer->LoadClips(LoaderResources::VoiceClips(language));
    const std::string fallback = LoaderResources::VoicesFallbackLanguage();
    int borrowed = 0;
    if (language != fallback)
        borrowed = mixer->LoadClips(LoaderResources::VoiceClips(fallback), true);
    mixer->SetVoiceVolume(m_volume);
    mixer->SetFxVolume(m_volume); // whatever the category: every line of this mixer is a voice
    mixer->SetGeneralVolume(m_generalVolume);
    mixer->SetMuted(m_muted);
    mixer->SetInterfaceMuted(m_silenced);
    {
        std::lock_guard<std::mutex> guard(m_swap);
        m_mixer = std::move(mixer);
    }
    m_language = language;
    m_lines = static_cast<std::size_t>(lines + borrowed);
    std::cout << "[VoiceManager] " << lines << " voice lines in '" << language << "'";
    if (borrowed > 0)
        std::cout << ", " << borrowed << " from '" << fallback << "'";
    std::cout << std::endl;
    return m_lines > 0;
}

void VoiceManager::SetLanguage(const std::string &language)
{
    if (language == m_language && m_mixer)
        return;
    load(language);
}

bool VoiceManager::Play(const std::string &event, std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(m_swap);
    return m_mixer && m_mixer->Play(event, 1.0f, 0.0f, nowMs);
}

bool VoiceManager::Announce(const std::string &event, std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(m_swap);
    return m_mixer && m_mixer->Announce(event, nowMs);
}

void VoiceManager::Update(std::uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(m_swap);
    if (m_mixer)
        m_mixer->Update(nowMs);
}

void VoiceManager::Mix(std::int16_t *out, int frames)
{
    std::lock_guard<std::mutex> guard(m_swap);
    if (m_mixer)
        m_mixer->Mix(out, frames);
}

void VoiceManager::StopAll()
{
    std::lock_guard<std::mutex> guard(m_swap);
    if (m_mixer)
        m_mixer->StopAll();
}

void VoiceManager::SetGeneralVolume(int volume)
{
    m_generalVolume = volume;
    std::lock_guard<std::mutex> guard(m_swap);
    if (m_mixer)
        m_mixer->SetGeneralVolume(volume);
}

void VoiceManager::SetVolume(int volume)
{
    m_volume = volume;
    std::lock_guard<std::mutex> guard(m_swap);
    if (m_mixer)
    {
        m_mixer->SetVoiceVolume(volume);
        m_mixer->SetFxVolume(volume);
    }
}

void VoiceManager::SetSilenced(bool silenced)
{
    m_silenced = silenced;
    std::lock_guard<std::mutex> guard(m_swap);
    if (m_mixer)
        m_mixer->SetInterfaceMuted(silenced);
}

void VoiceManager::SetMuted(bool muted)
{
    m_muted = muted;
    std::lock_guard<std::mutex> guard(m_swap);
    if (m_mixer)
        m_mixer->SetMuted(muted);
}

bool VoiceManager::Has(const std::string &event) const
{
    std::lock_guard<std::mutex> guard(m_swap);
    return m_mixer && m_mixer->Has(event);
}

} // namespace OpenArtOfWar
