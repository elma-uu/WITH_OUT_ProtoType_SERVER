#include "ServerMain.h"
#include "Network/EchoServer.h"
#include "Network/Database.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace Wop
{
    int RunServer(uint16_t port, const char* label)
    {
        const uint32_t kWorkerThreadCount =
            std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 4;
        constexpr uint32_t kMaxPlayers = 32;

        // 실제 배포에서는 EchoServer::kMatchWindow(10초)를 그대로 쓰지만, 로컬
        // 라이브 소켓 테스트 스위트가 방 하나 형성될 때마다 실제로 10초씩
        // 기다리게 만들면 전체 스위트가 몇 분씩 걸린다 -- 테스트 러너 스크립트가
        // 이 환경변수를 짧게(예: 300ms) 세팅해서 서버를 띄우면 그 값을 쓴다.
        // 세팅 안 하면(일반 실행) EchoServer::kMatchWindow 그대로.
        std::chrono::milliseconds matchWindow = EchoServer::kMatchWindow;
        {
            // _dupenv_s, not std::getenv: MSVC's /sdl flags getenv as
            // unsafe (C4996, promoted to a hard error under /sdl) --
            // _dupenv_s is its suggested replacement. Allocates *overrideMs
            // via malloc when found; must be freed ourselves.
            char* overrideMs = nullptr;
            size_t overrideMsLen = 0;
            if (_dupenv_s(&overrideMs, &overrideMsLen, "WOP_MATCH_WINDOW_MS") == 0 && overrideMs != nullptr)
            {
                const int parsed = std::atoi(overrideMs);
                if (parsed > 0)
                    matchWindow = std::chrono::milliseconds(parsed);
                free(overrideMs);
            }
        }

        if (!Database::Get().Connect())
        {
            std::printf("[%s] Warning: could not connect to WithStandGameDB; "
                         "running without accounts/persistence.\n", label);
        }

        EchoServer server(port, kWorkerThreadCount, kMaxPlayers, matchWindow);

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
