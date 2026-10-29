#include "openaow/net/Transport.hpp"

#include <deque>
#include <map>

namespace OpenArtOfWar::Net {

struct LoopbackHub::State {
    struct Packet {
        PeerId from;
        Channel channel;
        std::vector<uint8_t> data;
    };
    // Inbox of the server and of every client (by client id); events in arrival order
    std::deque<TransportEvent> serverInbox;
    std::map<PeerId, std::deque<TransportEvent>> clientInbox;
    std::map<PeerId, bool> clientConnected;
    PeerId nextClient{1};
    bool serverAlive{false};
};

namespace {

class LoopbackServer : public Transport {
public:
    explicit LoopbackServer(std::shared_ptr<LoopbackHub::State> s) : m_s(std::move(s)) { m_s->serverAlive = true; }
    ~LoopbackServer() override {
        m_s->serverAlive = false;
        for (auto& [id, inbox] : m_s->clientInbox) {
            if (m_s->clientConnected[id]) inbox.push_back({TransportEvent::Type::Disconnected, 0, Channel::Control, {}});
            m_s->clientConnected[id] = false;
        }
    }
    void send(PeerId peer, Channel channel, const uint8_t* data, size_t size) override {
        auto it = m_s->clientInbox.find(peer);
        if (it == m_s->clientInbox.end() || !m_s->clientConnected[peer]) return;
        it->second.push_back({TransportEvent::Type::Received, 0, channel, std::vector<uint8_t>(data, data + size)});
    }
    void poll(std::vector<TransportEvent>& out) override {
        while (!m_s->serverInbox.empty()) {
            out.push_back(std::move(m_s->serverInbox.front()));
            m_s->serverInbox.pop_front();
        }
    }
    void disconnect(PeerId peer) override {
        if (!m_s->clientConnected[peer]) return;
        m_s->clientConnected[peer] = false;
        m_s->clientInbox[peer].push_back({TransportEvent::Type::Disconnected, 0, Channel::Control, {}});
        m_s->serverInbox.push_back({TransportEvent::Type::Disconnected, peer, Channel::Control, {}});
    }
    uint32_t roundTripMs(PeerId) const override { return 0; }

private:
    std::shared_ptr<LoopbackHub::State> m_s;
};

class LoopbackClient : public Transport {
public:
    explicit LoopbackClient(std::shared_ptr<LoopbackHub::State> s) : m_s(std::move(s)), m_id(m_s->nextClient++) {
        m_s->clientInbox[m_id];
        m_s->clientConnected[m_id] = m_s->serverAlive;
        if (m_s->serverAlive) {
            m_s->clientInbox[m_id].push_back({TransportEvent::Type::Connected, 0, Channel::Control, {}});
            m_s->serverInbox.push_back({TransportEvent::Type::Connected, m_id, Channel::Control, {}});
        }
    }
    ~LoopbackClient() override { disconnect(0); }
    void send(PeerId, Channel channel, const uint8_t* data, size_t size) override {
        if (!m_s->clientConnected[m_id]) return;
        m_s->serverInbox.push_back({TransportEvent::Type::Received, m_id, channel, std::vector<uint8_t>(data, data + size)});
    }
    void poll(std::vector<TransportEvent>& out) override {
        auto& inbox = m_s->clientInbox[m_id];
        while (!inbox.empty()) {
            out.push_back(std::move(inbox.front()));
            inbox.pop_front();
        }
    }
    void disconnect(PeerId) override {
        if (!m_s->clientConnected[m_id]) return;
        m_s->clientConnected[m_id] = false;
        m_s->serverInbox.push_back({TransportEvent::Type::Disconnected, m_id, Channel::Control, {}});
    }
    uint32_t roundTripMs(PeerId) const override { return 0; }

private:
    std::shared_ptr<LoopbackHub::State> m_s;
    PeerId m_id;
};

} // namespace

LoopbackHub::LoopbackHub() : m_state(std::make_shared<State>()) {}
LoopbackHub::~LoopbackHub() = default;
std::unique_ptr<Transport> LoopbackHub::server() { return std::make_unique<LoopbackServer>(m_state); }
std::unique_ptr<Transport> LoopbackHub::client() { return std::make_unique<LoopbackClient>(m_state); }

} // namespace OpenArtOfWar::Net
