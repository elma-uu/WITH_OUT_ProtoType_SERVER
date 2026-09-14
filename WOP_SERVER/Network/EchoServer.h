#pragma once
#include "NetCommon.h"
#include "Session.h"
#include "Database.h"
#include "Room.h"
#include "Matchmaker.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Wop
{
    // Accepts TCP connections and owns the RIO/IOCP plumbing -- all actual
    // gameplay state (loot/doors/enemies, and who broadcasts to whom) lives
    // on Room now, not here (see Room.h). Step 2 of the Login/Game server
    // separation (매칭 서버 설계): a logged-in session no longer joins a
    // shared world automatically -- EnqueueForMatch (called from Session's
    // own C2S_Login/C2S_JoinMatch handling) always queues it with
    // matchmaker_, which groups waiting sessions into a fresh 2~4-person
    // Room the instant enough have queued (or, failing that, after up to
    // matchWindow if nobody else shows up -- see Matchmaker.h). Rooms are
    // created/destroyed on demand now instead of the single server-lifetime
    // defaultRoom_ step 1 used.
    //
    // 사용자 요청: 한번 형성된 Room은 멤버가 고정된다 -- 이전에는 자리가 남은
    // 기존 Room에 새 세션을 바로 끼워 넣는 "late join" 경로가 있었지만
    // (door_late_join_test가 검증하던 그 동작), 이미 시작된 매칭에 아무도
    // 나중에 끼어들 수 없어야 한다는 요청에 따라 제거했다 -- 재접속을 포함해
    // 모든 세션은 항상 matchmaker_ 대기열로 가서 자기 자신의 새 매칭 시도로
    // 새 Room을 형성한다.
    class EchoServer
    {
    public:
        /*-------------------
         생성/시작/정지
        -------------------*/
        // maxPlayers caps concurrent sessions; a connection beyond that gets
        // S2C_LoginFail{ServerFull} and is closed. See main.cpp for the
        // actual value in use. matchWindow defaults to kMatchWindow (the
        // real 10-second production wait) -- overridable so the live socket
        // test suite (WOP_MATCH_WINDOW_MS, see ServerMain.cpp) doesn't have
        // to actually sit through 10 real seconds per room formed.
        // roomResyncInterval is passed through to every Room this server
        // creates (see Room::MaybeResyncAllMembers) -- same reasoning,
        // overridable via WOP_RESYNC_INTERVAL_MS so the test suite doesn't
        // get extra roster traffic injected mid-test every second.
        EchoServer(uint16_t port, uint32_t workerThreadCount, uint32_t maxPlayers = 2,
                   std::chrono::milliseconds matchWindow = kMatchWindow,
                   std::chrono::milliseconds roomResyncInterval = Room::kDefaultResyncInterval);
        ~EchoServer();

        EchoServer(const EchoServer&) = delete;
        EchoServer& operator=(const EchoServer&) = delete;

        bool Start();
        void Stop();

        // Loads the 2D obstacle boxes a multiplayer map's blocking geometry
        // was exported to (see ExportLevelObstaclesCommandlet on the
        // client side). Safe to call even if the file doesn't exist yet --
        // enemies just move in straight lines until it does. Call before
        // Start() (or any time). Stored and applied to every Room created
        // from here on (see CreateRoom) -- there's only ever one Multi map's
        // worth of geometry today, unlike Rooms themselves there's no
        // per-room variant of this yet.
        void LoadEnemyObstacles(const std::string& path);

        // Called once by Session, right after a successful C2S_Login/
        // C2S_JoinMatch (see those cases' comments) -- NOT at connection
        // time anymore. Always queues `session` with matchmaker_ to be
        // grouped into a fresh squad (see this class's header comment for
        // why there's no "join an existing room" shortcut anymore).
        void EnqueueForMatch(std::shared_ptr<Session> session);

        // Matchmaking policy -- see Matchmaker.h and this class's header
        // comment. Public (not just used as this constructor's own default
        // argument) so ServerMain.cpp's WOP_MATCH_WINDOW_MS override and the
        // test suite both have one authoritative value to fall back to
        // instead of a second hardcoded "10000".
        static constexpr uint32_t kMaxSquadSize = 4;
        // 사용자 요청: 최대 10초까지 기다렸다가, 그때까지 모인 인원(1명이어도)으로
        // 시작한다 -- 대기열이 kMaxSquadSize로 꽉 차면 그 전에도 즉시 형성된다.
        static constexpr std::chrono::milliseconds kMatchWindow{10000};

    private:
        static constexpr ULONG kCompletionQueueSize = 8192;

        /*-------------------
         초기화 (Winsock/RIO/AcceptEx)
        -------------------*/
        bool InitWinsock();
        bool CreateListenSocket();
        bool LoadAcceptEx();
        bool InitRio();

        /*-------------------
         스레드 루프
        -------------------*/
        void AcceptLoop();
        void WorkerLoop();
        // Ticks every live Room's server-driven enemy AI and the
        // matchmaker's solo-timeout check on the same cadence -- was
        // EnemyAiLoop back when there was only ever one Room to tick.
        void BackgroundTickLoop();

        void OnAccepted(SOCKET clientSocket);
        void UnregisterSession(uint32_t sessionId);

        // Rejects a connection once already at maxPlayers_: sends
        // S2C_LoginFail{reason: ServerFull} (best-effort, blocking) and
        // closes the socket without ever creating a Session for it.
        void RejectServerFull(SOCKET clientSocket);

        // Creates a fresh Room (applying enemyObstaclesPath_), registers it
        // in rooms_, and adds every session in `squad` to it -- the
        // formRoom callback Matchmaker invokes once a squad is ready.
        void CreateRoomForSquad(std::vector<std::shared_ptr<Session>> squad);

        /*-------------------
         멤버 변수
        -------------------*/
        uint16_t port_;
        uint32_t workerThreadCount_;
        uint32_t maxPlayers_;
        std::chrono::milliseconds roomResyncInterval_;

        bool winsockReady_ = false;
        SOCKET listenSocket_ = INVALID_SOCKET;
        LPFN_ACCEPTEX acceptEx_ = nullptr;

        HANDLE iocp_ = nullptr;
        RIO_CQ recvCq_ = RIO_INVALID_CQ;
        RIO_CQ sendCq_ = RIO_INVALID_CQ;
        OVERLAPPED recvNotifyOverlapped_{};
        OVERLAPPED sendNotifyOverlapped_{};

        std::atomic<bool> running_{false};
        std::thread acceptThread_;
        std::vector<std::thread> workerThreads_;
        std::thread backgroundThread_;

        std::mutex sessionsLock_;
        std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions_;
        std::atomic<uint32_t> nextSessionId_{1};

        std::string enemyObstaclesPath_;

        std::mutex roomsLock_;
        std::unordered_map<uint32_t, std::shared_ptr<Room>> rooms_;
        std::atomic<uint32_t> nextRoomId_{1};

        Matchmaker matchmaker_;
    };
}
