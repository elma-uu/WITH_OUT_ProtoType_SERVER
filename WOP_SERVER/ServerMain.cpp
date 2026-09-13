#include "ServerMain.h"
#include "Network/EchoServer.h"
#include "Network/Database.h"
#include <cstdio>
#include <thread>

namespace Wop
{
    int RunServer(uint16_t port, const char* label)
    {
        const uint32_t kWorkerThreadCount =
            std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 4;
        constexpr uint32_t kMaxPlayers = 32;

        if (!Database::Get().Connect())
        {
            std::printf("[%s] Warning: could not connect to WithStandGameDB; "
                         "running without accounts/persistence.\n", label);
        }

        EchoServer server(port, kWorkerThreadCount, kMaxPlayers);

        // Multi map's exported wall geometry, for server-driven enemy
        // obstacle avoidance -- see ExportLevelObstaclesCommandlet on the
        // client side. No-op (enemies just move in straight lines) if this
        // file is missing. Applied even on what's conventionally "the
        // Login server": it runs the exact same Room/EnemyAI code as the
        // Game server (see this function's header comment), so a match
        // formed there (today's compatibility path for C2S_Login sessions
        // that never go through a ticket -- see Session::
        // FinishAuthenticatedLogin) still gets real obstacle-aware zombies.
        server.LoadEnemyObstacles("Data/L_Stage2_obstacles.txt");

        if (!server.Start())
        {
            std::printf("[%s] Failed to start EchoServer.\n", label);
            return 1;
        }

        std::printf("[%s] Press Enter to stop the server...\n", label);
        std::getchar();

        server.Stop();
        return 0;
    }
}
