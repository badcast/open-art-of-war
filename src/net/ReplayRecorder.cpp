#include "net/ReplayRecorder.hpp"
#include "core/Mods.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "net/MapFiles.hpp"
#include "net/NetMatch.hpp"
#include "openaow/net/ReplayFile.hpp"

#include <nlohmann/json.hpp>

#include <ctime>
#include <filesystem>
#include <iostream>

namespace OpenArtOfWar
{

std::string ReplayRecorder::Directory()
{
    return (std::filesystem::path(Assets::UserDataDir()) / "replays").string();
}

void ReplayRecorder::Begin(Info info)
{
    if (m_active)
        return; // a reconnect: the same match goes on
    m_info = std::move(info);
    m_turns.clear();
    m_checksums.clear();
    m_startedAt = static_cast<std::int64_t>(std::time(nullptr));
    m_active = true;
}

void ReplayRecorder::OnBundle(const Net::TurnBundle &bundle)
{
    if (!m_active || m_turns.count(bundle.turn))
        return;
    std::vector<std::uint8_t> bytes = Net::encodeTurn(bundle);
    if (!bytes.empty())
        m_turns[bundle.turn] = std::move(bytes);
}

void ReplayRecorder::OnChecksum(std::uint32_t tick, std::uint64_t hash)
{
    if (m_active)
        m_checksums[tick] = hash;
}

std::string ReplayRecorder::Finish(const std::string &outcome, std::uint32_t tick)
{
    if (!m_active)
        return {};
    m_active = false;
    if (m_turns.empty())
        return {}; // nothing was played
    // The turns must follow each other from 0; a gap (a bug) would make the file unplayable
    Net::ReplayData data;
    std::uint32_t expected = 0;
    for (const auto &[turn, bytes] : m_turns)
    {
        if (turn != expected)
            break;
        data.turns.push_back(bytes);
        ++expected;
    }
    data.setup = m_info.setupBytes;
    for (const auto &[t, h] : m_checksums)
        data.checksums.emplace_back(t, h);

    const MatchSetup &s = m_info.setup;
    // The room's own names for the map: the path of the setup bytes, before it was resolved to a file
    MatchSetup raw;
    NetMatch::DecodeSetup(m_info.setupBytes, raw);
    nlohmann::json players = nlohmann::json::array();
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        const PlayerSlot &p = s.slots[i];
        if (!p.active)
            continue;
        players.push_back(
            {{"slot", i},
             {"name", i < static_cast<int>(m_info.names.size()) ? m_info.names[i] : std::string()},
             {"faction", static_cast<int>(p.faction)},
             {"team", p.team},
             {"color", p.color},
             {"human", p.human},
             {"ai_level", p.aiLevel},
             {"spawn", s.startPosition[i]}});
    }
    const std::uint32_t ticks = static_cast<std::uint32_t>(data.turns.size()) * m_info.ticksPerTurn;
    nlohmann::json meta
        = {{"format", 1},
           {"game_version", OAOW_VERSION},
           {"protocol", Net::kProtocolVersion},
           {"sim_build_id", NetMatch::SimBuildId()},
           {"source", "client"},
           {"recorded_at", m_startedAt},
           {"map", raw.mapPath},
           {"map_hash", raw.mapHash},
           {"assets_hash", m_info.assetsHash},
           {"seed", raw.seed},
           {"turn_ms", m_info.turnMs},
           {"ticks_per_turn", m_info.ticksPerTurn},
           {"turns", data.turns.size()},
           {"ticks", ticks},
           {"players", players},
           {"starting_credits", raw.startingCredits},
           {"fog", raw.fogOfWar},
           {"prebuilt", raw.prebuilt},
           {"night", raw.night},
           {"local_slot", s.local},
           {"result", {{"outcome", outcome}, {"tick", tick}}},
           {"checksum_interval", NetMatch::kHashIntervalTicks},
           {"mode", m_info.mode}};
    if (m_info.mode == "campaign")
    {
        meta["mission"] = m_info.mission;
        meta["player_faction"] = m_info.playerFaction;
        meta["difficulty"] = m_info.difficulty;
    }
    // The mods this computer played with (a network game plays the host's data: then they did not count)
    if (!Mods::Active().empty() && !Assets::HasSessionOverlay())
    {
        nlohmann::json mods = nlohmann::json::array();
        for (const Mods::ModInfo &m : Mods::Active())
            mods.push_back({{"id", m.id}, {"name", m.name}, {"version", m.version}});
        meta["mods"] = mods;
    }
    data.meta = meta.dump(2);

    // The map and the simulation data this match was played with: the file plays on its own
    std::vector<std::uint8_t> bytes;
    const std::string mapFile = MapFiles::Resolve(raw.mapPath, raw.mapHash);
    if (!mapFile.empty() && MapFiles::Read(mapFile, bytes))
        data.files["maps/" + std::filesystem::path(raw.mapPath).filename().string()] = bytes;
    for (const std::string &f : LoaderResources::NetHashFiles())
    {
        if (Net::validReplayFileName("data/" + f) && Assets::ReadBytes(f, bytes))
            data.files["data/" + f] = bytes;
    }
    {
        const std::string manifest = LoaderResources::NetManifestText();
        data.files["data/" + std::string(LoaderResources::kManifestPath)]
            = std::vector<std::uint8_t>(manifest.begin(), manifest.end());
    }

    const std::string path
        = (std::filesystem::path(Directory()) / Net::replayFileName(raw.mapPath, m_startedAt)).string();
    std::string error;
    if (!Net::writeReplay(path, data, error))
    {
        std::cerr << "[Replay] cannot save " << path << ": " << error << std::endl;
        return {};
    }
    std::cout << "[Replay] saved " << path << " (" << data.turns.size() << " turns)" << std::endl;
    return path;
}

} // namespace OpenArtOfWar
