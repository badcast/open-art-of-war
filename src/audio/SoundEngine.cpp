#include "audio/SoundEngine.hpp"
#include "core/DebugEnv.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
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

SoundEngine::SoundEngine() {}

SoundEngine::~SoundEngine()
{
    Cleanup();
}

bool SoundEngine::Init(const std::string &assetRoot)
{
    if (m_initialized)
        return true;

    std::string tracklistText;
    if (Assets::ReadText(LoaderResources::Tracklist(assetRoot), tracklistText))
    {
        const nlohmann::json tracklist = nlohmann::json::parse(tracklistText, nullptr, false);
        if (tracklist.is_object())
        {
            auto parseTrack = [&](const nlohmann::json &item, Track &track) {
                if (!item.is_object() || !item.contains("filename") || !item["filename"].is_string())
                    return false;
                const std::string filename = item["filename"].get<std::string>();
                if (filename.empty())
                    return false;
                track.path = LoaderResources::AudioFile(filename, assetRoot);
                track.name = item.contains("name") && item["name"].is_string()
                                 ? item["name"].get<std::string>()
                                 : filename.substr(0, filename.rfind('.'));
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
                return true;
            };

            if (tracklist.contains("mainmenu") && tracklist["mainmenu"].is_array())
            {
                for (const auto &item : tracklist["mainmenu"])
                {
                    if (parseTrack(item, m_menuTrack))
                    {
                        m_hasMenuTrack = true;
                        break;
                    }
                }
            }
            if (tracklist.contains("tracks") && tracklist["tracks"].is_array())
            {
                for (const auto &item : tracklist["tracks"])
                {
                    Track track;
                    if (parseTrack(item, track))
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

    std::shuffle(m_shuffledTracks.begin(), m_shuffledTracks.end(), OpenArtOfWar::Utility::rng());

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

    m_assetRoot = assetRoot;
    // Sound effects: their own stream, fed by the mixer (decoded once at start)
    m_sfx.LoadBank(LoaderResources::SfxConfig(assetRoot), LoaderResources::SfxClips(assetRoot));
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
    self->m_sfxBuffer.assign(static_cast<size_t>(frames) * 2, 0);
    self->m_sfx.Mix(self->m_sfxBuffer.data(), frames);
    self->m_voices.Mix(self->m_sfxBuffer.data(), frames); // the voices on top
    SDL_PutAudioStreamData(stream, self->m_sfxBuffer.data(), frames * 4);
}

void SDLCALL SoundEngine::FeedAudio(void *userdata, SDL_AudioStream *stream, int additionalAmount, int /*totalAmount*/)
{
    auto *self = static_cast<SoundEngine *>(userdata);
    SDL_LockMutex(self->m_lock);
    if (self->m_decoder && additionalAmount > 0)
    {
        const int channels = self->m_channels;
        int framesWanted = additionalAmount / static_cast<int>(sizeof(short) * channels) + 1;
        self->m_pcm.resize(static_cast<size_t>(framesWanted * channels));
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
    std::vector<uint8_t> oggData;
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

void SoundEngine::PlayMenuMusic()
{
    if (!m_initialized || !m_hasMenuTrack)
        return;
    SDL_LockMutex(m_lock);
    const bool alreadyPlaying = m_decoder != nullptr;
    SDL_UnlockMutex(m_lock);
    if (m_musicContext == 1 && alreadyPlaying)
        return;
    if (LoadTrack(m_menuTrack, true))
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
    const size_t index = m_shuffledTracks[m_nextGameTrack++];
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
    const Track &track = menuTrack ? m_menuTrack : m_gameTracks[gameIndex];
    if (LoadTrack(track, menuTrack))
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

// In the game the playlist is the game tracks only; the main menu theme belongs to the menu
void SoundEngine::NextTrack()
{
    const int count = GetTrackCount();
    const int first = (m_lastArea == 2 && m_hasMenuTrack && !m_gameTracks.empty()) ? 1 : 0;
    const int n = count - first;
    if (!m_initialized || n <= 0)
        return;
    const int cur = m_currentTrack < 0 ? -1 : (m_hasMenuTrack ? m_currentTrack : m_currentTrack - 1);
    const int rel = cur - first;
    PlayPlayerTrack(first + (rel < 0 ? 0 : (rel + 1) % n));
}

void SoundEngine::PrevTrack()
{
    const int count = GetTrackCount();
    const int first = (m_lastArea == 2 && m_hasMenuTrack && !m_gameTracks.empty()) ? 1 : 0;
    const int n = count - first;
    if (!m_initialized || n <= 0)
        return;
    const int cur = m_currentTrack < 0 ? -1 : (m_hasMenuTrack ? m_currentTrack : m_currentTrack - 1);
    const int rel = cur - first;
    PlayPlayerTrack(first + (rel <= 0 ? n - 1 : rel - 1));
}

double SoundEngine::GetTrackPosition() const
{
    if (!m_initialized || !m_decoder || m_sampleRate <= 0)
        return 0.0;
    return static_cast<double>(m_currentSample - m_startSample) / m_sampleRate;
}

double SoundEngine::GetTrackLength() const
{
    if (!m_initialized || !m_decoder || m_sampleRate <= 0)
        return 0.0;
    return static_cast<double>(m_endSample - m_startSample) / m_sampleRate;
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
        SDL_ClearAudioStream(m_stream);
}

std::string SoundEngine::GetCurrentTrackName() const
{
    if (m_currentTrack < 0)
        return {};
    if (m_currentTrack == 0 && m_hasMenuTrack)
        return m_menuTrack.name;
    const size_t i = static_cast<size_t>(m_currentTrack - 1);
    return i < m_gameTracks.size() ? m_gameTracks[i].name : std::string();
}

void SoundEngine::Update(bool inMainMenu, bool inGame)
{
    if (!m_initialized)
        return;
    m_voices.Update(SDL_GetTicks()); // the next announcer line once the current one is over
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
    SDL_ClearAudioStream(m_stream);
    SDL_UnlockMutex(m_lock);
    m_currentTrack = -1;
    m_musicContext = 0;
    m_manualPlayback = false;
}

void SoundEngine::ApplyGain()
{
    if (m_stream)
    {
        SDL_SetAudioStreamGain(m_stream, m_muted ? 0.0f : static_cast<float>(m_volume) / 128.0f);
    }
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

void SoundEngine::PlaySfx(const std::string &event, float gain, float pan, float muffle)
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
    PlaySfx(std::string(SoundIds::Name(sound)), gain, pan, muffle);
}

const SfxMixer::EventConfig *SoundEngine::SfxConfig(WorldSfx sound) const
{
    return m_sfx.Config(SoundIds::Name(sound));
}

void SoundEngine::Announce(Announcement line)
{
    Announce(std::string(SoundIds::Name(line)));
}

void SoundEngine::PlayVoice(UnitReply reply, bool infantry)
{
    PlayVoice(std::string(SoundIds::Name(reply, infantry)));
}

void SoundEngine::Announce(const std::string &event)
{
    if (!m_sfxStream)
        return;
    const bool queued = m_voices.Announce(event, SDL_GetTicks());
    static const bool trace = DebugEnv("OAOW_SFX_TRACE") != nullptr;
    if (trace && queued)
        std::cout << "[Sfx] announcer (" << m_voices.Language() << "): " << event << std::endl;
}

void SoundEngine::PlayVoice(const std::string &event)
{
    if (!m_sfxStream)
        return;
    const bool played = m_voices.Play(event, SDL_GetTicks());
    static const bool trace = DebugEnv("OAOW_SFX_TRACE") != nullptr;
    if (trace && played)
        std::cout << "[Sfx] voice (" << m_voices.Language() << "): " << event << std::endl;
}

void SoundEngine::SetVoiceLanguage(const std::string &language)
{
    if (!m_initialized || language == m_voices.Language())
        return;
    m_voices.load(language, m_assetRoot.empty() ? LoaderResources::kDefaultRoot : m_assetRoot);
}

void SoundEngine::ToggleMute()
{
    m_muted = !m_muted;
    m_sfx.SetMuted(m_muted);
    m_voices.SetMuted(m_muted);
    ApplyGain();
}

} // namespace OpenArtOfWar
