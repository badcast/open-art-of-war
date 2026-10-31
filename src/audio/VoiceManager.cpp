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
        if(Assets::readText(LoaderResources::voicesConfig(assetRoot), text))
            mixer->configure(JsonFile::readFromString(text));
        else
            std::cerr << "[VoiceManager] " << LoaderResources::voicesConfig(assetRoot) << " not found: voices play with default settings" << std::endl;
        int lines = mixer->loadClips(LoaderResources::voicesDir(language, assetRoot));
        const std::string fallback = LoaderResources::voicesFallbackLanguage();
        int borrowed = 0;
        if(language != fallback)
            borrowed = mixer->loadClips(LoaderResources::voicesDir(fallback, assetRoot), true);
        mixer->setVoiceVolume(m_volume);
        mixer->setFxVolume(m_volume); // whatever the category: every line of this mixer is a voice
        mixer->setMuted(m_muted);
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

    void VoiceManager::setLanguage(const std::string &language)
    {
        if(language == m_language && m_mixer)
            return;
        load(language, m_root.empty() ? LoaderResources::kDefaultRoot : m_root);
    }

    bool VoiceManager::play(const std::string &event, uint64_t nowMs)
    {
        std::lock_guard<std::mutex> guard(m_swap);
        return m_mixer && m_mixer->play(event, 1.0f, 0.0f, nowMs);
    }

    bool VoiceManager::announce(const std::string &event, uint64_t nowMs)
    {
        std::lock_guard<std::mutex> guard(m_swap);
        return m_mixer && m_mixer->announce(event, nowMs);
    }

    void VoiceManager::update(uint64_t nowMs)
    {
        std::lock_guard<std::mutex> guard(m_swap);
        if(m_mixer)
            m_mixer->update(nowMs);
    }

    void VoiceManager::mix(int16_t *out, int frames)
    {
        std::lock_guard<std::mutex> guard(m_swap);
        if(m_mixer)
            m_mixer->mix(out, frames);
    }

    void VoiceManager::stopAll()
    {
        std::lock_guard<std::mutex> guard(m_swap);
        if(m_mixer)
            m_mixer->stopAll();
    }

    void VoiceManager::setVolume(int volume)
    {
        m_volume = volume;
        std::lock_guard<std::mutex> guard(m_swap);
        if(m_mixer)
        {
            m_mixer->setVoiceVolume(volume);
            m_mixer->setFxVolume(volume);
        }
    }

    void VoiceManager::setMuted(bool muted)
    {
        m_muted = muted;
        std::lock_guard<std::mutex> guard(m_swap);
        if(m_mixer)
            m_mixer->setMuted(muted);
    }

    bool VoiceManager::has(const std::string &event) const
    {
        std::lock_guard<std::mutex> guard(m_swap);
        return m_mixer && m_mixer->has(event);
    }

} // namespace OpenArtOfWar
