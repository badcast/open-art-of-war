#include "openaow/net/Net.hpp"
#include "openaow/net/Transport.hpp"
#include "transport/EnetSupport.hpp"

#include <cstring>
#include <unordered_map>

#include "openaow/net/Discovery.hpp"

namespace OpenArtOfWar::Net {

namespace {

constexpr size_t kChannels = static_cast<size_t>(Channel::Count);

enet_uint32 packetFlags(Channel c) {
    // Realtime: newest wins, nothing resent; everything else reliable and in order per channel
    return c == Channel::Realtime ? 0 : ENET_PACKET_FLAG_RELIABLE;
}

class EnetTransport;

// Hosts and their transports, for ENet's raw packet hook (single-threaded like the transports)
std::unordered_map<ENetHost*, EnetTransport*>& registry() {
    static std::unordered_map<ENetHost*, EnetTransport*> r;
    return r;
}

class EnetTransport : public Transport {
public:
    EnetTransport(ENetHost* host, bool client) : m_host(host), m_client(client) {
        registry()[host] = this;
        host->intercept = &EnetTransport::intercept;
    }
    ~EnetTransport() override {
        registry().erase(m_host);
        for (auto& [id, peer] : m_peers) enet_peer_disconnect_now(peer, 0);
        enet_host_flush(m_host);
        enet_host_destroy(m_host);
    }

    void addPendingClientPeer(ENetPeer* peer) { m_pending = peer; }

    void send(PeerId peer, Channel channel, const uint8_t* data, size_t size) override {
        auto it = m_peers.find(peer);
        if (it == m_peers.end()) return;
        ENetPacket* p = enet_packet_create(data, size, packetFlags(channel));
        if (enet_peer_send(it->second, static_cast<enet_uint8>(channel), p) != 0) enet_packet_destroy(p);
    }

    void poll(std::vector<TransportEvent>& out) override {
        ENetEvent ev;
        while (enet_host_service(m_host, &ev, 0) > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    const PeerId id = m_client ? 0 : m_nextId++;
                    ev.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
                    m_peers[id] = ev.peer;
                    // A dropped peer is noticed within the game's timeout, not ENet's half minute
                    enet_peer_timeout(ev.peer, 32, 5000, 15000);
                    out.push_back({TransportEvent::Type::Connected, id, Channel::Control, {}});
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    const PeerId id = static_cast<PeerId>(reinterpret_cast<uintptr_t>(ev.peer->data));
                    if (m_peers.erase(id) || m_client) {
                        out.push_back({TransportEvent::Type::Disconnected, id, Channel::Control, {}});
                    }
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    const PeerId id = static_cast<PeerId>(reinterpret_cast<uintptr_t>(ev.peer->data));
                    TransportEvent e{TransportEvent::Type::Received, id,
                                     ev.channelID < kChannels ? static_cast<Channel>(ev.channelID) : Channel::Control,
                                     std::vector<uint8_t>(ev.packet->data, ev.packet->data + ev.packet->dataLength)};
                    enet_packet_destroy(ev.packet);
                    out.push_back(std::move(e));
                    break;
                }
                default:
                    break;
            }
        }
        enet_host_flush(m_host);
    }

    void disconnect(PeerId peer) override {
        auto it = m_peers.find(peer);
        if (it == m_peers.end()) return;
        enet_peer_disconnect_later(it->second, 0);
    }

    uint32_t roundTripMs(PeerId peer) const override {
        auto it = m_peers.find(peer);
        return it == m_peers.end() ? 0 : it->second->roundTripTime;
    }

    void setRoomInfo(const RoomInfo& info) override {
        m_room = info;
        m_hasRoom = true;
    }

    // A datagram that is not ENet's: a local network browser asking who is there
    void sendRaw(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data) override {
        ENetAddress to{};
        // An address, or a name resolved once and kept
        if (enet_address_set_host_ip(&to, ip.c_str()) != 0) {
            auto known = m_resolved.find(ip);
            if (known == m_resolved.end()) {
                if (enet_address_set_host(&to, ip.c_str()) != 0) return;
                known = m_resolved.emplace(ip, to.host).first;
            }
            to.host = known->second;
        }
        to.port = port;
        ENetBuffer buffer;
        buffer.data = const_cast<uint8_t*>(data.data());
        buffer.dataLength = data.size();
        enet_socket_send(m_host->socket, &to, &buffer, 1);
    }

    void setRawHandler(RawHandler handler) override { m_raw = std::move(handler); }

    bool connectTo(const std::string& hostName, uint16_t port) override {
        if (!m_client || !m_peers.empty() || m_pending) return false;
        ENetAddress address{};
        if (enet_address_set_host(&address, hostName.c_str()) != 0) return false;
        address.port = port;
        m_pending = enet_host_connect(m_host, &address, kChannels, 0);
        if (m_pending) m_pending->data = nullptr;
        return m_pending != nullptr;
    }

    // Datagrams that are not ENet's: local network probes (answered by a listening transport) and raw
    // datagrams (punching, socket hellos) for the handler
    static int ENET_CALLBACK intercept(ENetHost* host, ENetEvent* event) {
        (void)event;
        auto it = registry().find(host);
        RawKind kind{};
        const uint8_t* payload = nullptr;
        size_t payloadSize = 0;
        if (decodeRaw(host->receivedData, host->receivedDataLength, kind, payload, payloadSize)) {
            if (it != registry().end() && it->second->m_raw) {
                char ip[32] = {};
                enet_address_get_host_ip(&host->receivedAddress, ip, sizeof ip);
                it->second->m_raw(ip, host->receivedAddress.port, host->receivedData, host->receivedDataLength);
            }
            return 1;
        }
        uint32_t nonce = 0;
        if (!decodeProbe(host->receivedData, host->receivedDataLength, nonce)) return 0;   // ENet's own
        if (it == registry().end() || it->second->m_client || !it->second->m_hasRoom) return 1;
        ENetAddress bound{};
        enet_socket_get_address(host->socket, &bound);
        const std::vector<uint8_t> answer = encodeAnswer(nonce, bound.port, it->second->m_room);
        ENetBuffer buffer;
        buffer.data = const_cast<uint8_t*>(answer.data());
        buffer.dataLength = answer.size();
        enet_socket_send(host->socket, &host->receivedAddress, &buffer, 1);
        return 1;
    }

private:
    ENetHost* m_host;
    bool m_client;
    ENetPeer* m_pending{nullptr};
    PeerId m_nextId{1};
    std::unordered_map<PeerId, ENetPeer*> m_peers;
    RoomInfo m_room;
    bool m_hasRoom{false};
    RawHandler m_raw;
    std::unordered_map<std::string, enet_uint32> m_resolved;
    Session m_session;   // keeps the socket layer up for as long as the transport lives
};

} // namespace

std::unique_ptr<Transport> createEnetServer(uint16_t port, int maxPeers, std::string* error, uint16_t* boundPort,
                                            const std::string& bindAddress) {
    if (!initialize()) {
        if (error) *error = "socket layer";
        return nullptr;
    }
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    if (!bindAddress.empty() && bindAddress != "0.0.0.0" && enet_address_set_host_ip(&address, bindAddress.c_str()) != 0) {
        shutdown();
        if (error) *error = "bad bind address " + bindAddress;
        return nullptr;
    }
    address.port = port;
    ENetHost* host = enet_host_create(&address, static_cast<size_t>(maxPeers), kChannels, 0, 0);
    if (!host) {
        shutdown();
        if (error) *error = "port " + std::to_string(port) + " is in use";
        return nullptr;
    }
    configureHost(host);
    if (boundPort) {
        ENetAddress bound{};
        enet_socket_get_address(host->socket, &bound);
        *boundPort = bound.port;
    }
    auto t = std::make_unique<EnetTransport>(host, false);
    shutdown();   // from here on the transport holds its own reference (Session)
    return t;
}

std::unique_ptr<Transport> createEnetSocket(std::string* error) {
    if (!initialize()) {
        if (error) *error = "socket layer";
        return nullptr;
    }
    ENetHost* host = enet_host_create(nullptr, 1, kChannels, 0, 0);
    if (!host) {
        shutdown();
        if (error) *error = "no socket";
        return nullptr;
    }
    configureHost(host);
    auto t = std::make_unique<EnetTransport>(host, true);
    shutdown();
    return t;
}

namespace {
constexpr char kRawMagic[8] = {'O', 'A', 'O', 'W', 'R', 'A', 'W', '1'};
} // namespace

std::vector<uint8_t> encodeRaw(RawKind kind, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out(kRawMagic, kRawMagic + 8);
    out.push_back(static_cast<uint8_t>(kind));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool decodeRaw(const uint8_t* data, size_t size, RawKind& kind, const uint8_t*& payload, size_t& payloadSize) {
    if (size < 9 || std::memcmp(data, kRawMagic, 8) != 0) return false;
    kind = static_cast<RawKind>(data[8]);
    payload = data + 9;
    payloadSize = size - 9;
    return true;
}

std::unique_ptr<Transport> createEnetClient(const std::string& hostName, uint16_t port, std::string* error) {
    if (!initialize()) {
        if (error) *error = "socket layer";
        return nullptr;
    }
    ENetHost* host = enet_host_create(nullptr, 1, kChannels, 0, 0);
    if (!host) {
        shutdown();
        if (error) *error = "no socket";
        return nullptr;
    }
    configureHost(host);
    ENetAddress address{};
    if (enet_address_set_host(&address, hostName.c_str()) != 0) {
        enet_host_destroy(host);
        shutdown();
        if (error) *error = "unknown host " + hostName;
        return nullptr;
    }
    address.port = port;
    ENetPeer* peer = enet_host_connect(host, &address, kChannels, 0);
    if (!peer) {
        enet_host_destroy(host);
        shutdown();
        if (error) *error = "cannot connect";
        return nullptr;
    }
    peer->data = nullptr;   // PeerId 0
    auto t = std::make_unique<EnetTransport>(host, true);
    t->addPendingClientPeer(peer);
    shutdown();
    return t;
}

} // namespace OpenArtOfWar::Net
