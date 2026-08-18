#include "Network/EchoServer.h"
#include "Network/Database.h"
#include <cstdio>
#include <thread>

int main()
{
    constexpr uint16_t kPort = 7777;
    // hardware_concurrency() can return 0 if it can't be determined (rare,
    // but happens in some sandboxed/virtualized environments) -- fall back
    // to the old fixed value of 4 in that case.
    const uint32_t kWorkerThreadCount =
        std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 4;
    // Demo day: raised from 2 so the room isn't capped. sessions_ is a plain
    // unordered_map (see EchoServer.h) so this isn't a hardcoded-size issue,
    // just a policy cap -- pick any number the network/hardware can take.
    constexpr uint32_t kMaxPlayers = 32;

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
