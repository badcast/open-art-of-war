#include "audio/SoundEngine.hpp"
#include "core/AssetStore.hpp"
#include "core/DebugEnv.hpp"
#include "core/JsonFile.hpp"
#include "core/LoaderResources.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
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

SoundEngine::SoundEngine() {}

SoundEngine::~SoundEngine()
{
    Cleanup();
}

bool SoundEngine::Init()
{
    if (m_initialized)
        return true;

    std::string tracklistText;
    std::string tracklistPath = LoaderResources::Tracklist();
    if (!Assets::ReadText(tracklistPath, tracklistText))
    {
        if (tracklistPath.find(".yaml") != std::string::npos)
        {
            std::string alt = tracklistPath;
            alt.replace(alt.find(".yaml"), 5, ".json");
            Assets::ReadText(alt, tracklistText);
        }
        else if (tracklistPath.find(".json") != std::string::npos)
        {
            std::string alt = tracklistPath;
            alt.replace(alt.find(".json"), 5, ".yaml");
            Assets::ReadText(alt, tracklistText);
        }
    }

    if (!tracklistText.empty())
    {
        const nlohmann::json tracklist = JsonFile::ReadFromString(tracklistText);
        if (tracklist.is_object())
        {
            auto parseTrack
                = [&](const nlohmann::json &item, const std::string &overrideFilename, Track &track) -> bool {
                std::string filename = overrideFilename;
                if (filename.empty() && item.is_object() && item.contains("filename") && item["filename"].is_string())
                    filename = item["filename"].get<std::string>();
                if (filename.empty())
                    return false;
                track.path = LoaderResources::AudioFile(filename);
                if (!overrideFilename.empty())
                {
                    std::string stem = std::filesystem::path(filename).stem().string();
                    if (item.is_object() && item.contains("name") && item["name"].is_string())
                    {
                        std::string base = item["name"].get<std::string>();
                        track.name = (stem == "mainmenu" || stem.empty()) ? base : base + " (" + stem + ")";
                    }
                    else
                    {
                        track.name = stem;
                    }
                }
                else
                {
                    track.name = item.is_object() && item.contains("name") && item["name"].is_string()
                                     ? item["name"].get<std::string>()
                                     : filename.substr(0, filename.rfind('.'));
                }
                if (item.is_object())
                {
                    if ((item.contains("effective_min") && !item["effective_min"].is_number())
                        || (item.contains("effective_max") && !item["effective_max"].is_number()))
                    {
                        std::cerr << "[SoundEngine] Invalid time range for " << track.path << std::endl;
                        return false;
                    }
                    track.effectiveMin = item.value("effective_min", 0.0);
                    track.effectiveMax = item.value("effective_max", -1.0);
                    if (!std::isfinite(track.effectiveMin) || !std::isfinite(track.effectiveMax)
                        || track.effectiveMin < 0.0)
                    {
                        std::cerr << "[SoundEngine] Invalid time range for " << track.path << std::endl;
                        return false;
                    }
                }
                return true;
            };

            auto parseMenuItem = [&](const nlohmann::json &item) {
                if (item.is_object() && item.contains("multifiles") && item["multifiles"].is_array())
                {
                    for (const auto &mf : item["multifiles"])
                    {
                        if (mf.is_string())
                        {
                            Track t;
                            if (parseTrack(item, mf.get<std::string>(), t))
                                m_menuTracks.push_back(std::move(t));
                        }
                        else if (mf.is_object())
                        {
                            Track t;
                            if (parseTrack(mf, "", t))
                                m_menuTracks.push_back(std::move(t));
                        }
                    }
                }
                else
                {
                    Track t;
                    if (parseTrack(item, "", t))
                        m_menuTracks.push_back(std::move(t));
                }
            };

            if (tracklist.contains("mainmenu"))
            {
                const auto &mm = tracklist["mainmenu"];
                if (mm.is_array())
                {
                    for (const auto &item : mm)
                        parseMenuItem(item);
                }
                else if (mm.is_object())
                {
                    parseMenuItem(mm);
                }
            }
            if (!m_menuTracks.empty())
            {
                m_hasMenuTrack = true;
                m_menuTrack = m_menuTracks.front();
            }

            if (tracklist.contains("tracks") && tracklist["tracks"].is_array())
            {
                for (const auto &item : tracklist["tracks"])
                {
                    Track track;
                    if (parseTrack(item, "", track))
                        m_gameTracks.push_back(std::move(track));
                }
            }
        }
    }
    if (!m_hasMenuTrack || m_gameTracks.empty())
    {
        std::cerr << "[SoundEngine] Warning: tracklist is missing or has no usable menu/game tracks" << std::endl;
    }
    m_shuffledTracks.resize(m_gameTracks.size());
    std::iota(m_shuffledTracks.begin(), m_shuffledTracks.end(), 0);

    std::shuffle(m_shuffledTracks.begin(), m_shuffledTracks.end(), OpenArtOfWar::Utility::VisualRng());

    // Placeholder format; each track switches the stream input to its own rate and channel count
    SDL_AudioSpec spec {SDL_AUDIO_S16, 2, 44100};
    m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SoundEngine::FeedAudio, this);
    if (!m_stream)
    {
        std::cerr << "[SoundEngine] Warning: could not open audio device: " << SDL_GetError() << std::endl;
        return false;
    }
    m_lock = SDL_CreateMutex();
    SDL_ResumeAudioStreamDevice(m_stream);

    // Sound effects: their own stream, fed by the mixer (decoded once at start)
    m_sfx.LoadBank(LoaderResources::SfxConfig(), LoaderResources::SfxClips());
    m_sfx.SetFxVolume(m_fxVolume);
    m_sfx.SetVoiceVolume(m_voiceVolume);
    m_sfx.SetUiVolume(m_uiVolume);
    m_sfx.SetMuted(m_muted);
    m_voices.SetVolume(m_voiceVolume);
    m_voices.SetMuted(m_muted);
    SDL_AudioSpec sfxSpec {SDL_AUDIO_S16, 2, SfxMixer::kRate};
    m_sfxStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &sfxSpec, &SoundEngine::FeedSfx, this);
    if (m_sfxStream)
        SDL_ResumeAudioStreamDevice(m_sfxStream);
    else
        std::cerr << "[SoundEngine] Warning: no stream for sound effects: " << SDL_GetError() << std::endl;

    std::cout << "[SoundEngine] SDL3 audio ready, " << m_gameTracks.size() << " game music tracks." << std::endl;
    m_initialized = true;
    ApplyGain();
    return true;
}

void SoundEngine::Cleanup()
{
    StopAmbient();
    if (m_sfxStream)
    {
        SDL_DestroyAudioStream(m_sfxStream);
        m_sfxStream = nullptr;
    }
    m_sfx.StopAll();
    m_voices.StopAll();
    if (m_stream)
    {
        SDL_DestroyAudioStream(m_stream); // stops the callback before the decoder goes away
        m_stream = nullptr;
    }
    CloseDecoder();
    if (m_lock)
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

void SoundEngine::CloseDecoder()
{
    if (m_decoder)
    {
        stb_vorbis_close(m_decoder);
        m_decoder = nullptr;
        m_oggData.clear();
    }
}

void SDLCALL SoundEngine::FeedSfx(void *userdata, SDL_AudioStream *stream, int additionalAmount, int /*totalAmount*/)
{
    auto *self = static_cast<SoundEngine *>(userdata);
    const int frames = additionalAmount / 4; // stereo int16
    if (frames <= 0)
        return;
    self->m_sfxBuffer.assign(static_cast<std::size_t>(frames) * 2, 0);
    self->m_sfx.Mix(self->m_sfxBuffer.data(), frames);
    self->m_voices.Mix(self->m_sfxBuffer.data(), frames); // the voices on top
    SDL_PutAudioStreamData(stream, self->m_sfxBuffer.data(), frames * 4);
}

void SDLCALL
SoundEngine::FeedAmbient(void *userdata, SDL_AudioStream *stream, int additionalAmount, int /*totalAmount*/)
{
    auto *self = static_cast<SoundEngine *>(userdata);
    if (!self->m_ambientDecoder || additionalAmount <= 0)
        return;
    const int channels = self->m_ambientChannels;
    const int framesWanted = additionalAmount / static_cast<int>(sizeof(short) * channels) + 1;
    self->m_ambientPcm.resize(static_cast<std::size_t>(framesWanted * channels));
    int produced = 0;
    bool rewound = false; // a file that gives nothing even from its start stops here instead of spinning
    while (produced < framesWanted)
    {
        const int got = stb_vorbis_get_samples_short_interleaved(
            self->m_ambientDecoder,
            channels,
            self->m_ambientPcm.data() + produced * channels,
            (framesWanted - produced) * channels);
        if (got > 0)
        {
            produced += got;
            rewound = false;
            continue;
        }
        if (rewound || !stb_vorbis_seek_start(self->m_ambientDecoder))
            break;
        rewound = true; // the end: round again (a loop)
    }
    if (produced > 0)
        SDL_PutAudioStreamData(
            stream, self->m_ambientPcm.data(), produced * channels * static_cast<int>(sizeof(short)));
}

void SoundEngine::PlayAmbient(bool night)
{
    if (!m_initialized)
        return;
    const std::string path = LoaderResources::Ambient(night);
    if (path == m_ambientPath && m_ambientStream)
        return;
    StopAmbient();
    if (path.empty() || !Assets::ReadBytes(path, m_ambientOgg) || m_ambientOgg.empty())
    {
        std::cerr << "[SoundEngine] Warning: no ambient sound " << path << std::endl;
        m_ambientOgg.clear();
        return;
    }
    int error = 0;
    m_ambientDecoder
        = stb_vorbis_open_memory(m_ambientOgg.data(), static_cast<int>(m_ambientOgg.size()), &error, nullptr);
    if (!m_ambientDecoder)
    {
        std::cerr << "[SoundEngine] Warning: could not decode " << path << " (stb_vorbis error " << error << ")"
                  << std::endl;
        m_ambientOgg.clear();
        return;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(m_ambientDecoder);
    m_ambientChannels = std::clamp(info.channels, 1, 2);
    SDL_AudioSpec spec {SDL_AUDIO_S16, m_ambientChannels, static_cast<int>(info.sample_rate)};
    m_ambientStream
        = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SoundEngine::FeedAmbient, this);
    if (!m_ambientStream)
    {
        std::cerr << "[SoundEngine] Warning: no stream for the ambient sound: " << SDL_GetError() << std::endl;
        StopAmbient();
        return;
    }
    m_ambientPath = path;
    ApplyGain();
    SDL_ResumeAudioStreamDevice(m_ambientStream);
}

void SoundEngine::StopAmbient()
{
    if (m_ambientStream)
    {
        SDL_DestroyAudioStream(m_ambientStream); // stops the callback before the decoder goes away
        m_ambientStream = nullptr;
    }
    if (m_ambientDecoder)
    {
        stb_vorbis_close(m_ambientDecoder);
        m_ambientDecoder = nullptr;
    }
    m_ambientOgg.clear();
    m_ambientPath.clear();
}

void SoundEngine::SetAmbientVolume(int volume)
{
    m_ambientVolume = std::clamp(volume, 0, 128);
    ApplyGain();
}

void SDLCALL SoundEngine::FeedAudio(void *userdata, SDL_AudioStream *stream, int additionalAmount, int /*totalAmount*/)
{
    auto *self = static_cast<SoundEngine *>(userdata);
    SDL_LockMutex(self->m_lock);
    if (self->m_decoder && additionalAmount > 0)
    {
        const int channels = self->m_channels;
        int framesWanted = additionalAmount / static_cast<int>(sizeof(short) * channels) + 1;
        self->m_pcm.resize(static_cast<std::size_t>(framesWanted * channels));
        int produced = 0;
        while (produced < framesWanted)
        {
            if (self->m_currentSample >= self->m_endSample)
            {
                if (!self->m_loopTrack)
                {
                    self->CloseDecoder();
                    self->m_trackFinished = true;
                    break;
                }
                if (!stb_vorbis_seek(self->m_decoder, self->m_startSample))
                {
                    self->CloseDecoder();
                    self->m_trackFinished = true;
                    break;
                }
                self->m_currentSample = self->m_startSample;
            }
            const unsigned int samplesLeft = self->m_endSample - self->m_currentSample;
            const int framesToRead = std::min(framesWanted - produced, static_cast<int>(samplesLeft));
            if (framesToRead <= 0)
                break;
            int got = stb_vorbis_get_samples_short_interleaved(
                self->m_decoder, channels, self->m_pcm.data() + produced * channels, framesToRead * channels);
            if (got > 0)
            {
                produced += got;
                self->m_currentSample += static_cast<unsigned int>(got);
                continue;
            }
            self->CloseDecoder();
            self->m_trackFinished = true;
            break;
        }
        if (produced > 0)
        {
            SDL_PutAudioStreamData(stream, self->m_pcm.data(), produced * channels * static_cast<int>(sizeof(short)));
        }
    }
    SDL_UnlockMutex(self->m_lock);
}

bool SoundEngine::LoadTrack(const Track &track, bool loop)
{
    if (!m_initialized)
        return false;
    // The decoder reads straight from this buffer, so it lives as long as the decoder does
    std::vector<std::uint8_t> oggData;
    int error = 0;
    stb_vorbis *decoder = nullptr;
    if (Assets::ReadBytes(track.path, oggData) && !oggData.empty())
    {
        decoder = stb_vorbis_open_memory(oggData.data(), static_cast<int>(oggData.size()), &error, nullptr);
    }
    if (!decoder)
    {
        std::cerr << "[SoundEngine] Warning: could not decode " << track.path << " (stb_vorbis error " << error << ")"
                  << std::endl;
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(decoder);
    const unsigned int length = stb_vorbis_stream_length_in_samples(decoder);
    const double sampleRate = static_cast<double>(info.sample_rate);
    const unsigned int startSample
        = static_cast<unsigned int>(std::min(static_cast<double>(length), std::floor(track.effectiveMin * sampleRate)));
    const unsigned int endSample = track.effectiveMax < 0.0
                                       ? length
                                       : static_cast<unsigned int>(std::min(
                                             static_cast<double>(length), std::floor(track.effectiveMax * sampleRate)));
    if (endSample <= startSample || !stb_vorbis_seek(decoder, startSample))
    {
        std::cerr << "[SoundEngine] Warning: invalid or unseekable time range for " << track.path << std::endl;
        stb_vorbis_close(decoder);
        return false;
    }

    SDL_LockMutex(m_lock);
    CloseDecoder();
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

bool SoundEngine::PlayNamedLoop(std::string_view name)
{
    const auto it = std::find_if(
        m_gameTracks.begin(), m_gameTracks.end(), [&](const Track &track) { return track.name == name; });
    if (it == m_gameTracks.end() || !LoadTrack(*it, true))
        return false;
    m_musicContext = 3;
    m_currentTrack = static_cast<int>(it - m_gameTracks.begin()) + 1;
    m_manualPlayback = false;
    return true;
}

void SoundEngine::PlayMenuMusic()
{
    if (!m_initialized || !m_hasMenuTrack || m_menuTracks.empty())
        return;
    SDL_LockMutex(m_lock);
    const bool alreadyPlaying = m_decoder != nullptr;
    SDL_UnlockMutex(m_lock);
    if (m_musicContext == 1 && alreadyPlaying)
        return;

    if (m_musicContext == 1 && m_menuTracks.size() > 1)
    {
        m_currentMenuTrack = (m_currentMenuTrack + 1) % m_menuTracks.size();
    }
    else if (m_musicContext != 1)
    {
        m_currentMenuTrack = m_currentMenuTrack % m_menuTracks.size();
    }

    const Track &track = m_menuTracks[m_currentMenuTrack];
    m_menuTrack = track;
    const bool loop = (m_menuTracks.size() == 1);
    if (LoadTrack(track, loop))
    {
        m_musicContext = 1;
        m_currentTrack = 0;
    }
    else
    {
        StopMusic();
    }
}

void SoundEngine::AdvanceGameTrack()
{
    if (!m_initialized || m_gameTracks.empty())
        return;
    if (m_nextGameTrack >= m_shuffledTracks.size())
    {
        std::mt19937 random(std::random_device {}());
        std::shuffle(m_shuffledTracks.begin(), m_shuffledTracks.end(), random);
        if (m_shuffledTracks.size() > 1 && m_shuffledTracks.front() == m_lastGameTrack)
        {
            std::swap(m_shuffledTracks.front(), m_shuffledTracks[1]);
        }
        m_nextGameTrack = 0;
    }
    const std::size_t index = m_shuffledTracks[m_nextGameTrack++];
    m_lastGameTrack = index;
    if (LoadTrack(m_gameTracks[index], false))
    {
        m_musicContext = 2;
        m_currentTrack = static_cast<int>(index + 1);
    }
    else
    {
        StopMusic();
    }
}

void SoundEngine::PlayRandomGameTrack()
{
    if (!m_initialized || m_gameTracks.empty())
        return;
    m_nextGameTrack = m_shuffledTracks.size();
    AdvanceGameTrack();
}

void SoundEngine::PlayPlayerTrack(int index)
{
    const bool menuTrack = index == 0 && m_hasMenuTrack;
    const int gameIndex = m_hasMenuTrack ? index - 1 : index;
    const Track &track
        = menuTrack ? (m_menuTracks.empty() ? m_menuTrack : m_menuTracks[m_currentMenuTrack]) : m_gameTracks[gameIndex];
    if (LoadTrack(track, menuTrack && m_menuTracks.size() == 1))
    {
        m_musicContext = menuTrack ? 1 : 2;
        m_currentTrack = menuTrack ? 0 : gameIndex + 1;
        m_manualPlayback = true;
    }
    else
    {
        StopMusic();
    }
}

// The main menu plays its own tracks (everything listed under `mainmenu`) round and round until the user touches the
// player (next / previous / a seek); from then on its playlist is those tracks followed by all the others, and goes on
// through them. In the game the playlist is the game tracks only; the main menu theme belongs to the menu.
void SoundEngine::PlayMenuPlaylist(std::size_t position)
{
    const std::size_t menuCount = m_menuTracks.size();
    const std::size_t total = menuCount + m_gameTracks.size();
    if (!m_initialized || total == 0)
        return;
    position %= total;
    const bool menuTrack = position < menuCount;
    const Track &track = menuTrack ? m_menuTracks[position] : m_gameTracks[position - menuCount];
    if (menuTrack)
    {
        m_currentMenuTrack = position;
        m_menuTrack = track;
    }
    if (LoadTrack(track, false))
    {
        m_musicContext = menuTrack ? 1 : 2;
        m_currentTrack = menuTrack ? 0 : static_cast<int>(position - menuCount + 1);
        m_manualPlayback = true;
    }
    else
    {
        StopMusic();
    }
}

void SoundEngine::StepTrack(int direction)
{
    if (m_lastArea == 1 && !m_menuTracks.empty())
    {
        const std::size_t menuCount = m_menuTracks.size();
        const std::size_t total = menuCount + m_gameTracks.size();
        std::size_t at = m_musicContext == 2 && m_currentTrack > 0
                             ? menuCount + static_cast<std::size_t>(m_currentTrack - 1)
                             : m_currentMenuTrack % menuCount;
        PlayMenuPlaylist((at + (direction > 0 ? 1 : total - 1)) % total);
        return;
    }
    const int count = GetTrackCount();
    const int first = (m_lastArea == 2 && m_hasMenuTrack && !m_gameTracks.empty()) ? 1 : 0;
    const int n = count - first;
    if (!m_initialized || n <= 0)
        return;
    const int cur = m_currentTrack < 0 ? -1 : (m_hasMenuTrack ? m_currentTrack : m_currentTrack - 1);
    const int rel = cur - first;
    if (direction > 0)
        PlayPlayerTrack(first + (rel < 0 ? 0 : (rel + 1) % n));
    else
        PlayPlayerTrack(first + (rel <= 0 ? n - 1 : rel - 1));
}

void SoundEngine::NextTrack()
{
    StepTrack(1);
}

void SoundEngine::PrevTrack()
{
    StepTrack(-1);
}

double SoundEngine::GetTrackPosition() const
{
    if (!m_initialized)
        return 0.0;
    SDL_LockMutex(m_lock);
    const double pos
        = (!m_decoder || m_sampleRate <= 0) ? 0.0 : static_cast<double>(m_currentSample - m_startSample) / m_sampleRate;
    SDL_UnlockMutex(m_lock);
    return pos;
}

double SoundEngine::GetTrackLength() const
{
    if (!m_initialized)
        return 0.0;
    SDL_LockMutex(m_lock);
    const double len
        = (!m_decoder || m_sampleRate <= 0) ? 0.0 : static_cast<double>(m_endSample - m_startSample) / m_sampleRate;
    SDL_UnlockMutex(m_lock);
    return len;
}

void SoundEngine::SeekTrack(double ratio)
{
    if (!m_initialized)
        return;
    // SDL_ClearAudioStream waits for the stream lock the audio callback holds while it waits for m_lock: clear outside
    // m_lock
    bool sought = false;
    SDL_LockMutex(m_lock);
    if (m_decoder && m_endSample > m_startSample)
    {
        const unsigned int target
            = m_startSample + static_cast<unsigned int>(std::clamp(ratio, 0.0, 0.999) * (m_endSample - m_startSample));
        if (stb_vorbis_seek(m_decoder, target))
        {
            m_currentSample = target;
            sought = true;
        }
    }
    SDL_UnlockMutex(m_lock);
    if (sought)
    {
        SDL_ClearAudioStream(m_stream);
        // Seeking in the main menu is touching the player: the menu theme stops looping, the other tracks follow
        if (m_lastArea == 1)
            m_manualPlayback = true;
    }
}

std::string SoundEngine::GetCurrentTrackName() const
{
    if (m_currentTrack < 0)
        return {};
    if (m_currentTrack == 0 && m_hasMenuTrack)
    {
        if (m_currentMenuTrack < m_menuTracks.size())
            return m_menuTracks[m_currentMenuTrack].name;
        return m_menuTrack.name;
    }
    const std::size_t i = static_cast<std::size_t>(m_currentTrack - 1);
    return i < m_gameTracks.size() ? m_gameTracks[i].name : std::string();
}

void SoundEngine::Update(bool inMainMenu, bool inGame, bool preserveMusic)
{
    if (!m_initialized)
        return;
    m_voices.Update(SDL_GetTicks()); // the next announcer line once the current one is over
    if (preserveMusic)
        return;
    const int area = inMainMenu ? 1 : (inGame ? 2 : 0);
    if (area != m_lastArea)
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

    if (m_manualPlayback && (inMainMenu || inGame))
    {
        if (finished || !hasDecoder)
            NextTrack();
        return;
    }
    if (inMainMenu)
    {
        if (!m_hasMenuTrack)
        {
            if (m_musicContext != 0)
                StopMusic();
        }
        else if (m_musicContext != 1 || !hasDecoder || finished)
        {
            PlayMenuMusic();
        }
    }
    else if (inGame)
    {
        if (m_gameTracks.empty())
        {
            if (m_musicContext != 0)
                StopMusic();
        }
        else if (m_musicContext != 2 || !hasDecoder || finished)
        {
            AdvanceGameTrack();
        }
    }
    else if (m_musicContext != 0)
    {
        StopMusic();
    }
}

void SoundEngine::StopMusic()
{
    if (!m_initialized)
        return;
    SDL_LockMutex(m_lock);
    CloseDecoder();
    SDL_UnlockMutex(m_lock);
    // Clear outside m_lock to avoid ABBA deadlock with FeedAudio holding the stream lock
    if (m_stream)
        SDL_ClearAudioStream(m_stream);
    m_currentTrack = -1;
    m_musicContext = 0;
    m_manualPlayback = false;
}

void SoundEngine::ApplyGain()
{
    const float general = static_cast<float>(m_generalVolume) / 128.0f;
    if (m_stream)
    {
        SDL_SetAudioStreamGain(
            m_stream, m_muted ? 0.0f : static_cast<float>(m_volume) / 128.0f * general * m_sceneMusicGain);
    }
    if (m_ambientStream)
        SDL_SetAudioStreamGain(
            m_ambientStream, m_muted ? 0.0f : static_cast<float>(m_ambientVolume) / 128.0f * general);
}

void SoundEngine::SetGeneralVolume(int volume)
{
    m_generalVolume = std::clamp(volume, 0, 128);
    m_sfx.SetGeneralVolume(m_generalVolume);
    m_voices.SetGeneralVolume(m_generalVolume);
    ApplyGain(); // the music and the surroundings play on their own streams
}

void SoundEngine::SetSceneMusicGain(float gain)
{
    m_sceneMusicGain = std::clamp(gain, 0.0f, 1.0f);
    ApplyGain();
}

void SoundEngine::SetMusicVolume(int volume)
{
    m_volume = std::clamp(volume, 0, 128);
    ApplyGain();
}

void SoundEngine::SetInterfaceMuted(bool muted)
{
    m_sfx.SetInterfaceMuted(muted);
    m_voices.SetSilenced(muted);
}

void SoundEngine::SetReverb(bool enabled)
{
    m_sfx.SetReverb(enabled);
}

void SoundEngine::SetFxVolume(int volume)
{
    m_fxVolume = std::clamp(volume, 0, 128);
    m_sfx.SetFxVolume(m_fxVolume);
}

void SoundEngine::SetUiVolume(int volume)
{
    m_uiVolume = std::clamp(volume, 0, 128);
    m_sfx.SetUiVolume(m_uiVolume);
}

void SoundEngine::SetVoiceVolume(int volume)
{
    m_voiceVolume = std::clamp(volume, 0, 128);
    m_voices.SetVolume(m_voiceVolume);
}

void SoundEngine::PlaySfx(std::string_view event, float gain, float pan, float muffle)
{
    if (!m_sfxStream)
        return;
    const bool played = m_sfx.Play(event, gain, pan, SDL_GetTicks(), muffle);
    // OAOW_SFX_TRACE=1 (debug builds): every sound that starts, to check the events without listening
    static const bool trace = DebugEnv("OAOW_SFX_TRACE") != nullptr;
    if (trace && played)
        std::cout << "[Sfx] " << event << " gain " << gain << " pan " << pan << std::endl;
}

void SoundEngine::PlaySfx(WorldSfx sound, float gain, float pan, float muffle)
{
    PlaySfx(std::string_view(SoundIds::Name(sound)), gain, pan, muffle);
}

const SfxMixer::EventConfig *SoundEngine::SfxConfig(WorldSfx sound) const
{
    return m_sfx.Config(SoundIds::Name(sound));
}

const SfxMixer::EventConfig *SoundEngine::SfxConfig(std::string_view event) const
{
    return event.empty() || !m_sfx.Has(event) ? nullptr : m_sfx.Config(event);
}

void SoundEngine::Announce(Announcement line)
{
    Announce(std::string_view(SoundIds::Name(line)));
}

void SoundEngine::PlayVoice(UnitReply reply, bool infantry)
{
    PlayVoice(std::string_view(SoundIds::Name(reply, infantry)));
}

void SoundEngine::Announce(std::string_view event)
{
    if (!m_sfxStream)
        return;
    const bool queued = m_voices.Announce(event, SDL_GetTicks());
    static const bool trace = DebugEnv("OAOW_SFX_TRACE") != nullptr;
    if (trace && queued)
        std::cout << "[Sfx] announcer (" << m_voices.Language() << "): " << event << std::endl;
}

void SoundEngine::PlayVoice(std::string_view event)
{
    if (!m_sfxStream)
        return;
    const bool played = m_voices.Play(event, SDL_GetTicks());
    static const bool trace = DebugEnv("OAOW_SFX_TRACE") != nullptr;
    if (trace && played)
        std::cout << "[Sfx] voice (" << m_voices.Language() << "): " << event << std::endl;
}

void SoundEngine::SetVoiceLanguage(std::string_view language)
{
    if (!m_initialized || language == m_voices.Language())
        return;
    m_voices.load(language);
}

void SoundEngine::ToggleMute()
{
    m_muted = !m_muted;
    m_sfx.SetMuted(m_muted);
    m_voices.SetMuted(m_muted);
    ApplyGain();
}

} // namespace OpenArtOfWar
