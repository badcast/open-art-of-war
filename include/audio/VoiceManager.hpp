#pragma once

#include "audio/SfxMixer.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace OpenArtOfWar
{

// Voices of the game: unit replies and the announcer, in the language of the game.
// assets/data/audio_voices.json holds the event settings, assets/audio/voices/<language>/<event>_<n>.ogg the lines;
// a line missing in the language comes from the fallback language (English). Switching the language loads
// the other set (the clips are decoded once per switch). Mixed into the sound effects stream by SoundEngine;
// mix() runs on the audio thread, everything else on the game thread.
class VoiceManager
{
public:
    // Loads the lines of `language` (with the fallback ones); false when no line could be loaded
    bool Load(const std::string &language);
    inline bool load(const std::string &language)
    {
        return Load(language);
    }
    // Loads another language when it differs from the current one
    void SetLanguage(const std::string &language);
    const std::string &Language() const
    {
        return m_language;
    }

    // A unit reply (category voice); false when unknown or held back by its limits
    bool Play(const std::string &event, std::uint64_t nowMs);
    // An announcer line: queued, one at a time, each with its own cooldown
    bool Announce(const std::string &event, std::uint64_t nowMs);
    // Starts the next announcer line when the current one is over; once per frame
    void Update(std::uint64_t nowMs);
    // Adds `frames` stereo frames (interleaved int16) of the voices speaking
    void Mix(std::int16_t *out, int frames);
    void StopAll();

    void SetVolume(int volume); // 0..128
    void SetMuted(bool muted);
    // The voices keep quiet (an interface that is silent: replies and the announcer)
    void SetSilenced(bool silenced);
    bool Has(const std::string &event) const;
    std::size_t LineCount() const
    {
        return m_lines;
    }

private:
    mutable std::mutex m_swap; // guards m_mixer against the audio thread while a language is loaded
    std::unique_ptr<SfxMixer> m_mixer;
    std::string m_language;
    int m_volume {128};
    bool m_muted {false};
    bool m_silenced {false};
    std::size_t m_lines {0};
};

} // namespace OpenArtOfWar
