#include "ServerMain.h"

// GameServer -- clients redeem a match ticket here (C2S_JoinMatch, from
// S2C_MatchTicket) and play. See ServerMain.h/RunServer's comment for what
// actually differs from main_login.cpp (just this port number and the log
// label -- nothing else).
int main()
{
    return Wop::RunServer(7778, "GameServer");
}
