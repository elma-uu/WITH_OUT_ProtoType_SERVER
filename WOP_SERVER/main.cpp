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

    // Multi map's exported wall geometry, for server-driven enemy obstacle
    // avoidance -- see ExportLevelObstaclesCommandlet on the client side.
    // No-op (enemies just move in straight lines) if this file is missing.
    server.LoadEnemyObstacles("Data/L_Stage2_obstacles.txt");

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
