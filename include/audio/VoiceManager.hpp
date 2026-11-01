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
    bool load(const std::string &language, const std::string &assetRoot);
    // Loads another language when it differs from the current one
    void SetLanguage(const std::string &language);
    const std::string &Language() const
    {
        return m_language;
    }

    // A unit reply (category voice); false when unknown or held back by its limits
    bool Play(const std::string &event, uint64_t nowMs);
    // An announcer line: queued, one at a time, each with its own cooldown
    bool Announce(const std::string &event, uint64_t nowMs);
    // Starts the next announcer line when the current one is over; once per frame
    void Update(uint64_t nowMs);
    // Adds `frames` stereo frames (interleaved int16) of the voices speaking
    void Mix(int16_t *out, int frames);
    void StopAll();

    void SetVolume(int volume); // 0..128
    void SetMuted(bool muted);
    bool Has(const std::string &event) const;
    size_t LineCount() const
    {
        return m_lines;
    }

private:
    mutable std::mutex m_swap; // guards m_mixer against the audio thread while a language is loaded
    std::unique_ptr<SfxMixer> m_mixer;
    std::string m_language;
    std::string m_root;
    int m_volume {128};
    bool m_muted {false};
    size_t m_lines {0};
};

} // namespace OpenArtOfWar
