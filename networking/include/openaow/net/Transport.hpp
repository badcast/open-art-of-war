#pragma once

#include "openaow/net/Discovery.hpp"
#include "openaow/net/Protocol.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace OpenArtOfWar::Net {

using PeerId = uint32_t;

struct TransportEvent {
    enum class Type : uint8_t { Connected, Disconnected, Received };
    Type type{Type::Received};
    PeerId peer{0};
    Channel channel{Channel::Control};
    std::vector<uint8_t> data;
};

// A way to talk to peers. The channel decides the delivery: Realtime is unreliable but sequenced, the
// others are reliable and ordered per channel. Single-threaded: poll() and send() from one thread.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(PeerId peer, Channel channel, const uint8_t* data, size_t size) = 0;
    // Everything that happened since the last poll; also sends what was queued
    virtual void poll(std::vector<TransportEvent>& out) = 0;
    virtual void disconnect(PeerId peer) = 0;
    // Round trip time to the peer in ms (0 when unknown)
    virtual uint32_t roundTripMs(PeerId peer) const = 0;
    // A listening transport that local network browsers can find answers their probes with this
    virtual void setRoomInfo(const RoomInfo& info) { (void)info; }

    // Raw datagrams through the same socket (NAT hole punching, telling a server the socket's public
    // address). They start with their own magic, ENet never sees them.
    using RawHandler = std::function<void(const std::string& ip, uint16_t port, const uint8_t* data, size_t size)>;
    virtual void sendRaw(const std::string& ip, uint16_t port, const std::vector<uint8_t>& data) {
        (void)ip; (void)port; (void)data;
    }
    virtual void setRawHandler(RawHandler handler) { (void)handler; }
    // A client socket made without a peer (createEnetSocket): connect it now; PeerId 0 from then on
    virtual bool connectTo(const std::string& host, uint16_t port) { (void)host; (void)port; return false; }
};

// Raw datagrams: magic, kind, payload
enum class RawKind : uint8_t { Hello = 1, Punch = 2 };
std::vector<uint8_t> encodeRaw(RawKind kind, const std::vector<uint8_t>& payload);
bool decodeRaw(const uint8_t* data, size_t size, RawKind& kind, const uint8_t*& payload, size_t& payloadSize);

// UDP via ENet. A server listens on `port` (0: any free one, see boundPort); a client connects to
// host:port and sees its single peer as PeerId 0. `error` gets the reason on failure.
std::unique_ptr<Transport> createEnetServer(uint16_t port, int maxPeers, std::string* error = nullptr,
                                            uint16_t* boundPort = nullptr, const std::string& bindAddress = "");
std::unique_ptr<Transport> createEnetClient(const std::string& host, uint16_t port, std::string* error = nullptr);
// A client socket that is not connected yet (punch first, then connectTo)
std::unique_ptr<Transport> createEnetSocket(std::string* error = nullptr);

// In-memory transport (a listen server's own client, tests): one server end, any number of client
// ends; delivery on the next poll, nothing lost. The hub must outlive its ends.
class LoopbackHub {
public:
    LoopbackHub();
    ~LoopbackHub();
    std::unique_ptr<Transport> server();
    std::unique_ptr<Transport> client();

    struct State;

private:
    std::shared_ptr<State> m_state;
};

} // namespace OpenArtOfWar::Net
