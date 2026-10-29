#include "audio/SoundEngine.hpp"
#include "core/AssetStore.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Utility.hpp"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace OpenArtOfWar
{

    SoundEngine::SoundEngine()
    {
    }

    SoundEngine::~SoundEngine()
    {
        cleanup();
    }

    bool SoundEngine::init(const std::string &assetRoot)
    {
        if(m_initialized)
            return true;

        std::string tracklistText;
        if(Assets::readText(assetRoot + "/data/tracklist.json", tracklistText))
        {
            const nlohmann::json tracklist = nlohmann::json::parse(tracklistText, nullptr, false);
            if(tracklist.is_object())
            {
                auto parseTrack = [&](const nlohmann::json &item, Track &track)
                {
                    if(!item.is_object() || !item.contains("filename") || !item["filename"].is_string())
                        return false;
                    const std::string filename = item["filename"].get<std::string>();
                    if(filename.empty())
                        return false;
                    track.path = assetRoot + "/audio/" + filename;
                    track.name = item.contains("name") && item["name"].is_string() ? item["name"].get<std::string>() : filename.substr(0, filename.rfind('.'));
                    if((item.contains("effective_min") && !item["effective_min"].is_number()) || (item.contains("effective_max") && !item["effective_max"].is_number()))
                    {
                        std::cerr << "[SoundEngine] Invalid time range for " << track.path << std::endl;
                        return false;
                    }
                    track.effectiveMin = item.value("effective_min", 0.0);
                    track.effectiveMax = item.value("effective_max", -1.0);
                    if(!std::isfinite(track.effectiveMin) || !std::isfinite(track.effectiveMax) || track.effectiveMin < 0.0)
                    {
                        std::cerr << "[SoundEngine] Invalid time range for " << track.path << std::endl;
                        return false;
                    }
                    return true;
                };

                if(tracklist.contains("mainmenu") && tracklist["mainmenu"].is_array())
                {
                    for(const auto &item : tracklist["mainmenu"])
                    {
                        if(parseTrack(item, m_menuTrack))
                        {
                            m_hasMenuTrack = true;
                            break;
                        }
                    }
                }
                if(tracklist.contains("tracks") && tracklist["tracks"].is_array())
                {
                    for(const auto &item : tracklist["tracks"])
                    {
                        Track track;
                        if(parseTrack(item, track))
                            m_gameTracks.push_back(std::move(track));
                    }
                }
            }
        }
        if(!m_hasMenuTrack || m_gameTracks.empty())
        {
            std::cerr << "[SoundEngine] Warning: tracklist is missing or has no usable menu/game tracks" << std::endl;
        }
        m_shuffledTracks.resize(m_gameTracks.size());
        std::iota(m_shuffledTracks.begin(), m_shuffledTracks.end(), 0);

        std::shuffle(m_shuffledTracks.begin(), m_shuffledTracks.end(), OpenArtOfWar::Utility::rng());

        // Placeholder format; each track switches the stream input to its own rate and channel count
        SDL_AudioSpec spec {SDL_AUDIO_S16, 2, 44100};
        m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SoundEngine::feedAudio, this);
        if(!m_stream)
        {
            std::cerr << "[SoundEngine] Warning: could not open audio device: " << SDL_GetError() << std::endl;
            return false;
        }
        m_lock = SDL_CreateMutex();
        SDL_ResumeAudioStreamDevice(m_stream);

        std::cout << "[SoundEngine] SDL3 audio ready, " << m_gameTracks.size() << " game music tracks." << std::endl;
        m_initialized = true;
        applyGain();
        return true;
    }

    void SoundEngine::cleanup()
    {
        if(m_stream)
        {
            SDL_DestroyAudioStream(m_stream); // stops the callback before the decoder goes away
            m_stream = nullptr;
        }
        closeDecoder();
        if(m_lock)
        {
            SDL_DestroyMutex(m_lock);
            m_lock = nullptr;
        }
        m_gameTracks.clear();
        m_shuffledTracks.clear();
        m_hasMenuTrack = false;
        m_initialized = false;
        m_currentTrack = -1;
        m_musicContext = 0;
        m_manualPlayback = false;
    }

    void SoundEngine::closeDecoder()
    {
        if(m_decoder)
        {
            stb_vorbis_close(m_decoder);
            m_decoder = nullptr;
            m_oggData.clear();
        }
    }

    void SDLCALL SoundEngine::feedAudio(void *userdata, SDL_AudioStream *stream, int additionalAmount, int /*totalAmount*/)
    {
        auto *self = static_cast<SoundEngine *>(userdata);
        SDL_LockMutex(self->m_lock);
        if(self->m_decoder && additionalAmount > 0)
        {
            const int channels = self->m_channels;
            int framesWanted = additionalAmount / static_cast<int>(sizeof(short) * channels) + 1;
            self->m_pcm.resize(static_cast<size_t>(framesWanted * channels));
            int produced = 0;
            while(produced < framesWanted)
            {
                if(self->m_currentSample >= self->m_endSample)
                {
                    if(!self->m_loopTrack)
                    {
                        self->closeDecoder();
                        self->m_trackFinished = true;
                        break;
                    }
                    if(!stb_vorbis_seek(self->m_decoder, self->m_startSample))
                    {
                        self->closeDecoder();
                        self->m_trackFinished = true;
                        break;
                    }
                    self->m_currentSample = self->m_startSample;
                }
                const unsigned int samplesLeft = self->m_endSample - self->m_currentSample;
                const int framesToRead = std::min(framesWanted - produced, static_cast<int>(samplesLeft));
                if(framesToRead <= 0)
                    break;
                int got = stb_vorbis_get_samples_short_interleaved(self->m_decoder, channels, self->m_pcm.data() + produced * channels, framesToRead * channels);
                if(got > 0)
                {
                    produced += got;
                    self->m_currentSample += static_cast<unsigned int>(got);
                    continue;
                }
                self->closeDecoder();
                self->m_trackFinished = true;
                break;
            }
            if(produced > 0)
            {
                SDL_PutAudioStreamData(stream, self->m_pcm.data(), produced * channels * static_cast<int>(sizeof(short)));
            }
        }
        SDL_UnlockMutex(self->m_lock);
    }

    bool SoundEngine::loadTrack(const Track &track, bool loop)
    {
        if(!m_initialized)
            return false;
        // The decoder reads straight from this buffer, so it lives as long as the decoder does
        std::vector<uint8_t> oggData;
        int error = 0;
        stb_vorbis *decoder = nullptr;
        if(Assets::readBytes(track.path, oggData) && !oggData.empty())
        {
            decoder = stb_vorbis_open_memory(oggData.data(), static_cast<int>(oggData.size()), &error, nullptr);
        }
        if(!decoder)
        {
            std::cerr << "[SoundEngine] Warning: could not decode " << track.path << " (stb_vorbis error " << error << ")" << std::endl;
            return false;
        }
        const stb_vorbis_info info = stb_vorbis_get_info(decoder);
        const unsigned int length = stb_vorbis_stream_length_in_samples(decoder);
        const double sampleRate = static_cast<double>(info.sample_rate);
        const unsigned int startSample = static_cast<unsigned int>(std::min(static_cast<double>(length), std::floor(track.effectiveMin * sampleRate)));
        const unsigned int endSample = track.effectiveMax < 0.0 ? length : static_cast<unsigned int>(std::min(static_cast<double>(length), std::floor(track.effectiveMax * sampleRate)));
        if(endSample <= startSample || !stb_vorbis_seek(decoder, startSample))
        {
            std::cerr << "[SoundEngine] Warning: invalid or unseekable time range for " << track.path << std::endl;
            stb_vorbis_close(decoder);
            return false;
        }

        SDL_LockMutex(m_lock);
        closeDecoder();
        m_oggData = std::move(oggData);
        m_decoder = decoder;
        m_channels = std::clamp(info.channels, 1, 2);
        m_sampleRate = static_cast<int>(info.sample_rate);
        m_startSample = startSample;
        m_endSample = endSample;
        m_currentSample = startSample;
        m_loopTrack = loop;
        m_trackFinished = false;
        SDL_AudioSpec spec {SDL_AUDIO_S16, m_channels, static_cast<int>(info.sample_rate)};
        SDL_UnlockMutex(m_lock);
        SDL_ClearAudioStream(m_stream);
        SDL_SetAudioStreamFormat(m_stream, &spec, nullptr);
        return true;
    }

    void SoundEngine::playMenuMusic()
    {
        if(!m_initialized || !m_hasMenuTrack)
            return;
        SDL_LockMutex(m_lock);
        const bool alreadyPlaying = m_decoder != nullptr;
        SDL_UnlockMutex(m_lock);
        if(m_musicContext == 1 && alreadyPlaying)
            return;
        if(loadTrack(m_menuTrack, true))
        {
            m_musicContext = 1;
            m_currentTrack = 0;
        }
        else
        {
            stopMusic();
        }
    }

    void SoundEngine::advanceGameTrack()
    {
        if(!m_initialized || m_gameTracks.empty())
            return;
        if(m_nextGameTrack >= m_shuffledTracks.size())
        {
            std::mt19937 random(std::random_device {}());
            std::shuffle(m_shuffledTracks.begin(), m_shuffledTracks.end(), random);
            if(m_shuffledTracks.size() > 1 && m_shuffledTracks.front() == m_lastGameTrack)
            {
                std::swap(m_shuffledTracks.front(), m_shuffledTracks[1]);
            }
            m_nextGameTrack = 0;
        }
        const size_t index = m_shuffledTracks[m_nextGameTrack++];
        m_lastGameTrack = index;
        if(loadTrack(m_gameTracks[index], false))
        {
            m_musicContext = 2;
            m_currentTrack = static_cast<int>(index + 1);
        }
        else
        {
            stopMusic();
        }
    }

    void SoundEngine::playRandomGameTrack()
    {
        if(!m_initialized || m_gameTracks.empty())
            return;
        m_nextGameTrack = m_shuffledTracks.size();
        advanceGameTrack();
    }

    void SoundEngine::playPlayerTrack(int index)
    {
        const bool menuTrack = index == 0 && m_hasMenuTrack;
        const int gameIndex = m_hasMenuTrack ? index - 1 : index;
        const Track &track = menuTrack ? m_menuTrack : m_gameTracks[gameIndex];
        if(loadTrack(track, menuTrack))
        {
            m_musicContext = menuTrack ? 1 : 2;
            m_currentTrack = menuTrack ? 0 : gameIndex + 1;
            m_manualPlayback = true;
        }
        else
        {
            stopMusic();
        }
    }

    // In the game the playlist is the game tracks only; the main menu theme belongs to the menu
    void SoundEngine::nextTrack()
    {
        const int count = getTrackCount();
        const int first = (m_lastArea == 2 && m_hasMenuTrack && !m_gameTracks.empty()) ? 1 : 0;
        const int n = count - first;
        if(!m_initialized || n <= 0)
            return;
        const int cur = m_currentTrack < 0 ? -1 : (m_hasMenuTrack ? m_currentTrack : m_currentTrack - 1);
        const int rel = cur - first;
        playPlayerTrack(first + (rel < 0 ? 0 : (rel + 1) % n));
    }

    void SoundEngine::prevTrack()
    {
        const int count = getTrackCount();
        const int first = (m_lastArea == 2 && m_hasMenuTrack && !m_gameTracks.empty()) ? 1 : 0;
        const int n = count - first;
        if(!m_initialized || n <= 0)
            return;
        const int cur = m_currentTrack < 0 ? -1 : (m_hasMenuTrack ? m_currentTrack : m_currentTrack - 1);
        const int rel = cur - first;
        playPlayerTrack(first + (rel <= 0 ? n - 1 : rel - 1));
    }

    double SoundEngine::getTrackPosition() const
    {
        if(!m_initialized || !m_decoder || m_sampleRate <= 0)
            return 0.0;
        return static_cast<double>(m_currentSample - m_startSample) / m_sampleRate;
    }

    double SoundEngine::getTrackLength() const
    {
        if(!m_initialized || !m_decoder || m_sampleRate <= 0)
            return 0.0;
        return static_cast<double>(m_endSample - m_startSample) / m_sampleRate;
    }

    void SoundEngine::seekTrack(double ratio)
    {
        if(!m_initialized)
            return;
        // SDL_ClearAudioStream waits for the stream lock the audio callback holds while it waits for m_lock: clear outside m_lock
        bool sought = false;
        SDL_LockMutex(m_lock);
        if(m_decoder && m_endSample > m_startSample)
        {
            const unsigned int target = m_startSample + static_cast<unsigned int>(std::clamp(ratio, 0.0, 0.999) * (m_endSample - m_startSample));
            if(stb_vorbis_seek(m_decoder, target))
            {
                m_currentSample = target;
                sought = true;
            }
        }
        SDL_UnlockMutex(m_lock);
        if(sought)
            SDL_ClearAudioStream(m_stream);
    }

    std::string SoundEngine::getCurrentTrackName() const
    {
        if(m_currentTrack < 0)
            return {};
        if(m_currentTrack == 0 && m_hasMenuTrack)
            return m_menuTrack.name;
        const size_t i = static_cast<size_t>(m_currentTrack - 1);
        return i < m_gameTracks.size() ? m_gameTracks[i].name : std::string();
    }

    void SoundEngine::update(bool inMainMenu, bool inGame)
    {
        if(!m_initialized)
            return;
        const int area = inMainMenu ? 1 : (inGame ? 2 : 0);
        if(area != m_lastArea)
        {
            m_lastArea = area;
            m_manualPlayback = false;
        }
        bool finished = false;
        bool hasDecoder = false;
        SDL_LockMutex(m_lock);
        finished = m_trackFinished;
        m_trackFinished = false;
        hasDecoder = m_decoder != nullptr;
        SDL_UnlockMutex(m_lock);

        if(m_manualPlayback && (inMainMenu || inGame))
        {
            if(finished || !hasDecoder)
                nextTrack();
            return;
        }
        if(inMainMenu)
        {
            if(!m_hasMenuTrack)
            {
                if(m_musicContext != 0)
                    stopMusic();
            }
            else if(m_musicContext != 1 || !hasDecoder || finished)
            {
                playMenuMusic();
            }
        }
        else if(inGame)
        {
            if(m_gameTracks.empty())
            {
                if(m_musicContext != 0)
                    stopMusic();
            }
            else if(m_musicContext != 2 || !hasDecoder || finished)
            {
                advanceGameTrack();
            }
        }
        else if(m_musicContext != 0)
        {
            stopMusic();
        }
    }

    void SoundEngine::stopMusic()
    {
        if(!m_initialized)
            return;
        SDL_LockMutex(m_lock);
        closeDecoder();
        SDL_ClearAudioStream(m_stream);
        SDL_UnlockMutex(m_lock);
        m_currentTrack = -1;
        m_musicContext = 0;
        m_manualPlayback = false;
    }

    void SoundEngine::applyGain()
    {
        if(m_stream)
        {
            SDL_SetAudioStreamGain(m_stream, m_muted ? 0.0f : static_cast<float>(m_volume) / 128.0f);
        }
    }

    void SoundEngine::setMusicVolume(int volume)
    {
        m_volume = std::clamp(volume, 0, 128);
        applyGain();
    }

    void SoundEngine::setFxVolume(int volume)
    {
        m_fxVolume = std::clamp(volume, 0, 128);
    }

    void SoundEngine::toggleMute()
    {
        m_muted = !m_muted;
        applyGain();
    }

} // namespace OpenArtOfWar
