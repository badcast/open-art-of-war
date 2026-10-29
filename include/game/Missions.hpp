#pragma once

#include "core/Config.hpp"
#include <string>
#include <vector>

namespace OpenArtOfWar
{

    // Text and map of a campaign mission as the menus show it (name, objective and briefing are gettext msgids)
    struct MissionInfo
    {
        int id {0};
        int mapIndex {37};
        std::string name;
        std::string mapName;
        std::string objective;
        std::string briefing;
    };

    struct Rect
    {
        int x, y, w, h;
    }; // tile rectangle
    struct Point
    {
        int x, y;
    };

    using RectList = std::vector<Rect>;
    using PointList = std::vector<Point>;
    using ZoneSet = std::vector<RectList>; // one list per group

    // Preplaced building (type 0..14) or wall (type 15+); the side follows from the type
    struct MissionEntity
    {
        int type, x, y, direction;
    };

    // Mission script command in the original encoding (opcodes are handled by MissionController)
    struct MissionCommand
    {
        int cmd, x, y, val, delay, group;
    };

    struct PatrolPath
    {
        int y {0};
        std::vector<int> g;
        std::vector<int> h;
    };

    struct MissionData
    {
        std::string mapFile; // "m37", "map_mission_1": file of assets/maps without extension
        // Defense mission: the enemy has no base, its squads do not stand around at the start but spawn at their
        // points one after another and march on the player's base
        bool defense {false};
        int mapIndex {0}; // number of the original map record
        std::vector<MissionEntity> entities;
        ZoneSet wallZones;
        std::vector<ZoneSet> buildZones;
        ZoneSet mineZones;
        ZoneSet defenseZones;
        std::vector<PatrolPath> patrolPaths;
        std::vector<PointList> rallyPoints;
        std::vector<int> dialogs; // indices of the campaign dialogues the mission shows, in order of the "cmd 39" scripts
        std::vector<MissionCommand> scripts;
    };

    struct MissionDef
    {
        MissionInfo info;
        MissionData data;
    };

    // The missions of both campaigns. The data lives in MissionsResistance.cpp and MissionsConfederation.cpp.
    namespace Missions
    {

        const std::vector<MissionDef> &resistance();    // Liberation of Peru, ids 0..6 are the campaign
        const std::vector<MissionDef> &confederation(); // Global Confederation, ids 0..6
        const std::vector<MissionDef> &campaign(Faction faction);
        const MissionDef *find(Faction faction, int missionId);

    } // namespace Missions

} // namespace OpenArtOfWar
