#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace OpenArtOfWar::Net
{

// A tiny HTTP endpoint on a TCP port for monitoring: every request gets the current text (JSON) back.
// Non-blocking; serve() handles the connections that are waiting.
class StatusPage
{
public:
    StatusPage();
    ~StatusPage();
    StatusPage(const StatusPage &) = delete;
    StatusPage &operator=(const StatusPage &) = delete;

    bool open(std::uint16_t port, std::string_view bindAddress, std::string &error);
    void serve(std::string_view json);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace OpenArtOfWar::Net
