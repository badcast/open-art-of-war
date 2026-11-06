#pragma once

#include "game/SimCommand.hpp"
#include <cstdint>
#include <vector>

namespace OpenArtOfWar
{

// Compact wire / replay form of the commands of one player for one turn. Only the fields a command
// type uses are written (unit lists delta-coded, numbers as zig-zag varints): a move order of 12
// units is ~12 bytes. The player is not part of it: a network session takes it from the sender.
// Decoding checks every value and refuses the whole turn on anything malformed.
namespace SimCommandCodec
{

constexpr std::size_t kMaxCommandsPerTurn = 64;
constexpr std::size_t kMaxIdsPerCommand = 512;

std::vector<std::uint8_t> Encode(const std::vector<SimCommand> &commands);
bool Decode(const std::uint8_t *data, std::size_t size, PlayerId player, std::vector<SimCommand> &out);

} // namespace SimCommandCodec
} // namespace OpenArtOfWar
