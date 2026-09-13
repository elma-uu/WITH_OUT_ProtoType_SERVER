#pragma once
#include <cstdint>

namespace Wop
{
    // Shared entry point for both server executables (WOP_LoginServer and
    // WOP_GameServer -- see 매칭 서버 설계, step 4 of the Login/Game server
    // split). Both link the EXACT same EchoServer/Session/Room/Matchmaker/
    // Database code and are functionally full supersets of each other
    // today -- the only difference between "the Login server" and "the
    // Game server" is which port a client happens to connect to for which
    // purpose (see C2S_RequestMatch/C2S_JoinMatch's schema comments), not
    // anything this function does differently. `label` is purely for the
    // console log prefix, so two instances running side by side (as they
    // do in every live test now) are distinguishable in their own output.
    int RunServer(uint16_t port, const char* label);
}
