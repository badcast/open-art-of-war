#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace OpenArtOfWar::Net {

// A tiny HTTP endpoint on a TCP port for monitoring: every request gets the current text (JSON) back.
// Non-blocking; serve() handles the connections that are waiting.
class StatusPage {
public:
    StatusPage();
    ~StatusPage();
    StatusPage(const StatusPage&) = delete;
    StatusPage& operator=(const StatusPage&) = delete;

    bool open(uint16_t port, const std::string& bindAddress, std::string& error);
    void serve(const std::string& json);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace OpenArtOfWar::Net
