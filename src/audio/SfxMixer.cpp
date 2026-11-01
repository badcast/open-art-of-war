#include "audio/SfxMixer.hpp"
#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace OpenArtOfWar
{

SfxPlacement PlaceSfx(float worldX, float worldY, float viewX, float viewY, float viewW, float viewH, float rangePx)
{
    SfxPlacement p;
    // Distance from the view rectangle (0 inside)
    const float dx = std::max({viewX - worldX, 0.0f, worldX - (viewX + viewW)});
    const float dy = std::max({viewY - worldY, 0.0f, worldY - (viewY + viewH)});
    const float d = std::sqrt(dx * dx + dy * dy);
    if (rangePx <= 0.0f || d >= rangePx)
        return p;
    const float k = 1.0f - d / rangePx;
    p.audible = true;
    p.gain = k * k;
    const float cx = viewX + viewW * 0.5f;
    p.pan = std::clamp((worldX - cx) / std::max(1.0f, viewW * 0.6f), -1.0f, 1.0f) * 0.85f;
    return p;
}

namespace
{
SfxMixer::Category ParseCategory(const std::string &s)
{
    if (s == "weapon")
        return SfxMixer::Category::Weapon;
    if (s == "impact")
        return SfxMixer::Category::Impact;
    if (s == "voice")
        return SfxMixer::Category::Voice;
    if (s == "announcer")
        return SfxMixer::Category::Announcer;
    if (s == "alert")
        return SfxMixer::Category::Alert;
    return SfxMixer::Category::Ui;
}

// "fire_rifle_2.ogg" -> "fire_rifle"; empty when the name has no _<number>.ogg ending
std::string EventOfFile(const std::string &path)
{
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".ogg") != 0)
        return {};
    name.resize(name.size() - 4);
    const size_t us = name.find_last_of('_');
    if (us == std::string::npos || us + 1 >= name.size())
        return {};
    for (size_t i = us + 1; i < name.size(); ++i)
    {
        if (name[i] < '0' || name[i] > '9')
            return {};
    }
    return name.substr(0, us);
}

// Ogg Vorbis -> mono int16 at SfxMixer::kRate
bool DecodeOgg(const std::vector<uint8_t> &data, std::vector<int16_t> &out)
{
    int channels = 0, rate = 0;
    short *pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(data.data(), static_cast<int>(data.size()), &channels, &rate, &pcm);
    if (frames <= 0 || !pcm || channels <= 0 || rate <= 0)
    {
        std::free(pcm);
        return false;
    }
    std::vector<float> mono(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
    {
        float s = 0.0f;
        for (int c = 0; c < channels; ++c)
            s += pcm[i * channels + c];
        mono[static_cast<size_t>(i)] = s / static_cast<float>(channels);
    }
    std::free(pcm);
    const double ratio = static_cast<double>(rate) / SfxMixer::kRate;
    const size_t n = static_cast<size_t>(frames / ratio);
    out.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        const double src = i * ratio;
        const size_t a = std::min(static_cast<size_t>(src), mono.size() - 1);
        const size_t b = std::min(a + 1, mono.size() - 1);
        const float t = static_cast<float>(src - static_cast<double>(a));
        out[i] = static_cast<int16_t>(std::clamp(mono[a] + (mono[b] - mono[a]) * t, -32768.0f, 32767.0f));
    }
    return !out.empty();
}
} // namespace

bool SfxMixer::LoadBank(const std::string &configPath, const std::vector<std::string> &clips)
{
    std::string text;
    if (Assets::ReadText(configPath, text))
        Configure(JsonFile::ReadFromString(text));
    else
        std::cerr << "[SfxMixer] " << configPath << " not found: sounds play with default settings" << std::endl;
    const int loaded = LoadClips(clips);
    std::cout << "[SfxMixer] " << loaded << " sound clips for " << EventCount() << " events" << std::endl;
    return loaded > 0;
}

int SfxMixer::LoadClips(const std::vector<std::string> &files, bool onlyMissing)
{
    int loaded = 0;
    // onlyMissing: decided once per event before its first clip of this list (all its variants come along)
    std::map<std::string, bool> take;
    for (const std::string &file : files)
    {
        const std::string event = EventOfFile(file);
        if (event.empty())
            continue;
        if (onlyMissing)
        {
            auto it = take.find(event);
            if (it == take.end())
                it = take.emplace(event, !Has(event)).first;
            if (!it->second)
                continue;
        }
        std::vector<uint8_t> bytes;
        std::vector<int16_t> pcm;
        if (!Assets::ReadBytes(file, bytes) || !DecodeOgg(bytes, pcm))
        {
            std::cerr << "[SfxMixer] could not decode " << file << std::endl;
            continue;
        }
        AddClip(event, std::move(pcm));
        ++loaded;
    }
    return loaded;
}

void SfxMixer::Configure(const nlohmann::json &doc)
{
    std::lock_guard<std::mutex> guard(m_lock);
    if (!doc.is_object())
        return;
    m_maxVoices = std::clamp(doc.value("voices", m_maxVoices), 4, 128);
    if (doc.contains("defaults") && doc["defaults"].is_object())
        m_defaults = doc["defaults"];
    if (!doc.contains("events") || !doc["events"].is_object())
        return;
    for (auto it = doc["events"].begin(); it != doc["events"].end(); ++it)
    {
        nlohmann::json e = m_defaults;
        if (it.value().is_object())
            e.update(it.value());
        int idx = -1;
        if (auto found = m_eventIndex.find(it.key()); found != m_eventIndex.end())
            idx = found->second;
        else
        {
            idx = static_cast<int>(m_events.size());
            m_events.push_back({});
            m_events.back().name = it.key();
            m_eventIndex[it.key()] = idx;
        }
        EventConfig &c = m_events[static_cast<size_t>(idx)].cfg;
        c.category = ParseCategory(e.value("category", std::string("ui")));
        c.volume = std::clamp(e.value("volume", 1.0f), 0.0f, 2.0f);
        c.pitchJitter = std::clamp(e.value("pitch_jitter", 0.0f), 0.0f, 12.0f);
        c.maxInstances = std::clamp(e.value("max_instances", 4), 1, 64);
        c.minIntervalMs = std::max(0, e.value("min_interval_ms", 0));
        c.cooldownMs = std::max(0, e.value("cooldown_ms", 0));
        c.priority = std::clamp(e.value("priority", 5), 0, 10);
        c.positional = e.value("positional", false);
        c.rangeTiles = std::max(0.0f, e.value("range_tiles", 12.0f));
    }
}

void SfxMixer::AddClip(const std::string &event, std::vector<int16_t> pcm)
{
    if (pcm.size() < 2)
        return;
    std::lock_guard<std::mutex> guard(m_lock);
    int idx = -1;
    if (auto found = m_eventIndex.find(event); found != m_eventIndex.end())
        idx = found->second;
    else
    {
        // A clip without settings: the defaults of audio_sfx.json (or the built-in ones)
        idx = static_cast<int>(m_events.size());
        m_events.push_back({});
        m_events.back().name = event;
        m_eventIndex[event] = idx;
    }
    m_clips.push_back(std::move(pcm));
    m_events[static_cast<size_t>(idx)].cfg.clips.push_back(static_cast<int>(m_clips.size()) - 1);
}

int SfxMixer::EventIndex(const std::string &event) const
{
    auto it = m_eventIndex.find(event);
    return it == m_eventIndex.end() ? -1 : it->second;
}

bool SfxMixer::Has(const std::string &event) const
{
    std::lock_guard<std::mutex> guard(m_lock);
    const int idx = EventIndex(event);
    return idx >= 0 && !m_events[static_cast<size_t>(idx)].cfg.clips.empty();
}

const SfxMixer::EventConfig *SfxMixer::Config(const std::string &event) const
{
    std::lock_guard<std::mutex> guard(m_lock);
    const int idx = EventIndex(event);
    return idx < 0 ? nullptr : &m_events[static_cast<size_t>(idx)].cfg;
}

size_t SfxMixer::EventCount() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return m_events.size();
}

int SfxMixer::InstancesLocked(int eventIdx) const
{
    int n = 0;
    for (const Voice &v : m_voices)
        n += v.event == eventIdx ? 1 : 0;
    return n;
}

bool SfxMixer::StartLocked(int eventIdx, float gain, float pan, uint64_t nowMs, bool announcer)
{
    EventState &ev = m_events[static_cast<size_t>(eventIdx)];
    const EventConfig &c = ev.cfg;
    if (c.clips.empty() || gain <= 0.001f || c.volume <= 0.0f)
        return false;
    if (!announcer)
    {
        if (ev.played && c.minIntervalMs > 0 && nowMs - ev.lastPlayMs < static_cast<uint64_t>(c.minIntervalMs))
            return false;
        if (InstancesLocked(eventIdx) >= c.maxInstances)
            return false;
    }
    // A full mixer gives up its least important voice (the oldest of them), never a more important one
    if (static_cast<int>(m_voices.size()) >= m_maxVoices)
    {
        int victim = -1;
        for (int i = 0; i < static_cast<int>(m_voices.size()); ++i)
        {
            const Voice &v = m_voices[static_cast<size_t>(i)];
            if (v.announcer || v.priority > c.priority)
                continue;
            if (victim < 0 || v.priority < m_voices[static_cast<size_t>(victim)].priority
                || (v.priority == m_voices[static_cast<size_t>(victim)].priority
                    && v.pos > m_voices[static_cast<size_t>(victim)].pos))
                victim = i;
        }
        if (victim < 0)
            return false;
        m_voices.erase(m_voices.begin() + victim);
    }
    std::uniform_int_distribution<size_t> pickClip(0, c.clips.size() - 1);
    Voice v;
    v.clip = c.clips[pickClip(m_rng)];
    if (c.pitchJitter > 0.0f)
    {
        std::uniform_real_distribution<float> semis(-c.pitchJitter, c.pitchJitter);
        v.step = std::pow(2.0, semis(m_rng) / 12.0);
    }
    const float g = std::clamp(gain, 0.0f, 1.0f) * c.volume;
    const float p = std::clamp(pan, -1.0f, 1.0f);
    // Constant-power pan
    const float angle = (p + 1.0f) * 0.25f * 3.14159265f;
    v.gainL = g * std::cos(angle) * 1.41421356f;
    v.gainR = g * std::sin(angle) * 1.41421356f;
    v.priority = c.priority;
    v.event = eventIdx;
    v.voiceVolume = c.category == Category::Voice || c.category == Category::Announcer;
    v.announcer = announcer;
    m_voices.push_back(v);
    ev.lastPlayMs = nowMs;
    ev.played = true;
    return true;
}

bool SfxMixer::Play(const std::string &event, float gain, float pan, uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(m_lock);
    const int idx = EventIndex(event);
    if (idx < 0)
        return false;
    if (m_events[static_cast<size_t>(idx)].cfg.category == Category::Announcer)
        return AnnounceLocked(idx, nowMs); // announcer lines always go through the queue
    return StartLocked(idx, gain, pan, nowMs, false);
}

bool SfxMixer::Announce(const std::string &event, uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(m_lock);
    const int idx = EventIndex(event);
    return idx >= 0 && AnnounceLocked(idx, nowMs);
}

bool SfxMixer::AnnounceLocked(int idx, uint64_t nowMs)
{
    if (m_events[static_cast<size_t>(idx)].cfg.clips.empty())
        return false;
    const EventState &ev = m_events[static_cast<size_t>(idx)];
    if (ev.played && ev.cfg.cooldownMs > 0 && nowMs - ev.lastPlayMs < static_cast<uint64_t>(ev.cfg.cooldownMs))
        return false;
    if (std::find(m_announcerQueue.begin(), m_announcerQueue.end(), idx) != m_announcerQueue.end())
        return false;
    for (const Voice &v : m_voices)
    {
        if (v.announcer && v.event == idx)
            return false; // saying it right now
    }
    if (m_announcerQueue.size() >= 3)
        m_announcerQueue.pop_front(); // old news
    m_announcerQueue.push_back(idx);
    return true;
}

void SfxMixer::Update(uint64_t nowMs)
{
    std::lock_guard<std::mutex> guard(m_lock);
    for (const Voice &v : m_voices)
    {
        if (v.announcer)
            return; // one line at a time
    }
    while (!m_announcerQueue.empty())
    {
        const int idx = m_announcerQueue.front();
        m_announcerQueue.pop_front();
        if (StartLocked(idx, 1.0f, 0.0f, nowMs, true))
            return;
    }
}

void SfxMixer::Mix(int16_t *out, int frames)
{
    std::lock_guard<std::mutex> guard(m_lock);
    if (frames <= 0)
        return;
    std::vector<float> acc(static_cast<size_t>(frames) * 2, 0.0f);
    for (Voice &v : m_voices)
    {
        const std::vector<int16_t> &clip = m_clips[static_cast<size_t>(v.clip)];
        const float vol = m_muted ? 0.0f : (v.voiceVolume ? m_voiceGain : m_fxGain);
        const float gl = v.gainL * vol, gr = v.gainR * vol;
        const double last = static_cast<double>(clip.size() - 1);
        for (int i = 0; i < frames && v.pos < last; ++i)
        {
            const size_t a = static_cast<size_t>(v.pos);
            const float t = static_cast<float>(v.pos - static_cast<double>(a));
            const float s = clip[a] + (clip[a + 1] - clip[a]) * t;
            acc[static_cast<size_t>(i) * 2] += s * gl;
            acc[static_cast<size_t>(i) * 2 + 1] += s * gr;
            v.pos += v.step;
        }
    }
    m_voices.erase(
        std::remove_if(
            m_voices.begin(),
            m_voices.end(),
            [this](const Voice &v) {
                return v.pos >= static_cast<double>(m_clips[static_cast<size_t>(v.clip)].size() - 1);
            }),
        m_voices.end());
    for (size_t i = 0; i < acc.size(); ++i)
    {
        // Soft limit: many explosions at once saturate gently instead of clipping hard
        const float x = (static_cast<float>(out[i]) + acc[i]) / 32768.0f;
        const float y = x / (1.0f + 0.25f * std::fabs(x));
        out[i] = static_cast<int16_t>(std::clamp(y * 1.25f * 32767.0f, -32768.0f, 32767.0f));
    }
}

void SfxMixer::StopAll()
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_voices.clear();
    m_announcerQueue.clear();
}

void SfxMixer::SetFxVolume(int volume)
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_fxGain = std::clamp(volume, 0, 128) / 128.0f;
}

void SfxMixer::SetVoiceVolume(int volume)
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_voiceGain = std::clamp(volume, 0, 128) / 128.0f;
}

void SfxMixer::SetMuted(bool muted)
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_muted = muted;
}

int SfxMixer::ActiveVoices() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return static_cast<int>(m_voices.size());
}

bool SfxMixer::AnnouncerBusy() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    for (const Voice &v : m_voices)
    {
        if (v.announcer)
            return true;
    }
    return false;
}

size_t SfxMixer::AnnouncerQueued() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return m_announcerQueue.size();
}

} // namespace OpenArtOfWar
