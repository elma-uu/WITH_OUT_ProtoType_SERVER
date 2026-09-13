#include "ServerMain.h"

// LoginServer -- clients authenticate here (C2S_Login) and request a match
// ticket (C2S_RequestMatch) to hand off to the Game server. See
// ServerMain.h/RunServer's comment for what actually differs from
// main_game.cpp (just this port number and the log label -- nothing else).
int main()
{
    return Wop::RunServer(7777, "LoginServer");
}
