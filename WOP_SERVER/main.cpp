#include "Network/EchoServer.h"
#include <cstdio>

int main()
{
    constexpr uint16_t kPort = 7777;
    constexpr uint32_t kWorkerThreadCount = 4;

    Wop::EchoServer server(kPort, kWorkerThreadCount);
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
