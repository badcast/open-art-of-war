#include "openaow/net/StatusPage.hpp"
#include "openaow/net/Net.hpp"
#include "transport/EnetSupport.hpp"

namespace OpenArtOfWar::Net {

struct StatusPage::Impl {
    Session session;
    ENetSocket socket{ENET_SOCKET_NULL};
};

StatusPage::StatusPage() : m_impl(std::make_unique<Impl>()) {}

StatusPage::~StatusPage() {
    if (m_impl->socket != ENET_SOCKET_NULL) enet_socket_destroy(m_impl->socket);
}

bool StatusPage::open(uint16_t port, const std::string& bindAddress, std::string& error) {
    m_impl->socket = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
    if (m_impl->socket == ENET_SOCKET_NULL) {
        error = "no socket";
        return false;
    }
    enet_socket_set_option(m_impl->socket, ENET_SOCKOPT_REUSEADDR, 1);
    enet_socket_set_option(m_impl->socket, ENET_SOCKOPT_NONBLOCK, 1);
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    if (!bindAddress.empty() && bindAddress != "0.0.0.0") enet_address_set_host_ip(&address, bindAddress.c_str());
    address.port = port;
    if (enet_socket_bind(m_impl->socket, &address) != 0 || enet_socket_listen(m_impl->socket, 16) != 0) {
        enet_socket_destroy(m_impl->socket);
        m_impl->socket = ENET_SOCKET_NULL;
        error = "TCP port " + std::to_string(port) + " is in use";
        return false;
    }
    return true;
}

void StatusPage::serve(const std::string& json) {
    if (m_impl->socket == ENET_SOCKET_NULL) return;
    for (int i = 0; i < 8; ++i) {
        ENetAddress from{};
        const ENetSocket client = enet_socket_accept(m_impl->socket, &from);
        if (client == ENET_SOCKET_NULL) return;
        // The request itself does not matter: read what is there, answer, close
        char request[1024];
        ENetBuffer in{};
        in.data = request;
        in.dataLength = sizeof request;
        enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
        if (enet_socket_wait(client, &wait, 200) == 0 && (wait & ENET_SOCKET_WAIT_RECEIVE)) enet_socket_receive(client, nullptr, &in, 1);
        const std::string response = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n"
                                     "Content-Length: " + std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
        ENetBuffer out{};
        out.data = const_cast<char*>(response.data());
        out.dataLength = response.size();
        enet_socket_send(client, nullptr, &out, 1);
        enet_socket_shutdown(client, ENET_SOCKET_SHUTDOWN_READ_WRITE);
        enet_socket_destroy(client);
    }
}

} // namespace OpenArtOfWar::Net
