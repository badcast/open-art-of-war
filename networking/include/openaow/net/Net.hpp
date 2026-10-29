#pragma once

namespace OpenArtOfWar::Net {

// Starts the socket layer (WinSock on Windows). Counted: every successful initialize() needs one
// shutdown(); the last one releases it. Thread-safe.
bool initialize();
void shutdown();

// RAII helper for initialize() / shutdown()
class Session {
public:
    Session() : m_ok(initialize()) {}
    ~Session() {
        if (m_ok) shutdown();
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    bool ok() const { return m_ok; }

private:
    bool m_ok;
};

} // namespace OpenArtOfWar::Net
