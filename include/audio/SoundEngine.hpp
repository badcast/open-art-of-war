#pragma once

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
     * Music is configured by <assetRoot>/data/tracklist.json.
     */
    class SoundEngine
    {
    public:
        SoundEngine();
        ~SoundEngine();

        bool init(const std::string &assetRoot = "assets");
        void update(bool inMainMenu, bool inGame);
        void playMenuMusic();
        void playRandomGameTrack();
        void stopMusic();
        void nextTrack(); // player: mainmenu -> tracks... -> mainmenu
        void prevTrack();
        void setMusicVolume(int volume); // 0..128
        void setFxVolume(int volume);    // 0..128 (no sound effects yet)
        void toggleMute();

        bool isMuted() const
        {
            return m_muted;
        }
        int getCurrentTrack() const
        {
            return m_currentTrack;
        }
        int getTrackCount() const
        {
            return static_cast<int>(m_gameTracks.size() + (m_hasMenuTrack ? 1 : 0));
        }
        std::string getCurrentTrackName() const;
        double getTrackPosition() const; // seconds into the current track
        double getTrackLength() const;   // seconds of the current track's played range, 0 when nothing plays
        void seekTrack(double ratio);    // 0..1 within the played range
        void cleanup();

    private:
        struct Track
        {
            std::string path;
            std::string name;
            double effectiveMin {0.0};
            double effectiveMax {-1.0};
        };

        static void SDLCALL feedAudio(void *userdata, SDL_AudioStream *stream, int additionalAmount, int totalAmount);
        void applyGain();
        void closeDecoder();
        bool loadTrack(const Track &track, bool loop);
        void advanceGameTrack();
        void playPlayerTrack(int index);

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
        SDL_Mutex *m_lock {nullptr}; // guards the decoder shared with the audio thread
        stb_vorbis *m_decoder {nullptr};
        std::vector<uint8_t> m_oggData; // encoded track the decoder reads from
        int m_channels {2};
        int m_sampleRate {44100};
        std::vector<short> m_pcm;
    };

} // namespace OpenArtOfWar
