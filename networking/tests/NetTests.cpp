// Unit tests of OpenAOWNetworking: CRC-32, bit stream, message codecs (round trip, every truncation,
// random garbage) and a real ENet connection over localhost with the game's host setup.
#include "openaow/net/BitStream.hpp"
#include "openaow/net/Crc32.hpp"
#include "openaow/net/Net.hpp"
#include "openaow/net/Protocol.hpp"
#include "openaow/net/Discovery.hpp"
#include "openaow/net/MasterClient.hpp"
#include "openaow/net/Rendezvous.hpp"
#include "openaow/net/RoomServer.hpp"
#include "openaow/net/Session.hpp"
#include "openaow/net/Transport.hpp"
#include "transport/EnetSupport.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace OpenArtOfWar::Net;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                          \
    } while (0)

std::uint32_t crc32Bitwise(const std::uint8_t* p, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

void testCrc32() {
    std::printf("[crc32]\n");
    CHECK(crc32("123456789", 9) == 0xCBF43926u);   // the standard check value
    CHECK(crc32("", 0) == 0u);
    std::mt19937 rng(1);
    std::vector<std::uint8_t> buf(4096);
    for (auto& b : buf) b = static_cast<std::uint8_t>(rng());
    for (std::size_t n : {1u, 7u, 8u, 9u, 63u, 64u, 1000u, 4096u}) {
        CHECK(crc32(buf.data(), n) == crc32Bitwise(buf.data(), n));
        const std::size_t half = n / 3;
        CHECK(crc32(buf.data() + half, n - half, crc32(buf.data(), half)) == crc32(buf.data(), n));
    }
}

void testBitStream() {
    std::printf("[bitstream]\n");
    std::uint8_t buf[512];
    BitWriter w(buf, sizeof buf);
    w.writeBits(5, 3);
    w.writeBool(true);
    w.writeRanged(-3, -10, 10);
    w.writeVarUint(0);
    w.writeVarUint(127);
    w.writeVarUint(128);
    w.writeVarUint(0xFFFFFFFFFFFFFFFFull);
    w.writeVarInt(-1);
    w.writeVarInt(-123456789);
    w.writeU16(0xBEEF);
    w.writeU64(0x0123456789ABCDEFull);
    w.writeFloat(-0.15625f);
    w.writeString("Привет", 64);
    w.writeIdList({3, 4, 5, 9, 1000, 1001, 0xFFFFFFFFu});
    w.writeBits(0xFFFFFFFFu, 32);
    const std::size_t size = w.finish();
    CHECK(!w.overflowed());

    BitReader r(buf, size);
    CHECK(r.readBits(3) == 5);
    CHECK(r.readBool());
    CHECK(r.readRanged(-10, 10) == -3);
    CHECK(r.readVarUint() == 0);
    CHECK(r.readVarUint() == 127);
    CHECK(r.readVarUint() == 128);
    CHECK(r.readVarUint() == 0xFFFFFFFFFFFFFFFFull);
    CHECK(r.readVarInt() == -1);
    CHECK(r.readVarInt() == -123456789);
    CHECK(r.readU16() == 0xBEEF);
    CHECK(r.readU64() == 0x0123456789ABCDEFull);
    CHECK(r.readFloat() == -0.15625f);
    CHECK(r.readString(64) == "Привет");
    std::vector<std::uint32_t> ids;
    CHECK(r.readIdList(ids, 100));
    CHECK((ids == std::vector<std::uint32_t>{3, 4, 5, 9, 1000, 1001, 0xFFFFFFFFu}));
    CHECK(r.readBits(32) == 0xFFFFFFFFu);
    CHECK(!r.failed());
    CHECK(r.atEnd());

    // Past the end: zeros and failed(), never a crash
    CHECK(r.readU32() == 0);
    CHECK(r.failed());

    // String longer than the reader allows
    {
        std::uint8_t b[64];
        BitWriter sw(b, sizeof b);
        sw.writeString("abcdef", 64);
        BitReader sr(b, sw.finish());
        sr.readString(3);
        CHECK(sr.failed());
    }
    // Writer overflow is reported
    {
        std::uint8_t b[4];
        BitWriter ow(b, sizeof b);
        ow.writeU32(1);
        CHECK(!ow.overflowed());
        ow.writeBits(1, 1);
        ow.finish();
        CHECK(ow.overflowed());
    }
    // A huge count in an ID list must not allocate
    {
        std::uint8_t b[16];
        BitWriter hw(b, sizeof b);
        hw.writeVarUint(1u << 30);
        BitReader hr(b, hw.finish());
        CHECK(!hr.readIdList(ids, 1u << 31));
        CHECK(ids.empty());
    }
    // Size of a typical selection: 40 units with nearby IDs
    {
        std::vector<std::uint32_t> sel;
        for (std::uint32_t i = 0; i < 40; ++i) sel.push_back(500 + i * 3 + (i % 3));   // gaps 0..4
        std::uint8_t b[256];
        BitWriter sw(b, sizeof b);
        sw.writeIdList(sel);
        const std::size_t n = sw.finish();
        std::printf("  40-unit selection: %zu bytes (raw u32: 160)\n", n);
        CHECK(n <= 24);
        BitReader sr(b, n);
        std::vector<std::uint32_t> back;
        CHECK(sr.readIdList(back, 100) && back == sel);
    }
    // Random round trips of mixed field widths
    std::mt19937 rng(7);
    for (int round = 0; round < 2000; ++round) {
        std::vector<std::pair<int, std::uint32_t>> fields;
        std::uint8_t b[600];
        BitWriter rw(b, sizeof b);
        for (int i = 0; i < 100; ++i) {
            const int bits = 1 + static_cast<int>(rng() % 32);
            const std::uint32_t v = bits == 32 ? rng() : rng() & ((1u << bits) - 1u);
            fields.emplace_back(bits, v);
            rw.writeBits(v, bits);
        }
        BitReader rr(b, rw.finish());
        bool same = true;
        for (const auto& [bits, v] : fields) same = same && rr.readBits(bits) == v;
        CHECK(same && !rr.failed());
    }
}

// Encodes `m`, then decodes it back and checks the round trip and every truncated prefix
template <typename T>
bool roundTrip(const T& m, MsgType type, T& out, std::size_t* sizeOut = nullptr) {
    std::uint8_t buf[4096];
    BitWriter w(buf, sizeof buf);
    encode(w, m);
    const std::size_t size = w.finish();
    if (sizeOut) *sizeOut = size;
    if (w.overflowed()) return false;
    BitReader r(buf, size);
    if (readType(r) != type) return false;
    if (!decode(r, out)) return false;
    // Every shorter packet is rejected
    for (std::size_t cut = 1; cut < size; ++cut) {
        BitReader tr(buf, cut);
        readType(tr);
        T scratch;
        if (decode(tr, scratch)) {
            std::printf("  truncated to %zu of %zu bytes still decodes\n", cut, size);
            return false;
        }
    }
    return true;
}

void testProtocol() {
    std::printf("[protocol]\n");
    {
        Hello h, o;
        h.gameVersion = "0.0.1-alpha";
        h.simBuildId = 0x1122334455667788ull;
        h.assetsHash = 42;
        h.playerName = "Командир";
        h.token[3] = 9;
        CHECK(roundTrip(h, MsgType::Hello, o));
        CHECK(o.gameVersion == h.gameVersion && o.simBuildId == h.simBuildId && o.assetsHash == 42 &&
              o.playerName == h.playerName && o.token == h.token);
    }
    {
        Welcome m, o;
        m.slot = 7;
        m.token.fill(0xAB);
        m.turnMs = 100;
        m.ticksPerTurn = 2;
        CHECK(roundTrip(m, MsgType::Welcome, o));
        CHECK(o.slot == 7 && o.token == m.token && o.turnMs == 100 && o.ticksPerTurn == 2);
    }
    {
        Reject m, o;
        m.reason = RejectReason::AssetsMismatch;
        m.detail = "assets differ";
        CHECK(roundTrip(m, MsgType::Reject, o));
        CHECK(o.reason == m.reason && o.detail == m.detail);
    }
    {
        Chat m, o;
        m.fromSlot = 3;
        m.teamOnly = true;
        m.text = "gg";
        CHECK(roundTrip(m, MsgType::Chat, o));
        CHECK(o.fromSlot == 3 && o.teamOnly && o.text == "gg");
    }
    {
        Commands m, o;
        m.turn = 70000;
        m.bytes = {1, 2, 3, 250};
        CHECK(roundTrip(m, MsgType::Commands, o));
        CHECK(o.turn == 70000 && o.bytes == m.bytes);
    }
    {
        TurnBundle m, o;
        m.turn = 1234;
        std::size_t emptySize = 0;
        CHECK(roundTrip(m, MsgType::TurnBundle, o, &emptySize));
        CHECK(o.turn == 1234 && o.players.empty());
        std::printf("  empty turn: %zu bytes\n", emptySize);
        CHECK(emptySize <= 4);
        m.inputDelay = 3;
        m.players = {{0, {9, 8, 7}}, {5, {}}, {7, {1}}};
        CHECK(roundTrip(m, MsgType::TurnBundle, o));
        CHECK(o.inputDelay == 3 && o.players.size() == 3 && o.players[0].bytes == m.players[0].bytes &&
              o.players[1].slot == 5 && o.players[2].bytes == m.players[2].bytes);
        // Two batches of the same player (a late one joined this turn) keep their order
        m.players = {{2, {1}}, {2, {2}}};
        CHECK(roundTrip(m, MsgType::TurnBundle, o));
        CHECK(o.players.size() == 2 && o.players[0].bytes[0] == 1 && o.players[1].bytes[0] == 2);
    }
    {
        WorldHash m, o;
        m.tick = 99;
        m.hash = 0xDEADBEEFCAFEBABEull;
        CHECK(roundTrip(m, MsgType::WorldHash, o));
        CHECK(o.tick == 99 && o.hash == m.hash);
    }
    {
        Heartbeat m, o;
        m.sendTimeMs = 123;
        m.echoTimeMs = 456;
        m.simTick = 789;
        CHECK(roundTrip(m, MsgType::Heartbeat, o));
        CHECK(o.sendTimeMs == 123 && o.echoTimeMs == 456 && o.simTick == 789);
    }
    // Oversized command blob is rejected
    {
        Commands m, o;
        m.bytes.assign(kMaxPlayerCommandBytes + 1, 1);
        std::uint8_t buf[2048];
        BitWriter w(buf, sizeof buf);
        encode(w, m);
        BitReader r(buf, w.finish());
        readType(r);
        CHECK(!decode(r, o));
    }
}

// Random bytes into every decoder: they may accept or refuse, but must stay inside the buffer
// (run under -fsanitize=address,undefined to catch the rest)
void testFuzz() {
    std::printf("[fuzz]\n");
    std::mt19937 rng(12345);
    std::vector<std::uint8_t> buf;
    int accepted = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 300000; ++i) {
        buf.resize(rng() % 64);
        for (auto& b : buf) b = static_cast<std::uint8_t>(rng());
        BitReader r(buf.data(), buf.size());
        Hello h; Welcome we; Reject re; Chat c; Commands co; TurnBundle tb; WorldHash wh; Heartbeat hb;
        bool ok = false;
        switch (i % 8) {
            case 0: ok = decode(r, h); break;
            case 1: ok = decode(r, we); break;
            case 2: ok = decode(r, re); break;
            case 3: ok = decode(r, c); break;
            case 4: ok = decode(r, co); break;
            case 5: ok = decode(r, tb); break;
            case 6: ok = decode(r, wh); break;
            default: ok = decode(r, hb); break;
        }
        accepted += ok ? 1 : 0;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("  300000 random packets in %.0f ms, %d decoded as valid\n", ms, accepted);
    CHECK(true);
}

// Host + client over 127.0.0.1 with the game's setup (CRC-32, range coder, 4 channels)
void testEnetLoopback() {
    std::printf("[enet loopback]\n");
    Session net;
    CHECK(net.ok());
    if (!net.ok()) return;

    ENetAddress any{};
    any.host = ENET_HOST_ANY;
    any.port = 0;   // any free port
    ENetHost* server = enet_host_create(&any, 4, static_cast<size_t>(Channel::Count), 0, 0);
    CHECK(server != nullptr);
    if (!server) return;
    configureHost(server);
    ENetAddress bound{};
    enet_socket_get_address(server->socket, &bound);

    ENetHost* client = enet_host_create(nullptr, 1, static_cast<size_t>(Channel::Count), 0, 0);
    CHECK(client != nullptr);
    if (!client) {
        enet_host_destroy(server);
        return;
    }
    configureHost(client);
    ENetAddress target{};
    enet_address_set_host_ip(&target, "127.0.0.1");
    target.port = bound.port;
    ENetPeer* peer = enet_host_connect(client, &target, static_cast<size_t>(Channel::Count), 0);
    CHECK(peer != nullptr);

    bool connected = false, received = false;
    TurnBundle got;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !received) {
        ENetEvent ev;
        while (enet_host_service(client, &ev, 1) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                connected = true;
                TurnBundle tb;
                tb.turn = 77;
                tb.players = {{1, {4, 2}}};
                std::uint8_t buf[256];
                BitWriter w(buf, sizeof buf);
                encode(w, tb);
                ENetPacket* p = enet_packet_create(buf, w.finish(), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(peer, static_cast<enet_uint8>(Channel::Lockstep), p);
            }
        }
        while (enet_host_service(server, &ev, 1) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                CHECK(ev.channelID == static_cast<enet_uint8>(Channel::Lockstep));
                BitReader r(ev.packet->data, ev.packet->dataLength);   // zero-copy from the packet
                received = readType(r) == MsgType::TurnBundle && decode(r, got);
                enet_packet_destroy(ev.packet);
            }
        }
    }
    CHECK(connected);
    CHECK(received);
    CHECK(got.turn == 77 && got.players.size() == 1 && got.players[0].bytes == std::vector<std::uint8_t>({4, 2}));
    enet_peer_disconnect_now(peer, 0);
    enet_host_destroy(client);
    enet_host_destroy(server);
}

GameIdentity testIdentity() { return GameIdentity{"1.0-test", 0x1234, 0x5678}; }

ClientConfig clientConfig(const std::string& name) {
    ClientConfig c;
    c.identity = testIdentity();
    c.playerName = name;
    return c;
}

// Host and three clients in memory: join, roster, start, turns with everybody's commands in the same
// order on every client, a late command moved to the next turn, a desync, a reconnect
void testLockstepSession() {
    std::printf("[lockstep session]\n");
    LoopbackHub hub;
    HostConfig hc;
    hc.identity = testIdentity();
    hc.turnMs = 100;
    hc.inputDelayMin = 2;
    SessionHost host(hc);
    host.addTransport(hub.server());
    std::vector<std::unique_ptr<SessionClient>> clients;
    for (int i = 0; i < 3; ++i) clients.push_back(std::make_unique<SessionClient>(hub.client(), clientConfig("P" + std::to_string(i))));
    uint32_t now = 0;
    auto step = [&](uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            now += 10;
            for (auto& c : clients) if (c) c->update(now);
            host.update(now);
            for (auto& c : clients) if (c) c->update(now);
        }
    };
    step(50);
    CHECK(host.playerCount() == 3);
    bool joined = true;
    for (auto& c : clients) joined = joined && c->state() == SessionClient::State::Joined;
    CHECK(joined);
    CHECK(clients[0]->slot() == 0 && clients[1]->slot() == 1 && clients[2]->slot() == 2);
    CHECK(clients[2]->roster().players.size() == 3);

    // A wrong game version is turned away
    {
        ClientConfig bad = clientConfig("X");
        bad.identity.assetsHash = 1;
        SessionClient intruder(hub.client(), bad);
        for (int i = 0; i < 3; ++i) {
            intruder.update(now);
            host.update(now);
            intruder.update(now);
        }
        CHECK(intruder.state() == SessionClient::State::Rejected && intruder.rejectReason() == RejectReason::AssetsMismatch);
        host.update(now);
    }

    host.startMatch({7, 7, 7}, now, 200);
    step(20);
    bool started = true;
    for (auto& c : clients) started = started && c->matchStarted() && c->matchSetup() == std::vector<uint8_t>{7, 7, 7};
    CHECK(started);

    // Turn 2: client 1 and 2 send commands; client 0 sends for turn 0 when turn 0 is already gone (late)
    clients[1]->sendCommands(2, {11});
    clients[2]->sendCommands(2, {22});
    step(500);   // turns 0..4 closed (start + 200 ms, then every 100 ms)
    clients[0]->sendCommands(0, {99});   // late: lands in the next open turn
    step(200);
    const TurnBundle* t2 = clients[0]->turn(2);
    CHECK(t2 && t2->players.size() == 2 && t2->players[0].slot == 1 && t2->players[1].slot == 2);
    bool sameEverywhere = true;
    for (auto& c : clients) {
        const TurnBundle* b = c->turn(2);
        sameEverywhere = sameEverywhere && b && b->players.size() == 2 && b->players[0].bytes == std::vector<uint8_t>{11};
    }
    CHECK(sameEverywhere);
    // The late one is in exactly one later turn, the same for everybody
    uint32_t lateTurn = 0;
    for (uint32_t t = 3; t < clients[0]->turnsReceived(); ++t) {
        const TurnBundle* b = clients[0]->turn(t);
        if (b && !b->players.empty() && b->players[0].slot == 0) lateTurn = t;
    }
    CHECK(lateTurn > 2);
    CHECK(clients[2]->turn(lateTurn) && clients[2]->turn(lateTurn)->players.size() == 1);
    CHECK(clients[0]->inputDelay() == 2);
    std::printf("  turns received %u, late command moved to turn %u\n", clients[0]->turnsReceived(), lateTurn);

    // A pause stops the turns for everybody; going on does not bring a burst of turns
    clients[1]->requestPause(true);
    step(20);
    const uint32_t pausedAt = clients[0]->turnsReceived();
    CHECK(host.paused() && clients[2]->pauseState().paused && clients[2]->pauseState().bySlot == 1 &&
          clients[2]->pauseState().pausesLeft == 2);
    step(1000);
    CHECK(clients[0]->turnsReceived() == pausedAt);
    clients[0]->requestPause(false);
    step(110);
    CHECK(!host.paused() && !clients[1]->pauseState().paused);
    CHECK(clients[0]->turnsReceived() <= pausedAt + 2);
    // Equal checksums: fine; one different: everybody hears about the desync
    for (auto& c : clients) c->sendWorldHash(20, 0xAAAA);
    step(20);
    CHECK(!host.desynced());
    clients[0]->sendWorldHash(40, 0xAAAA);
    clients[1]->sendWorldHash(40, 0xAAAA);
    clients[2]->sendWorldHash(40, 0xBBBB);
    step(20);
    CHECK(host.desynced() && host.desyncTick() == 40);
    CHECK(clients[1]->desynced() && clients[1]->desyncTick() == 40);

    // Client 2 drops and comes back with its token: same slot, the match and every turn again
    const SessionToken token = clients[2]->token();
    const uint32_t before = clients[0]->turnsReceived();
    clients[2].reset();
    step(30);
    CHECK(!host.players()[2].connected);
    ClientConfig back = clientConfig("P2");
    back.token = token;
    clients[2] = std::make_unique<SessionClient>(hub.client(), back);
    step(30);
    CHECK(clients[2]->state() == SessionClient::State::Joined && clients[2]->slot() == 2);
    CHECK(clients[2]->matchStarted() && clients[2]->turnsReceived() >= before);
    CHECK(clients[2]->turn(2) && clients[2]->turn(2)->players.size() == 2);
    // A stranger cannot join a running match
    SessionClient late(hub.client(), clientConfig("Late"));
    for (int i = 0; i < 3; ++i) {
        late.update(now);
        host.update(now);
        late.update(now);
    }
    CHECK(late.state() == SessionClient::State::Rejected && late.rejectReason() == RejectReason::MatchRunning);
}

// The room before a match: map, picks, a start position only one may hold, ready, leaving frees the slot
void testLobby() {
    std::printf("[lobby]\n");
    LoopbackHub hub;
    HostConfig hc;
    hc.identity = testIdentity();
    SessionHost host(hc);
    host.addTransport(hub.server());
    auto a = std::make_unique<SessionClient>(hub.client(), clientConfig("A"));
    auto b = std::make_unique<SessionClient>(hub.client(), clientConfig("B"));
    auto pump = [&] {
        for (int i = 0; i < 3; ++i) {
            if (a) a->update(0);
            if (b) b->update(0);
            host.update(0);
        }
        if (a) a->update(0);
        if (b) b->update(0);
    };
    pump();
    host.setLobbyMap("assets/maps/m26.json", 4);
    pump();
    CHECK(b->lobby().map == "assets/maps/m26.json" && b->lobby().spawnCount == 4 && b->lobby().slots.size() == 2);
    CHECK(b->lobby().slots[1].choice.team == 1 && b->lobby().slots[0].choice.faction == kRandom);
    LobbyChoice ca;
    ca.faction = 0;
    ca.spawn = 2;
    a->sendLobbyChoice(ca);
    pump();
    LobbyChoice cb;
    cb.faction = 1;
    cb.team = 1;
    cb.spawn = 2;   // A holds it already
    b->sendLobbyChoice(cb);
    pump();
    CHECK(a->lobby().slots[0].choice.spawn == 2 && a->lobby().slots[1].choice.spawn == kRandom);
    CHECK(a->lobby().slots[1].choice.faction == 1);
    CHECK(!host.everyoneReady(2));
    ca.ready = true;
    cb.ready = true;
    cb.spawn = 9;   // the map has no ninth start: refused on decode, nothing changes
    a->sendLobbyChoice(ca);
    b->sendLobbyChoice(cb);
    pump();
    CHECK(!host.everyoneReady(2));
    cb.spawn = 3;
    b->sendLobbyChoice(cb);
    pump();
    CHECK(host.everyoneReady(2) && !host.everyoneReady(3));
    // A file the host offers comes to whoever asks; an unknown one comes back empty
    host.offerFile(0xABCDEF, std::vector<uint8_t>(300000, 7));
    a->requestFile(0xABCDEF);
    a->requestFile(0x1234);
    pump();
    uint64_t fileHash = 0;
    std::vector<uint8_t> fileBytes;
    CHECK(a->takeFile(fileHash, fileBytes) && fileHash == 0xABCDEF && fileBytes.size() == 300000 && fileBytes[299999] == 7);
    CHECK(a->takeFile(fileHash, fileBytes) && fileHash == 0x1234 && fileBytes.empty());
    CHECK(!a->takeFile(fileHash, fileBytes));
    // Another map: nobody is ready any more
    host.setLobbyMap("assets/maps/brod.json", 2);
    pump();
    CHECK(!host.everyoneReady(2) && a->lobby().slots[1].choice.spawn == kRandom);
    // A computer player: in the next free slot, always ready, counted for the start (a 2-start map is
    // full with two people: refused there)
    OwnerSetAi add;
    host.setAiSlot(add);
    pump();
    CHECK(a->lobby().slots.size() == 2);
    host.setLobbyMap("assets/maps/m26.json", 4);
    add.level = 2;
    add.choice.faction = 1;
    host.setAiSlot(add);
    pump();
    CHECK(a->lobby().slots.size() == 3 && a->lobby().slots[2].ai && a->lobby().slots[2].aiLevel == 2 &&
          a->lobby().slots[2].choice.ready && a->lobby().slots[2].choice.faction == 1);
    ca.ready = true;
    a->sendLobbyChoice(ca);
    cb.ready = true;
    cb.spawn = kRandom;
    b->sendLobbyChoice(cb);
    pump();
    CHECK(host.everyoneReady(3));
    OwnerSetAi drop;
    drop.slot = 2;
    drop.remove = true;
    host.setAiSlot(drop);
    pump();
    CHECK(a->lobby().slots.size() == 2);
    // B leaves the room: its slot is free for the next one
    b.reset();
    pump();
    CHECK(host.playerCount() == 1 && a->lobby().slots.size() == 1);
    SessionClient c(hub.client(), clientConfig("C"));
    for (int i = 0; i < 3; ++i) {
        c.update(0);
        host.update(0);
        c.update(0);
    }
    CHECK(c.slot() == 1);

    // The room holds as many players as the map has start positions: a map for two is full with two
    host.setLobbyMap("assets/maps/map_skirmish_5.json", 2);
    SessionClient d(hub.client(), clientConfig("D"));
    for (int i = 0; i < 3; ++i) {
        d.update(0);
        host.update(0);
        d.update(0);
    }
    CHECK(d.state() == SessionClient::State::Rejected && d.rejectReason() == RejectReason::RoomFull);
    CHECK(host.playerCount() == 2);
    // ...and a map with fewer positions than there are players is not taken
    host.setLobbyMap("assets/maps/m26.json", 4);
    host.setLobbyMap("assets/maps/map_skirmish_5.json", 1);
    pump();
    CHECK(a->lobby().map == "assets/maps/m26.json" && a->lobby().spawnCount == 4);
}

// The same over real UDP on localhost
void testEnetSession() {
    std::printf("[enet session]\n");
    HostConfig hc;
    hc.identity = testIdentity();
    SessionHost host(hc);
    uint16_t port = 0;
    std::string error;
    host.addTransport(createEnetServer(0, 8, &error, &port));
    CHECK(port != 0);
    SessionClient client(createEnetClient("127.0.0.1", port, &error), clientConfig("Remote"));
    const auto start = std::chrono::steady_clock::now();
    auto nowMs = [&] { return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()); };
    bool started = false, sent = false;
    while (nowMs() < 5000) {
        client.update(nowMs());
        host.update(nowMs());
        if (!started && client.state() == SessionClient::State::Joined) {
            host.startMatch({1}, nowMs(), 0);
            started = true;
        }
        if (started && !sent && client.matchStarted() && client.turnsReceived() == 1) {
            client.sendCommands(3, {42});
            sent = true;
        }
        if (client.turnsReceived() > 5) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(client.state() == SessionClient::State::Joined);
    CHECK(client.turnsReceived() > 5);
    const TurnBundle* t3 = client.turn(3);
    CHECK(t3 && t3->players.size() == 1 && t3->players[0].bytes == std::vector<uint8_t>{42});
    std::printf("  joined on port %u, %u turns over UDP\n", port, client.turnsReceived());
}

// A browser finds a host on this computer through its broadcast / loopback probes
void testDiscovery() {
    std::printf("[discovery]\n");
    {
        RoomInfo info{"1.0", "Room", "assets/maps/m26.json", 3, 8, false};
        const std::vector<uint8_t> bytes = encodeAnswer(77, 38101, info);
        uint32_t nonce = 0;
        uint16_t port = 0;
        RoomInfo back;
        CHECK(decodeAnswer(bytes.data(), bytes.size(), nonce, port, back) && nonce == 77 && port == 38101 &&
              back.name == "Room" && back.players == 3);
        for (size_t cut = 0; cut < bytes.size(); ++cut) CHECK(!decodeAnswer(bytes.data(), cut, nonce, port, back));
        const std::vector<uint8_t> probe = encodeProbe(5);
        CHECK(decodeProbe(probe.data(), probe.size(), nonce) && nonce == 5);
        CHECK(!decodeProbe(bytes.data(), bytes.size(), nonce));
    }
    const uint16_t base = 38170;
    HostConfig hc;
    hc.identity = testIdentity();
    hc.roomName = "LAN party";
    SessionHost host(hc);
    std::string error;
    auto server = createEnetServer(static_cast<uint16_t>(base + 1), 8, &error);   // the second port of the span
    CHECK(server != nullptr);
    if (!server) return;
    host.addTransport(std::move(server));
    host.setLobbyMap("assets/maps/brod.json", 4);
    LanBrowser browser(base);
    CHECK(browser.ok());
    const auto start = std::chrono::steady_clock::now();
    auto nowMs = [&] { return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()); };
    while (nowMs() < 2500 && browser.rooms().empty()) {
        browser.update(nowMs());
        host.update(nowMs());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(browser.rooms().size() == 1);
    if (!browser.rooms().empty()) {
        const FoundRoom& r = browser.rooms()[0];
        CHECK(r.port == base + 1 && r.info.name == "LAN party" && r.info.map == "assets/maps/brod.json" && r.info.maxPlayers == 4);
        std::printf("  found '%s' at %s:%u, ping %u ms\n", r.info.name.c_str(), r.address.c_str(), r.port, r.pingMs);
        // The game socket still speaks ENet: a client joins through the same port
        SessionClient client(createEnetClient(r.address, r.port, &error), clientConfig("Finder"));
        while (nowMs() < 5000 && client.state() != SessionClient::State::Joined) {
            client.update(nowMs());
            host.update(nowMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(client.state() == SessionClient::State::Joined);
    }
}

// openaow-server end to end over UDP: create a room, find it by code, two players in it, the owner
// picks the map and starts, turns run
void testRoomServer() {
    std::printf("[room server]\n");
    RoomServerConfig cfg;
    cfg.port = 38150;
    cfg.roomPortFirst = 38151;
    cfg.roomPortLast = 38155;
    cfg.name = "Test server";
    RoomServer server(cfg);
    std::string error;
    CHECK(server.start(error));
    const auto start = std::chrono::steady_clock::now();
    auto nowMs = [&] { return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()); };
    auto spin = [&](const std::function<bool()>& done, uint32_t limitMs, const std::function<void()>& also = {}) {
        // done() may consume what it checks (takeRoom): ask it once per round
        const uint32_t until = nowMs() + limitMs;
        while (nowMs() < until) {
            server.update(nowMs());
            if (also) also();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    const GameIdentity id = testIdentity();

    MasterClient creator("127.0.0.1", cfg.port);
    creator.createRoom(id, "Friday", true);
    RoomAddress room;
    CHECK(spin([&] { creator.update(nowMs()); return creator.takeRoom(room); }, 3000));
    CHECK(room.code.size() == kRoomCodeLength && room.port == 38151);
    CHECK(creator.serverInfo().name == "Test server");

    MasterClient other("127.0.0.1", cfg.port);
    std::string lower = room.code;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    other.findRoom(lower);
    other.requestList(id.gameVersion);
    RoomAddress found;
    CHECK(spin([&] { other.update(nowMs()); return other.takeRoom(found); }, 3000));
    CHECK(found.port == room.port && found.code == room.code);
    CHECK(spin([&] { other.update(nowMs()); return other.hasList(); }, 3000));
    CHECK(other.rooms().size() == 1 && other.rooms()[0].code == room.code && other.rooms()[0].name == "Friday");
    other.findRoom("ZZZZZZ");
    RejectReason why{};
    CHECK(spin([&] { other.update(nowMs()); return other.takeRefusal(why); }, 3000) && why == RejectReason::NotFound);
    other.requestList("another version");
    CHECK(spin([&] { other.update(nowMs()); return other.hasList() && other.rooms().empty(); }, 3000));

    // Two players in the room; the first is its owner
    SessionClient a(createEnetClient("127.0.0.1", room.port, &error), clientConfig("Owner"));
    SessionClient b(createEnetClient("127.0.0.1", room.port, &error), clientConfig("Guest"));
    auto clients = [&] { a.update(nowMs()); b.update(nowMs()); };
    CHECK(spin([&] { return a.state() == SessionClient::State::Joined && b.state() == SessionClient::State::Joined; }, 3000, clients));
    b.sendOwnerSetMap("assets/maps/hacked.json", 2);   // not the owner: ignored
    a.sendOwnerSetMap("assets/maps/m26.json", 4);
    CHECK(spin([&] { return b.lobby().map == "assets/maps/m26.json"; }, 3000, clients));
    LobbyChoice ready;
    ready.ready = true;
    a.sendLobbyChoice(ready);
    b.sendLobbyChoice(ready);
    spin([&] { return false; }, 200, clients);
    // The owner gives the room its map; the other player fetches it from the room
    a.uploadFile(0x77, std::vector<uint8_t>(20000, 3));
    b.uploadFile(0x88, {1});   // not the owner: ignored
    spin([&] { return false; }, 200, clients);
    b.requestFile(0x77);
    b.requestFile(0x88);
    uint64_t fh = 0;
    std::vector<uint8_t> fb, fb2;
    CHECK(spin([&] { return b.takeFile(fh, fb); }, 3000, clients) && fh == 0x77 && fb.size() == 20000);
    CHECK(spin([&] { return b.takeFile(fh, fb2); }, 3000, clients) && fh == 0x88 && fb2.empty());
    b.sendOwnerStart({9});   // not the owner: ignored
    a.sendOwnerStart({4, 2}, 0);
    CHECK(spin([&] { return a.matchStarted() && b.matchStarted() && b.turnsReceived() > 3; }, 4000, clients));
    CHECK(b.matchSetup() == std::vector<uint8_t>({4, 2}));
    const std::string json = server.statusJson();
    CHECK(json.find(room.code) != std::string::npos && json.find("\"running\":true") != std::string::npos);
    std::printf("  room %s on port %u: %u turns, status %zu bytes\n", room.code.c_str(), room.port, b.turnsReceived(), json.size());
}

// A game hosted by a player, found through the server by its code, joined after the hole punching
// (over localhost the punching is trivial, the whole exchange runs though)
void testHostedGame() {
    std::printf("[player-hosted game through the server]\n");
    RoomServerConfig cfg;
    cfg.port = 38140;
    cfg.roomPortFirst = 38141;
    cfg.roomPortLast = 38142;
    RoomServer server(cfg);
    std::string error;
    CHECK(server.start(error));
    HostConfig hc;
    hc.identity = testIdentity();
    hc.roomName = "Home game";
    SessionHost host(hc);
    uint16_t gamePort = 0;
    auto socket = createEnetServer(0, 8, &error, &gamePort);
    Transport& hostSocket = *socket;
    host.addTransport(std::move(socket));
    host.setLobbyMap("assets/maps/brod.json", 2);
    HostAnnouncer announcer("127.0.0.1", cfg.port, hostSocket, gamePort, "127.0.0.1");
    const auto start = std::chrono::steady_clock::now();
    auto nowMs = [&] { return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()); };
    auto tick = [&] {
        server.update(nowMs());
        host.update(nowMs());
        announcer.update(nowMs(), host.roomInfo(), true);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    while (nowMs() < 3000 && announcer.code().empty()) tick();
    CHECK(announcer.code().size() == kRoomCodeLength);
    const std::vector<RoomListing> list = server.listing(testIdentity().gameVersion);
    CHECK(list.size() == 1 && list[0].hosted && list[0].name == "Home game" && list[0].code == announcer.code());

    HostedJoin join("127.0.0.1", cfg.port, announcer.code());
    while (nowMs() < 8000 && join.state() == HostedJoin::State::Working) {
        join.update(nowMs());
        tick();
    }
    CHECK(join.state() == HostedJoin::State::Ready);
    if (join.state() != HostedJoin::State::Ready) {
        std::printf("  join failed: %s\n", join.error().c_str());
        return;
    }
    SessionClient client(join.takeTransport(), clientConfig("Visitor"));
    while (nowMs() < 12000 && client.state() != SessionClient::State::Joined) {
        client.update(nowMs());
        tick();
    }
    CHECK(client.state() == SessionClient::State::Joined && client.slot() == 0);
    std::printf("  hosted game %s joined through the server (lan ip here: %s)\n", announcer.code().c_str(), localLanIp().c_str());

    HostedJoin nowhere("127.0.0.1", cfg.port, "ZZZZZZ");
    while (nowMs() < 16000 && nowhere.state() == HostedJoin::State::Working) {
        nowhere.update(nowMs());
        tick();
    }
    CHECK(nowhere.state() == HostedJoin::State::Failed);
}

// Room password, a player who leaves for good, and a running match whose players are gone
void testPasswordAndLeave() {
    std::printf("[password, leave, zombie match]\n");
    LoopbackHub hub;
    HostConfig hc;
    hc.identity = testIdentity();
    hc.password = "secret";
    hc.reconnectWindowMs = 300;
    SessionHost host(hc);
    host.addTransport(hub.server());
    uint32_t now = 0;
    std::vector<SessionClient*> all;
    auto step = [&](uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            now += 10;
            for (SessionClient* c : all) c->update(now);
            host.update(now);
            for (SessionClient* c : all) c->update(now);
        }
    };
    ClientConfig noPassword = clientConfig("A");
    SessionClient a(hub.client(), noPassword);
    all.push_back(&a);
    step(50);
    CHECK(a.state() == SessionClient::State::Rejected && a.rejectReason() == RejectReason::BadPassword);
    CHECK(host.playerCount() == 0);

    ClientConfig good = clientConfig("B");
    good.password = "secret";
    SessionClient b(hub.client(), good);
    ClientConfig good2 = clientConfig("C");
    good2.password = "secret";
    SessionClient c(hub.client(), good2);
    all = {&b, &c};
    step(50);
    CHECK(b.state() == SessionClient::State::Joined && c.state() == SessionClient::State::Joined && host.playerCount() == 2);

    // Leaving for good frees the slot at once
    c.sendLeave();
    step(50);
    CHECK(host.playerCount() == 1);

    // A running match: a dropped player is given up after the window, the last one leaving ends the match
    ClientConfig third = clientConfig("D");
    third.password = "secret";
    auto d = std::make_unique<SessionClient>(hub.client(), third);
    all = {&b, d.get()};
    step(50);
    CHECK(host.playerCount() == 2);
    host.startMatch({1, 2, 3}, now, 0);
    step(100);
    CHECK(host.matchRunning());
    all = {&b};
    d.reset();                 // the connection just drops
    step(100);
    CHECK(host.playerCount() == 2);     // still kept for a reconnect
    step(400);
    CHECK(host.playerCount() == 1);     // given up after the window
    b.sendLeave();
    step(50);
    CHECK(host.playerCount() == 0 && !host.matchRunning());
}

} // namespace

int main() {
    testCrc32();
    testBitStream();
    testProtocol();
    testFuzz();
    testEnetLoopback();
    testLockstepSession();
    testLobby();
    testPasswordAndLeave();
    testEnetSession();
    testDiscovery();
    testRoomServer();
    testHostedGame();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
