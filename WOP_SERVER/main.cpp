#include "Network/EchoServer.h"
#include "Network/Database.h"
#include <cstdio>
#include <thread>

int main()
{
    constexpr uint16_t kPort = 7777;
    const uint32_t kWorkerThreadCount =
        std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 4;
    constexpr uint32_t kMaxPlayers = 32;

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
