#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <vector>

namespace OpenArtOfWar
{

    // Where a sound in the world is heard: full volume inside the view, fading out over `rangePx` beyond its
    // edge, panned by its place across the view
    struct SfxPlacement
    {
        bool audible {false};
        float gain {0.0f};
        float pan {0.0f}; // -1 left .. 1 right
    };
    SfxPlacement PlaceSfx(float worldX, float worldY, float viewX, float viewY, float viewW, float viewH, float rangePx);

    // Sound effects: short clips decoded once (mono 44.1 kHz), mixed into a stereo stream by up to `voices`
    // voices. Events (assets/data/sfx.json) pick one of their clips at random and bring their own volume,
    // pitch spread, limits and priority. Announcer lines are queued and play one after the other.
    // Thread-safe: mix() runs on the audio thread, everything else on the game thread. No audio device needed
    // (the game's SoundEngine feeds mix() to SDL), so it works and can be tested headless.
    class SfxMixer
    {
    public:
        static constexpr int kRate = 44100;

        enum class Category : uint8_t
        {
            Weapon,
            Impact,
            Ui,
            Voice,     // unit replies
            Announcer, // queued, one at a time
            Alert
        };

        struct EventConfig
        {
            Category category {Category::Ui};
            float volume {1.0f};
            float pitchJitter {0.0f}; // semitones, either way
            int maxInstances {4};
            int minIntervalMs {0};
            int cooldownMs {0}; // announcer: this line not again before
            int priority {5};   // 0..10
            bool positional {false};
            float rangeTiles {12.0f};
            std::vector<int> clips;
        };

        // sfx.json and the <event>_<n>.ogg files of `dir` (read through AssetStore); false when nothing loaded
        bool LoadBank(const std::string &configPath, const std::string &dir);
        // The <event>_<n>.ogg clips of `dir`; `onlyMissing`: only for events that have no clip yet (a fallback
        // language). Returns the number of clips loaded.
        int LoadClips(const std::string &dir, bool onlyMissing = false);
        // Event settings from JSON ({"defaults": {...}, "events": {name: {...}}, "voices": n})
        void Configure(const nlohmann::json &doc);
        // A clip of an event (mono, kRate); tests and loadBank
        void AddClip(const std::string &event, std::vector<int16_t> pcm);

        bool Has(const std::string &event) const;
        const EventConfig *Config(const std::string &event) const;
        size_t EventCount() const;

        // Starts one of the event's clips; `gain` 0..1 (distance), `pan` -1..1. False when the event is
        // unknown, silent or held back by its limits.
        bool Play(const std::string &event, float gain, float pan, uint64_t nowMs);
        // Queues an announcer line (dropped while its cooldown runs or when it is queued already)
        bool Announce(const std::string &event, uint64_t nowMs);
        // Starts the next announcer line once the current one is over; call once per frame
        void Update(uint64_t nowMs);
        // Adds `frames` stereo frames (interleaved int16) of everything playing
        void Mix(int16_t *out, int frames);
        void StopAll();

        void SetFxVolume(int volume);    // 0..128: weapons, impacts, interface, alerts
        void SetVoiceVolume(int volume); // 0..128: unit replies and the announcer
        void SetMuted(bool muted);

        int ActiveVoices() const;
        bool AnnouncerBusy() const;
        size_t AnnouncerQueued() const;

    private:
        struct Voice
        {
            int clip {-1};
            double pos {0.0};
            double step {1.0};
            float gainL {0.0f};
            float gainR {0.0f};
            int priority {0};
            int event {-1};
            bool voiceVolume {false}; // Voice / Announcer category
            bool announcer {false};
        };
        struct EventState
        {
            EventConfig cfg;
            std::string name;
            uint64_t lastPlayMs {0};
            bool played {false};
        };

        int EventIndex(const std::string &event) const;
        bool StartLocked(int eventIdx, float gain, float pan, uint64_t nowMs, bool announcer);
        bool AnnounceLocked(int eventIdx, uint64_t nowMs);
        int InstancesLocked(int eventIdx) const;

        mutable std::mutex m_lock;
        std::vector<std::vector<int16_t>> m_clips;
        std::vector<EventState> m_events;
        std::map<std::string, int> m_eventIndex;
        nlohmann::json m_defaults = nlohmann::json::object();
        std::vector<Voice> m_voices;
        int m_maxVoices {32};
        std::deque<int> m_announcerQueue;
        std::mt19937 m_rng {0x5f5fu}; // presentation only, never the simulation's random numbers
        float m_fxGain {1.0f};
        float m_voiceGain {1.0f};
        bool m_muted {false};
    };

} // namespace OpenArtOfWar
