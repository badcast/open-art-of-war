#pragma once

#include "core/Config.hpp"

#include <string>
#include <vector>

namespace OpenArtOfWar
{

// Campaign mission dialogues. The texts live only in the translation catalogs (po/*.po), under the keys
//   dialog.<res|conf>.<mission>.title
//   dialog.<res|conf>.<mission>.<line>.speaker
//   dialog.<res|conf>.<mission>.<line>.text      (lines numbered from 1, as many as the catalog has)
// so every language gets its own text through gettext; this class only knows the ids and builds the keys.

// Resistance campaign, missions 1..7
constexpr int DialogMission_res_ID1 = 1;
constexpr int DialogMission_res_ID2 = 2;
constexpr int DialogMission_res_ID3 = 3;
constexpr int DialogMission_res_ID4 = 4;
constexpr int DialogMission_res_ID5 = 5;
constexpr int DialogMission_res_ID6 = 6;
constexpr int DialogMission_res_ID7 = 7;
// Confederation campaign, missions 1..7
constexpr int DialogMission_conf_ID1 = 101;
constexpr int DialogMission_conf_ID2 = 102;
constexpr int DialogMission_conf_ID3 = 103;
constexpr int DialogMission_conf_ID4 = 104;
constexpr int DialogMission_conf_ID5 = 105;
constexpr int DialogMission_conf_ID6 = 106;
constexpr int DialogMission_conf_ID7 = 107;

struct DialogLine
{
    std::string speaker; // translated
    std::string text;    // translated
};

struct MissionDialog
{
    int id {0};
    std::string title; // translated
    std::vector<DialogLine> lines;
    bool Empty() const
    {
        return lines.empty();
    }
};

class MissionDialogs
{
public:
    // The dialogue in the current language; empty when the id is unknown or the catalog has no lines for it
    static MissionDialog Get(int dialogId);

    // The dialogue of a campaign mission (missionIndex from 0), 0 when that mission has none
    static int IdForMission(Faction campaign, int missionIndex);

    // The catalog key prefix of a dialogue ("dialog.conf.3"), empty for an unknown id
    static std::string KeyPrefix(int dialogId);

    // Seconds after the start of a campaign mission before its dialogue opens
    static constexpr float kDelaySeconds = 5.0f;
};

} // namespace OpenArtOfWar
