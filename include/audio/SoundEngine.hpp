#pragma once

#include "audio/SoundIds.hpp"

#include "audio/SfxMixer.hpp"
#include "audio/VoiceManager.hpp"
#include "core/LoaderResources.hpp"

#include <SDL3/SDL.h>
#include <cstddef>
#include <string>
#include <vector>

struct stb_vorbis;

namespace OpenArtOfWar
{

/**
 * @brief Background music player: Ogg Vorbis tracks streamed through an SDL3 audio stream.
 *
 * Music is configured by <assetRoot>/data/audio_tracks.json.
 */
class SoundEngine
{
public:
    SoundEngine();
    ~SoundEngine();

    bool Init(const std::string &assetRoot = LoaderResources::kDefaultRoot);
    void Update(bool inMainMenu, bool inGame);
    void PlayMenuMusic();
    void PlayRandomGameTrack();
    void StopMusic();
    void NextTrack(); // player: mainmenu -> tracks... -> mainmenu
    void PrevTrack();
    void SetMusicVolume(int volume); // 0..128
    // The interface goes silent (a replay): UI sounds, alerts, unit replies and the announcer; battle sounds and the
    // music stay
    void SetInterfaceMuted(bool muted);
    void SetReverb(bool enabled); // reverb of explosions and heavy guns
    void SetFxVolume(int volume);    // 0..128: weapons and impacts
    void SetUiVolume(int volume);    // 0..128: interface clicks, dialogs and alerts
    void SetVoiceVolume(int volume); // 0..128: unit replies and the announcer
    // The sound of the surroundings in a match (main.json audio.ambient_day / ambient_night), looped on its own stream
    // until StopAmbient; the same one asked again keeps playing
    void PlayAmbient(bool night);
    void StopAmbient();
    void SetAmbientVolume(int volume); // 0..128

    // Sound effects (assets/data/audio_sfx.json, assets/audio/sfx), mixed into their own audio stream
    SfxMixer &Sfx()
    {
        return m_sfx;
    }
    // An event heard as is (interface, announcer-free alerts); `gain` 0..1, `pan` -1..1
    void PlaySfx(const std::string &event, float gain = 1.0f, float pan = 0.0f, float muffle = 0.0f);
    // The sounds of the game itself, by constant (SoundIds.hpp)
    void PlaySfx(WorldSfx sound, float gain = 1.0f, float pan = 0.0f, float muffle = 0.0f);
    const SfxMixer::EventConfig *SfxConfig(WorldSfx sound) const;
    // An event by name with clips to play (null: unknown, or no clip of it was found)
    const SfxMixer::EventConfig *SfxConfig(const std::string &event) const;
    void Announce(Announcement line);
    void PlayVoice(UnitReply reply, bool infantry);
    // Voices (VoiceManager, the game's language): an announcer line (queued, one at a time) and a unit reply
    void Announce(const std::string &event);
    void PlayVoice(const std::string &event);
    // The language of the voices (the game's language; lines missing in it are English)
    void SetVoiceLanguage(const std::string &language);
    VoiceManager &Voices()
    {
        return m_voices;
    }
    void ToggleMute();

    bool IsMuted() const
    {
        return m_muted;
    }
    int GetCurrentTrack() const
    {
        return m_currentTrack;
    }
    int GetTrackCount() const
    {
        return static_cast<int>(m_gameTracks.size() + (m_hasMenuTrack ? 1 : 0));
    }
    std::string GetCurrentTrackName() const;
    double GetTrackPosition() const; // seconds into the current track
    double GetTrackLength() const;   // seconds of the current track's played range, 0 when nothing plays
    void SeekTrack(double ratio);    // 0..1 within the played range
    void Cleanup();

private:
    struct Track
    {
        std::string path;
        std::string name;
        double effectiveMin {0.0};
        double effectiveMax {-1.0};
    };

    static void SDLCALL FeedAudio(void *userdata, SDL_AudioStream *stream, int additionalAmount, int totalAmount);
    static void SDLCALL FeedSfx(void *userdata, SDL_AudioStream *stream, int additionalAmount, int totalAmount);
    static void SDLCALL FeedAmbient(void *userdata, SDL_AudioStream *stream, int additionalAmount, int totalAmount);
    void ApplyGain();
    void CloseDecoder();
    bool LoadTrack(const Track &track, bool loop);
    void AdvanceGameTrack();
    void PlayPlayerTrack(int index);

    bool m_initialized {false};
    bool m_muted {false};
    int m_volume {96};
    int m_fxVolume {128};
    int m_currentTrack {-1};
    bool m_manualPlayback {false}; // the player was used: tracks follow the playlist instead of the default music
    int m_lastArea {0};            // 1 = menu, 2 = game, 0 = other; manual playback ends when it changes
    int m_musicContext {0};        // 0 = stopped, 1 = menu, 2 = game
    std::vector<Track> m_gameTracks;
    std::vector<size_t> m_shuffledTracks;
    size_t m_nextGameTrack {0};
    size_t m_lastGameTrack {static_cast<size_t>(-1)};
    Track m_menuTrack;
    bool m_hasMenuTrack {false};
    bool m_trackFinished {false};
    bool m_loopTrack {false};
    unsigned int m_startSample {0};
    unsigned int m_endSample {0};
    unsigned int m_currentSample {0};

    SDL_AudioStream *m_stream {nullptr};
    SfxMixer m_sfx;
    VoiceManager m_voices; // unit replies and the announcer, mixed into the same stream
    std::string m_assetRoot;
    SDL_AudioStream *m_sfxStream {nullptr}; // the effects: stereo 44.1 kHz from m_sfx
    std::vector<int16_t> m_sfxBuffer;       // audio thread only
    int m_voiceVolume {128};
    // The ambient loop: its decoder is only touched by its stream's callback while the stream exists
    SDL_AudioStream *m_ambientStream {nullptr};
    stb_vorbis *m_ambientDecoder {nullptr};
    std::vector<uint8_t> m_ambientOgg;
    std::vector<short> m_ambientPcm; // audio thread only
    int m_ambientChannels {2};
    std::string m_ambientPath;
    int m_ambientVolume {96};
    int m_uiVolume {128};
    SDL_Mutex *m_lock {nullptr}; // guards the decoder shared with the audio thread
    stb_vorbis *m_decoder {nullptr};
    std::vector<uint8_t> m_oggData; // encoded track the decoder reads from
    int m_channels {2};
    int m_sampleRate {44100};
    std::vector<short> m_pcm;
};

} // namespace OpenArtOfWar
