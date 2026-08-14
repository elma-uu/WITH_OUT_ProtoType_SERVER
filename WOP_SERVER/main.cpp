#include "Network/EchoServer.h"
#include "Network/Database.h"
#include <cstdio>

int main()
{
    constexpr uint16_t kPort = 7777;
    constexpr uint32_t kWorkerThreadCount = 4;
    constexpr uint32_t kMaxPlayers = 2;

    // Best-effort: keep running without persistence (deterministic spawn,
    // no accounts) rather than refuse to start the game server over a
    // DB hiccup -- see Session.cpp's C2S_Login case for the fallback.
    if (!Wop::Database::Get().Connect())
    {
        std::printf("Warning: could not connect to WithStandGameDB; "
                     "running without accounts/persistence.\n");
    }

    Wop::EchoServer server(kPort, kWorkerThreadCount, kMaxPlayers);
    if (!server.Start())
    {
        std::printf("Failed to start EchoServer.\n");
        return 1;
    }

    std::printf("Press Enter to stop the server...\n");
    std::getchar();

    server.Stop();
    return 0;
}
