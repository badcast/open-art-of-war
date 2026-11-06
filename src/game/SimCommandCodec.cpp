#include "game/SimCommandCodec.hpp"
#include "openaow/net/BitStream.hpp"

#include <algorithm>

namespace OpenArtOfWar::SimCommandCodec
{

namespace
{

// Fields each command type carries
enum Field : std::uint8_t
{
    Ids = 1,
    X = 2,
    Y = 4,
    A = 8,
    B = 16,
    C = 32,
    D = 64
};

constexpr std::uint8_t kSchema[static_cast<int>(CommandType::Count)] = {
    Ids | X | Y | A | B,   // OrderAt
    Ids | X | Y,           // AttackWall
    Ids | X | Y,           // SetRally
    Ids,                   // ToggleSiege
    Ids,                   // Stop
    Ids,                   // HoldPosition
    X | Y | A,             // PlaceBuilding
    A | B | C,             // Produce
    A | B,                 // CancelProduce
    A,                     // StartResearch
    Ids,                   // Demolish
    A,                     // DebugCredits
    X | Y | A | B | C | D, // DebugSpawn
    0,                     // PlayerLeft
    Ids | A,               // SetStance
    Ids,                   // ToggleRepair
    X | Y | A,             // View
    A | B,                 // SetRepeat
};

constexpr int kTypeBits = 5;
static_assert(static_cast<int>(CommandType::Count) <= (1 << kTypeBits), "command type needs more bits");

// World coordinates and indices stay far below this: anything bigger is a broken or hostile packet
constexpr std::int64_t kValueLimit = 1 << 24;

void WriteValue(Net::BitWriter &w, std::int32_t v)
{
    w.writeVarInt(v);
}

bool ReadValue(Net::BitReader &r, std::int32_t &v)
{
    const std::int64_t raw = r.readVarInt();
    if (raw <= -kValueLimit || raw >= kValueLimit)
    {
        r.fail();
        return false;
    }
    v = static_cast<std::int32_t>(raw);
    return !r.failed();
}

} // namespace

std::vector<std::uint8_t> Encode(const std::vector<SimCommand> &commands)
{
    // Worst case: every id a 5-byte varint
    std::size_t capacity = 16;
    for (const SimCommand &c : commands)
        capacity += 48 + c.ids.size() * 5;
    std::vector<std::uint8_t> buf(capacity);
    Net::BitWriter w(buf.data(), buf.size());
    w.writeVarUint(commands.size());
    for (const SimCommand &c : commands)
    {
        const std::uint8_t schema = kSchema[static_cast<int>(c.type)];
        w.writeBits(static_cast<std::uint32_t>(c.type), kTypeBits);
        w.writeBool(c.debug);
        if (schema & Ids)
        {
            std::vector<std::uint32_t> ids = c.ids; // the list coding needs them ascending and unique
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            w.writeIdList(ids);
        }
        if (schema & X)
            WriteValue(w, c.x);
        if (schema & Y)
            WriteValue(w, c.y);
        if (schema & A)
            WriteValue(w, c.a);
        if (schema & B)
            WriteValue(w, c.b);
        if (schema & C)
            WriteValue(w, c.c);
        if (schema & D)
            WriteValue(w, c.d);
    }
    buf.resize(w.finish());
    return buf;
}

bool Decode(const std::uint8_t *data, std::size_t size, PlayerId player, std::vector<SimCommand> &out)
{
    out.clear();
    Net::BitReader r(data, size);
    const std::uint64_t count = r.readVarUint();
    if (r.failed() || count > kMaxCommandsPerTurn)
        return false;
    out.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i)
    {
        SimCommand c;
        const std::uint32_t type = r.readBits(kTypeBits);
        if (type >= static_cast<std::uint32_t>(CommandType::Count))
            return false;
        c.type = static_cast<CommandType>(type);
        c.player = player;
        c.debug = r.readBool();
        const std::uint8_t schema = kSchema[type];
        if ((schema & Ids) && !r.readIdList(c.ids, kMaxIdsPerCommand))
            return false;
        if ((schema & X) && !ReadValue(r, c.x))
            return false;
        if ((schema & Y) && !ReadValue(r, c.y))
            return false;
        if ((schema & A) && !ReadValue(r, c.a))
            return false;
        if ((schema & B) && !ReadValue(r, c.b))
            return false;
        if ((schema & C) && !ReadValue(r, c.c))
            return false;
        if ((schema & D) && !ReadValue(r, c.d))
            return false;
        if (r.failed())
            return false;
        out.push_back(std::move(c));
    }
    return !r.failed() && r.atEnd();
}

} // namespace OpenArtOfWar::SimCommandCodec
