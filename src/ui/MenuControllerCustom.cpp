#include "ui/MenuController.hpp"
#include "MenuControllerInternal.hpp"
#include "ui/MapFilter.hpp"
#include "ui/MatchOptions.hpp"
#include "audio/SoundRequests.hpp"
#include <nlohmann/json.hpp>
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "graphics/ShaderEffects.hpp"
#include "game/Sides.hpp"
#include "ui/UiKit.hpp"
#include "core/Localization.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/Camera.hpp"
#include "graphics/TextureManager.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "input/InputManager.hpp"
#include "audio/SoundEngine.hpp"
#include "game/MissionController.hpp"
#include "game/TileMap.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/Pathfinder.hpp"
#include "game/AIController.hpp"
#include "storage/SaveManager.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <sstream>
#include <filesystem>
#include <vector>

namespace OpenArtOfWar
{

std::vector<bool> MenuController::CustomTakenColors(int row) const
{
    std::vector<bool> taken(Settings::kArmyColors, false);
    for (int i = 0; i < static_cast<int>(m_customSlots.size()); ++i)
    {
        if (i == 0 && m_customSpectator)
            continue; // a watcher shows no colour
        const int c = m_customSlots[i].color;
        if (i != row && c >= 1 && c <= Settings::kArmyColors)
            taken[c - 1] = true;
    }
    return taken;
}

int MenuController::CustomSlotAt(int position) const
{
    for (std::size_t i = 0; i < m_customSlots.size(); ++i)
        if (m_customSlots[i].position == position)
            return static_cast<int>(i);
    return -1;
}

std::vector<int> MenuController::CustomSeatCodes(int position) const
{
    if (CustomSlotAt(position) == 0)
        return {1, 2, 3, 4, 5, 6, 7}; // the person's own place: he stays, or leaves it (Open / a computer player /
                                      // blocked) and watches
    if (CustomSlotAt(position) < 0)
        return {1, 2, 3, 4, 5, 6, 7}; // you / open / four computer levels / blocked
    return {1, 2, 3, 4, 5, 6, 7};
}

void MenuController::AddCustomAi(int position, int level, bool autoFilled)
{
    CustomSlot add;
    add.position = position;
    add.level = level;
    add.autoFilled = autoFilled;
    const std::vector<bool> takenColors = CustomTakenColors(-1);
    for (int c = 0; c < static_cast<int>(takenColors.size()); ++c)
    {
        if (!takenColors[c])
        {
            add.color = c + 1;
            break;
        }
    }
    for (int t = 0; t < kMaxPlayers; ++t)
    { // the first team nobody is in yet
        bool used = false;
        for (const CustomSlot &cs : m_customSlots)
            used = used || cs.team == t;
        if (!used)
        {
            add.team = t;
            break;
        }
    }
    m_customSlots.push_back(add);
}

void MenuController::SetCustomSeat(int position, int code)
{
    if (position < 0 || position >= kMaxPlayers || m_customSlots.empty())
        return;
    const int held = CustomSlotAt(position);
    if (code == 1)
    {
        // The person moves here: the place he leaves is open; a computer player here goes
        if (held == 0)
            return;
        if (held > 0)
            m_customSlots.erase(m_customSlots.begin() + held);
        m_customBlocked &= ~(1u << position);
        m_customOpen &= ~(1u << position);
        if (m_customSlots[0].position >= 0)
            m_customOpen |= 1u << m_customSlots[0].position; // the place he leaves is open
        m_customSlots[0].position = position;
        m_customSpectator = false;
        // While he watched, a computer player may have taken his colour
        if (const std::vector<bool> taken = CustomTakenColors(0); m_customSlots[0].color >= 1
                                                                  && m_customSlots[0].color <= Settings::kArmyColors
                                                                  && taken[m_customSlots[0].color - 1])
        {
            for (int c = 0; c < static_cast<int>(taken.size()); ++c)
            {
                if (!taken[c])
                {
                    m_customSlots[0].color = c + 1;
                    break;
                }
            }
        }
        return;
    }
    if (held == 0)
    {
        // Anything but "you" on the person's own place: he does not take part, the computer players fight and he
        // watches (the place becomes Open, a computer player or blocked)
        m_customSpectator = true;
        if (m_customSlots[0].position >= 0)
            m_customOpen |= 1u << m_customSlots[0].position;
        m_customSlots[0].position = -1;
        SetCustomSeat(position, code);
        return;
    }
    if (code >= 3 && code <= 6)
    {
        m_customBlocked &= ~(1u << position);
        m_customOpen &= ~(1u << position);
        if (held > 0)
        {
            m_customSlots[held].level = code - 3;
            m_customSlots[held].autoFilled = false; // chosen by hand now
            return;
        }
        AddCustomAi(position, code - 3, false);
        return;
    }
    // Open or blocked: nobody plays there
    if (held > 0)
        m_customSlots.erase(m_customSlots.begin() + held);
    if (code == 2)
        m_customOpen |= 1u << position;
    else
        m_customOpen &= ~(1u << position);
    if (code == 7)
        m_customBlocked |= 1u << position;
    else
        m_customBlocked &= ~(1u << position);
}

// The person plus at least one computer player, never more than the map has start positions
void MenuController::EnsureCustomSlots()
{
    if (m_skirmishSelected < 0 || m_skirmishSelected >= static_cast<int>(m_skirmishMaps.size()))
        return;
    const std::size_t most
        = static_cast<std::size_t>(std::clamp(m_skirmishMaps[m_skirmishSelected].players, 2, kMaxPlayers));
    if (m_customSlots.empty())
    {
        m_customSlots.resize(1);
        m_customSlots[0].color = 1;
        m_customSlots[0].position = 0;
    }
    // A watching person holds no start position: the computer players may fill every one of them
    const std::size_t slotsMost = most + (m_customSpectator ? 1u : 0u);
    if (m_customSlots.size() > slotsMost)
        m_customSlots.resize(slotsMost);
    m_customBlocked &= (1u << most) - 1;
    // Positions the map does not have, or held twice: the players take the lowest free ones
    std::vector<bool> used(kMaxPlayers, false);
    for (CustomSlot &cs : m_customSlots)
    {
        if (cs.position < 0 || cs.position >= static_cast<int>(most) || used[cs.position])
            cs.position = -1;
        else
            used[cs.position] = true;
    }
    for (CustomSlot &cs : m_customSlots)
    {
        if (cs.position >= 0 || (m_customSpectator && &cs == &m_customSlots[0]))
            continue;
        for (int pos = 0; pos < static_cast<int>(most); ++pos)
        {
            if (!used[pos] && !((m_customBlocked >> pos) & 1u))
            {
                cs.position = pos;
                used[pos] = true;
                break;
            }
        }
    }
    // Automatic filling: every start position nobody holds, blocked or left Open by hand gets a computer player of
    // the last chosen level; those not changed by hand follow that level when it changes
    const int level = std::clamp(static_cast<int>(m_difficulty), 0, 3);
    for (CustomSlot &cs : m_customSlots)
        if (cs.autoFilled)
            cs.level = level;
    for (int pos = 0; pos < static_cast<int>(most); ++pos)
    {
        if (CustomSlotAt(pos) >= 0 || ((m_customBlocked >> pos) & 1u) || ((m_customOpen >> pos) & 1u))
            continue;
        AddCustomAi(pos, level, true);
    }
}

} // namespace OpenArtOfWar
