#include "replay/ReplayPlayer.hpp"
#include "core/AssetStore.hpp"
#include "core/Localization.hpp"
#include "game/SimCommandCodec.hpp"
#include "game/Simulation.hpp"
#include "net/MapFiles.hpp"
#include "net/NetMatch.hpp"
#include "openaow/net/ReplayFile.hpp"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <iostream>

#include <algorithm>
#include <filesystem>

namespace OpenArtOfWar
{

namespace
{

constexpr std::size_t kMaxOverlayBytes = 8u * 1024u * 1024u;

bool EndsWith(const std::string &s, const char *suffix)
{
    const std::string t = suffix;
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// The tick a recorded camera applies from, the number of turns decoded for it
bool FillInfo(const Net::ReplayData &data, ReplayPlayer::Info &info, std::string &error)
{
    const nlohmann::json meta = nlohmann::json::parse(data.meta, nullptr, false);
    if (!meta.is_object() || meta.value("format", 0) != 1)
    {
        error = _("This replay was made by a newer version of the game");
        return false;
    }
    info.source = meta.value("source", std::string("client"));
    info.gameVersion = meta.value("game_version", std::string());
    info.mods.clear();
    if (meta.contains("mods") && meta["mods"].is_array())
        for (const auto &m : meta["mods"])
            if (m.is_object())
                info.mods.push_back(m.value("name", m.value("id", std::string())) + " " + m.value("version", std::string()));
    info.protocol = meta.value("protocol", 0);
    info.simBuildId = meta.value("sim_build_id", static_cast<std::uint64_t>(0));
    info.checksumsComparable = info.simBuildId != 0 && info.simBuildId == NetMatch::SimBuildId();
    info.recordedAt = meta.value("recorded_at", static_cast<std::int64_t>(0));
    info.ticksPerTurn = std::clamp<std::uint32_t>(meta.value("ticks_per_turn", 2u), 1, 16);
    info.turns = static_cast<std::uint32_t>(data.turns.size());
    info.ticks = info.turns * info.ticksPerTurn;
    info.localSlot = meta.value("local_slot", -1);
    info.mode = meta.value("mode", std::string("network"));
    info.mission = meta.value("mission", -1);
    info.playerFaction = meta.value("player_faction", 0);
    info.difficulty = meta.value("difficulty", 1);
    if (meta.contains("result") && meta["result"].is_object())
        info.outcome = meta["result"].value("outcome", std::string());
    else
        info.outcome = meta.value("end_reason", std::string());
    // The setup bytes are the truth about the match; the meta only adds names
    MatchSetup setup;
    if (!NetMatch::DecodeSetup(data.setup, setup))
    {
        error = _("The replay is damaged");
        return false;
    }
    info.map = setup.mapPath;
    info.mapHash = setup.mapHash;
    std::map<int, std::pair<std::string, std::vector<std::string>>> named;
    if (meta.contains("players") && meta["players"].is_array())
    {
        for (const auto &p : meta["players"])
        {
            if (!p.is_object())
                continue;
            std::vector<std::string> addresses;
            if (p.contains("addresses") && p["addresses"].is_array())
            {
                for (const auto &a : p["addresses"])
                    if (a.is_string())
                        addresses.push_back(a.get<std::string>());
            }
            named[p.value("slot", -1)] = {p.value("name", std::string()), addresses};
        }
    }
    info.players.clear();
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        const PlayerSlot &s = setup.slots[i];
        if (!s.active)
            continue;
        ReplayPlayer::Player p;
        p.slot = i;
        p.faction = s.faction;
        p.team = s.team;
        p.color = s.color;
        p.human = s.human;
        p.aiLevel = s.aiLevel;
        auto it = named.find(i);
        if (it != named.end())
        {
            p.name = it->second.first;
            p.addresses = it->second.second;
        }
        if (p.name.empty())
            p.name = s.human ? std::string(_("Player")) + " " + std::to_string(i + 1) : std::string(_("Computer"));
        info.players.push_back(std::move(p));
    }
    if (info.players.empty() || info.turns == 0)
    {
        error = _("The replay is empty");
        return false;
    }
    info.carriesData = !data.files.empty();
    return true;
}

} // namespace

ReplayPlayer::~ReplayPlayer()
{
    if (m_overlayInstalled)
        Assets::ClearSessionOverlay();
}

bool ReplayPlayer::ReadInfo(const std::string &path, Info &out, std::string &error)
{
    Net::ReplayData data;
    std::string why;
    if (!Net::readReplay(path, data, why))
    {
        error = std::string(_("Not a readable replay")) + " (" + why + ")";
        return false;
    }
    out = Info {};
    out.file = path;
    std::error_code ec;
    out.fileBytes = static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
    if (ec)
        out.fileBytes = 0;
    return FillInfo(data, out, error);
}

bool ReplayPlayer::Load(const std::string &path, std::string &error)
{
    Net::ReplayData data;
    std::string why;
    if (!Net::readReplay(path, data, why))
    {
        error = std::string(_("Not a readable replay")) + " (" + why + ")";
        return false;
    }
    m_info = Info {};
    m_info.file = path;
    if (!FillInfo(data, m_info, error))
        return false;
    m_turns = std::move(data.turns);
    m_setup = std::move(data.setup);
    m_files = std::move(data.files);
    m_checksums.clear();
    // A file of another simulation build hashed the world differently: it plays, but is not checked
    if (m_info.checksumsComparable)
    {
        for (const auto &[tick, hash] : data.checksums)
            m_checksums[tick] = hash;
    }
    else if (!data.checksums.empty())
    {
        std::cout << "[Replay] Recorded by another simulation build: its checksums are not compared" << std::endl;
    }
    // Every turn must read, and the cameras are collected on the way
    m_views.clear();
    for (std::size_t t = 0; t < m_turns.size(); ++t)
    {
        Net::TurnBundle bundle;
        if (!Net::decodeTurn(m_turns[t].data(), m_turns[t].size(), bundle) || bundle.turn != t)
        {
            error = _("The replay is damaged");
            return false;
        }
        for (const Net::PlayerTurn &p : bundle.players)
        {
            std::vector<SimCommand> commands;
            if (!SimCommandCodec::Decode(p.bytes.data(), p.bytes.size(), p.slot, commands))
            {
                error = _("The replay is damaged");
                return false;
            }
            for (const SimCommand &c : commands)
            {
                if (c.type == CommandType::View)
                    m_views[p.slot].push_back(
                        {static_cast<std::uint32_t>(t) * m_info.ticksPerTurn,
                         static_cast<float>(c.x),
                         static_cast<float>(c.y),
                         std::clamp(static_cast<float>(c.a) / 100.0f, 0.25f, 8.0f)});
            }
        }
    }
    m_mismatchTick = -1;
    return true;
}

bool ReplayPlayer::PrepareMatch(int watchSlot, MatchSetup &out, std::string &error)
{
    if (!NetMatch::DecodeSetup(m_setup, out))
    {
        error = _("The replay is damaged");
        return false;
    }
    // The simulation data of the match, as the file carries it (keys are paths below assets/)
    if (m_overlayInstalled)
    {
        Assets::ClearSessionOverlay();
        m_overlayInstalled = false;
    }
    const std::string prefix = "data/assets/";
    std::map<std::string, std::vector<std::uint8_t>> overlay;
    std::size_t total = 0;
    for (const auto &[name, bytes] : m_files)
    {
        if (name.compare(0, prefix.size(), prefix) != 0)
            continue;
        total += bytes.size();
        bool readable = total <= kMaxOverlayBytes;
        if (readable && EndsWith(name, ".json"))
            readable = !nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false).is_discarded();
        else if (readable && EndsWith(name, ".yaml"))
        {
            try
            {
                YAML::Load(std::string(bytes.begin(), bytes.end()));
            }
            catch (...)
            {
                readable = false;
            }
        }
        if (!readable)
        {
            error = _("The replay is damaged");
            return false;
        }
        overlay[name.substr(prefix.size())] = bytes;
    }
    if (!overlay.empty())
    {
        Assets::SetSessionOverlay(std::move(overlay));
        m_overlayInstalled = true;
    }
    // The map: the one in the file, else the game's own with the same hash
    std::string file;
    for (const auto &[name, bytes] : m_files)
    {
        if (name.compare(0, 5, "maps/") == 0 && MapFiles::HashOfBytes(bytes) == out.mapHash)
            file = MapFiles::StoreRaw(out.mapPath, out.mapHash, bytes);
        if (!file.empty())
            break;
    }
    if (file.empty())
        file = MapFiles::Resolve(out.mapPath, out.mapHash);
    if (file.empty())
    {
        error = std::string(_("The map of this replay is missing")) + ": " + out.mapPath;
        if (m_overlayInstalled)
        {
            Assets::ClearSessionOverlay();
            m_overlayInstalled = false;
        }
        return false;
    }
    out.mapPath = file;
    out.localOnly = false;
    // Whom to watch: the asked slot, else the recorded player, else the first human
    auto has = [&](int slot) {
        return slot >= 0 && slot < kMaxPlayers && out.slots[slot].active;
    };
    int local = has(watchSlot) ? watchSlot : (has(m_info.localSlot) ? m_info.localSlot : -1);
    for (int i = 0; local < 0 && i < kMaxPlayers; ++i)
        if (out.slots[i].active && out.slots[i].human)
            local = i;
    for (int i = 0; local < 0 && i < kMaxPlayers; ++i)
        if (out.slots[i].active)
            local = i;
    out.local = static_cast<PlayerId>(std::max(0, local));
    m_mismatchTick = -1;
    m_lastScheduledTurn = -1;
    return true;
}

void ReplayPlayer::ScheduleDue(Simulation &sim, std::uint32_t tick)
{
    if (m_info.ticksPerTurn == 0 || tick % m_info.ticksPerTurn != 0)
        return;
    const std::uint32_t turn = tick / m_info.ticksPerTurn;
    if (turn >= m_turns.size() || static_cast<std::int64_t>(turn) == m_lastScheduledTurn)
        return;
    m_lastScheduledTurn = static_cast<std::int64_t>(turn);
    Net::TurnBundle bundle;
    if (Net::decodeTurn(m_turns[turn].data(), m_turns[turn].size(), bundle))
        NetMatch::ScheduleBundle(sim, bundle, m_info.ticksPerTurn);
}

void ReplayPlayer::Verify(std::uint32_t tick, std::uint64_t checksum)
{
    auto it = m_checksums.find(tick);
    if (it != m_checksums.end() && it->second != checksum && m_mismatchTick < 0)
        m_mismatchTick = tick;
}

bool ReplayPlayer::CameraOf(int slot, std::uint32_t tick, float &x, float &y, float &zoom) const
{
    auto it = m_views.find(slot);
    if (it == m_views.end() || it->second.empty())
        return false;
    const std::vector<View> &v = it->second;
    // The last view at or before `tick`; before the first one, the first
    auto after = std::upper_bound(v.begin(), v.end(), tick, [](std::uint32_t t, const View &w) { return t < w.tick; });
    const View &cur = after == v.begin() ? v.front() : *(after - 1);
    x = cur.x;
    y = cur.y;
    zoom = cur.zoom;
    return true;
}

void ReplayPlayer::Rewind()
{
    m_mismatchTick = -1;
    m_lastScheduledTurn = -1;
}

} // namespace OpenArtOfWar
