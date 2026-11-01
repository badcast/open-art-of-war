#include "audio/VoiceManager.hpp"
#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"
#include "core/LoaderResources.hpp"

#include <iostream>
#include <set>

namespace OpenArtOfWar
{

    bool VoiceManager::load(const std::string &language, const std::string &assetRoot)
    {
        m_root = assetRoot;
        // A fresh set is prepared off to the side and swapped in, so the audio thread never mixes a half-loaded one
        auto mixer = std::make_unique<SfxMixer>();
        std::string text;
        if(Assets::ReadText(LoaderResources::VoicesConfig(assetRoot), text))
            mixer->Configure(JsonFile::ReadFromString(text));
        else
            std::cerr << "[VoiceManager] " << LoaderResources::VoicesConfig(assetRoot) << " not found: voices play with default settings" << std::endl;
        int lines = mixer->LoadClips(LoaderResources::VoicesDir(language, assetRoot));
        const std::string fallback = LoaderResources::VoicesFallbackLanguage();
        int borrowed = 0;
        if(language != fallback)
            borrowed = mixer->LoadClips(LoaderResources::VoicesDir(fallback, assetRoot), true);
        mixer->SetVoiceVolume(m_volume);
        mixer->SetFxVolume(m_volume); // whatever the category: every line of this mixer is a voice
        mixer->SetMuted(m_muted);
        {
            std::lock_guard<std::mutex> guard(m_swap);
            m_mixer = std::move(mixer);
        }
        m_language = language;
        m_lines = static_cast<size_t>(lines + borrowed);
        std::cout << "[VoiceManager] " << lines << " voice lines in '" << language << "'";
        if(borrowed > 0)
            std::cout << ", " << borrowed << " from '" << fallback << "'";
        std::cout << std::endl;
        return m_lines > 0;
    }

    void VoiceManager::SetLanguage(const std::string &language)
    {
        if(language == m_language && m_mixer)
            return;
        load(language, m_root.empty() ? LoaderResources::kDefaultRoot : m_root);
    }

    bool VoiceManager::Play(const std::string &event, uint64_t nowMs)
    {
        std::lock_guard<std::mutex> guard(m_swap);
        return m_mixer && m_mixer->Play(event, 1.0f, 0.0f, nowMs);
    }

    bool VoiceManager::Announce(const std::string &event, uint64_t nowMs)
    {
        std::lock_guard<std::mutex> guard(m_swap);
        return m_mixer && m_mixer->Announce(event, nowMs);
    }

    void VoiceManager::Update(uint64_t nowMs)
    {
        std::lock_guard<std::mutex> guard(m_swap);
        if(m_mixer)
            m_mixer->Update(nowMs);
    }

    void VoiceManager::Mix(int16_t *out, int frames)
    {
        std::lock_guard<std::mutex> guard(m_swap);
        if(m_mixer)
            m_mixer->Mix(out, frames);
    }

    void VoiceManager::StopAll()
    {
        std::lock_guard<std::mutex> guard(m_swap);
        if(m_mixer)
            m_mixer->StopAll();
    }

    void VoiceManager::SetVolume(int volume)
    {
        m_volume = volume;
        std::lock_guard<std::mutex> guard(m_swap);
        if(m_mixer)
        {
            m_mixer->SetVoiceVolume(volume);
            m_mixer->SetFxVolume(volume);
        }
    }

    void VoiceManager::SetMuted(bool muted)
    {
        m_muted = muted;
        std::lock_guard<std::mutex> guard(m_swap);
        if(m_mixer)
            m_mixer->SetMuted(muted);
    }

    bool VoiceManager::Has(const std::string &event) const
    {
        std::lock_guard<std::mutex> guard(m_swap);
        return m_mixer && m_mixer->Has(event);
    }

} // namespace OpenArtOfWar
